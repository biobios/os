# Baton 階層的スケジューラ ビジュアライザ

`docs/baton_hierarchical_scheduling_design.md` で設計された **階層的バトン委譲・デカルト木（Cartesian Tree）スケジューリング機構** のインタラクティブ・ビジュアライザおよびリファレンスシミュレータです。

---

## 📁 モジュール構成（リファクタリング完了）

シミュレータのコードレビューや設計書への逆輸入・参照を容易にするため、**「純粋なスケジューラロジック」** と **「プレゼンテーション / UI レンダラ」** を完全に分離（Decouple）しました：

```
tools/baton_visualizer/
├── scheduler.js   # 【コア】純粋なスケジューラシミュレータ (DOM/UI依存ゼロ、Node.js対応)
├── visualizer.js  # 【UI】SVG / DOM レンダリング & イベントハンドラ
├── index.html     # HTMLレイアウト & スタイル (上記2つのJSを読み込み)
└── README.md      # 本ドキュメント
```

### 1. `scheduler.js` (カーネルコアモデル)
- **外部依存ゼロ**: DOM API やブラウザ固有オブジェクトを一切使用していません。Node.js やヘッドレス環境の単体テストでもそのまま `require('./scheduler.js')` で動作します。
- **C++ との 1:1 対応**:
  - `BatonState` (`READY`, `RUNNING`, `WAIT_RETURN`, `WAIT_START`, `BLOCKED`)
  - `Baton` クラス（ポインタ群、クォンタム、デカルト木ポインタ、遅延タグ）
  - `BatonScheduler`（`cartesianPushDown`, `cartesianPush`, `cartesianPopLeaf`, `cartesianSplitAt`, `cartesianZipMerge`, `sysBatonSwitch`, `stepTime`, `handleTimeout` など）
- **真のインクリメンタル $O(1)$ / $O(M)$ アルゴリズム (OS カーネル完全準拠)**:
  - 毎回の全体木再構築（$O(N)$ ループ）を完全撤廃。
  - スイッチ時の単調スタックによる末尾 Push（償却 $O(1)$）、末尾離脱時の局所 Pop（最悪 $O(1)$）、上位タイムアウト時の分断（最悪 $O(1)$）。
  - 再開時サブツリーの厳密マージ（`cartesianZipMerge`: 通常 $O(1)$ / スパイン上限 $O(M)$）により Min-Heap 性・BST 性を完全保証。
  - 遅延タグ伝播（Lazy Push-Down）により、上位タイムアウト時の配下メモリ走査（$O(N)$）を排除し、アクセス時に $O(1)$ で遅延伝播。
- **イベントリスナー**: `onLog(cb)` と `onChange(cb)` による疎結合な通知。

### 2. `visualizer.js` (UI / レンダラ)
- `BatonVisualizer` クラスが `BatonScheduler` のイベントを購読し、SVG デカルト木、委譲チェーン、各種バッジ、インスペクターを描画します。

---

## 🚀 使い方

ブラウザで直接ファイルを開くだけで動作します：
```bash
# ブラウザで直接開く場合 (Linux)
xdg-open tools/baton_visualizer/index.html

# ローカル HTTP サーバー経由で開く場合
python3 -m http.server 8080 --directory tools/baton_visualizer/
# => http://localhost:8080 にアクセス
```

### 単体テスト (Node.js) の実行
`scheduler.js` 単体で以下のテストスクリプトを実行可能です：
```bash
node -e "
const { BatonScheduler } = require('./tools/baton_visualizer/scheduler.js');
const sim = new BatonScheduler();
sim.loadPreset1();
sim.stepTime(10.0);
console.log('Timeout ok, suspended:', sim.suspendedSubchains.length);
"
```

---

## 🎯 主な機能とアルゴリズム
1. **デカルト木 (Cartesian Tree) の $O(1)$ 最小値追跡**:
   単調スタックによる構築、根ノードの LAPIC タイマー直結。
2. **タイムアウト時の $O(1)$ サブチェーン分断**:
   `B->sub_min_baton = B->cartesian_right` による探索コストゼロの次点特定。
3. **ダイレクト・フォワーディング (Direct Forwarding)**:
   中間ノードをメモリ走査すらスキップして末尾スレッドへ直接ジャンプ。
4. **巻き戻り遅延同期 (Lazy Unwinding on Return)**:
   スキップされた中間ノードのメモリ同期は、将来そのノードに制御が復帰した瞬間に各ステップ $O(1)$ で実行。
