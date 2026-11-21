/*
 * panel.mjs —— DSH 面板的画面（占位版，等 design/dsh_panel 的设计稿落定后替换）
 *
 * 约束（research/57 §3）：
 *   · 480×480，圆心 (240,240)，内容安全圆 R=220；
 *   · 静态底板只进一次；时间/CTX/余额落在小区域；
 *   · 文字全部由电脑渲染（SVG → resvg）。
 */

export const COLORS = {
  bg: '#06080a',
  panel: '#0e1412',
  panelEdge: '#26302a',
  amber: '#e8c454',
  amberDim: '#8a7430',
  cyan: '#6fe3d2',
  text: '#edefe6',
  dim: '#98a094',
  danger: '#ff625a',
  ok: '#7bdc8b',
  warn: '#f2c14e',
};

const STATE_STYLE = {
  RUNNING: { color: COLORS.cyan, label: 'RUNNING' },
  NEEDS: { color: COLORS.warn, label: 'NEEDS YOU' },
  DONE: { color: COLORS.ok, label: 'DONE' },
  FAILED: { color: COLORS.danger, label: 'FAILED' },
  IDLE: { color: COLORS.dim, label: 'IDLE' },
};

export function createPanelState() {
  return {
    time: '--:--',
    battery: null, // 0..100
    bright: null,
    link: 'USB',
    session: { title: '等待 DSH 会话…', state: 'IDLE', tool: '', steps: 0, ctx: null },
    balance: null, // '¥12.34' 或 null
    approval: null, // { toolName, reason }
    question: null, // 预留：多选问答
    pcMode: false,
  };
}

/** 中文标题截断（按显示宽度粗略估算：ASCII 0.55em，CJK 1em）。 */
export function truncateToWidth(text, maxWidth, fontSize) {
  let w = 0;
  let out = '';
  for (const ch of String(text)) {
    const cw = /[\u3000-\u9fff\uff00-\uffef]/.test(ch) ? fontSize : fontSize * 0.55;
    if (w + cw > maxWidth) return out + '…';
    w += cw;
    out += ch;
  }
  return out;
}

function esc(s) {
  return String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
}

/** 装饰：同心环 + 刻度，纯几何，不依赖任何品牌素材。 */
function backdrop() {
  const cx = 240;
  const cy = 240;
  let ticks = '';
  for (let deg = 0; deg < 360; deg += 15) {
    const rad = (deg * Math.PI) / 180;
    const long = deg % 45 === 0;
    const r1 = 214;
    const r2 = long ? 204 : 210;
    const x1 = cx + Math.cos(rad) * r1;
    const y1 = cy + Math.sin(rad) * r1;
    const x2 = cx + Math.cos(rad) * r2;
    const y2 = cy + Math.sin(rad) * r2;
    ticks += `<line x1="${x1.toFixed(1)}" y1="${y1.toFixed(1)}" x2="${x2.toFixed(1)}" y2="${y2.toFixed(1)}" stroke="${COLORS.amberDim}" stroke-width="${long ? 2 : 1}" opacity="0.7"/>`;
  }
  return `
  <circle cx="240" cy="240" r="222" fill="none" stroke="${COLORS.panelEdge}" stroke-width="1" opacity="0.9"/>
  <circle cx="240" cy="240" r="196" fill="none" stroke="${COLORS.panelEdge}" stroke-width="1" opacity="0.45" stroke-dasharray="2 6"/>
  <circle cx="240" cy="240" r="120" fill="none" stroke="${COLORS.panelEdge}" stroke-width="1" opacity="0.35"/>
  ${ticks}`;
}

function ctxRing(cx, cy, r, ctx) {
  if (ctx == null) {
    return `<circle cx="${cx}" cy="${cy}" r="${r}" fill="none" stroke="${COLORS.panelEdge}" stroke-width="5" opacity="0.6"/>`;
  }
  const pct = Math.max(0, Math.min(1, ctx / 100));
  const circ = 2 * Math.PI * r;
  const on = circ * pct;
  const color = ctx >= 80 ? COLORS.danger : ctx >= 60 ? COLORS.warn : COLORS.cyan;
  return `
  <circle cx="${cx}" cy="${cy}" r="${r}" fill="none" stroke="${COLORS.panelEdge}" stroke-width="5" opacity="0.7"/>
  <circle cx="${cx}" cy="${cy}" r="${r}" fill="none" stroke="${color}" stroke-width="5"
          stroke-linecap="round" stroke-dasharray="${on.toFixed(1)} ${(circ - on).toFixed(1)}"
          transform="rotate(-90 ${cx} ${cy})"/>
  <text x="${cx}" y="${cy + 4}" fill="${COLORS.text}" font-size="15" font-weight="600" text-anchor="middle">${Math.round(ctx)}%</text>`;
}

function batteryGlyph(x, y, pct) {
  const fill = pct == null ? COLORS.dim : pct <= 15 ? COLORS.danger : pct <= 35 ? COLORS.warn : COLORS.ok;
  const w = 26;
  const h = 12;
  const inner = pct == null ? 0 : Math.max(0, Math.min(1, pct / 100));
  return `
  <g>
    <rect x="${x}" y="${y}" width="${w}" height="${h}" rx="2" fill="none" stroke="${COLORS.dim}" stroke-width="1.5"/>
    <rect x="${x + w + 1}" y="${y + 3}" width="3" height="${h - 6}" rx="1" fill="${COLORS.dim}"/>
    <rect x="${x + 2}" y="${y + 2}" width="${((w - 4) * inner).toFixed(1)}" height="${h - 4}" rx="1" fill="${fill}"/>
    <text x="${x - 6}" y="${y + h - 1}" fill="${COLORS.dim}" font-size="12" text-anchor="end">${pct == null ? '--' : pct + '%'}</text>
  </g>`;
}

/**
 * 组装整屏 SVG。
 * @param {ReturnType<createPanelState>} s
 */
export function buildSvg(s) {
  const st = STATE_STYLE[s.session.state] ?? STATE_STYLE.IDLE;
  const title = truncateToWidth(s.session.title, 250, 20);
  const meta = [s.session.tool, s.session.steps ? `${s.session.steps} steps` : '']
    .filter(Boolean)
    .join(' · ');

  const approval = s.approval
    ? `
    <g>
      <rect x="96" y="300" width="100" height="96" rx="10" fill="${COLORS.panel}" stroke="${COLORS.amber}" stroke-width="2"/>
      <text x="146" y="356" fill="${COLORS.amber}" font-size="22" font-weight="600" text-anchor="middle">允许</text>
      <rect x="284" y="300" width="100" height="96" rx="10" fill="${COLORS.panel}" stroke="${COLORS.panelEdge}" stroke-width="2"/>
      <text x="334" y="356" fill="${COLORS.dim}" font-size="22" font-weight="600" text-anchor="middle">拒绝</text>
      <text x="240" y="286" fill="${COLORS.warn}" font-size="13" text-anchor="middle" letter-spacing="1">${esc(
        truncateToWidth(s.approval.toolName ?? 'TOOL', 240, 13),
      )} 请求执行</text>
    </g>`
    : '';

  const balance = s.balance
    ? `<text x="240" y="292" fill="${COLORS.dim}" font-size="16" text-anchor="middle" letter-spacing="1">BALANCE ${esc(
        s.balance,
      )}</text>`
    : '';

  return `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="480" height="480" viewBox="0 0 480 480">
  <defs>
    <clipPath id="disc"><circle cx="240" cy="240" r="222"/></clipPath>
  </defs>
  <rect width="480" height="480" fill="${COLORS.bg}"/>
  <g clip-path="url(#disc)">
    <rect width="480" height="480" fill="${COLORS.bg}"/>
    ${backdrop()}

    <!-- 顶部：小号时间 + 电池 -->
    <text x="240" y="76" fill="${COLORS.text}" font-size="30" font-weight="600" text-anchor="middle" letter-spacing="2">${esc(
      s.time,
    )}</text>
    <text x="240" y="96" fill="${COLORS.amberDim}" font-size="11" text-anchor="middle" letter-spacing="3">DSH // ${esc(
      s.link,
    )}</text>
    ${batteryGlyph(360, 52, s.battery)}

    <!-- 会话卡 -->
    <rect x="92" y="120" width="296" height="150" rx="14" fill="${COLORS.panel}" stroke="${COLORS.panelEdge}" stroke-width="1.5"/>
    <rect x="92" y="120" width="6" height="150" rx="3" fill="${st.color}" opacity="0.85"/>
    <text x="112" y="150" fill="${COLORS.dim}" font-size="11" letter-spacing="2">SESSION // ACTIVE</text>
    <text x="112" y="182" fill="${COLORS.text}" font-size="20" font-weight="600">${esc(title)}</text>
    <text x="112" y="208" fill="${st.color}" font-size="14" letter-spacing="1.5">${st.label}</text>
    <text x="112" y="234" fill="${COLORS.dim}" font-size="13">${esc(truncateToWidth(meta || '—', 170, 13))}</text>
    ${ctxRing(342, 200, 30, s.session.ctx)}

    ${balance}
    ${approval}

    <!-- 底部装饰 -->
    <text x="240" y="432" fill="${COLORS.amberDim}" font-size="10" text-anchor="middle" letter-spacing="3">PROTORIG ES // ${esc(
      s.pcMode ? 'PC MODE' : 'STANDBY',
    )}</text>
  </g>
</svg>`;
}
