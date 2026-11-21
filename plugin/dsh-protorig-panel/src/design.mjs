/*
 * design.mjs —— 把设计稿模板变成可渲染的 SVG
 *
 * 设计稿默认在包内 design/dsh_panel_v4/（随 npm 包发布）：
 *   tokens.json            颜色/字号/网格/热区
 * 需要旧模板版式时用 PROTORIG_DESIGN_DIR 指向内部设计源。
 *
 * 这里只做"状态 → 占位符值"的替换，不做版面计算 —— 版面永远由设计稿决定。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildOrbit } from './design-orbit.mjs';
import { buildMonitor } from './design-monitor.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_DIR = process.env.PROTORIG_DESIGN_DIR ?? path.resolve(here, '../design/dsh_panel_v4');

export function loadDesign(dir = DEFAULT_DIR) {
  const read = (f) => fs.readFileSync(path.join(dir, f), 'utf8');
  const tokens = JSON.parse(read('tokens.json'));
  if (tokens.layout === 'orbit-v3' || tokens.layout === 'monitor-v4') return { dir, tokens };
  return {
    dir,
    tpl: read('panel.tpl.svg'),
    approvalFrag: read('panel.frag.approval.svg'),
    tokens,
  };
}

/** 状态 → 徽章三层颜色 + 文案（颜色全部来自 tokens.json）。 */
export function stateStyle(colorTokens, state) {
  const c = colorTokens;
  const table = {
    RUNNING: { color: c.green.hex, bg: c.badge_ok_bg.hex, line: c.badge_ok_ln.hex, label: 'RUNNING' },
    NEEDS: { color: c.red.hex, bg: c.badge_err_bg.hex, line: c.badge_err_ln.hex, label: 'NEEDS YOU' },
    DONE: { color: c.green.hex, bg: c.badge_ok_bg.hex, line: c.badge_ok_ln.hex, label: 'DONE' },
    FAILED: { color: c.red.hex, bg: c.badge_err_bg.hex, line: c.badge_err_ln.hex, label: 'FAILED' },
    IDLE: { color: c.text_dim.hex, bg: c.badge_idle_bg.hex, line: c.badge_idle_ln.hex, label: 'IDLE' },
  };
  return table[state] ?? table.IDLE;
}

/** 按显示宽度截断（CJK 记 1em，ASCII 约 0.55em）。 */
export function truncate(text, maxWidthPx, fontSize) {
  let w = 0;
  let out = '';
  for (const ch of String(text ?? '')) {
    const cw = /[\u2e80-\u9fff\uff00-\uffef\u3000-\u303f]/.test(ch) ? fontSize : fontSize * 0.55;
    if (w + cw > maxWidthPx) return `${out}…`;
    w += cw;
    out += ch;
  }
  return out;
}

function fmtTok(n) {
  if (!Number.isFinite(n) || n < 0) return '—';
  if (n >= 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`;
  if (n >= 1000) return `${(n / 1000).toFixed(1)}K`;
  return String(Math.round(n));
}

/** 12 段阶梯带。 */
function stepSegsSvg(on, total, colorOn, colorOff) {
  const segs = [];
  for (let i = 0; i < total; i++) {
    const x = 152 + i * 16;
    segs.push(
      `<rect x="${x}" y="358" width="13" height="22" fill="${i < on ? colorOn : colorOff}"/>`,
    );
  }
  return segs.join('');
}

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);


/* ======================================================================
 * v2（瑞士风格）适配
 *
 * v2 的模板与 tokens 结构完全不同：颜色在 tokens.colors（bg/text/muted/rule/
 * accent/failed），字号只有 48/28/12 三档，时间与电量是右对齐的两行小字。
 * 这里只做"状态 → 占位符"的映射，版面仍然全部由设计稿决定。
 * ====================================================================== */

function splitTitle(title, maxWidthPx, fontSize) {
  const text = String(title ?? '').trim();
  if (!text) return ['', ''];
  const first = truncate(text, maxWidthPx, fontSize);
  if (!first.endsWith('…')) return [first, ''];
  // 第一行放不下：按宽度切两行，第二行再截断一次
  let w = 0;
  let cut = 0;
  for (const ch of text) {
    const cw = /[\u2e80-\u9fff\uff00-\uffef\u3000-\u303f]/.test(ch) ? fontSize : fontSize * 0.55;
    if (w + cw > maxWidthPx) break;
    w += cw;
    cut += ch.length;
  }
  return [text.slice(0, cut), truncate(text.slice(cut), maxWidthPx, fontSize)];
}

function buildV2(s, design) {
  const c = design.tokens.colors;
  const q = s.question;
  const hasApproval = !!s.approval;

  // v2 只用一个强调色 + 一个失败红：RUNNING/NEEDS/DONE 都是琥珀，FAILED 才是红
  const stateColor =
    s.session?.state === 'FAILED'
      ? c.failed.hex
      : s.session?.state === 'IDLE'
        ? c.muted.hex
        : c.accent.hex;

  const stepsOn = Math.max(0, Math.min(12, s.session?.steps ?? 0));
  const total = s.session?.stepsTotal;
  const stepsText = total ? `${stepsOn} / ${total}` : `${stepsOn}`;

  const [title1, title2] = splitTitle(s.session?.title, 288, 28);

  const approvalBlock = hasApproval
    ? design.approvalFrag.replace(/\{\{flashEta\}\}/g, String(Math.round(s.approval?.countdown ?? 10)))
    : '';

  const questionBlock = q
    ? design.questionFrag
        .replace(/\{\{optionLeft\}\}/g, esc(truncate(q.options?.[0] ?? '', 128, 28)))
        .replace(/\{\{optionRight\}\}/g, esc(truncate(q.options?.[1] ?? '', 128, 28)))
        .replace(/\{\{selectedLeft\}\}/g, q.selected?.includes(0) ? '已选' : '')
        .replace(/\{\{selectedRight\}\}/g, q.selected?.includes(1) ? '已选' : '')
        .replace(/\{\{selectionCount\}\}/g, esc(q.countText ?? ''))
        .replace(/\{\{questionPage\}\}/g, esc(q.pageText ?? '1 / 1'))
    : '';

  const flashing = Number.isFinite(s.flashPct) && s.flashPct >= 0;

  const subs = {
    time: esc(s.time ?? '--:--'),
    batteryLabel: Number.isFinite(s.battery) ? `${Math.round(s.battery)}%` : '—',
    state: esc(s.session?.state ?? 'IDLE'),
    stateColor,
    title: esc(title1),
    title2: esc(title2),
    title2Style: title2 ? '' : 'display:none',
    tool: esc(truncate(s.session?.tool || '—', 128, 12)),
    elapsed: esc(s.session?.elapsed ?? '00:00'),
    stepsText: esc(stepsText),
    ctx: Number.isFinite(s.session?.ctx) ? String(Math.round(s.session.ctx)) : '—',
    tok: fmtTok(s.session?.tokens),
    cmd: esc(truncate(s.approval?.cmd ?? s.approval?.reason ?? '', 288, 12)),
    cmdStyle: hasApproval ? '' : 'display:none',
    detail1: esc(s.detail1 ?? (hasApproval ? '写入远程 · 不可撤销' : '')),
    detail2: esc(s.detail2 ?? (hasApproval ? '需要人工确认' : '')),
    detailStyle: hasApproval ? '' : 'display:none',
    approvalBlock,
    approvalStyle: hasApproval ? '' : 'display:none',
    questionBlock,
    questionStyle: q ? '' : 'display:none',
    retryStyle: 'display:none',
    flash: flashing ? String(Math.round(s.flashPct)) : '0',
    flashStyle: flashing ? '' : 'display:none',
    flashEta: String(Math.round(s.flashEta ?? 0)),
    balance: esc(s.balance ?? ''),
    balanceStyle: s.balance ? '' : 'display:none',
    footer: esc(s.footer ?? ''),
    tapeStyle: hasApproval || q || flashing ? 'display:none' : '',
    linesStyle: hasApproval || q ? 'display:none' : '',
    // 旧 ABI：只写进 metadata，不参与绘制
    battery: Number.isFinite(s.battery) ? String(Math.round(s.battery)) : '0',
    stateBg: c.bg.hex,
    stateLine: c.rule.hex,
    stepOn: String(stepsOn),
    stepSegs: '',
    age: s.session?.ageSec != null ? `${Math.round(s.session.ageSec)}s` : '',
    ctxDash: '0 131.947',
    ctxStyle: 'display:none',
    steps: String(stepsOn),
    stepsTotal: total ? String(total) : '',
  };

  let svg = design.tpl.replace(/\{\{(\w+)\}\}/g, (_, k) => (k in subs ? subs[k] : ''));
  svg = svg.replace(/<!--[\s\S]*?-->/g, '');
  return svg;
}


/**
 * 状态 → 整屏 SVG。自动识别 v1 / v2 模板、v3 圆盘和 v4 三环布局。
 * @param {object} s 面板状态（见 README / 57 §4）
 * @param {ReturnType<loadDesign>} design
 */
export function buildPanelSvg(s, design) {
  if (design.tokens.layout === 'monitor-v4') return buildMonitor(s, design.tokens);
  if (design.tokens.layout === 'orbit-v3') return buildOrbit(s, design.tokens);
  if (design.tpl.includes('{{batteryLabel}}')) return buildV2(s, design);
  return buildV1(s, design);
}

function buildV1(s, design) {
  const tokens = design.tokens;
  const c = tokens.color;
  const st = stateStyle(c, s.session?.state);

  const ctx = Number.isFinite(s.session?.ctx) ? Math.max(0, Math.min(100, Math.round(s.session.ctx))) : 0;
  const C = 131.947; // 2πr, r=21
  const on = (C * ctx) / 100;

  const steps = Math.max(0, Math.min(12, s.session?.steps ?? 0));
  const hasBattery = Number.isFinite(s.battery);
  const hasBalance = !!s.balance;
  const hasApproval = !!s.approval;
  const flashing = Number.isFinite(s.flashPct) && s.flashPct >= 0;

  const subs = {
    time: esc(s.time ?? '--:--'),
    battery: hasBattery ? String(Math.round(s.battery)) : '0',
    state: esc(st.label),
    stateColor: st.color,
    stateBg: st.bg,
    stateLine: st.line,
    title: esc(truncate(s.session?.title ?? '', 288, 24)),
    tool: esc(truncate(s.session?.tool || '—', 150, 18)),
    steps: String(steps),
    stepsTotal: '12',
    stepOn: String(steps),
    stepSegs: stepSegsSvg(steps, 12, c.accent.hex, c.line.hex),
    tok: fmtTok(s.session?.tokens),
    age: s.session?.ageSec != null ? `${Math.max(0, Math.round(s.session.ageSec))}s AGO` : '—',
    ctx: String(ctx),
    ctxDash: `${on.toFixed(2)} ${(C - on).toFixed(2)}`,
    elapsed: s.session?.elapsed ?? '00:00',
    balance: esc(s.balance ?? ''),
    balanceStyle: hasBalance ? '' : 'display:none',
    footer: esc(s.footer ?? 'STREAM OK'),
    // 审批态：把 tool/meta 两行让位给命令预览块（设计稿 152-162 行）
    linesStyle: hasApproval ? 'display:none' : '',
    cmdStyle: hasApproval ? '' : 'display:none',
    cmd: esc(truncate(s.approval?.cmd ?? s.approval?.reason ?? '', 288, 14)),
    tapeStyle: hasApproval || flashing ? 'display:none' : '',
    approvalStyle: hasApproval ? '' : 'display:none',
    approvalBlock: hasApproval ? design.approvalFrag : '',
    flashStyle: flashing ? '' : 'display:none',
    flash: flashing ? String(Math.round(s.flashPct)) : '0',
    flashEta: flashing ? String(Math.round(s.flashEta ?? 0)) : '0',
  };

  let svg = design.tpl.replace(/\{\{(\w+)\}\}/g, (_, k) => (k in subs ? subs[k] : ''));
  // 设计稿里的注释是给人看的，XML 不允许注释里出现 '--'，渲染前整段去掉
  svg = svg.replace(/<!--[\s\S]*?-->/g, '');
  if (!hasBattery) svg = svg.replace('<g id="battery">', '<g id="battery" style="display:none">');
  return svg;
}
