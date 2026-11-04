/**
 * @file visualizer.js
 * @brief Baton スケジューラ・UIレンダラ & コントローラ
 * 
 * scheduler.js (BatonScheduler) の状態を購読し、
 * DOM / SVG 描画およびユーザー入力を制御する純粋なプレゼンテーション層。
 */

class BatonVisualizer {
  /**
   * @param {BatonScheduler} scheduler
   */
  constructor(scheduler) {
    this.sim = scheduler;
    this.selectedBatonId = null;
    this.autoPlayInterval = null;
    this.lastForwardingInfo = null;

    // スケジューラからのイベント購読
    this.sim.onLog((time, tag, msg) => this.appendLog(time, tag, msg));
    this.sim.onChange(() => this.updateUI());
  }

  init() {
    this.bindEvents();
    this.sim.loadPreset1();
  }

  // --- イベントバインド ---

  bindEvents() {
    // 時間制御ボタン
    document.getElementById('btn-step-1ms').addEventListener('click', () => {
      this.sim.stepTime(1.0);
    });

    document.getElementById('btn-step-5ms').addEventListener('click', () => {
      this.sim.stepTime(5.0);
    });

    document.getElementById('btn-step-event').addEventListener('click', () => {
      this.sim.stepToNextEvent();
    });

    // 自動実行
    const autoPlayBtn = document.getElementById('btn-auto-play');
    autoPlayBtn.addEventListener('click', () => {
      if (this.autoPlayInterval) {
        clearInterval(this.autoPlayInterval);
        this.autoPlayInterval = null;
        autoPlayBtn.textContent = '▶️ 自動実行';
      } else {
        const speed = parseInt(document.getElementById('select-speed').value, 10);
        autoPlayBtn.textContent = '⏸️ 一時停止';
        this.autoPlayInterval = setInterval(() => {
          if (this.sim.activeChain.length === 0) {
            clearInterval(this.autoPlayInterval);
            this.autoPlayInterval = null;
            autoPlayBtn.textContent = '▶️ 自動実行';
            return;
          }
          this.sim.stepTime(1.0);
        }, speed);
      }
    });

    document.getElementById('select-speed').addEventListener('change', () => {
      if (this.autoPlayInterval) {
        clearInterval(this.autoPlayInterval);
        const speed = parseInt(document.getElementById('select-speed').value, 10);
        this.autoPlayInterval = setInterval(() => {
          this.sim.stepTime(1.0);
        }, speed);
      }
    });

    // sys_baton_switch 実行
    document.getElementById('btn-do-switch').addEventListener('click', () => {
      const targetVal = document.getElementById('select-switch-target').value;
      const quantumVal = parseFloat(document.getElementById('input-quantum').value) || 10;
      let targetBaton = null;

      if (targetVal === '__NEW__' || !targetVal) {
        const customName = document.getElementById('input-new-name').value.trim() || `Task_${this.sim.nextId}`;
        targetBaton = this.sim.createBaton(customName, quantumVal);
        document.getElementById('input-new-name').value = '';
      } else {
        targetBaton = this.sim.getBaton(targetVal);
        if (quantumVal > 0) {
          targetBaton.quantum = quantumVal;
          if (targetBaton.state === BatonState.READY || targetBaton.state === BatonState.WAIT_START) {
            targetBaton.remaining = quantumVal;
          }
        }
      }

      if (targetBaton) {
        this.sim.sysBatonSwitch(targetBaton);
      }
    });

    // クォンタム設定のみ
    document.getElementById('btn-set-quantum').addEventListener('click', () => {
      const targetVal = document.getElementById('select-switch-target').value;
      const quantumVal = parseFloat(document.getElementById('input-quantum').value) || 10;
      if (targetVal && targetVal !== '__NEW__') {
        const b = this.sim.getBaton(targetVal);
        if (b) {
          this.sim.sysBatonSetQuantum(b, quantumVal);
        }
      }
    });

    // スレッドアクション
    document.getElementById('btn-act-block').addEventListener('click', () => {
      this.sim.voluntaryBlock();
    });

    document.getElementById('btn-act-exit').addEventListener('click', () => {
      this.sim.threadExit();
    });

    document.getElementById('btn-act-fault').addEventListener('click', () => {
      this.sim.raiseFault();
    });

    // プリセット
    document.getElementById('btn-preset-1').addEventListener('click', () => this.sim.loadPreset1());
    document.getElementById('btn-preset-2').addEventListener('click', () => this.sim.loadPreset2());
    document.getElementById('btn-preset-3').addEventListener('click', () => this.sim.loadPreset3());
    document.getElementById('btn-reset').addEventListener('click', () => {
      if (this.autoPlayInterval) {
        clearInterval(this.autoPlayInterval);
        this.autoPlayInterval = null;
        document.getElementById('btn-auto-play').textContent = '▶️ 自動実行';
      }
      this.selectedBatonId = null;
      document.getElementById('log-terminal').innerHTML = '';
      this.sim.reset();
    });

    // ログ消去
    document.getElementById('btn-clear-log').addEventListener('click', () => {
      document.getElementById('log-terminal').innerHTML = '';
    });

    // リサイズ時再描画
    window.addEventListener('resize', () => {
      this.renderCartesianTree();
    });
  }

  // --- ログ出力 ---

  appendLog(timeMs, tag, message) {
    const terminal = document.getElementById('log-terminal');
    if (!terminal) return;
    const line = document.createElement('div');
    line.className = 'log-line';
    line.innerHTML = `
      <span class="log-time">[${timeMs.toFixed(1)}ms]</span>
      <span class="log-tag ${tag}">[${tag}]</span>
      <span>${message}</span>
    `;
    terminal.appendChild(line);
    terminal.scrollTop = terminal.scrollHeight;
  }

  // --- UI 全体更新 ---

  updateUI() {
    // 物理時間
    document.getElementById('system-time').textContent = `T: ${this.sim.currentTime.toFixed(1)} ms`;

    // LAPIC ハードウェアタイマー表示
    const lapicDisplay = document.getElementById('lapic-timer-display');
    const lapicTarget = document.getElementById('lapic-target-node');
    const lapicFill = document.getElementById('lapic-progress-fill');

    if (this.sim.cartesianRoot) {
      const rootEff = this.sim.getEffectiveRemaining(this.sim.cartesianRoot);
      lapicTarget.textContent = `Root: ${this.sim.cartesianRoot.name}`;
      lapicDisplay.innerHTML = `${this.sim.cartesianRoot.remaining.toFixed(1)}ms <span style="color: var(--accent-cyan);">(${rootEff.toFixed(1)}ms)</span>`;
      const pct = Math.max(0, Math.min(100, (rootEff / this.sim.cartesianRoot.quantum) * 100));
      lapicFill.style.width = pct + '%';
    } else {
      lapicTarget.textContent = 'Root: なし';
      lapicDisplay.textContent = '停止 (Idle)';
      lapicFill.style.width = '0%';
    }

    // 実行中ノード表示
    document.getElementById('current-running-label').innerHTML =
      this.sim.runningBaton ? `${this.sim.runningBaton.name} [${this.sim.runningBaton.remaining.toFixed(1)}ms <span style="color: var(--accent-cyan);">(${this.sim.getEffectiveRemaining(this.sim.runningBaton).toFixed(1)}ms)</span>]` : 'なし (Idle)';

    // 委譲先ドロップダウンの更新
    const targetSelect = document.getElementById('select-switch-target');
    const currentSelectedVal = targetSelect.value;
    targetSelect.innerHTML = '<option value="__NEW__">+ 新規バトンを作成してスイッチ</option>';

    for (const b of this.sim.getAllBatons()) {
      const opt = document.createElement('option');
      opt.value = b.id;
      const eff = this.sim.getEffectiveRemaining(b);
      let label = `${b.name} [${b.state}] - ${b.remaining.toFixed(1)}ms (${eff.toFixed(1)}ms) / ${b.quantum}ms`;
      if (b.state === BatonState.WAIT_START) {
        label += ' ★WAIT_START (ちぎれたサブチェーン再開)';
      }
      opt.textContent = label;
      targetSelect.appendChild(opt);
    }
    if (currentSelectedVal && [...targetSelect.options].some(o => o.value === currentSelectedVal)) {
      targetSelect.value = currentSelectedVal;
    }

    // 各ビューのレンダリング
    this.renderCallStack();
    this.renderCartesianTree();
    this.renderTable();
    this.renderInspector();
  }

  // --- 委譲チェーン (Call Stack) レンダリング ---

  renderCallStack() {
    const container = document.getElementById('chain-flow-container');
    container.innerHTML = '';

    if (this.sim.activeChain.length === 0 && this.sim.suspendedSubchains.length === 0) {
      container.innerHTML = '<div style="color: var(--text-muted); font-size: 0.8rem; padding: 12px 0;">委譲チェーンは空です。</div>';
      document.getElementById('suspended-subchains-panel').style.display = 'none';
      return;
    }

    // 1. アクティブチェーン
    for (let i = 0; i < this.sim.activeChain.length; i++) {
      const b = this.sim.activeChain[i];
      const eff = this.sim.getEffectiveRemaining(b);
      const nodeEl = document.createElement('div');
      let classList = ['chain-node'];

      if (b === this.sim.runningBaton) classList.push('running');
      else if (b.state === BatonState.WAIT_RETURN) classList.push('wait-return');

      if (b === this.sim.cartesianRoot) classList.push('min-root');

      nodeEl.className = classList.join(' ');
      nodeEl.innerHTML = `
        <span style="font-weight: bold; font-size: 0.85rem; color: var(--text-bright);">${b.name}</span>
        <span style="font-family: var(--font-mono); font-size: 0.72rem; color: var(--accent-yellow); margin-top: 2px;">
          ${b.remaining.toFixed(1)}ms <span style="color: var(--accent-cyan);">(${eff.toFixed(1)}ms)</span>
        </span>
        <span style="font-size: 0.6rem; color: var(--text-muted);">実記録 (概念)</span>
        <span class="state-badge state-${b.state}" style="margin-top: 4px;">${b.state}</span>
      `;
      nodeEl.onclick = () => {
        this.selectedBatonId = b.id;
        this.updateUI();
      };
      container.appendChild(nodeEl);

      if (i < this.sim.activeChain.length - 1) {
        const arrow = document.createElement('div');
        arrow.className = 'chain-arrow';
        arrow.textContent = '➔';
        container.appendChild(arrow);
      }
    }

    // 2. タイムアウトでちぎれたサブチェーン
    const suspendedPanel = document.getElementById('suspended-subchains-panel');
    const suspendedList = document.getElementById('suspended-cards-list');
    suspendedList.innerHTML = '';

    if (this.sim.suspendedSubchains.length > 0) {
      suspendedPanel.style.display = 'flex';

      for (const sc of this.sim.suspendedSubchains) {
        const breakEl = document.createElement('div');
        breakEl.className = 'chain-split-break';
        breakEl.innerHTML = `
          <span>✂️ タイムアウト分断</span>
          <span style="font-size: 0.6rem; color: var(--accent-orange); font-weight: normal;">(WAIT_START)</span>
        `;
        container.appendChild(breakEl);

        const suspendedWrapper = document.createElement('div');
        suspendedWrapper.className = 'suspended-subchain-wrapper';

        for (let j = 0; j < sc.nodes.length; j++) {
          const sb = sc.nodes[j];
          const eff = this.sim.getEffectiveRemaining(sb);
          const sNode = document.createElement('div');
          let cList = ['chain-node', 'suspended-node'];
          if (sb === sc.root) cList.push('wait-start');
          if (sb === sc.tail) cList.push('tail-indicator');

          sNode.className = cList.join(' ');
          sNode.innerHTML = `
            <span style="font-weight: bold; font-size: 0.85rem; color: var(--text-bright);">${sb.name}</span>
            <span style="font-family: var(--font-mono); font-size: 0.72rem; color: var(--accent-yellow); margin-top: 2px;">
              ${sb.remaining.toFixed(1)}ms <span style="color: var(--accent-cyan);">(${eff.toFixed(1)}ms)</span>
            </span>
            <span style="font-size: 0.6rem; color: var(--text-muted);">実記録 (概念)</span>
            <span class="state-badge state-${sb.state}" style="margin-top: 4px;">${sb.state}</span>
          `;
          sNode.onclick = () => {
            this.selectedBatonId = sb.id;
            this.updateUI();
          };
          suspendedWrapper.appendChild(sNode);

          if (j < sc.nodes.length - 1) {
            const arrow = document.createElement('div');
            arrow.className = 'chain-arrow';
            arrow.style.color = 'var(--accent-orange)';
            arrow.textContent = '➔';
            suspendedWrapper.appendChild(arrow);
          }
        }
        container.appendChild(suspendedWrapper);

        // 下部カードと再開ボタン
        const card = document.createElement('div');
        card.style.background = 'rgba(22, 27, 34, 0.8)';
        card.style.border = '1px solid var(--border-color)';
        card.style.borderRadius = '4px';
        card.style.padding = '8px 10px';
        card.style.display = 'flex';
        card.style.justifyContent = 'space-between';
        card.style.alignItems = 'center';
        card.style.flexWrap = 'wrap';
        card.style.gap = '8px';

        const subMinText = sc.subMin ? `${sc.subMin.name} (${sc.subMin.remaining.toFixed(1)}ms)` : 'なし';
        card.innerHTML = `
          <div style="font-size: 0.76rem; display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">
            <span>チェーン: <strong style="color: var(--text-bright);">[${sc.nodes.map(n => n.name).join(' ➔ ')}]</strong></span>
            <span style="color: var(--accent-orange);">★配下最短 (sub_min): <strong>${subMinText}</strong></span>
            <span style="color: var(--accent-green);">🎯末尾ジャンプ先 (tail): <strong>${sc.tail.name}</strong></span>
          </div>
          <button class="btn btn-sm btn-cyan btn-resume" data-root-id="${sc.root.id}">
            ⚡ このサブチェーンを再開 (sys_baton_switch)
          </button>
        `;
        suspendedList.appendChild(card);
      }

      suspendedList.querySelectorAll('.btn-resume').forEach(btn => {
        btn.onclick = (e) => {
          const rootId = e.currentTarget.getAttribute('data-root-id');
          const target = this.sim.getBaton(rootId);
          if (target) {
            this.sim.sysBatonSwitch(target);
          }
        };
      });
    } else {
      suspendedPanel.style.display = 'none';
    }
  }

  // --- デカルト木 (Cartesian Tree) レンダリング ---

  renderCartesianTree() {
    const svg = document.getElementById('tree-svg');
    if (!svg) return;
    svg.innerHTML = '';

    if (!this.sim.cartesianRoot && this.sim.suspendedSubchains.length === 0) {
      const text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
      text.setAttribute('x', '50%');
      text.setAttribute('y', '50%');
      text.setAttribute('text-anchor', 'middle');
      text.setAttribute('fill', '#8b949e');
      text.setAttribute('font-size', '14');
      text.textContent = '有効なデカルト木はありません (バトンを実行してください)';
      svg.appendChild(text);
      return;
    }

    const svgWidth = svg.clientWidth || 600;
    const svgHeight = svg.clientHeight || 450;

    const hasSuspended = this.sim.suspendedSubchains.length > 0;
    let activeWidth = svgWidth;
    let suspStartX = svgWidth;
    let suspWidth = 0;

    if (hasSuspended) {
      const activeCount = Math.max(1, this.sim.activeChain.length);
      let suspTotalCount = 0;
      for (const sc of this.sim.suspendedSubchains) suspTotalCount += sc.nodes.length;

      const activeRatio = Math.max(0.3, Math.min(0.5, activeCount / (activeCount + suspTotalCount)));
      activeWidth = svgWidth * activeRatio;
      suspStartX = activeWidth;
      suspWidth = svgWidth - activeWidth;
    }

    // 1. アクティブツリー
    if (this.sim.cartesianRoot && this.sim.activeChain.length > 0) {
      this.drawSubCartesianTree(svg, this.sim.cartesianRoot, this.sim.activeChain, 0, activeWidth, svgHeight, 'アクティブ木 (CPU走行中)', false);
    }

    // 2. 分断サブツリー群
    if (hasSuspended) {
      let currX = suspStartX;
      const subWidth = suspWidth / this.sim.suspendedSubchains.length;

      for (const sc of this.sim.suspendedSubchains) {
        if (sc.cartesianRoot && sc.nodes.length > 0) {
          this.drawSubCartesianTree(svg, sc.cartesianRoot, sc.nodes, currX, subWidth, svgHeight, `✂️ 分断サブツリー (${sc.root.name} 待機中)`, true, sc);
          currX += subWidth;
        }
      }
    }
  }

  drawSubCartesianTree(svg, root, chain, startX, width, height, titleText, isSuspended, subchainMeta = null) {
    if (isSuspended) {
      const boundRect = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
      boundRect.setAttribute('x', startX + 6);
      boundRect.setAttribute('y', 28);
      boundRect.setAttribute('width', width - 12);
      boundRect.setAttribute('height', height - 36);
      boundRect.setAttribute('rx', '8');
      boundRect.setAttribute('fill', 'rgba(240, 136, 62, 0.04)');
      boundRect.setAttribute('stroke', '#f0883e');
      boundRect.setAttribute('stroke-width', '1.5');
      boundRect.setAttribute('stroke-dasharray', '5,3');
      svg.appendChild(boundRect);
    }

    const header = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    header.setAttribute('x', startX + width / 2);
    header.setAttribute('y', 44);
    header.setAttribute('text-anchor', 'middle');
    header.setAttribute('fill', isSuspended ? '#f0883e' : '#58a6ff');
    header.setAttribute('font-size', '11');
    header.setAttribute('font-weight', 'bold');
    header.textContent = titleText;
    svg.appendChild(header);

    const inOrderPositions = new Map();
    for (let i = 0; i < chain.length; i++) {
      inOrderPositions.set(chain[i], i);
    }

    const depths = new Map();
    function calcDepth(node, d) {
      if (!node) return;
      depths.set(node, d);
      calcDepth(node.cartesian_left, d + 1);
      calcDepth(node.cartesian_right, d + 1);
    }
    calcDepth(root, 0);

    const totalNodes = chain.length;
    const xStep = width / (totalNodes + 1);
    const maxDepth = Math.max(...depths.values(), 0);
    const yStep = Math.min(85, (height - 110) / (maxDepth + 1));

    const coords = new Map();
    for (const b of chain) {
      const x = startX + (inOrderPositions.get(b) + 1) * xStep;
      const y = 80 + depths.get(b) * yStep;
      coords.set(b, { x, y });
    }

    function drawEdge(parent, child, isRight) {
      if (!parent || !child) return;
      const pCoord = coords.get(parent);
      const cCoord = coords.get(child);
      if (!pCoord || !cCoord) return;

      const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
      line.setAttribute('x1', pCoord.x);
      line.setAttribute('y1', pCoord.y);
      line.setAttribute('x2', cCoord.x);
      line.setAttribute('y2', cCoord.y);
      line.setAttribute('stroke', isRight ? '#f0883e' : '#58a6ff');
      line.setAttribute('stroke-width', isRight ? '2.5' : '1.5');
      if (isRight) {
        line.setAttribute('stroke-dasharray', '4,2');
      }
      svg.appendChild(line);

      const midX = (pCoord.x + cCoord.x) / 2;
      const midY = (pCoord.y + cCoord.y) / 2;
      const label = document.createElementNS('http://www.w3.org/2000/svg', 'text');
      label.setAttribute('x', midX + (isRight ? 8 : -8));
      label.setAttribute('y', midY);
      label.setAttribute('font-size', '9');
      label.setAttribute('fill', isRight ? '#f0883e' : '#58a6ff');
      label.setAttribute('text-anchor', isRight ? 'start' : 'end');
      label.textContent = isRight ? 'R (配下最短★)' : 'L';
      svg.appendChild(label);

      drawEdge(child, child.cartesian_left, false);
      drawEdge(child, child.cartesian_right, true);
    }

    drawEdge(root, root.cartesian_left, false);
    drawEdge(root, root.cartesian_right, true);

    for (const b of chain) {
      const coord = coords.get(b);
      if (!coord) continue;
      const { x, y } = coord;
      const eff = this.sim.getEffectiveRemaining(b);
      const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');
      g.setAttribute('transform', `translate(${x}, ${y})`);
      g.style.cursor = 'pointer';
      g.onclick = () => {
        this.selectedBatonId = b.id;
        this.updateUI();
      };

      const isRoot = (b === root);
      const isRunning = (b === this.sim.runningBaton);
      const isSelected = (b.id === this.selectedBatonId);
      const isSubMinTarget = (subchainMeta && subchainMeta.subMin === b);

      const rect = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
      rect.setAttribute('x', '-50');
      rect.setAttribute('y', '-22');
      rect.setAttribute('width', '100');
      rect.setAttribute('height', '44');
      rect.setAttribute('rx', '6');

      let strokeColor = isSuspended ? '#f0883e' : '#30363d';
      let strokeWidth = '1.5';
      let fill = '#161b22';

      if (isRoot) {
        strokeColor = isSuspended ? '#f0883e' : '#d29922';
        strokeWidth = '2.5';
        fill = isSuspended ? '#2c1e11' : '#2a2211';
      }
      if (isRunning) {
        strokeColor = '#3fb950';
        strokeWidth = '3';
        fill = '#12261a';
      }
      if (isSelected) {
        rect.setAttribute('filter', 'drop-shadow(0 0 8px #58a6ff)');
      }

      rect.setAttribute('stroke', strokeColor);
      rect.setAttribute('stroke-width', strokeWidth);
      rect.setAttribute('fill', fill);
      g.appendChild(rect);

      const title = document.createElementNS('http://www.w3.org/2000/svg', 'text');
      title.setAttribute('x', '0');
      title.setAttribute('y', '-7');
      title.setAttribute('text-anchor', 'middle');
      title.setAttribute('fill', isRoot ? '#f0f6fc' : '#c9d1d9');
      title.setAttribute('font-size', '10.5');
      title.setAttribute('font-weight', 'bold');
      title.textContent = b.name;
      g.appendChild(title);

      const timeText = document.createElementNS('http://www.w3.org/2000/svg', 'text');
      timeText.setAttribute('x', '0');
      timeText.setAttribute('y', '7');
      timeText.setAttribute('text-anchor', 'middle');
      timeText.setAttribute('font-size', '9');
      timeText.setAttribute('font-family', 'monospace');
      timeText.innerHTML = `<tspan fill="#d29922">${b.remaining.toFixed(1)}ms</tspan> <tspan fill="#58a6ff">(${eff.toFixed(1)}ms)</tspan>`;
      g.appendChild(timeText);

      const subLabel = document.createElementNS('http://www.w3.org/2000/svg', 'text');
      subLabel.setAttribute('x', '0');
      subLabel.setAttribute('y', '17');
      subLabel.setAttribute('text-anchor', 'middle');
      subLabel.setAttribute('fill', '#8b949e');
      subLabel.setAttribute('font-size', '7');
      subLabel.textContent = isSubMinTarget ? '★配下最短キャッシュ' : '実記録 (概念)';
      g.appendChild(subLabel);

      if (isRoot) {
        const crown = document.createElementNS('http://www.w3.org/2000/svg', 'text');
        crown.setAttribute('x', '0');
        crown.setAttribute('y', '-25');
        crown.setAttribute('text-anchor', 'middle');
        crown.setAttribute('fill', isSuspended ? '#f0883e' : '#d29922');
        crown.setAttribute('font-size', '9.5');
        crown.setAttribute('font-weight', 'bold');
        crown.textContent = isSuspended ? '⏸️ WAIT_START' : '👑 ROOT (LAPIC)';
        g.appendChild(crown);
      }

      svg.appendChild(g);
    }
  }

  // --- バトン一覧テーブル ---

  renderTable() {
    const tbody = document.getElementById('baton-table-body');
    if (!tbody) return;
    tbody.innerHTML = '';

    for (const b of this.sim.getAllBatons()) {
      const eff = this.sim.getEffectiveRemaining(b);
      const tr = document.createElement('tr');
      if (b.id === this.selectedBatonId) tr.className = 'selected';
      tr.style.cursor = 'pointer';
      tr.onclick = () => {
        this.selectedBatonId = b.id;
        this.updateUI();
      };

      tr.innerHTML = `
        <td style="font-weight: 600;">${b.name}</td>
        <td><span class="state-badge state-${b.state}">${b.state}</span></td>
        <td style="font-family: var(--font-mono);">
          <span style="color: var(--accent-yellow);">${b.remaining.toFixed(1)}</span>
          <span style="color: var(--accent-cyan);">(${eff.toFixed(1)})</span>
          <span style="color: var(--text-muted);">/ ${b.quantum}ms</span>
        </td>
        <td>${b.caller_baton ? b.caller_baton.name : '-'}</td>
        <td>${b.tail_baton ? b.tail_baton.name : '-'}</td>
      `;
      tbody.appendChild(tr);
    }
  }

  // --- インスペクター ---

  renderInspector() {
    const container = document.getElementById('inspector-content');
    if (!container) return;
    const b = this.selectedBatonId ? this.sim.getBaton(this.selectedBatonId) : (this.sim.runningBaton || this.sim.cartesianRoot);

    if (!b) {
      container.innerHTML = '<div style="color: var(--text-muted); text-align: center; padding: 12px 0;">バトンがありません</div>';
      return;
    }

    const eff = this.sim.getEffectiveRemaining(b);
    const unsynced = (b.remaining - eff);

    container.innerHTML = `
      <div style="font-size: 0.95rem; font-weight: bold; color: var(--accent-cyan); border-bottom: 1px solid var(--border-color); padding-bottom: 4px; margin-bottom: 4px; display: flex; justify-content: space-between;">
        <span>${b.name}</span>
        <span class="state-badge state-${b.state}">${b.state}</span>
      </div>
      <div class="inspector-row"><span class="inspector-key">希望クォンタム (quantum):</span><span class="inspector-val">${b.quantum} ms</span></div>
      <div class="inspector-row"><span class="inspector-key">実記録残り時間 (remaining):</span><span class="inspector-val" style="color: var(--accent-yellow);">${b.remaining.toFixed(2)} ms</span></div>
      <div class="inspector-row"><span class="inspector-key">概念上の残り時間 (effective):</span><span class="inspector-val" style="color: var(--accent-cyan);">${eff.toFixed(2)} ms</span></div>
      <div class="inspector-row"><span class="inspector-key">未同期経過差分 (Δt):</span><span class="inspector-val" style="color: ${unsynced > 0.001 ? 'var(--accent-orange)' : 'var(--text-bright)'};">${unsynced.toFixed(2)} ms ${unsynced > 0.001 ? '★O(1)遅延保持中' : ''}</span></div>
      ${b.lazy_delta ? `<div class="inspector-row"><span class="inspector-key">遅延タグ (lazy_delta):</span><span class="inspector-val" style="color: var(--accent-orange);">${b.lazy_delta.toFixed(2)} ms ★Push-Down保留中</span></div>` : ''}
      ${b.subchain_consumed_offset ? `<div class="inspector-row"><span class="inspector-key">サブチェーン累積消費 (offset):</span><span class="inspector-val" style="color: var(--accent-orange);">${b.subchain_consumed_offset.toFixed(2)} ms</span></div>` : ''}
      ${b.pending_lazy_offset ? `<div class="inspector-row"><span class="inspector-key">巻き戻り遅延オフセット:</span><span class="inspector-val" style="color: var(--accent-orange);">${b.pending_lazy_offset.toFixed(2)} ms ★スキップ保留中</span></div>` : ''}
      <div class="inspector-row"><span class="inspector-key">最終同期時刻 (last_sync_tsc):</span><span class="inspector-val">${b.last_sync_tsc.toFixed(2)} ms</span></div>
      <div class="inspector-row"><span class="inspector-key">呼出元 (caller_baton):</span><span class="inspector-val">${b.caller_baton ? b.caller_baton.name : 'nullptr'}</span></div>
      <div class="inspector-row"><span class="inspector-key">呼出先 (callee_baton):</span><span class="inspector-val">${b.callee_baton ? b.callee_baton.name : 'nullptr'}</span></div>
      <div class="inspector-row"><span class="inspector-key">サブ末尾 (tail_baton):</span><span class="inspector-val" style="color: var(--accent-orange);">${b.tail_baton ? b.tail_baton.name : 'nullptr'}</span></div>
      <div style="border-top: 1px dashed var(--border-color); margin: 4px 0;"></div>
      <div style="font-weight: bold; color: var(--accent-purple);">デカルト木 (Cartesian Tree) ポインタ:</div>
      <div class="inspector-row"><span class="inspector-key">cartesian_parent:</span><span class="inspector-val">${b.cartesian_parent ? b.cartesian_parent.name : 'nullptr (Root)'}</span></div>
      <div class="inspector-row"><span class="inspector-key">cartesian_left (親側最小):</span><span class="inspector-val">${b.cartesian_left ? b.cartesian_left.name : 'nullptr'}</span></div>
      <div class="inspector-row"><span class="inspector-key">cartesian_right (配下最小★):</span><span class="inspector-val" style="color: var(--accent-orange);">${b.cartesian_right ? b.cartesian_right.name + ' [' + b.cartesian_right.remaining.toFixed(1) + 'ms (' + this.sim.getEffectiveRemaining(b.cartesian_right).toFixed(1) + 'ms)]' : 'nullptr'}</span></div>
      <div class="inspector-row"><span class="inspector-key">sub_min_baton (保存最短):</span><span class="inspector-val" style="color: var(--accent-green);">${b.sub_min_baton ? b.sub_min_baton.name : 'nullptr'}</span></div>
    `;
  }
}
