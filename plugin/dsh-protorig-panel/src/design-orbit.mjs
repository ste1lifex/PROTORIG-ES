const palette = {
  ink: '#090D0C',
  field: '#101815',
  line: '#34443B',
  paper: '#F5F3E6',
  muted: '#A6B6A9',
  gold: '#F5D568',
  green: '#A9DEBA',
  red: '#FF9C87',
};

const modeLabels = {
  RUNNING: ['运行中', 'gold'],
  NEEDS: ['等待操作', 'gold'],
  DONE: ['本轮完成', 'green'],
  FAILED: ['运行失败', 'red'],
  IDLE: ['等待任务', 'muted'],
  WAITING: ['等待连接', 'muted'],
  LOST: ['连接中断', 'red'],
  FLASHING: ['固件写入中', 'gold'],
  APPROVAL: ['审批待确认', 'gold'],
};

const xml = (value) => String(value ?? '').replace(/[^\u0009\u000a\u000d\u0020-\ud7ff\ue000-\ufffd\u{10000}-\u{10ffff}]/gu, '').replace(/[&<>"]/g, (c) => ({
  '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;',
})[c]);

function lines(value, width, size, limit) {
  const chars = [...String(value ?? '').replace(/\s+/g, ' ').trim()];
  const advance = (c) => {
    if (/[ilI.,:;'!| ]/.test(c)) return size * 0.4;
    return /[\x21-\x7e]/.test(c) && !/[MWmw@%]/.test(c) ? size * 0.72 : size;
  };
  const result = [];
  for (let index = 0; index < limit && chars.length; index++) {
    let used = 0;
    const row = [];
    while (chars.length && used + advance(chars[0]) <= width) {
      used += advance(chars[0]);
      row.push(chars.shift());
    }
    if (index === limit - 1 && chars.length) {
      while (row.length && used + size > width) used -= advance(row.pop());
      row.push('…');
    }
    result.push(row.join('').trim());
  }
  return { rows: result, clipped: chars.length > 0 };
}

function label(value, x, y, size, color = 'paper', weight = 400) {
  return `<text x="${x}" y="${y}" fill="${palette[color]}" font-size="${size}" font-weight="${weight}" text-anchor="middle">${xml(value)}</text>`;
}

function arc(radius, from, to) {
  const point = (degree) => {
    const angle = degree * Math.PI / 180;
    return `${(240 + radius * Math.sin(angle)).toFixed(2)} ${(240 - radius * Math.cos(angle)).toFixed(2)}`;
  };
  return `M${point(from)} A${radius} ${radius} 0 0 ${to > from ? 1 : 0} ${point(to)}`;
}

function rail(progress, color) {
  return `<path d="${arc(214, 233, 127)}" fill="none" stroke="#3D4F41" stroke-width="3"/>
    ${progress > 0 ? `<path d="${arc(214, 233, 233 - 106 * progress / 100)}" fill="none" stroke="${palette[color]}" stroke-width="4"/>` : ''}
    <path d="M67 369l6-5m334 0 6 5" stroke="#7B8F7D" stroke-width="2"/>`;
}

const roundedPercent = (value) => Number.isFinite(value) ? Math.round(Math.max(0, Math.min(100, value))) : null;

function approvalView(approval) {
  const raw = approval.cmd ?? '';
  const exact = typeof raw === 'string' && /^[!-~]+(?: [!-~]+)*$/.test(raw);
  const command = lines(exact ? raw : approval.reason, 336, 22, 1);
  // Display the entire command on one line before offering execution.
  const countdown = Number.isFinite(approval.countdown) ? Math.ceil(Math.max(0, approval.countdown)) : null;
  const computer = approval.canAnswer === false || countdown === 0;
  const canAllow = !computer && exact && !command.clipped;
  const tool = lines(approval.toolName || '工具', 144, 22, 1).rows[0];
  const note = computer || !canAllow ? '请到电脑查看' : countdown == null ? '仅允许本次执行' : `${countdown} 秒后转到电脑`;
  return { computer, canAllow, tool, note, command: command.rows[0] || '命令详情请在电脑查看' };
}

function questionView(q) {
  const options = q.options ?? [];
  const index = Math.min(Math.max(0, Math.trunc(q.index) || 0), Math.max(0, options.length - 1));
  const option = options[index];
  /* 长选项要能读完：先按 18 px 折行，超出一屏就按时间翻页（USB 是宿主整帧渲染，
   * 翻页只是换一次绘制内容；BLE 位图槽只有 300×30，那边仍然只用第一行）。
   * 2026-11-21 排期见 local/docs/research/pc-dsh/90。 */
  const wrapped = lines(typeof option === 'string' ? option : option?.label, 278, 18, 99)
    .rows.filter((row) => row);
  const pages = [];
  for (let i = 0; i < wrapped.length; i += 3) pages.push(wrapped.slice(i, i + 3));
  if (!pages.length) pages.push(['请到电脑回答']);
  /* 从卡片出现那一刻开始算，别用绝对时间——否则卡一亮出来就可能停在第 2 页，
   * 用户会漏掉开头。q.shownAt 由 PanelService 在出卡时写入。 */
  const shownAt = Number.isFinite(q.shownAt) ? q.shownAt : Date.now();
  const pageIndex = pages.length > 1
    ? Math.floor(Math.max(0, Date.now() - shownAt) / 2600) % pages.length
    : 0;
  const choiceRows = pages[pageIndex];
  const scrolling = pages.length > 1;
  const choice = choiceRows[0];
  /* 题干也要能读完：19 px、每页 2 行，超了按同一套时钟翻页（页码放标题行，不占底部）。
   * 2026-11-21 实机：题干单行截断，用户差点看不懂问的是什么。 */
  const promptWrapped = lines(
    q.prompt || (q.multiSelect ? '请选择所有适用项' : '请选择一项'), 354, 19, 99,
  ).rows.filter((row) => row);
  const promptPages = [];
  for (let i = 0; i < promptWrapped.length; i += 2) promptPages.push(promptWrapped.slice(i, i + 2));
  if (!promptPages.length) promptPages.push(['']);
  const promptPage = promptPages.length > 1
    ? Math.floor(Math.max(0, Date.now() - shownAt) / 2600) % promptPages.length
    : 0;
  const promptRows = promptPages[promptPage];
  const selected = q.selected ?? [];
  /* 已移交电脑：卡片留在屏上做交代，但不再接受触摸。调用方要据此把整张卡画成"不可操作"，
   * 否则用户会在一个点不动的按钮上反复戳（2026-11-21 实机反馈）。 */
  const computer = q.canAnswer === false;
  /* 注意：**不要**因为提示语或选项放不下就禁用整张卡。2026-11-21 实机踩过——
   * 长题干/长选项会让按钮全部失效，用户点了没反应还以为卡坏了。
   * 版式放不下就截断显示，选择权始终给用户。 */
  const enabled = !computer && options.length > 0;
  /* 选项文字放不下时**不能**把这一项判成不可选（2026-11-21 实机：长选项被静默禁用，
   * 用户点了没反应、只能去电脑上答）。这里允许选，只是显示会被截断成一行。 */
  const canChoose = enabled && !!choiceRows[0];
  /* 页码按设计稿（design/dsh_panel_v4/index.html）放在**选项提示**里：
   *   `${index+1} / ${total} · 点按选择` / `… · 已选 · 点按取消`；
   * 标题行只有 `DSH 问答 · 单选|多选`，不再带页码。 */
  const hintText = computer
    ? '已移交电脑'
    : !canChoose
      ? '请到电脑查看'
      : selected.includes(index) ? '已选 · 点按取消' : '点按选择';
  const total = options.length;
  const progress = total > 0 ? `${index + 1} / ${total}` : '—';
  const hint = `${progress} · ${hintText}${scrolling ? ` · ${pageIndex + 1}/${pages.length}` : ''}`;
  const promptPageText = promptPages.length > 1 ? `${promptPage + 1}/${promptPages.length}` : '';
  const pageText = progress;
  const remaining = total > 0 ? total - index - 1 : 0;
  // 用户反馈"不知道怎么切换选项"：页码放标题行，下一项按钮给剩余数量
  const nextHint = computer ? '已移交电脑' : !enabled ? '请到电脑查看' : remaining > 0 ? `还有 ${remaining} 个` : '回到第 1 页';
  /* 提交按钮必须自己解释"为什么点了没反应"：没选任何项时固件只震一下，
   * 屏上什么都没有 = 用户以为卡坏了。 */
  const submitHint = computer ? '已移交电脑' : selected.length > 0 ? `已选 ${selected.length}` : '先选一项';
  return {
    choice, prompt: promptRows[0] ?? '', canChoose, chosen: selected.includes(index),
    more: total > 1, count: selected.length, canSubmit: enabled && selected.length > 0,
    hint, pageText, nextHint, submitHint, computer, choiceRows, scrolling,
    promptRows, promptPageText,
  };
}

function button(name, value, box, emphasis = false, enabled = true, hint = '') {
  const [x, y, w, h] = box;
  const mid = x + w / 2;
  const bg = emphasis && enabled ? palette.gold : '#1B2821';
  const border = emphasis && enabled ? palette.gold : enabled ? '#465D50' : '#2A3A30';
  const ink = emphasis && enabled ? 'ink' : enabled ? 'paper' : 'muted';
  return `<g${enabled ? ` data-action="${name}"` : ''}>
    <rect x="${x}" y="${y}" width="${w}" height="${h}" fill="transparent"/>
    <path d="M${x + 12} ${y + 4}H${x + w - 4}V${y + h - 12}L${x + w - 12} ${y + h - 4}H${x + 4}V${y + 12}Z" fill="${bg}" stroke="${border}" stroke-width="2"/>
    ${label(value, mid, y + (hint ? 48 : 58), 24, ink, 700)}
    ${hint ? label(hint, mid, y + 74, 16, emphasis && enabled ? 'ink' : 'muted') : ''}
  </g>`;
}

function buildOrbit(s, tokens) {
  const zones = tokens.hotzones;
  const q = s.question;
  const approval = s.approval;
  const flash = Number.isFinite(s.flashPct) && s.flashPct >= 0;
  const session = s.session ?? {};
  const reported = s.connection === 'lost' ? 'LOST' : s.connection === 'waiting' ? 'WAITING' : session.state;
  let mode = Object.hasOwn(modeLabels, reported) && reported !== 'APPROVAL' ? reported : 'IDLE';
  if (mode !== 'LOST' && mode !== 'WAITING') {
    if (flash) mode = 'FLASHING';
    else if (approval) mode = 'APPROVAL';
    else if (q) mode = 'QUESTION';
  }
  const live = ['RUNNING', 'NEEDS', 'DONE', 'FAILED'].includes(mode);
  const ctx = live ? roundedPercent(session.ctx) : null;
  const battery = roundedPercent(s.battery);
  const time = /^\d{2}:\d{2}$/.test(s.time ?? '') ? s.time : '--:--';
  const timeSlots = [117, 184, 240, 296, 363];

  const header = `<g aria-hidden="true">
    <path d="M176 61l9-15 9 15Zm9-8v5" fill="none" stroke="${palette.gold}" stroke-width="2"/>
    <text x="202" y="62" fill="${palette.muted}" font-size="19" font-weight="700" letter-spacing="2.6">DSH</text>
    <path d="M268 54h28v3h-28Zm0 7h17v3h-17Z" fill="${palette.gold}"/>
    ${[...time].map((digit, index) => label(digit, timeSlots[index], 166, digit === ':' ? 82 : 104, 'paper', 700)).join('')}
  </g>`;

  const [state, color] = modeLabels[mode] ?? modeLabels.NEEDS;
  const badge = (value = state) => `<path d="M161 186H326v27l-8 9H154v-28Z" fill="#202C23" stroke="#3B4C40"/>
    <path d="M167 209l5-9 5 9Z" fill="${palette[color]}"/>
    ${label(value, 244, 212, 23, color, 700)}`;

  const batteryText = battery == null ? '' : `<path d="M149 409h19v10h-19Zm20 3h3v4h-3Z" fill="none" stroke="${palette.muted}" stroke-width="1.5"/>
    <rect x="152" y="412" width="${Math.round(battery * 0.13)}" height="4" fill="${palette.muted}"/>
    ${label(`${battery}%`, 199, 421, 18, 'muted')}`;
  const balance = lines(s.balance, 101, 18, 1).rows[0];
  const footer = `${batteryText}${balance ? label(balance, 293, 421, 18, 'muted') : ''}
    ${batteryText && balance ? '<path d="M240 406v17" stroke="#45574B"/>' : ''}`;

  let body = '';
  if (live) {
    const title = lines(session.title || '未命名会话', 350, 30, 2).rows;
    const steps = Number.isFinite(session.steps) ? Math.max(0, Math.round(session.steps)) : null;
    const total = Number.isFinite(session.stepsTotal) && session.stepsTotal >= steps && session.stepsTotal > 0
      ? Math.round(session.stepsTotal) : null;
    const stepText = steps == null ? '—' : total ? `${steps} / ${total}` : String(steps);
    body = `${badge()}${title.map((row, index) => label(row, 240, title.length === 1 ? 271 : 258 + index * 34, 30, 'paper', 700)).join('')}
      <path d="M92 311H388M240 330v48" stroke="${palette.line}"/>
      ${label(lines(stepText, 132, 34, 1).rows[0], 155, 354, 34, 'paper', 700)}
      ${label(ctx == null ? '—' : `${ctx}%`, 325, 354, 34, 'gold', 700)}
      ${label('步骤', 155, 381, 18, 'muted')}
      ${label('上下文', 325, 381, 18, 'muted')}
      ${footer}`;
  } else if (mode === 'APPROVAL') {
    const { computer, canAllow, tool, note, command } = approvalView(approval);
    body = `${badge(computer ? '电脑待确认' : state)}
      ${label(`${tool} 请求执行`, 240, 250, 23, 'paper', 700)}
      ${label(command, 240, 286, 22, 'muted')}
      ${button('deny', '拒绝', zones.deny, false, !computer)}
      ${button('allow', '允许一次', zones.allow, true, canAllow)}
      ${label(note, 240, 436, 18, 'muted')}`;
  } else if (mode === 'QUESTION') {
    const { choice, prompt, canChoose, chosen, more, count, canSubmit, hint, pageText, nextHint } = questionView(q);
    body = `${label(`DSH 问答 · ${q.multiSelect ? '多选' : '单选'}`, 240, 197, 17, 'gold', 700)}
      ${label(prompt, 240, 222, 25, 'paper', 700)}
      ${button('option', choice, zones.option, chosen, canChoose, hint)}
      ${button('next', '下一项', zones.next, false, more)}
      ${button('submit', '提交', zones.submit, true, canSubmit, `已选 ${count}`)}`;
  } else if (mode === 'FLASHING') {
    const pct = roundedPercent(s.flashPct) ?? 0;
    body = `${badge()}
      ${label('请保持 USB 连接', 240, 273, 27, 'paper', 700)}
      ${label(`${pct}%`, 240, 339, 48, 'gold', 700)}
      <rect x="117" y="363" width="246" height="5" fill="#34443B"/>
      <rect x="117" y="363" width="${Math.round(246 * pct / 100)}" height="5" fill="${palette.gold}"/>`;
  } else {
    const empty = {
      IDLE: ['当前没有会话', '在电脑上启动 DSH 任务'],
      LOST: ['DSH 状态已暂停', '检查 USB 连接后重新进入'],
      WAITING: ['等待 DSH 面板', '连接 USB，从表盘右滑进入'],
    }[mode];
    body = `${badge()}
      ${label(empty[0], 240, 273, 30, 'paper', 700)}
      ${label(empty[1], 240, 320, 21, 'muted')}
      ${mode === 'IDLE' ? footer : ''}`;
  }

  return `<svg xmlns="http://www.w3.org/2000/svg" width="480" height="480" viewBox="0 0 480 480" font-family="Source Han Sans SC, sans-serif">
    <defs>
      <pattern id="dots" width="9" height="9" patternUnits="userSpaceOnUse"><circle cx="3" cy="3" r="1.25" fill="#426455"/></pattern>
    </defs>
    <path fill="${palette.ink}" d="M0 0h480v480H0z"/>
    <circle cx="240" cy="240" r="219" fill="${palette.field}" stroke="#4F6353"/>
    <path d="${arc(214, -32, -13)}M190 33h13M277 33h13" fill="none" stroke="#A99555" stroke-width="2"/>
    <path d="${arc(214, 13, 32)}" fill="none" stroke="#A99555" stroke-width="2"/>
    <path d="M49 192h58l-23 89H45q-9-44 4-89Zm382 0h-58l23 89h39q9-44-4-89Z" fill="url(#dots)" opacity=".6"/>
    ${ctx != null ? rail(ctx, 'gold') : ''}
    ${header}${body}
  </svg>`;
}

export { buildOrbit, xml, lines, roundedPercent, approvalView, questionView };
