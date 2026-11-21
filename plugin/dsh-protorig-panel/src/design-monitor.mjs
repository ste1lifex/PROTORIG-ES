import { xml, lines, roundedPercent, approvalView, questionView } from './design-orbit.mjs';
import { dshFishPath } from './assets/dsh-fish.mjs';

const monitorWhite = '#F0F0F0';
const monitorGray = '#A8A8A8';
const monitorYellow = '#FFFA00';
const monitorBlue = '#8FB8FF';

// The same order, radii, sweep and vendor colors as Desktop's pc-rings-5.
function monitorChannels(metrics) {
  const vendor = `${metrics.gpuVendor ?? ''} ${metrics.gpuName ?? ''}`.toLowerCase();
  const gpuColor = /nvidia|geforce|quadro/.test(vendor) ? '#76B900'
    : /\bamd\b|radeon|advanced micro devices/.test(vendor) ? '#ED1C24'
      : /intel/.test(vendor) ? '#0091FF' : monitorBlue;
  return [
    ['CPU', 228, monitorYellow, metrics.cpuPercent],
    ['GPU', 218, gpuColor, metrics.gpuPercent],
    ['RAM', 208, monitorWhite, metrics.memoryPercent],
  ].map(([name, radius, color, value]) => ({
    name, radius, color, percent: Number.isFinite(value) && value >= 0 ? Math.min(100, value) : null,
  }));
}

function monitorArc(radius) {
  const point = (angle) => {
    const radians = angle * Math.PI / 180;
    return `${(240 + radius * Math.sin(radians)).toFixed(3)} ${(240 - radius * Math.cos(radians)).toFixed(3)}`;
  };
  return `M${point(210)} A${radius} ${radius} 0 1 1 ${point(510)}`;
}

function monitorText(value, x, y, size, color = monitorWhite, weight = 400, align = 'middle') {
  return `<text x="${x}" y="${y}" fill="${color}" font-size="${size}" font-weight="${weight}" text-anchor="${align}">${xml(value)}</text>`;
}

function monitorRings(channels) {
  return channels.map(({ name, radius, color, percent }) => {
    const length = radius * Math.PI * 300 / 180;
    const dash = percent == null ? [0.6, 2] : [0.18, 1.4866667];
    const track = dash.map((value) => (value * length / 100).toFixed(2)).join(' ');
    const arc = monitorArc(radius);
    // Keep the path fixed so load changes only affect its visible endpoint.
    const progress = percent < 100 ? ` stroke-dasharray="${(percent * length / 100).toFixed(3)} ${length.toFixed(3)}"` : '';
    return `<g data-channel="${name}" fill="none">
      <path d="${arc}" stroke="#4C4C4C" stroke-width="3" stroke-dasharray="${track}" opacity="${percent == null ? 0.6 : 1}"/>
      ${percent > 0 ? `<path d="${arc}" stroke="${color}" stroke-width="6"${progress}/>` : ''}
    </g>`;
  }).join('');
}

function monitorRate(value) {
  if (!Number.isFinite(value) || value < 0) return '--';
  const units = ['B/s', 'KB/s', 'MB/s', 'GB/s'];
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return `${value >= 100 ? Math.round(value) : value.toFixed(1)} ${units[unit]}`;
}

const monitorReading = (percent) => percent == null ? '--' : `${percent.toFixed(1)}%`;

function monitorReadings(channels, metrics, compact) {
  if (compact) {
    return channels.map(({ name, percent, color }, index) =>
      monitorText(`${name} ${monitorReading(percent)}`, 125 + index * 115, 149, 17, color, 700)).join('');
  }
  const [cpu, gpu, ram] = channels;
  const vram = Number.isFinite(metrics.gpuMemoryPercent) && metrics.gpuMemoryPercent >= 0
    ? Math.min(100, metrics.gpuMemoryPercent) : null;
  const value = ({ percent, color }, x, y, size) => {
    if (percent == null) return monitorText('--', x, y, size, monitorGray, 700);
    return `<text x="${x}" y="${y}" text-anchor="middle" font-size="${size}" font-weight="700" fill="${color}">${percent.toFixed(1)}<tspan font-size="${size > 30 ? 20 : 16}" dx="5">%</tspan></text>`;
  };
  return `${monitorText('CPU', 157, 156, 16, cpu.color)}
    ${value(cpu, 157, 206, 48)}
    ${monitorText('GPU', 322, 144, 14, gpu.color)}
    ${value(gpu, 322, 169, 28)}
    ${monitorText(`VRAM ${monitorReading(vram)}`, 322, 186, 14, gpu.color)}
    ${monitorText('RAM', 322, 201, 14, ram.color)}
    ${value(ram, 322, 230, 28)}
    ${monitorText(`RX  ${monitorRate(metrics.networkRxBps)}`, 157, 248, 16, monitorGray)}
    ${monitorText(`TX  ${monitorRate(metrics.networkTxBps)}`, 322, 248, 16, monitorGray)}`;
}

function monitorButton(name, value, [x, y, w, h], { enabled = true, accent = false, hint = '' } = {}) {
  const active = enabled && accent;
  const ink = active ? monitorYellow : enabled ? monitorWhite : '#707070';
  const middle = x + w / 2;
  const iconY = y + (name === 'option' ? 14 : 20);
  const iconInk = enabled && name === 'option' ? monitorYellow : ink;
  const icons = {
    deny: `M${middle - 5} ${iconY - 5}l10 10m0-10-10 10`,
    allow: `M${middle - 8} ${iconY}h16m-6-6 6 6-6 6`,
    next: `M${middle - 7} ${iconY - 5}l5 5-5 5m8-10 5 5-5 5`,
    submit: `M${middle - 6} ${iconY}l4 5 9-10`,
    option: active ? `M${middle - 6} ${iconY}l4 5 9-10` : `M${middle} ${iconY - 6}l6 6-6 6-6-6Z`,
  };
  return `<g${enabled ? ` data-action="${name}"` : ''}>
    <path d="${icons[name]}" fill="none" stroke="${iconInk}" stroke-width="2"/>
    ${monitorText(value, middle, y + (hint ? 50 : 58), 24, ink, 700)}
    ${hint ? monitorText(hint, middle, y + 74, 16, enabled ? monitorGray : '#707070') : ''}
    <path d="M${middle - 24} ${y + h - 9}h48" stroke="${active ? monitorYellow : enabled ? '#757575' : '#333'}" stroke-width="2"/>
  </g>`;
}

function monitorCalendar(value) {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(value ?? '')) return '';
  const day = new Date(`${value}T00:00:00Z`);
  if (!Number.isFinite(day.getTime()) || day.toISOString().slice(0, 10) !== value) return '';
  return `${monitorText(value.slice(5).replace('-', '/'), 310, 84, 18)}
    ${monitorText(`星期${'日一二三四五六'[day.getUTCDay()]}`, 310, 111, 17, monitorGray)}`;
}

function monitorFooter(s) {
  const battery = roundedPercent(s.battery);
  const balance = lines(s.balance, 90, 17, 1).rows[0];
  const power = battery == null ? '' : `电量 ${battery}%`;
  return monitorText([power, balance].filter(Boolean).join('  ·  '), 240, 416, 17, monitorGray);
}

function monitorFocus(s) {
  const session = s.session ?? {};
  const states = {
    RUNNING: ['DSH 运行中', monitorWhite],
    NEEDS: ['DSH 等待操作', monitorYellow],
    DONE: ['DSH 本轮完成', monitorBlue],
    FAILED: ['DSH 运行失败', '#FF6B6B'],
  };
  const state = Object.hasOwn(states, session.state) ? states[session.state] : null;
  if (!state) return `<svg x="117" y="281" width="42" height="31" viewBox="0 0 23.16 17.04">
      <path d="${dshFishPath}" fill="${monitorWhite}"/>
    </svg>
    <text x="171" y="308" fill="${monitorWhite}" font-family="Microsoft YaHei, Source Han Sans SC, sans-serif" font-size="32" font-weight="500">探索未至之境</text>
    ${monitorText('DSH 等待任务', 240, 363, 23, monitorGray, 700)}${monitorFooter(s)}`;
  const ctx = Number.isFinite(session.ctx) && session.ctx >= 0 ? roundedPercent(session.ctx) : null;
  const barColor = ctx >= 80 ? monitorYellow : monitorBlue;
  const steps = Number.isFinite(session.steps) ? Math.min(99999, Math.max(0, Math.round(session.steps))) : '—';
  const tool = lines(session.tool, 95, 18, 1).rows[0];
  const title = lines(session.title || '未命名会话', 316, 25, 1).rows[0];
  return `${monitorText(state[0], 240, 276, 20, state[1], 700)}
    ${monitorText(title, 240, 310, 25, monitorWhite, 700)}
    ${monitorText([tool, `${steps} 步`].filter(Boolean).join(' · '), 94, 338, 18, monitorGray, 400, 'start')}
    ${monitorText(`CTX ${ctx == null ? '--' : `${ctx}%`}`, 94, 364, 17, monitorGray, 400, 'start')}
    <rect x="180" y="356" width="206" height="6" fill="#333"/>
    ${ctx > 0 ? `<rect x="180" y="356" width="${Math.round(206 * ctx / 100)}" height="6" fill="${barColor}"/>` : ''}
    ${monitorFooter(s)}`;
}

function buildMonitor(s, tokens) {
  const time = /^\d{2}:\d{2}$/.test(s.time ?? '') ? s.time : '--:--';
  const slots = [136, 166, 192, 218, 248];
  const clock = [...time].map((digit, i) => monitorText(digit, slots[i], 112, 48, monitorWhite, 700)).join('')
    + monitorCalendar(s.date);
  const online = s.connection !== 'lost' && s.connection !== 'waiting';
  const flashing = online && Number.isFinite(s.flashPct) && s.flashPct >= 0;
  const metrics = online && !flashing ? s.metrics ?? {} : {};
  const channels = monitorChannels(metrics);
  const approval = online && !flashing ? s.approval : null;
  const question = online && !flashing && !approval ? s.question : null;
  const { deny, allow, option, next, submit } = tokens.hotzones;
  let readings = '';
  let body;
  if (!online) {
    const lost = s.connection === 'lost';
    body = `${monitorText(lost ? '连接已中断' : '等待 USB 连接', 240, 257, 29, lost ? '#FF6B6B' : monitorWhite, 700)}
      ${monitorText(lost ? '检查 USB 后重新进入' : '连接后从表盘右滑进入', 240, 299, 20, monitorGray)}`;
  } else if (flashing) {
    const progress = roundedPercent(s.flashPct);
    body = `${monitorText('固件写入中', 240, 213, 25, monitorYellow, 700)}
      ${monitorText(`${progress}%`, 240, 288, 54, monitorWhite, 700)}
      <rect x="112" y="313" width="256" height="6" fill="#333"/>
      <rect x="112" y="313" width="${Math.round(256 * progress / 100)}" height="6" fill="${monitorYellow}"/>
      ${monitorText('请保持 USB 连接', 240, 354, 22, monitorGray)}`;
  } else {
    readings = monitorReadings(channels, metrics, !!(approval || question));
    if (approval) {
      const info = approvalView(approval);
      body = `${monitorText(info.computer ? '电脑待确认' : 'DSH 审批待确认', 240, 192, 25, monitorYellow, 700)}
        ${monitorText(`${info.tool} 请求执行`, 240, 229, 21, monitorWhite, 700)}
        ${monitorText(info.command, 240, 263, 22, monitorGray)}
        ${monitorButton('deny', '拒绝', deny, { enabled: !info.computer })}
        ${monitorButton('allow', '允许一次', allow, { enabled: info.canAllow, accent: true })}
        ${monitorText(info.note, 240, 419, 17, monitorGray)}`;
    } else if (question) {
      const info = questionView(question);
      body = `${monitorText(`DSH 问答 · ${question.multiSelect ? '多选' : '单选'}`, 240, 177, 17, monitorYellow, 700)}
        ${monitorText(info.prompt, 240, 207, 25, monitorWhite, 700)}
        ${monitorButton('option', info.choice, option, { enabled: info.canChoose, accent: info.chosen, hint: info.hint })}
        ${monitorButton('next', '下一项', next, { enabled: info.more })}
        ${monitorButton('submit', '提交', submit, { enabled: info.canSubmit, accent: true, hint: `已选 ${info.count}` })}`;
    } else {
      body = monitorFocus(s);
    }
  }
  return `<svg xmlns="http://www.w3.org/2000/svg" width="480" height="480" viewBox="0 0 480 480" font-family="Source Han Sans SC, sans-serif">
    <rect width="480" height="480" fill="#000"/>
    ${monitorRings(channels)}${clock}${readings}${body}
  </svg>`;
}

export { buildMonitor };
