#!/usr/bin/env node
/*
 * cli.mjs —— 不上 DSH 也能跑的手表联调工具
 *
 *   node tools/cli.mjs hello            握手（可选 --enter 先发 pcmode 1）
 *   node tools/cli.mjs sync             插线对时（settime）
 *   node tools/cli.mjs bands            整屏渐变 → 走 16 行带，打印真实带宽
 *   node tools/cli.mjs panel [--seconds] 跑占位面板（每秒刷新时间）
 *   node tools/cli.mjs touch [--seconds] 监听触摸帧
 *   node tools/cli.mjs testcard [--enter] 几何标定卡（核对触摸坐标）
 *   node tools/cli.mjs bright --level N [--enter]  0x02 亮度（用 0x91 自证并恢复原值）
 *   node tools/cli.mjs vibrate [--ms N] [--enter]  0x03 震动（无回执，需人确认震感）
 *   node tools/cli.mjs flash --bin <xxx.bin> [--rom COMx]  手工烧录（Windows 后端）
 *
 * 通用参数：--port COM8 --enter --keep --seconds N --debug
 */
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { WatchLink } from '../src/link.mjs';
import { FrameBuffer, BandFlusher } from '../src/framebuffer.mjs';
import { PanelRenderer } from '../src/renderer.mjs';
import { MonitorCanvasRenderer } from '../src/render-monitor-canvas.mjs';
import { loadDesign, buildPanelSvg } from '../src/design.mjs';
import { rgb565, bright, vibrate } from '../src/protocol.mjs';

const argv = process.argv.slice(2);
const here = path.dirname(fileURLToPath(import.meta.url));
/**
 * `panel` 默认用**包里那份 monitor-v4 设计稿**，和 `src/panel-service.mjs` 的
 * `PACKAGED_DESIGN` 是同一份。以前默认落到 `src/design.mjs` 的 `local/design/dsh_panel`
 * （旧 v1 SVG 模板），台架上看到的是早就不用的版式，还容易误判成"插件渲错了"；
 * 而且 `local/` 不随 npm 包发布，装到仓库外直接 ENOENT。
 */
const DEFAULT_PANEL_DESIGN = path.resolve(here, '../design/dsh_panel_v4');
const cmd = argv[0] ?? 'hello';
const opt = {
  port: flag('--port'),
  enter: argv.includes('--enter'),
  keep: argv.includes('--keep'),
  debug: argv.includes('--debug'),
  seconds: Number(flag('--seconds') ?? 0),
  stats: argv.includes('--stats'),
  fps: Number(flag('--fps') ?? 1),
  design: flag('--design'),
};

function flag(name) {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : undefined;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function waitFor(emitter, event, ms = 3000) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => {
      emitter.off(event, on);
      reject(new Error(`等 ${event} 超时（${ms} ms）`));
    }, ms);
    const on = (v) => {
      clearTimeout(t);
      emitter.off(event, on);
      resolve(v);
    };
    emitter.on(event, on);
  });
}

/** 等链路状态到位（按状态轮询，避免事件竞态）。 */
async function waitState(link, key, want, ms = 3000) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (link.state[key] === want) return true;
    await sleep(40);
  }
  return false;
}

function attachLogging(link) {
  link.rxTail = [];
  link.on('line', (e) => { link.rxTail.push(e.text); if (link.rxTail.length > 40) link.rxTail.shift(); });
  link.on('open', (e) => console.log(`[link] open ${e.path}`));
  link.on('close', (e) => console.log(`[link] close (${e.reason})`));
  link.on('error', (e) => console.error('[link] error', e.message));
  link.on('pc', (e) => console.log(`[watch] @PCM1 PC ${e.value}`));
  link.on('mode', (e) => console.log(`[watch] @PCM1 MODE ${e.value}`));
  link.on('hello', (e) =>
    console.log(`[watch] HELLO ver=${e.ver} maxW=${e.maxW} maxH=${e.maxH} fmt=${e.fmt}`),
  );
  if (opt.debug) link.on('line', (e) => console.log(`[raw] ${e.text}`));
}

/** 打开链路，已进入电脑模式时握手；--enter 可在台架主动进入。 */
async function connect({ enter = opt.enter, hello = true } = {}) {
  const link = new WatchLink({ debug: opt.debug });
  attachLogging(link);
  await link.open(opt.port);
  console.log(`[link] port ${link.path} @115200`);

  // 插线即对时（research/57 §6bis）
  const cmdline = await link.syncTime();
  console.log(`[time] ${cmdline}`);
  await sleep(150);

  // PC 1 / MODE 1 只在跳变时广播，重连后必须主动问一次
  const st = await link.queryState();
  if (st) {
    console.log(
      `[watch] active=${st.active} transport=${st.transport} passthrough=${st.passthrough} usbraw=${st.usbraw}`,
    );
  }

  if (enter && !link.state.pcMode) {
    await link.shell('pcmode 1');
    if (!(await waitState(link, 'pcMode', true, 3000))) {
      console.log('[warn] 没等到 @PCM1 PC 1（手表可能没被主机配置，或不在可进入状态）');
    }
  }
  if (hello && (enter || link.state.pcMode) && !link.state.passthrough) {
    // 文本命令和二进制帧之间留一点空隙：两者若被合成一个 USB 包，固件按包首字节
    // 判定成"二进制帧"，整包交给状态机；状态机在 ST_SYNC0 遇到非 0xA5 的文本字节
    // 会静默丢弃 —— 命令就没了（不是"整包交给 shell"）。
    await sleep(200);
    // 实测：紧接着 enter 发的第一帧 HELLO 有时不触发透传，重发几次就稳了
    for (let attempt = 1; attempt <= 3 && !link.state.passthrough; attempt++) {
      await link.sayHello();
      if (await waitState(link, 'passthrough', true, 1200)) break;
      console.log(`[warn] HELLO 第 ${attempt} 次没打开透传，重发`);
      await sleep(400);
    }
    if (!link.state.passthrough) {
      console.log('[warn] 没等到 @PCM1 MODE 1（透传没打开）');
    }
  }
  console.log(
    `[state] pcMode=${link.state.pcMode} passthrough=${link.state.passthrough}` +
      (link.state.hello
        ? ` hello=${link.state.hello.maxW}x${link.state.hello.maxH} fmt=${link.state.hello.fmt}`
        : ''),
  );
  return link;
}

async function leave(link) {
  // --keep 只表示"不退电脑模式"，串口必须关（否则进程挂着、COM 口不释放）
  if (!opt.keep && link.state.pcMode) {
    await link.shell('pcmode 0').catch(() => {});
    await sleep(250); // 别让 close 抢在命令前面，否则手表停在电脑模式
  }
  await link.close().catch(() => {});
}

/* ------------------------------------------------------------------ 命令 */

async function cmdHello() {
  const link = await connect();
  console.log('[state]', JSON.stringify(link.state));
  if (!link.state.pcMode) {
    console.log('提示：加 --enter 才会先发 pcmode 1 进电脑模式');
  }
  await link.requestStatus().catch(() => {});
  await sleep(400);
  await leave(link);
}

async function cmdSync() {
  const link = new WatchLink();
  attachLogging(link);
  await link.open(opt.port);
  const line = await link.syncTime();
  console.log(`[time] sent: ${line}`);
  await sleep(200);
  await link.close();
}

async function cmdBands() {
  const link = await connect();
  if (!link.state.passthrough) throw new Error('还没进透传（MODE 1），无法发像素');
  const fb = new FrameBuffer();
  // 30 条带各一个颜色，肉眼能数出有没有丢带
  for (let b = 0; b < fb.bandCount; b++) {
    const hue = (b / fb.bandCount) * 360;
    const { r, g, bl } = hslToRgb(hue / 360, 0.75, 0.5);
    fb.fillRect(0, b * fb.bandH, fb.w, fb.bandH, rgb565(r, g, bl));
  }
  const flusher = new BandFlusher(link, fb);
  const stat = await flusher.flush({ force: true });
  console.log(
    `[flush] bands=${stat.bands} frames=${stat.frames} bytes=${stat.bytes} ${stat.ms.toFixed(1)} ms  ` +
      `≈ ${((stat.bytes / 1024) / (stat.ms / 1000)).toFixed(0)} KB/s`,
  );
  console.log('屏幕上应该是 30 条彩虹带（每带 16 行）；缺一条就是丢了。');
  await sleep(1500);
  await leave(link);
}

async function cmdPanel() {
  const design = loadDesign(opt.design ?? DEFAULT_PANEL_DESIGN);
  const monitor = design.tokens.layout === 'monitor-v4';
  let metrics;
  if (monitor) {
    // 指标采集早就内联进包（src/metrics/），别再引仓库外的 desktop/（已移到 local/desktop，不随包发布）
    const { MetricsCollector } = (await import('../src/metrics/metrics.cjs')).default;
    metrics = new MetricsCollector();
  }
  const link = await connect();
  if (!link.state.passthrough) throw new Error('还没进透传（MODE 1），无法发像素');
  // monitor-v4 走 Skia（2ms/帧）；其它设计仍走 resvg（SVG 模板）
  const renderer = monitor
    ? await new MonitorCanvasRenderer().init()
    : await new PanelRenderer().init();
  const fb = new FrameBuffer();
  const flusher = new BandFlusher(link, fb);

  // 演示状态（真接线后由 DSH session/event 填充）
  const state = {
    time: hhmm(new Date()),
    battery: null,
    balance: '¥12.34',
    footer: 'STREAM OK · 1.0MB/s',
    flashPct: -1,
    approval: argv.includes('--needs') ? { toolName: 'bash', cmd: monitor ? 'git status --short' : undefined } : null,
    session: {
      title: '实现手表端副屏的圆形布置',
      state: argv.includes('--needs') ? 'NEEDS' : 'RUNNING',
      tool: 'bash',
      steps: 7,
      ctx: 33,
      tokens: 47200,
      ageSec: 2,
      elapsed: '01:23',
    },
  };

  link.on('status', (st) => {
    state.battery = st.battery;
  });
  if (link.state.status) state.battery = link.state.status.battery;

  const t0 = Date.now();
  let n = 0;
  let firstFrame = true;
  while (true) {
    const now = new Date();
    state.time = hhmm(now);
    if (monitor) {
      state.date = [now.getFullYear(), String(now.getMonth() + 1).padStart(2, '0'), String(now.getDate()).padStart(2, '0')].join('-');
    }
    state.session.ageSec = Math.round((Date.now() - t0) / 1000);
    if (metrics) state.metrics = await metrics.sample();
    const img = monitor ? renderer.draw(state, design.tokens) : renderer.render(buildPanelSvg(state, design));
    fb.setFromRGBA(img.rgba);
    const s = await flusher.flush({ force: firstFrame });
    if (firstFrame) {
      console.log(`[flush] 整屏 bands=${s.bands} frames=${s.frames} bytes=${s.bytes} ${s.ms.toFixed(1)} ms`);
      firstFrame = false;
    } else if (s.bands > 0) {
      console.log(`[t=${((Date.now() - t0) / 1000).toFixed(0)}s] dirty=${s.bands} bytes=${s.bytes} ${s.ms.toFixed(1)} ms`);
    }
    n++;
    // 每帧把固件的接收计数打出来：bad/ring_drop 一涨就说明瓦片被截断
    if (opt.stats) {
      await link.shell('pcmode');
      await sleep(180);
      const line = (link.tail ?? []).slice(-6).reverse().find((t) => t.includes('[pc] active='));
      if (line) console.log('[watch]', line.replace(/^\s+/, ''));
    }
    await sleep(Math.max(20, Math.round(1000 / opt.fps)));
    if (opt.seconds && (Date.now() - t0) / 1000 >= opt.seconds) break;
  }
  await leave(link);
}


/**
 * 标定卡：不用字体，只用几何，肉眼就能读出"错位了多少"。
 *   · 外圈 r=220 细环（贴安全圆）
 *   · 正中十字 ±14
 *   · 四个径向刻度（上/右/下/左，r=190..205）
 *   · 四个不对称角标：左上 24×24 实心、右上 12×12、左下 8×8、右下 24×12 横条
 *   · 顶部竖线 x=240（y=20..80）、左侧横线 y=240（x=20..80）
 */
async function cmdTestCard() {
  const link = await connect();
  if (!link.state.passthrough) throw new Error('还没进透传（MODE 1），无法发像素');
  const fb = new FrameBuffer();
  const W = 0xffff;
  const Y = 0xffe0;
  const R = 0xf800;
  const C = 0x07ff;
  fb.fill(0x0000);
  const cx = 240, cy = 240;
  // 外圈
  for (let a = 0; a < 360; a += 1) {
    const r = (a * Math.PI) / 180;
    const x = Math.round(cx + 220 * Math.sin(r));
    const y = Math.round(cy - 220 * Math.cos(r));
    fb.fillRect(x - 1, y - 1, 3, 3, W);
  }
  // 正交刻度
  for (const [dx, dy] of [[0, -1], [1, 0], [0, 1], [-1, 0]]) {
    for (let rr = 190; rr <= 205; rr++) {
      const x = cx + dx * rr, y = cy + dy * rr;
      fb.fillRect(x - 1, y - 1, 3, 3, Y);
    }
  }
  // 中心十字
  fb.fillRect(cx - 14, cy - 1, 29, 3, W);
  fb.fillRect(cx - 1, cy - 14, 3, 29, W);
  // 不对称角标（离中心 ~135px）
  fb.fillRect(cx - 135 - 12, cy - 135 - 12, 24, 24, R);   // 左上大块
  fb.fillRect(cx + 135 - 6, cy - 135 - 6, 12, 12, C);     // 右上小块
  fb.fillRect(cx - 135 - 4, cy + 135 - 4, 8, 8, Y);       // 左下最小
  fb.fillRect(cx + 135 - 12, cy + 135 - 6, 24, 12, W);    // 右下横条
  // 顶部竖线 + 左侧横线（L 参考）
  fb.fillRect(cx - 1, 20, 3, 60, W);
  fb.fillRect(20, cy - 1, 60, 3, W);

  const flusher = new BandFlusher(link, fb);
  const stat = await flusher.flush({ force: true });
  console.log(`[testcard] bands=${stat.bands} bytes=${stat.bytes} ${stat.ms.toFixed(0)} ms`);
  console.log('看这几点：中心十字是否正中 / 四个黄刻度是否等距 / 左上红块 24×24 是否在左上 / 顶部白竖线是否在正中');
  await sleep(opt.seconds ? opt.seconds * 1000 : 20000);
  await leave(link);
}

/**
 * 亮度执行器 0x02：设置后固件会强制常亮，并把实际亮度放进 0x91 状态帧的 bright 字段。
 * 所以这里能自我验证：发 200 → 下一帧 0x91 应报 bright=200 → 再发回原值。
 */
async function cmdBright() {
  const level = Math.max(32, Math.min(255, Number(flag('--level') ?? 200)));
  const link = await connect();
  // 0x02 设完即生效，固件每 1 s 把实际亮度放进 0x91 的 bright 字段：可以自证
  await link.requestStatus().catch(() => {});
  await sleep(400);
  const before = link.state.status?.bright ?? null;
  await link.write(bright(level));
  let after = null;
  for (let i = 0; i < 30; i += 1) {
    await sleep(100);
    after = link.state.status?.bright ?? null;
    if (after === level) break;
  }
  console.log(`[bright] 发送 0x02 level=${level}；0x91 回报：前=${before} 后=${after}`);
  if (after === level) console.log('[bright] PASS：固件 0x02 生效（状态帧自证）');
  else console.log(`[bright] FAIL：2 秒内 0x91 没回到 ${level}（检查是否在电脑模式）`);
  if (Number.isFinite(before) && before !== level) {
    await link.write(bright(before));
    await sleep(1400);
    console.log(`[bright] 已恢复原亮度 ${before}；0x91 回报=${link.state.status?.bright ?? '?'}`);
  }
  await leave(link);
}

/** 震动执行器 0x03：无回执，只能确认帧发出；是否有震感需要人看/摸。 */
async function cmdVibrate() {
  const ms = Math.max(20, Math.min(2000, Number(flag('--ms') ?? 300)));
  const link = await connect();
  await link.write(vibrate(ms));
  console.log(`[vibrate] 已发送 0x03 ms=${ms}（70% 占空，上限 2000ms）；请确认表是否有震感`);
  await sleep(800);
  await leave(link);
}


/**
 * 手工烧录（不经过 DSH 插件）：走加固后的 FlashService（detached 子进程 + 锁）。
 *   node tools/cli.mjs flash --bin ../../firmware/lvgl_test/build/build_out/protorig_m2_bl616.bin
 *   node tools/cli.mjs flash --bin xxx.bin --rom COM5   # 板子已在 ROM 模式
 */
async function cmdFlash() {
  const bin = flag('--bin');
  const rom = flag('--rom');
  if (!bin) throw new Error('用法：cli.mjs flash --bin <xxx.bin> [--rom COMx]');
  const { FlashService } = await import('../src/flash.mjs');
  const link = new WatchLink({ debug: opt.debug });
  await link.open().catch(() => {});
  const panel = {
    stateForApi: () => ({ connected: link.state.connected }),
    suspendForFlash: async () => {
      const port = link.path;
      await link.close().catch(() => {});
      await sleep(300);
      return port;
    },
    resumeAfterFlash: () => {},
  };
  const logger = {
    info: (m) => console.log(`[flash] ${m}`),
    warn: (m) => console.log(`[flash] ${m}`),
    error: (m) => console.log(`[flash] ${m}`),
    debug: () => {},
  };
  const service = new FlashService({ panel, logger });
  const started = rom ? await service.recover(bin, { port: rom }) : await service.start(bin);
  if (!started.ok) {
    console.error(`[flash] 启动失败：${started.error}`);
    process.exitCode = 1;
    return;
  }
  console.log(`[flash] 任务 ${started.jobId} · ${started.preflight.sha8} · ${started.preflight.size} B`);
  const view = await new Promise((resolve) => {
    const timer = setInterval(() => {
      const v = service.view();
      process.stdout.write(`  stage=${v.stage} ${v.seconds}s\r`);
      if (!v.running) {
        clearInterval(timer);
        resolve(v);
      }
    }, 1000);
  });
  console.log('');
  console.log(`[flash] ${view.ok ? 'OK' : 'FAILED'} · ${view.stage} · ${view.seconds}s${view.error ? ` · ${view.error}` : ''}`);
  if (view.logTail) console.log(view.logTail.split('\n').slice(-6).join('\n'));
  if (view.ok) console.log('[flash] 请手动拨动开关重启：SW2 ON→OFF→ON（拔 USB 不会断开电池供电）');
}

async function cmdTouch() {
  const link = await connect({ enter: true });
  console.log('[touch] 摸屏幕（--seconds 控制时长，默认 20s）');
  link.on('touch', (t) => console.log(`[touch] ${t.pressed ? 'DOWN' : 'UP'} (${t.x},${t.y})`));
  await sleep((opt.seconds || 20) * 1000);
  await leave(link);
}

function hhmm(d) {
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}

function hslToRgb(h, s, l) {
  const f = (n) => {
    const k = (n + h * 12) % 12;
    const a = s * Math.min(l, 1 - l);
    return Math.round(255 * (l - a * Math.max(-1, Math.min(k - 3, Math.min(9 - k, 1)))));
  };
  return { r: f(0), g: f(8), bl: f(4) };
}

/* ------------------------------------------------------------------ 入口 */

const table = { hello: cmdHello, sync: cmdSync, bands: cmdBands, panel: cmdPanel, touch: cmdTouch, testcard: cmdTestCard, bright: cmdBright, vibrate: cmdVibrate, flash: cmdFlash };
const fn = table[cmd];
if (!fn) {
  console.log(`未知命令 ${cmd}\n可用：${Object.keys(table).join(' / ')}`);
  process.exit(2);
}
fn().catch((e) => {
  console.error('[fatal]', e.message);
  process.exit(1);
});
