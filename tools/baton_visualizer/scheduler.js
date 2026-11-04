/**
 * @file scheduler.js
 * @brief Baton 階層的スケジューラ・デカルト木コアシミュレータ
 * 
 * docs/baton_hierarchical_scheduling_design.md の仕様に基づく純粋なスケジューラ実装。
 * UI/DOM 依存を一切持たず、Node.js や単体テスト環境でもそのまま動作・レビュー可能です。
 */

// --- 定数・列挙型 ---

/**
 * @enum {string} BatonState
 * バトンの実行・待機状態マシン (設計書 4.2 節)
 */
const BatonState = {
  READY: 'READY',             // アイドル状態。チェーン未所属
  RUNNING: 'RUNNING',         // 現在 CPU を実行中 (チェーン末尾のリーフ)
  WAIT_RETURN: 'WAIT_RETURN', // 子へバトンを委譲して待機中
  WAIT_START: 'WAIT_START',   // 上位タイムアウトにより分断・一時停止中 (フォワーディング対象)
  BLOCKED: 'BLOCKED'          // IPCやI/O待機中
};

/**
 * @enum {string} SchedReason
 * バトン復帰理由 (設計書 2.4 節)
 */
const SchedReason = {
  TIMEOUT: 'TIMEOUT', // クォンタム枯渇 (下位または上位)
  BLOCKED: 'BLOCKED', // 自発的ブロック (IPC受信待ちなど)
  EXIT: 'EXIT',       // スレッドの正常終了
  FAULT: 'FAULT',     // 例外・クラッシュ
  SUCCESS: 'SUCCESS'
};

/**
 * @class SchedResult
 * システムコール復帰結果 (設計書 5.2 節)
 */
class SchedResult {
  constructor(reason, consumedMs) {
    this.reason = reason;
    this.consumedMs = consumedMs;
  }
}

/**
 * @class Baton
 * バトン（CPU時間トークン・実行権）構造体 (設計書 4.1 節)
 * C++ の class Baton : public KernelObject と 1:1 に対応。
 */
class Baton {
  /**
   * @param {string} id 内部識別子
   * @param {string} name バトン名 (例: "A", "Worker")
   * @param {number} quantumMs 希望クォンタム (ms)
   */
  constructor(id, name, quantumMs) {
    this.id = id;
    this.name = name;

    // --- タイムスライス・仮想時間 ---
    this.quantum = quantumMs;          // uint64_t quantum (希望クォンタム)
    this.remaining = quantumMs;        // uint64_t remaining (メモリ上の実記録残り時間)
    this.last_sync_tsc = 0;            // uint64_t last_sync_tsc (最終同期時刻)

    // --- 委譲チェーン (Call Stack) ポインタ ---
    this.caller_baton = null;          // Baton* caller_baton (親方向)
    this.callee_baton = null;          // Baton* callee_baton (子方向)
    this.tail_baton = null;            // Baton* tail_baton (サブチェーン末尾 ★フォワーディング用)

    // --- デカルト木 (Cartesian Tree) ポインタ (計24バイト) ---
    this.cartesian_parent = null;      // Baton* cartesian_parent
    this.cartesian_left = null;        // Baton* cartesian_left (親側の最小ノード)
    this.cartesian_right = null;       // Baton* cartesian_right (子側の最小ノード ★O(1)次点特定用)

    // --- 中断サブチェーン管理キャッシュ & O(1) 遅延評価タグ ---
    this.sub_min_baton = null;         // Baton* sub_min_baton (配下サブチェーンの最短ノード)
    this.lazy_delta = 0;               // uint64_t lazy_delta (部分木への遅延減算タグ ★Push-Down用)
    this.subchain_consumed_offset = 0; // 分断中に消費された累積時間 (デバッグ・検証用)
    this.pending_lazy_offset = 0;      // 巻き戻り時まで適用保留された遅延オフセット (O(1) Unwinding用)

    // --- 状態 ---
    this.state = BatonState.READY;
  }

  resetCartesian() {
    this.cartesian_parent = null;
    this.cartesian_left = null;
    this.cartesian_right = null;
    this.lazy_delta = 0;
  }
}

/**
 * @class BatonScheduler
 * 階層的バトン委譲・デカルト木スケジューラコア
 */
class BatonScheduler {
  constructor() {
    this.batons = new Map();
    this.currentTime = 0;             // 物理現在時間 (ms)
    this.lastSyncTime = 0;            // メモリ同期時刻 (ms)
    this.runningBaton = null;         // 現在CPU走行中の末尾ノード (Leaf)
    this.activeChain = [];            // アクティブ委譲チェーン [A, B, ...]
    this.suspendedSubchains = [];     // タイムアウト分断保持中のサブチェーン群
    this.cartesianRoot = null;        // デカルト木の根ノード (全体最小値)
    this.nextId = 1;

    // ログおよびイベント通知リスナー (UIとの疎結合用)
    this.logListeners = [];
    this.changeListeners = [];
  }

  // --- リスナー管理 ---

  onLog(callback) {
    this.logListeners.push(callback);
  }

  onChange(callback) {
    this.changeListeners.push(callback);
  }

  notifyLog(tag, message) {
    for (const listener of this.logListeners) {
      listener(this.currentTime, tag, message);
    }
  }

  notifyChange() {
    for (const listener of this.changeListeners) {
      listener(this);
    }
  }

  // --- バトン生成・管理 ---

  createBaton(name, quantumMs) {
    const id = 'baton_' + (this.nextId++);
    const b = new Baton(id, name, quantumMs);
    b.last_sync_tsc = this.currentTime;
    this.batons.set(id, b);
    this.notifyChange();
    return b;
  }

  getBaton(id) {
    return this.batons.get(id);
  }

  getAllBatons() {
    return Array.from(this.batons.values());
  }

  // --- 時間計算・遅延評価 (Lazy Evaluation) ---

  /**
   * 概念上の残り時間 (実効残り時間) の O(1) 算出
   * メモリ構造体を毎ミリ秒書き換えることなく、未同期経過時間や遅延タグ、保留オフセットを差し引いた実効値を計算する。
   */
  getEffectiveRemaining(b) {
    if (!b) return 0;
    const lazy = b.lazy_delta || 0;
    const pending = b.pending_lazy_offset || 0;

    // アクティブチェーン内の場合: 未同期の経過時間を差し引く
    if (this.activeChain.includes(b)) {
      const unsyncedElapsed = this.currentTime - this.lastSyncTime;
      return Math.max(0, b.remaining - lazy - pending - unsyncedElapsed);
    }

    return Math.max(0, b.remaining - lazy - pending);
  }

  /**
   * イベント発生契機でのアクティブチェーンメモリ同期
   */
  syncAllActive() {
    const elapsed = this.currentTime - this.lastSyncTime;
    if (elapsed <= 0) return;

    for (const b of this.activeChain) {
      b.remaining = Math.max(0, b.remaining - elapsed);
      b.last_sync_tsc = this.currentTime;
    }
    this.notifyLog('SYNC', `🔄 メモリ同期発生: アクティブチェーンの remaining 構造体を一括更新 (未同期差分 Δt = ${elapsed.toFixed(1)}ms)`);
    this.lastSyncTime = this.currentTime;
  }

  // --- デカルト木 (Cartesian Tree) インクリメンタル・アルゴリズム (実際のOSカーネル処理) ---

  /**
   * 遅延タグの伝播 (Push-Down: 完全最悪 O(1))
   * 親ノードを訪問・操作する際、そのノードに溜まった lazy_delta を実記録時間に反映し、左右の子ノードに O(1) 伝播する。
   * @param {Baton} node 対象ノード
   */
  cartesianPushDown(node) {
    if (!node || !node.lazy_delta) return;
    const delta = node.lazy_delta;
    node.remaining = Math.max(0, node.remaining - delta);
    if (node.cartesian_left) {
      node.cartesian_left.lazy_delta = (node.cartesian_left.lazy_delta || 0) + delta;
    }
    if (node.cartesian_right) {
      node.cartesian_right.lazy_delta = (node.cartesian_right.lazy_delta || 0) + delta;
    }
    node.lazy_delta = 0;
  }

  /**
   * 単調スタックによる末尾へのインクリメンタル Push (償却 O(1))
   * OS カーネルが sys_baton_switch で新規バトンをチェーン末尾に追加する処理。
   * 全体の再構築ではなく、直前の末尾ノードから親ポインタを遡って局所的にポインタを付け替える。
   * @param {Baton} target 追加するバトン
   */
  cartesianPush(target) {
    target.resetCartesian();

    if (!this.runningBaton) {
      // チェーン最初のノード
      this.cartesianRoot = target;
      return;
    }

    let curr = this.runningBaton;
    let lastPopped = null;

    // 直前の末尾から親ポインタを遡り、target より実効残り時間が大きいノードをポップ（飛び越える）
    while (curr && this.getEffectiveRemaining(curr) > this.getEffectiveRemaining(target)) {
      this.cartesianPushDown(curr);
      lastPopped = curr;
      curr = curr.cartesian_parent;
    }

    if (curr) {
      this.cartesianPushDown(curr);
      // 自分より小さい（または等しい）親が見つかった場合
      target.cartesian_parent = curr;
      target.cartesian_left = lastPopped;
      if (lastPopped) {
        lastPopped.cartesian_parent = target;
      }
      curr.cartesian_right = target;
    } else {
      // 根まで遡って、target が新しい全体最小（根）になった場合
      target.cartesian_parent = null;
      target.cartesian_left = this.cartesianRoot;
      if (this.cartesianRoot) {
        this.cartesianRoot.cartesian_parent = target;
      }
      this.cartesianRoot = target;
    }
  }

  /**
   * 末尾ノードのインクリメンタル Pop (完全最悪 O(1))
   * 正常終了 (EXIT)、自発的ブロック (BLOCKED)、下位タイムアウト時に末尾バトンを木から外す処理。
   * 末尾ノードは右の子を絶対に持たないため、左の子を親の右の子に付け替えるだけで完了する。
   * @param {Baton} leaf 離脱する末尾バトン
   */
  cartesianPopLeaf(leaf) {
    this.cartesianPushDown(leaf);
    const child = leaf.cartesian_left;
    const p = leaf.cartesian_parent;

    if (p) {
      p.cartesian_right = child;
      if (child) child.cartesian_parent = p;
    } else {
      this.cartesianRoot = child;
      if (child) child.cartesian_parent = null;
    }

    leaf.resetCartesian();
  }

  /**
   * 上位タイムアウトによる木のインクリメンタル分断 (完全最悪 O(1))
   * タイムアウトした根ノード expired で木を親側と子側に O(1) 分断する。
   * サブツリーの根ノードに経過時間を遅延タグ (lazy_delta) として O(1) 付与する。
   * @param {Baton} expired タイムアウトした根ノード
   * @param {number} elapsed 今回の消費時間
   * @returns {Baton|null} 分断されたサブツリーの根 (expired.cartesian_right)
   */
  cartesianSplitAt(expired, elapsed = 0) {
    this.cartesianPushDown(expired);
    const callerRoot = expired.cartesian_left;
    const subRoot = expired.cartesian_right;

    if (callerRoot) callerRoot.cartesian_parent = null;
    if (subRoot) {
      subRoot.cartesian_parent = null;
      // ★ サブツリーの根 1 箇所にのみ遅延タグを記録 (完全最悪 O(1))
      subRoot.lazy_delta = (subRoot.lazy_delta || 0) + elapsed;
    }

    expired.resetCartesian();

    this.cartesianRoot = callerRoot;
    return subRoot;
  }

  /**
   * 厳密なデカルト木マージ (ZipMerge: 通常 O(1) / スパイン上限 O(M))
   * leftTree の右スパインと rightTree の左スパインを縫い合わせて Min-Heap 性と BST 性を完全保証する。
   * @param {Baton} leftTree 親側のデカルト木 (委譲チェーン上で先)
   * @param {Baton} rightTree サブチェーンのデカルト木 (委譲チェーン上で後)
   * @returns {Baton|null} マージ後の根ノード
   */
  cartesianZipMerge(leftTree, rightTree) {
    if (!leftTree) return rightTree;
    if (!rightTree) return leftTree;

    this.cartesianPushDown(leftTree);
    this.cartesianPushDown(rightTree);

    // 実効残り時間の小さい方を親ノード（上位）とする (安定同点時は左側親を優先)
    if (this.getEffectiveRemaining(leftTree) <= this.getEffectiveRemaining(rightTree)) {
      const mergedRight = this.cartesianZipMerge(leftTree.cartesian_right, rightTree);
      leftTree.cartesian_right = mergedRight;
      if (mergedRight) mergedRight.cartesian_parent = leftTree;
      return leftTree;
    } else {
      const mergedLeft = this.cartesianZipMerge(leftTree, rightTree.cartesian_left);
      rightTree.cartesian_left = mergedLeft;
      if (mergedLeft) mergedLeft.cartesian_parent = rightTree;
      return rightTree;
    }
  }

  /**
   * ダイレクト・フォワーディング再開時のサブツリーのマージ
   * 互換性ラッパー: 厳密な cartesianZipMerge を呼び出して木全体の根を更新する。
   * @param {Baton} target 再開されたバトン
   * @param {Baton} subRoot サブツリーの根ノード
   */
  cartesianMergeSubchain(target, subRoot) {
    if (!subRoot) return;
    this.cartesianRoot = this.cartesianZipMerge(this.cartesianRoot, subRoot);
  }

  /**
   * (旧方式 / デバッグ用) 配列全体からゼロから単調スタックで構築する関数
   */
  buildCartesianTreeForChain(chain) {
    if (!chain || chain.length === 0) return null;
    for (const b of chain) {
      b.resetCartesian();
    }

    const stack = [];
    for (let i = 0; i < chain.length; i++) {
      const curr = chain[i];
      let lastPopped = null;

      while (stack.length > 0 && stack[stack.length - 1].remaining > curr.remaining) {
        lastPopped = stack.pop();
      }

      if (stack.length > 0) {
        const parent = stack[stack.length - 1];
        parent.cartesian_right = curr;
        curr.cartesian_parent = parent;
      }

      if (lastPopped !== null) {
        curr.cartesian_left = lastPopped;
        lastPopped.cartesian_parent = curr;
      }

      stack.push(curr);
    }

    return stack[0];
  }

  // --- システムコール API ---

  /**
   * sys_baton_set_quantum: 自律的クォンタムの設定 (設計書 5.2 節)
   */
  sysBatonSetQuantum(target, quantumMs) {
    if (!target || quantumMs <= 0) return -1;
    target.quantum = quantumMs;
    if (target.state === BatonState.READY || target.state === BatonState.WAIT_START) {
      target.remaining = quantumMs;
    }
    this.notifyLog('INFO', `sys_baton_set_quantum: ${target.name} のクォンタムを ${quantumMs}ms に設定`);
    this.notifyChange();
    return 0;
  }

  /**
   * sys_baton_switch: バトンスイッチ & ダイレクト・フォワーディング (設計書 5.2 節)
   * @param {Baton} target 委譲先バトン
   * @returns {boolean} 成功成否
   */
  sysBatonSwitch(target) {
    this.syncAllActive();

    // 1. ループ検出 (O(1)): ターゲットが WAIT_RETURN ならデッドロック防止
    if (target.state === BatonState.WAIT_RETURN) {
      this.notifyLog('SWITCH', `❌ エラー: ${target.name} は既に親として待機中 (WAIT_RETURN) です (SYS_ERR_DEADLK)`);
      return false;
    }

    // 2. ダイレクト・フォワーディング (完全 O(1)): target が WAIT_START の場合
    if (target.state === BatonState.WAIT_START) {
      const trueTarget = target.tail_baton || target;
      this.notifyLog('FORWARD', `⚡ Direct Forwarding 発動! ${target.name} (WAIT_START) を検知。中間ノードを全スキップし、末尾 ${trueTarget.name} へ O(1) 直接ジャンプします`);

      // 分断リストから取り出し
      const subchainIdx = this.suspendedSubchains.findIndex(sc => sc.root === target);
      let reattachedNodes = [];
      let subchainCartesianRoot = null;
      if (subchainIdx !== -1) {
        const scObj = this.suspendedSubchains[subchainIdx];
        reattachedNodes = scObj.nodes;
        subchainCartesianRoot = scObj.cartesianRoot || target.sub_min_baton;
        this.suspendedSubchains.splice(subchainIdx, 1);
      } else {
        subchainCartesianRoot = target.sub_min_baton;
        let curr = target;
        while (curr) {
          reattachedNodes.push(curr);
          if (curr === trueTarget) break;
          curr = curr.callee_baton;
        }
      }

      if (this.runningBaton) {
        this.runningBaton.state = BatonState.WAIT_RETURN;
        this.runningBaton.callee_baton = target;
        target.caller_baton = this.runningBaton;
      }

      // クォンタム補充
      if (target.remaining <= 0) {
        target.remaining = target.quantum;
        target.lazy_delta = 0;
        target.last_sync_tsc = this.currentTime;
      }

      // ★ 完全 O(1) 状態遷移:
      // 中間ノード (C など) はメモリ走査ループ (O(N)) を回さず完全スキップ！
      target.state = BatonState.WAIT_RETURN;
      trueTarget.state = BatonState.RUNNING;

      // インクリメンタル Push & 厳密 Zip マージ (通常 O(1) / スパイン上限 O(M))
      this.cartesianPush(target);
      this.cartesianRoot = this.cartesianZipMerge(this.cartesianRoot, subchainCartesianRoot);

      // 真の実行末尾 trueTarget の祖先ノードの遅延タグを pushDown して末尾を最新同期 (O(M))
      if (trueTarget !== target) {
        let node = trueTarget;
        const ancestors = [];
        while (node) {
          ancestors.push(node);
          node = node.cartesian_parent;
        }
        for (let i = ancestors.length - 1; i >= 0; i--) {
          this.cartesianPushDown(ancestors[i]);
        }
        trueTarget.last_sync_tsc = this.currentTime;
      }

      target.subchain_consumed_offset = 0;

      // O(1) 最短値特定: 木の根 (cartesianRoot) の実効残り時間
      const nextBudget = this.cartesianRoot ? this.getEffectiveRemaining(this.cartesianRoot) : target.quantum;
      this.notifyLog('TIMER', `次のクォンタム設定: デカルト木の根の実効残り時間 = ${nextBudget.toFixed(1)}ms`);

      this.activeChain = this.activeChain.concat(reattachedNodes);
      this.runningBaton = trueTarget;

      this.notifyLog('TREE', `デカルト木厳密マージ (ZipMerge): 全体最小ノード(根)は ${this.cartesianRoot ? this.cartesianRoot.name + ' [' + this.cartesianRoot.remaining.toFixed(1) + 'ms (' + this.getEffectiveRemaining(this.cartesianRoot).toFixed(1) + 'ms)]' : 'なし'}`);
      this.notifyChange();
      return true;
    }

    // 3. 通常の sys_baton_switch
    if (!this.runningBaton) {
      target.state = BatonState.RUNNING;
      target.last_sync_tsc = this.currentTime;
      this.cartesianPush(target);
      this.runningBaton = target;
      this.activeChain = [target];
      this.notifyLog('SWITCH', `起動: 初期バトン ${target.name} [実記録: ${target.remaining.toFixed(1)}ms (${this.getEffectiveRemaining(target).toFixed(1)}ms)] が RUNNING に遷移`);
    } else {
      const caller = this.runningBaton;
      caller.state = BatonState.WAIT_RETURN;
      caller.callee_baton = target;
      target.caller_baton = caller;
      target.state = BatonState.RUNNING;
      target.last_sync_tsc = this.currentTime;

      // インクリメンタル Push (償却 O(1))
      this.cartesianPush(target);

      this.runningBaton = target;
      this.activeChain.push(target);
      this.notifyLog('SWITCH', `委譲: ${caller.name} -> ${target.name} [実記録: ${target.remaining.toFixed(1)}ms (${this.getEffectiveRemaining(target).toFixed(1)}ms)]`);
    }

    this.notifyLog('TREE', `デカルト木インクリメンタル更新: 全体最小ノード(根)は ${this.cartesianRoot ? this.cartesianRoot.name + ' [' + this.cartesianRoot.remaining.toFixed(1) + 'ms (' + this.getEffectiveRemaining(this.cartesianRoot).toFixed(1) + 'ms)]' : 'なし'}`);

    this.notifyChange();
    return true;
  }

  // --- 物理時間進行 & ハードウェアタイマー連動 ---

  /**
   * 物理時間を dt (ms) 進める (Tickless 動作)
   */
  stepTime(dt) {
    if (this.activeChain.length === 0 || !this.runningBaton) {
      this.notifyLog('INFO', '実行中のバトンが存在しません。');
      return;
    }

    // ハードウェアタイマーの期限 = デカルト木の根ノードの実効残り時間
    const rootEffective = this.cartesianRoot ? this.getEffectiveRemaining(this.cartesianRoot) : Infinity;
    const actualDt = Math.min(dt, rootEffective);

    if (actualDt <= 0) {
      this.handleTimeout();
      return;
    }

    this.currentTime += actualDt;

    const rootEffNow = this.getEffectiveRemaining(this.cartesianRoot);
    this.notifyLog('TIMER', `物理時間 +${actualDt.toFixed(1)}ms (T=${this.currentTime.toFixed(1)}ms)。メモリは書き換えずハードウェアタイマーのみ進行 [根 ${this.cartesianRoot.name}: ${this.cartesianRoot.remaining.toFixed(1)}ms (${rootEffNow.toFixed(1)}ms)]`);

    // タイマー発火判定
    if (this.cartesianRoot && rootEffNow <= 0.0001) {
      this.handleTimeout();
    } else {
      this.notifyChange();
    }
  }

  stepToNextEvent() {
    if (!this.cartesianRoot || this.activeChain.length === 0) return;
    const dt = this.getEffectiveRemaining(this.cartesianRoot);
    if (dt > 0) {
      this.stepTime(dt);
    } else {
      this.handleTimeout();
    }
  }

  // --- プリエンプション & 復帰イベント ---

  /**
   * タイムアウト割り込みハンドラ (設計書 3.3 (2) 節)
   */
  handleTimeout() {
    const expired = this.cartesianRoot;
    if (!expired) return;

    this.notifyLog('TIMEOUT', `🚨 LAPIC タイマー割り込み発火! 最短バトン ${expired.name} の時間が枯渇 (概念値: 0.0ms)`);

    // パターン1: 実行中の末尾ノード自身が枯渇した場合 (下位タイムアウト)
    if (expired === this.runningBaton) {
      this.syncAllActive();
      this.notifyLog('TIMEOUT', `下位タイムアウト: 走行中の ${expired.name} 自身がクォンタムを消費し切りました。直前の親へバトン返却 (SchedReason::TIMEOUT)`);

      // ★ インクリメンタル Pop (完全最悪 O(1)): 木から末尾ノードを除去
      this.cartesianPopLeaf(expired);

      const parent = expired.caller_baton;
      expired.state = BatonState.READY;
      expired.remaining = expired.quantum;
      expired.caller_baton = null;

      if (parent) {
        parent.callee_baton = null;
        parent.state = BatonState.RUNNING;
        // 巻き戻り遅延同期 (PushDown O(1))
        this.cartesianPushDown(parent);
        this.runningBaton = parent;
        this.activeChain.pop();
      } else {
        this.runningBaton = null;
        this.activeChain = [];
      }
    } else {
      // パターン2: 上位バトンが枯渇した場合 (完全 O(1) チェーン分断・保持)
      const elapsed = this.currentTime - this.lastSyncTime;
      const caller = expired.caller_baton;

      // ★ O(1) 処理: 親 caller (A) と 枯渇した expired (B) のみピンポイント同期
      if (caller) {
        this.cartesianPushDown(caller);
        caller.remaining = Math.max(0, caller.remaining - elapsed);
        caller.last_sync_tsc = this.currentTime;
      }
      expired.remaining = 0;
      expired.lazy_delta = 0;
      expired.last_sync_tsc = this.currentTime;

      // ★ O(1) 累積オフセット記録: 配下ノードを走査せず B に一撃記録 (デバッグ用)
      expired.subchain_consumed_offset = (expired.subchain_consumed_offset || 0) + elapsed;
      this.lastSyncTime = this.currentTime;

      const subMin = expired.cartesian_right;
      const tail = this.runningBaton;

      expired.sub_min_baton = subMin;
      expired.tail_baton = tail;
      expired.state = BatonState.WAIT_START;

      // ★ インクリメンタル分断 (完全最悪 O(1)): 木を親側とサブツリーに O(1) 分断 & サブツリー根へ lazy_delta 付与
      const subchainCartesianRoot = this.cartesianSplitAt(expired, elapsed);

      const idx = this.activeChain.indexOf(expired);
      let severedNodes = [];
      if (idx !== -1) {
        severedNodes = this.activeChain.slice(idx);
        this.activeChain = this.activeChain.slice(0, idx);
      }

      this.suspendedSubchains.push({
        root: expired,
        nodes: severedNodes,
        tail: tail,
        subMin: subMin,
        caller: caller,
        cartesianRoot: subchainCartesianRoot
      });

      this.notifyLog('TIMEOUT', `⚡ 上位タイムアウト検出 (${expired.name})!`);
      this.notifyLog('TREE', `O(1) 配下最短特定: ${expired.name}->cartesian_right = ${subMin ? subMin.name + ' [' + subMin.remaining.toFixed(1) + 'ms (' + this.getEffectiveRemaining(subMin).toFixed(1) + 'ms)]' : 'なし'} ★探索コストゼロ!`);
      this.notifyLog('FORWARD', `✂️ 完全 O(1) 分断保持: サブチェーン [${severedNodes.map(n => n.name).join(' ➔ ')}] の O(N) メモリ走査を回避。${expired.name}->subchain_consumed_offset に ${elapsed.toFixed(1)}ms を記録し、配下メモリは未変更のまま O(1) で即時分断しました。`);

      if (caller) {
        caller.callee_baton = null;
        caller.state = BatonState.RUNNING;
        this.runningBaton = caller;
        this.notifyLog('SWITCH', `制御が直前の親 ${caller.name} に SchedReason::TIMEOUT で巻き戻りました (コンテキストスイッチ O(1))`);
      } else {
        this.runningBaton = null;
        this.notifyLog('INFO', 'ルートスケジューラの上位が存在しないため、CPUアイドル状態に遷移');
      }
    }

    this.notifyChange();
  }

  /**
   * 自発的ブロック (IPC受信待ち等)
   */
  voluntaryBlock() {
    if (!this.runningBaton) return;
    this.syncAllActive();

    const runner = this.runningBaton;
    const parent = runner.caller_baton;

    // ★ インクリメンタル Pop (完全最悪 O(1))
    this.cartesianPopLeaf(runner);

    runner.state = BatonState.BLOCKED;
    this.notifyLog('INFO', `自発的ブロック: ${runner.name} が IPC 受信待機などでブロック (BLOCKED)`);

    if (parent) {
      parent.callee_baton = null;
      parent.state = BatonState.RUNNING;
      // 巻き戻り遅延同期 (PushDown O(1))
      this.cartesianPushDown(parent);
      this.runningBaton = parent;
      this.activeChain.pop();
      this.notifyLog('SWITCH', `直前の親 ${parent.name} へバトン返却 (SchedReason::BLOCKED)`);
    } else {
      this.runningBaton = null;
      this.activeChain = [];
    }

    this.notifyChange();
  }

  /**
   * スレッドの正常終了 (EXIT)
   */
  threadExit() {
    if (!this.runningBaton) return;
    this.syncAllActive();

    const runner = this.runningBaton;
    const parent = runner.caller_baton;

    // ★ インクリメンタル Pop (完全最悪 O(1))
    this.cartesianPopLeaf(runner);

    runner.state = BatonState.READY;
    runner.remaining = runner.quantum;
    this.notifyLog('INFO', `自発的終了: ${runner.name} が処理を完了し Exit (バトン返却)`);

    if (parent) {
      parent.callee_baton = null;
      parent.state = BatonState.RUNNING;
      // 巻き戻り遅延同期 (PushDown O(1))
      this.cartesianPushDown(parent);
      this.runningBaton = parent;
      this.activeChain.pop();
      this.notifyLog('SWITCH', `直前の親 ${parent.name} へバトン返却 (SchedReason::EXIT)`);
    } else {
      this.runningBaton = null;
      this.activeChain = [];
    }

    this.notifyChange();
  }

  /**
   * 例外・クラッシュ発生 (FAULT)
   */
  raiseFault() {
    if (!this.runningBaton) return;
    this.syncAllActive();

    const runner = this.runningBaton;
    const parent = runner.caller_baton;

    // ★ インクリメンタル Pop (完全最悪 O(1))
    this.cartesianPopLeaf(runner);

    runner.state = BatonState.BLOCKED;
    this.notifyLog('FAULT', `💥 例外発生: ${runner.name} でページフォールト等の未処理例外が発生!`);

    if (parent) {
      parent.callee_baton = null;
      parent.state = BatonState.RUNNING;
      // 巻き戻り遅延同期 (PushDown O(1))
      this.cartesianPushDown(parent);
      this.runningBaton = parent;
      this.activeChain.pop();
      this.notifyLog('FAULT', `直前の親 ${parent.name} へ安全に復帰 (SchedReason::FAULT)。OS全体のクラッシュを防止`);
    } else {
      this.runningBaton = null;
      this.activeChain = [];
    }

    this.notifyChange();
  }

  // --- プリセットシナリオ ---

  loadPreset1() {
    this.reset();
    const A = this.createBaton('A(Root)', 100);
    const B = this.createBaton('B(SubSched)', 10);
    const C = this.createBaton('C(Server)', 40);
    const D = this.createBaton('D(Worker)', 50);

    this.sysBatonSwitch(A);
    this.sysBatonSwitch(B);
    this.sysBatonSwitch(C);
    this.sysBatonSwitch(D);

    this.notifyLog('INFO', '=== プリセット1 読込: A -> B(10ms) -> C(40ms) -> D(50ms) ===');
    this.notifyLog('INFO', '10ms経過でBがタイムアウトし、O(1)でちぎれたサブチェーン[B->C->D]が分断保持されます。再開時は中間Cを完全スキップしてDへジャンプします。');
  }

  loadPreset2() {
    this.reset();
    const A = this.createBaton('A', 100);
    const B = this.createBaton('B', 10);
    const C = this.createBaton('C', 40);
    const D = this.createBaton('D', 25);
    const E = this.createBaton('E', 15);
    const F = this.createBaton('F', 30);

    this.sysBatonSwitch(A);
    this.sysBatonSwitch(B);
    this.sysBatonSwitch(C);
    this.sysBatonSwitch(D);
    this.sysBatonSwitch(E);
    this.sysBatonSwitch(F);

    this.notifyLog('INFO', '=== プリセット2 読込: 設計書 3.2 節の 6ノード木構造 ===');
    this.notifyLog('INFO', 'デカルト木の根は B(10ms)、Bの右の子は E(15ms ★Bより下流の最小値) となり、設計書の図と完全に一致します。');
  }

  loadPreset3() {
    this.reset();
    const A = this.createBaton('A(Root)', 100);
    const B = this.createBaton('B(SubSched)', 50);
    const C = this.createBaton('C(Parent)', 40);
    const D = this.createBaton('D(FaultyWorker)', 20);

    this.sysBatonSwitch(A);
    this.sysBatonSwitch(B);
    this.sysBatonSwitch(C);
    this.sysBatonSwitch(D);

    this.notifyLog('INFO', '=== プリセット3 読込: 例外 (FAULT) シナリオ ===');
    this.notifyLog('INFO', '「FAULT」ボタンを押すと、D がクラッシュしても親 C へ安全にバトンが返却されます。');
  }

  reset() {
    this.batons.clear();
    this.currentTime = 0;
    this.lastSyncTime = 0;
    this.runningBaton = null;
    this.activeChain = [];
    this.suspendedSubchains = [];
    this.cartesianRoot = null;
    this.nextId = 1;

    this.notifyLog('INFO', 'スケジューラをリセットしました。');
    this.notifyChange();
  }
}

// Node.js 環境でのモジュールエクスポート対応 (テスト・CI用)
if (typeof module !== 'undefined' && module.exports) {
  module.exports = {
    BatonState,
    SchedReason,
    SchedResult,
    Baton,
    BatonScheduler
  };
}
