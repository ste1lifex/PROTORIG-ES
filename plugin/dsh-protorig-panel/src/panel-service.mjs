/*
 * panel-service.mjs —— 手表面板运行时（链路 / 渲染 / 刷新 / 提醒 / 交互）
 *
 * 职责边界：
 *   · 这里只管"手表这一端"：串口链路、480×480 影子帧、脏块刷新、触摸、
 *     亮度/震动执行器、BLE 降级；
 *   · DSH 事件语义（session/event、审批、问答）在 index.mjs / approval.mjs /
 *     questions.mjs 里接线，通过本类的 onSessionEvent / requestApproval /
 *     requestQuestion 落屏。
 *
 * 关键约束（research/56 §9）：
 *   · 只有收到 MODE 1 才发像素；PC 0 / MODE 0 立即停发；
 *   · 手表没有帧缓存 → 每次重连/重进电脑模式必须整屏重发（fb.invalidate）；
 *   · 一条带拼成一次 write（≈15.4 KB），背压交给固件 500 ms 等待；
 *   · 文本命令和二进制帧不要同包发送（两者之间留 >150 ms）。
 */
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { WatchLink, probePortAdminId } from './link.mjs';
import { DeviceStore } from './device-store.mjs';
import { FrameBuffer, BandFlusher } from './framebuffer.mjs';
import { MonitorCanvasRenderer } from './render-monitor-canvas.mjs';
import { loadDesign } from './design.mjs';
import {
  ACK,
  bright,
  cardApproval,
  cardBitmapChunks,
  cardBitmapClear,
  cardQuestion,
  cardSession,
  frame,
  vibrate,
} from './protocol.mjs';
import { CardBitmapRenderer } from './card-bitmap.mjs';
import {
  createFocusState,
  projectEvent,
  buildCards,
  isSubagentSession,
  readSessionTitle,
  readSessionContext,
  mergeContext,
} from './focus.mjs';
import { normalizeConfig } from './config.mjs';
import { DATA_DIR, pickRomPort } from './flash.mjs';
import { crc32, parsePfi } from './face-pack.mjs';
import { normalizeFaceId } from './face-ids.mjs';
import {
  PANEL_CMD,
  PANEL_CMD_CLEAR,
  PANEL_CMD_END,
  PANEL_CMD_INFO,
  PANEL_IMAGE_LEN,
  PANEL_UPLOAD_CHUNK,
  panelBeginLine,
  panelChunks,
  panelCrcHex,
  panelStatusMatchesContract,
  parsePanelStatus,
} from './face-panel.mjs';
import { BleBridge } from './ble-bridge.mjs';
import { flashSupported, osKind, osLabel } from './platform.mjs';
import {
  advNameFor,
  describeSelection,
  deviceLabel,
  normalizeAdminId,
  parseAdvName,
  selectCandidate,
} from './device-id.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
/** 审批/问答队列上限：最多同时挂 5 张；等待时间由 config.queueWaitSec（10–60 s）控制。 */
const OVERLAY_QUEUE_MAX = 5;
const OVERLAY_QUEUE_WAIT_MAX_MS = 60000;
/* 设计稿 tokens 的查找顺序（resolveDesignDir）：
 *   1. 配置 `design`（绝对路径，用户显式指定最优先）；
 *   2. 包内 `design/dsh_panel_v4`（npm 包里带一份 tokens 镜像，见该目录 README）；
 *   3. 环境变量 PROTORIG_DESIGN_DIR（指向仓库设计稿，开发机常用）；
 *   4. 开发机可选回退（内部工作区，不随公开仓库发布；插件只读）。
 * 包内副本是权威 tokens 的镜像，`test/device.test.mjs` 有断言防漂移。 */
const PACKAGED_DESIGN = path.resolve(here, '../design/dsh_panel_v4');
const REPO_DESIGN = path.resolve(here, '../../../local/design/dsh_panel_v4');
/* 指标采集已是包内代码（src/metrics/，随 npm 包分发；不再依赖 desktop/ 仓库外部路径） */
const METRICS_PATH = path.resolve(here, './metrics/metrics.cjs');
const FAILURE_KINDS = new Set(['error', 'max-tokens', 'interrupted', 'aborted']);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/**
 * 「烧录中」的落盘标记。
 *
 * BL616 的 ROM 下载口与应用口 VID/PID **完全一样**（349b:6160，厂商都是 Bouffalo），
 * `isWatchPort()` 分辨不出来。烧录把板子打进 ROM 模式后，若宿主此时重启，
 * flashHold（内存态）就丢了，启动扫描会把 ROM 口当手表抓走 —— 下一次烧录必然
 * `Resource busy`，还得再重启宿主。所以这里落一份，重启后接着"只等原应用口"。
 */
function flashHoldFile() {
  const dir = process.env.PROTORIG_DATA_DIR
    || path.join(process.env.LOCALAPPDATA || process.env.USERPROFILE || os.homedir(), 'protorig-panel');
  return path.join(dir, 'flash-hold.json');
}
const FLASH_HOLD_TTL_MS = 30 * 60 * 1000;

/**
 * 像素帧之后、文本命令之前的最小空隙（ms）。
 *
 * 固件 bsp/usb_pc_link.c 的 protorig_usb_pc_rx() 按"包"判定这一包是帧还是 shell 文本：
 * 包首不是 0xA5 才交回 shell，否则整包喂给帧状态机，而 feed_byte() 在 ST_SYNC0
 * 遇到非 0xA5 字节是**静默丢弃**的。实测（12 轮，固件带回执）：
 * 服务持续画帧时 `pcmode 1` 有 6/12 次**根本没有回执**（命令没送达），
 * 裸链路 12/12 正常 —— 就是帧和文本挤在同一个 USB 传输窗口里被吞掉了。
 * JS 层 #enqueue 已串行，但字节在 USB 层仍会合流，所以必须留时间空隙。
 */
const TEXT_GAP_MS = 50;
const pad2 = (n) => String(n).padStart(2, '0');
const hhmm = (date) => `${pad2(date.getHours())}:${pad2(date.getMinutes())}`;
const ymd = (date) => `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`;

/** 提醒节奏：震动次数/时长（ms）与亮度脉冲次数。 */
const REMIND_PATTERN = {
  done: { vibrate: [120, 120], pulse: 2 },
  needs: { vibrate: [180, 180, 180], pulse: 3 },
  failed: { vibrate: [450, 450], pulse: 2 },
};
const VIBRATE_SCALE = { short: 0.6, medium: 1, long: 1.5 };

/**
 * 设计稿目录解析（顺序见文件头注释）。
 *
 * 参数都能注入（单测用）：`exists` 决定"这个目录有没有设计稿"，
 * 生产用 `tokens.json` 是否存在来判断。
 *
 * @returns {string} 设计稿目录绝对路径（可能不存在 —— 交给 loadDesign 报错）
 */
export function resolveDesignDir({
  configured = '',
  packaged = PACKAGED_DESIGN,
  env = process.env.PROTORIG_DESIGN_DIR ?? '',
  repo = REPO_DESIGN,
  exists = (dir) => Boolean(dir) && fs.existsSync(path.join(dir, 'tokens.json')),
} = {}) {
  const explicit = typeof configured === 'string' ? configured.trim() : '';
  if (explicit) return explicit;
  if (exists(packaged)) return packaged;
  if (env && exists(env)) return env;
  return repo;
}

/** 设计稿读不到时给一句能操作的说明。 */
function loadDesignOrExplain(dir, logger) {
  try {
    return loadDesign(dir);
  } catch (error) {
    logger.warn(
      `设计 tokens 读取失败（${dir}：${error.message}）。面板无法渲染；` +
        '请确认插件的 design/dsh_panel_v4/tokens.json 存在，或把 design 设置指向包含 tokens.json 的目录。',
    );
    throw error;
  }
}

export class PanelService {
  constructor({ config, logger, balance, platform = process.platform, deviceStore = null }) {
    this.rawConfig = config;
    this.config = normalizeConfig(config);
    this.logger = logger;
    this.balance = balance ?? null;
    this.platform = platform;
    /** 插件版本（设置页显示，便于确认页面跑的是哪一版）。 */
    this.pluginVersion = (() => { try { return require('../package.json').version; } catch { return ''; } })();
    /** 已见过的手表登记表（自动登记；设置页从中选择连接目标）。 */
    this.deviceStore = deviceStore ?? new DeviceStore({ logger });
    this.disposed = false;
    this.link = new WatchLink({
      // 重连统一由本类负责：WatchLink 自己的 autoReconnect 只会在 close 时再试一次，
      // 若那一刻手表还没枚举完（USB 刚拔），就会永久停在断开 —— 真机踩过。
      autoReconnect: false,
      reconnectDelayMs: this.config.reconnectDelayMs,
      debug: this.config.debug,
    });
    /** 可注入的串口发现函数（测试用；生产 = WatchLink.discover）。 */
    this.discover = () => WatchLink.discover(this.platform);
    /** 可注入的逐口探活函数（测试用；生产 = link.mjs 的 probePortAdminId）。 */
    this.probePort = probePortAdminId;
    this.opening = false;
    /** 固定串口设置变化时用于重开链路。 */
    this.appliedPort = undefined;
    /** 已应用的显示亮度（设置页改亮度后 tick 内即时下发 0x02）。 */
    this.lastAppliedBrightness = null;
    /** 已下发的 PC 模式息屏秒数（0 = 固件默认）。 */
    this.lastAppliedScreenOff = null;
    /* ---------------------------------------------------- 设备身份（P5/P6/P7） */
    /** 当前连接手表的 admin_id（'' = 还没问出来/旧固件）。 */
    this.deviceId = '';
    /** 固件版本（adminid 的 fw= 字段；旧固件为 ''）。 */
    this.firmwareVersion = '';
    /** 固件构建时间（adminid 的 build= 字段；同版本号区分不同构建）。 */
    this.firmwareBuild = '';
    /** 身份来源：'usb' | 'ble' | null。 */
    this.deviceSource = null;
    /** 串口路径 → 已探明的 admin_id（'' = 探过但没问出来）。换表保护靠它挑口。 */
    this.portIdentity = new Map();
    /** 本次**连续**连接上验证过的身份；拔线就清掉，重插必须重新问（可能是另一块表）。 */
    this.identityVerified = null;
    /** 已生效的绑定（设置页改 deviceId 后对比，变了才重开链路）。 */
    this.appliedDeviceId = this.config.deviceId;
    /** 已生效的 USB 连接开关（关掉时自动断开串口）。 */
    this.appliedUsbEnabled = this.config.usb.enabled !== false;
    this.appliedBleEnabled = this.config.ble.enabled !== false;
    /** 最近一次枚举到的候选口（/state 与设置页「设备」分组直接读它，不重复枚举）。 */
    this.candidates = [];
    this.candidatesAt = 0;
    this.scanning = null;
    /** 用户点了"断开"：暂停自动重连，直到"重连"/改设置。 */
    this.manualPaused = false;
    /** 上次记过的"没找到设备/绑定不匹配"原因，避免每 1.5 s 刷同一条日志。 */
    this.lastSelection = '';
    this.lastIdentityWarn = '';
    /** 烧录期间暂停 USB 重连，直到应用口（MI_00 / 原 COM）重新出现。 */
    this.flashHold = false;
    /** 表盘操作期间保持手表模式：抑制"收到 PC 1 就自动握手回电脑模式"（P8/P9）。 */
    this.faceHold = false;
    this.appPortBeforeFlash = null;
    this.warnedFlashWait = false;
    this.focus = createFocusState();
    this.titleOptions = { dshHome: process.env.DSH_HOME };
    this.synthetic = new WeakMap();
    this.syntheticSeq = 0;
    this.commands = new Map();
    this.status = { battery: null, bright: null, link: null, page: 0 };
    this.brightBase = null;
    this.pulsing = false;
    this.metrics = null;
    this.metricsCollector = null;
    this.metricsWarned = false;
    this.approval = null;
    this.question = null;
    /** 审批/问答 FIFO 队列（表上一次只显示一张卡）。 */
    this.overlayQueue = [];
    /** 限时通知卡（protorig_notify）：{title, body, until}。 */
    this.notice = null;
    this.noticeTimer = null;
    this.touchDown = null;
    /** 模式切换后的触摸静默期（滑动收尾的残点不能当成点击）。 */
    this.touchGuardUntil = 0;
    /** 握手进行中又收到 pc=1（滑出再滑回）：结束后补跑一次。 */
    this.handshakeQueued = false;
    this.design = null;
    this.tokens = null;
    this.renderer = null;
    this.fb = null;
    this.flusher = null;
    this.timer = null;
    this.soonTimer = null;
    this.metricsTimer = null;
    this.openTimer = null;
    this.handshaking = false;
    this.busy = false;
    this.chain = Promise.resolve();
    /** 表盘（P8/P9）：当前 face id（固件没有查询当前表盘的命令，插件侧记住即可）。 */
    this.faceId = 0;
    /** 图片表盘安装任务（同一时刻一个；进度给设置页轮询）。 */
    this.faceJob = null;
    this.faceSeq = 0;
    /** 默认表盘下半区面板（research/76 §2）：最近一次读到的槽状态（null = 还没问过）。 */
    this.panelInfo = null;
    this.panelInfoAt = 0;
    /** 面板安装任务（与图片表盘安装共用一条串口：同一时刻只能有一个在跑）。 */
    this.panelJob = null;
    this.panelSeq = 0;
    this.lastRemind = {};
    this.lastStatusReq = 0;
    /** 最后一次写像素帧的时刻（0 = 本次会话还没发过帧），文本命令据此避让。 */
    this.lastFrameAt = 0;
    /** BLE 卡片层的最近一次下发/热区状态（P1..P4）。 */
    this.bleCardKey = null;
    this.bleCardAt = 0;
    /** 位图槽 -> 已发送的文本键（只有文本变了才重传，避免吃满 2.9 KB/s）。 */
    this.bleBitmaps = new Map();
    /** 位图槽 -> {key, count}：同一张图连续被固件拒收的次数，超上限就放弃改用文本兜底。 */
    this.bleBitmapFails = new Map();
    /** 固件自报的热区（0x58）；没收到之前用 BLE_LAYOUT 兜底。 */
    this.bleZones = { kind: 0, list: [], at: 0 };
    this.cardBitmap = new CardBitmapRenderer();
    this.ble = new BleBridge({
      config: this.config,
      logger,
      onState: (connected) => {
        this.bleCardKey = null;   // 重连后必须重发一张卡（手表侧没有缓存）
        this.bleCardKind = null;
        this.blePage = null;
        this.bleBitmaps.clear();
        this.bleZones = { kind: 0, list: [], at: 0 };
        if (connected) {
          this.logger.info('USB 不在，手表切到本地三环（BLE 数字通道）');
          const timer = setTimeout(() => this.#applyBleDisplay(), 400);
          timer.unref?.();
        }
      },
      /* 手表 notify（触摸/状态/热区/回执）与 USB 的 link 事件走同一个入口。
       * 判定逻辑（点击/滑动/审批/问答）USB 与 BLE 共用一份（research/65 §4.4）。 */
      onEvent: (event) => this.handleWatchEvent(event),
      /* 广播名里带 admin_id（research/73 §1）：BLE 也能认出"这是哪块表"，
       * 换表保护对 BLE 链路同样生效。 */
      onDevice: ({ name }) => this.#onBleDevice(name),
    });
    this.ble.stop(); // 先标记为未启动：stop() 里的 stopping 在 start() 会复位
  }

  /* ------------------------------------------------------------ 生命周期 */

  async start() {
    this.startedAt = Date.now();
    this.#restoreFlashHold(); // 上次烧录没收尾：先接回"只等原应用口"的状态

    this.design = loadDesignOrExplain(resolveDesignDir({ configured: this.config.design }), this.logger);
    this.tokens = this.design.tokens;
    if (this.tokens.layout !== 'monitor-v4') {
      this.logger.warn(`设计 tokens 不是 monitor-v4（${this.tokens.layout ?? '未知'}），面板按 v4 渲染`);
    }
    this.renderer = await new MonitorCanvasRenderer().init();
    // 1-bit 卡片位图（标题/命令/题目）：装不上 canvas 就退回固件字库文本框（缺字 '?'）
    this.cardBitmap.init().catch((error) => {
      this.logger.warn(`卡片位图渲染不可用（${error.message}）；标题改用固件字库兜底`);
    });
    this.fb = new FrameBuffer();
    this.flusher = new BandFlusher(this.link, this.fb);
    this.#attachLink();
    this.#attachMetrics();
    this.#scheduleNext();
    this.#tryOpen();
    this.logger.info(
      `面板已启动：${this.config.fps} fps · 设计 ${this.design.dir} · ` +
        (this.config.port ? `串口 ${this.config.port}` : '串口自动发现'),
    );
  }

  async dispose() {
    if (this.disposed) return;
    this.disposed = true;
    clearTimeout(this.timer);
    clearTimeout(this.soonTimer);
    clearInterval(this.metricsTimer);
    if (globalThis.__protorigMetricsTimer === this.metricsTimer) globalThis.__protorigMetricsTimer = null;
    clearTimeout(this.openTimer);
    clearTimeout(this.noticeTimer);
    this.notice = null;
    /* 队列里等着的卡、以及正在显示的卡，都要给 await 方一个结果，别把宿主会话挂住。 */
    for (const entry of this.overlayQueue.splice(0)) entry.finish?.({ aborted: true });
    this.approval?.entry?.finish?.({ aborted: true });
    this.question?.entry?.finish?.({ aborted: true });
    // 等 BLE 断开完成（最多 1.8 s），别把后台扫描/连接留着
    await Promise.race([this.ble?.stop?.() ?? Promise.resolve(), sleep(1800)]);
    try {
      if (this.link.state.pcMode) {
        await Promise.race([this.link.shell('pcmode 0'), sleep(400)]);
      }
    } catch {
      /* 宿主退出时手表已经掉线也正常 */
    }
    await this.link.close().catch(() => {});
  }

  /* ------------------------------------------------------------ 链路 */

  #attachLink() {
    const link = this.link;
    link.on('open', ({ path: port }) => {
      this.logger.info(`手表已连接：${port}`);
      this.#handshake().catch((error) => this.logger.warn(`握手失败：${error.message}`));
    });
    link.on('close', ({ reason }) => {
      this.logger.info(`手表链路关闭（${reason === 'device' ? 'USB 拔出/掉线' : '主动关闭'}）`);
      // 拔线后同一个 COM 号可能插上来的是另一块表：身份验证必须重新做
      this.identityVerified = null;
      this.faceHold = false;
      this.touchDown = null;
      this.touchGuardUntil = Date.now() + 500;
      this.#scheduleOpen(); // 关键：close 之后必须持续重试，不能只试一次
    });
    link.on('error', (error) => {
      if (this.config.debug) this.logger.debug?.(`链路错误：${error.message}`);
    });
    link.on('pc', ({ value }) => {
      this.logger.info(`手表 @PCM1 PC ${value}${value ? '（进入电脑模式）' : '（退出电脑模式）'}`);
      // 退出/进入电脑模式都会伴随本地页滑动：静默 500 ms，别把滑动手势当点击
      this.touchDown = null;
      this.touchGuardUntil = Date.now() + 500;
      if (value === 1 && !this.faceHold) {
        // 滑回来时 passthrough 是关的：必须重新握手（settime→HELLO→MODE 1→整屏重发），
        // 否则手表一直停在本地页"需要连接电脑"。
        // 表盘操作期间例外：保持手表模式，别把刚切过去的表盘又盖成像素副屏。
        this.#requestHandshake();
      }
    });
    link.on('mode', ({ value }) => {
      this.logger.info(`手表 @PCM1 MODE ${value}${value ? '（透传开启）' : '（透传关闭）'}`);
      this.touchDown = null;
      this.touchGuardUntil = Date.now() + 500;
      if (value === 1) {
        this.fb?.invalidate();
        this.#paintSoon(0);
      }
    });
    link.on('hello', (hello) => {
      this.logger.info(`HELLO ver=${hello.ver} maxW=${hello.maxW} maxH=${hello.maxH} fmt=${hello.fmt}`);
    });
    link.on('status', (status) => {
      const batteryChanged = status.battery !== this.status.battery;
      this.status = status;
      if (!this.pulsing && Number.isFinite(status.bright)) this.brightBase = status.bright;
      if (batteryChanged) this.#paintSoon(0);
      // 固件每 1 s 主动报一次；万一停了超过 5 s 就补一条 STATUS_REQ
      const now = Date.now();
      if (now - this.lastStatusReq > 5000 && this.link.state.passthrough) {
        this.lastStatusReq = now;
        this.#command(() => this.link.requestStatus()).catch(() => {});
      }
    });
    link.on('touch', (touch) => this.#onTouch(touch));
    /* USB 文本行里的卡片层事件（0x58/0x59/0x92）：与 BLE notify 走同一个处理器。 */
    link.on('event', (event) => this.handleWatchEvent(event));
  }

  /**
   * 打开串口（单一重连监督器）。
   *
   * 每轮：
   *   1. `discover()` 按 VID 349B / PID 6160 探测（Windows 看 MI_00；macOS 看
   *      `/dev/cu.usbmodem*`）—— 异步、非阻塞，1.5 s 一轮；
   *   2. 有绑定 `deviceId` 时，用已探明的 `portIdentity` 挑口：已知不是绑定表的
   *      口直接跳过（P5）；还没身份的口先开进去，由握手里的 `adminid` 校验；
   *   3. 找到路径才 `open()`，避免用不存在的 COM 号反复开；
   *   4. 成功 → 'open' 事件里停 BLE + 握手（adminid/settime/HELLO/pcmode）+ 整屏重发；
   *   5. 失败/没找到 → 排下一轮，永不放弃。BLE 桥在此期间保持。
   */
  #tryOpen() {
    if (this.config.usb.enabled === false) return Promise.resolve(false); // 开关关：不碰串口
    if (this.disposed || this.opening || this.manualPaused || this.link.state.connected) {
      return Promise.resolve(false);
    }
    this.opening = true;
    clearTimeout(this.openTimer);
    this.openTimer = null;
    return (async () => {
      let path = this.config.port || '';
      if (this.flashHold) {
        // 烧录后板子停在 ROM 下载模式，ROM 口也是 VID 349B：只认原应用口
        // （COM 号相同）或 MI_00 应用接口，避免对 bootloader 串口做无关写入。
        const ports = await this.discover();
        const wanted = this.appPortBeforeFlash ? String(this.appPortBeforeFlash).toLowerCase() : '';
        const target = ports.find((port) => wanted && String(port.path).toLowerCase() === wanted)
          ?? ports.find((port) => /MI_00/i.test(String(port.pnpId ?? '')));
        if (!target) {
          if (!this.warnedFlashWait) {
            this.warnedFlashWait = true;
            this.logger.info('烧录完成：等待手动拨动开关重启（SW2 ON→OFF→ON）后的应用串口（识别到原 COM 或 MI_00 后自动重连）');
          }
          return false;
        }
        this.flashHold = false;
        this.warnedFlashWait = false;
        this.#dropFlashHoldFile();
        this.logger.info(`烧录后应用串口出现：${target.path}，恢复面板链路`);
        path = target.path;
      } else if (!path) {
        const ports = await this.discover();
        this.#rememberCandidates(ports);
        const annotated = ports.map((port) => ({
          ...port,
          admin: this.portIdentity.get(port.path) ?? '',
        }));
        const { candidate, reason } = selectCandidate(annotated, this.appliedDeviceId);
        if (!candidate) {
          const text = describeSelection(reason, { bound: this.appliedDeviceId });
          if (this.lastSelection !== text) {
            this.lastSelection = text;
            this.logger.info(`${text}；每 ${this.config.reconnectDelayMs} ms 探测一次`);
          }
          this.warnedNoDevice = true;
          return false;
        }
        if (this.lastSelection !== reason) {
          this.lastSelection = reason;
          if (this.appliedDeviceId || annotated.length > 1) {
            this.logger.info(describeSelection(reason, { bound: this.appliedDeviceId }));
          }
        }
        path = candidate.path;
      }
      if (!path) {
        const error = new Error('没找到手表：期望 VID 349B / PID 6160 的 USB 串口');
        if (!this.warnedNoDevice) {
          this.warnedNoDevice = true;
          this.logger.info(`${error.message}；每 ${this.config.reconnectDelayMs} ms 探测一次`);
        }
        return false;
      }
      if (this.link.state.connected) return false; // 探测期间已被别处打开
      const bleActive = () => this.ble.connected || this.ble.child || this.ble.starting;
      let stoppedBle = false;
      // 正常路径严格按"先停 BLE → 再开串口"；但端口被串口助手占着时 open 会失败，
      // 如果每 1.5 s 都先停 BLE 再失败，BLE 会被反复停/启（真机验证时踩过）。
      // 所以打开失败后 15 s 内不再先停，直接试开；成功后在下面补停。
      if (bleActive() && Date.now() >= (this.bleStopCooldownUntil ?? 0)) {
        const started = Date.now();
        await this.ble.stop();
        stoppedBle = true;
        this.logger.info(`BLE 数字通道已停（${Date.now() - started} ms），打开 USB 串口`);
      }
      try {
        await this.link.open(path);
      } catch (error) {
        if (stoppedBle) this.bleStopCooldownUntil = Date.now() + 15000;
        throw error;
      }
      if (bleActive()) {
        // 冷却期内没先停（或刚好又启动）的，串口已经打开，补停
        await this.ble.stop();
        this.logger.info('BLE 数字通道已停，切回像素面板');
      }
      this.warnedNoDevice = false;
      return true;
    })()
      .catch((error) => {
        // 设备在但打不开（被串口助手占用等）：保持重试，BLE 不撤
        if (!this.warnedNoDevice) {
          this.warnedNoDevice = true;
          this.logger.warn(`打开串口失败（${error.message}）；继续重试`);
        }
        return false;
      })
      .finally(() => {
        this.opening = false;
        if (!this.link.state.connected && !this.disposed) this.#scheduleOpen();
      });
  }

  #scheduleOpen() {
    if (this.disposed || this.manualPaused || this.openTimer || this.opening || this.link.state.connected) return;
    this.openTimer = setTimeout(() => {
      this.openTimer = null;
      void this.#tryOpen();
    }, this.config.reconnectDelayMs);
    this.openTimer.unref?.();
  }

  /* ------------------------------------------------------------ 设备身份 */

  #rememberCandidates(ports) {
    const list = Array.isArray(ports) ? ports : [];
    if (!list.length && Date.now() - this.candidatesAt < 5000) return; // 抖动时别把列表清空
    this.candidates = list;
    this.candidatesAt = Date.now();
  }

  /**
   * 当前连接设备的身份变化（P7 换表保护）。
   *
   * usb/ble 两条链路都走这里：换了表就清掉"已下发"缓存、整屏重发，
   * 否则插件会以为亮度/常亮已经设过 —— 新表停在默认值（research/64 §1.1）。
   *
   * @returns {boolean} 是否发生了换表（身份从 A 变成 B）
   */
  #noteDevice(admin, source) {
    const next = normalizeAdminId(admin);
    if (!next) return false;
    this.#registerSeen(next, source);
    const previous = this.deviceId;
    this.deviceSource = source === 'ble' ? 'ble' : 'usb';
    if (previous === next) {
      this.deviceId = next;
      return false;
    }
    this.deviceId = next;
    if (!previous) {
      this.logger.info(`设备身份：${deviceLabel({ admin: next, transport: source, port: this.link.path })}`);
      return false;
    }
    this.#resetForDeviceSwitch(previous, next);
    return true;
  }

  /** 只登记（不改变当前连接/绑定）：任何一次"见到表"都记进已登记列表。 */
  #registerSeen(admin, source) {
    const id = normalizeAdminId(admin);
    if (!id) return;
    try {
      this.deviceStore.note(id, source);
    } catch (error) {
      this.logger.debug?.(`设备登记失败（不影响连接）：${error.message}`);
    }
  }

  /** 已登记设备视图：给设置页选择用（自动登记，不需要用户手输编号）。 */
  #knownDevicesView() {
    const bound = normalizeAdminId(this.appliedDeviceId);
    const current = normalizeAdminId(this.deviceId);
    let list = [];
    try {
      list = this.deviceStore.list();
    } catch {
      list = [];
    }
    const connected = Boolean(this.link.state.connected || this.ble.connected);
    return list.map((item) => ({
      id: item.id,
      lastSeen: item.lastSeen,
      usb: Boolean(item.usbAt),
      ble: Boolean(item.bleAt),
      selected: Boolean(bound && item.id === bound),
      connected: Boolean(connected && current && item.id === current),
    }));
  }

  /**
   * 换表：清下发缓存 + 整屏重发 + 重置指标/焦点。
   *
   * 焦点只重置"上一块表上的临时状态"（步数/工具/审批计数/上下文压力），
   * **保留 sessionId** —— 否则换表后连当前会话都不跟了，要等下一次会话事件才恢复。
   * 标题由 #refreshFocus 在 2 s 内从会话文件重读。
   */
  #resetForDeviceSwitch(previous, next) {
    this.lastAppliedBrightness = null;
    this.lastAppliedAlwaysOn = null;
    this.fb?.invalidate();
    this.metrics = null;
    this.status = { battery: null, bright: null, link: null, page: 0 };
    this.brightBase = null;
    this.pulsing = false;
    this.commands.clear();
    this.lastRemind = {};
    this.bleCardKey = null;
    this.bleCardKind = null;
    this.bleBitmaps.clear();
    this.bleZones = { kind: 0, list: [], at: 0 };
    this.ble.lastAddress = null; // BLE 直连缓存属于旧表（CoreBluetooth/Windows 的地址都只对本机有效）
    /* 下半区面板状态也属于旧表：装没装过、CRC 是多少都不能沿用（否则设置页会显示上一块表的图）。 */
    this.panelInfo = null;
    this.panelInfoAt = 0;
    const sessionId = this.focus?.sessionId ?? null;
    this.focus = createFocusState(sessionId);
    this.logger.info(`换表：${previous} → ${next}（清亮度/常亮缓存、整屏重发、重置指标与焦点）`);
  }

  /**
   * 握手阶段问身份：`adminid` 最多问两次（串口刚打开时 shell 可能还没醒）。
   *
   * 同一次连接里（用户滑出再滑回电脑模式）已经验证过的身份直接复用：
   * 不必每滑一次都发一遍 shell 命令、也不用等回包。
   * 拔过线就会清空（见 #attachLink 的 close），重插一定重新问 —— 同一个 COM 号
   * 插上来的可能是另一块表。
   */
  async #identifyDevice() {
    if (!this.link.state.connected) return null;
    // 旧链路实现/测试替身没有 queryAdminId：当作"问不出来"，行为退回升级前
    if (typeof this.link.queryAdminId !== 'function') return null;
    const verified = this.identityVerified;
    if (verified && verified.path === this.link.path && verified.admin
      && verified.admin === normalizeAdminId(this.deviceId)) {
      return {
        admin: verified.admin, chip: '', chipOk: false, fw: this.firmwareVersion, build: this.firmwareBuild, cached: true,
      };
    }
    for (let attempt = 1; attempt <= 2; attempt += 1) {
      const found = await this.link.queryAdminId(900).catch(() => null);
      if (found?.admin) {
        this.identityVerified = { path: this.link.path, admin: found.admin };
        return found;
      }
      if (!this.link.state.connected || this.disposed) return null;
      if (attempt < 2) await sleep(150);
    }
    return null;
  }

  /**
   * 身份门禁：绑定了 deviceId 却连到别的表 → 关掉这个口，让下一轮挑别的候选。
   *
   * `unknown`（问不出编号，比如旧固件/串口被占用）时的处理：
   *   · 只有 1 个候选口 → 容忍（升级前就这个行为，不能因为问不到就不连）；
   *   · 多个候选口 → 放弃这个口（宁可连不上，也不要把设置下发到另一块表上）。
   *
   * @returns {'ok'|'mismatch'|'unknown-tolerated'|'unknown-skip'}
   */
  #acceptIdentity(identity) {
    const bound = normalizeAdminId(this.appliedDeviceId);
    const admin = normalizeAdminId(identity?.admin);
    /* 见过就登记（哪怕它不是你选的表）——设置页的"已登记设备"列表靠这个自动长出来。 */
    if (admin) this.#registerSeen(admin, 'usb');
    if (!bound) {
      if (admin) this.#noteDevice(admin, 'usb');
      return 'ok';
    }
    if (admin && admin === bound) {
      this.#noteDevice(admin, 'usb');
      return 'ok';
    }
    if (!admin) {
      const candidates = this.candidates?.length ?? 0;
      const known = this.portIdentity.get(this.link.path) ?? '';
      if (known && known !== bound) return 'mismatch';
      if (candidates <= 1) {
        if (this.lastIdentityWarn !== 'unknown') {
          this.lastIdentityWarn = 'unknown';
          this.logger.warn(`已绑定 ${bound}，但这个口没回 adminid：按唯一候选口先连（固件/串口异常时会出现）`);
        }
        return 'unknown-tolerated';
      }
      if (this.lastIdentityWarn !== 'skip') {
        this.lastIdentityWarn = 'skip';
        this.logger.warn(`已绑定 ${bound}，但这个口没回 adminid：有多个候选口，跳过它`);
      }
      return 'unknown-skip';
    }
    this.lastIdentityWarn = 'mismatch';
    this.logger.info(`这个口是 PROTORIG ES ${admin}，绑定的是 ${bound}：换下一个候选口`);
    return 'mismatch';
  }

  /** BLE 广播名后缀 → 身份（USB 在线时以 USB 的 adminid 为准）。 */
  #onBleDevice(name) {
    if (this.disposed || this.link.state.connected) return;
    const admin = parseAdvName(name);
    if (!admin) return;
    this.#noteDevice(admin, 'ble');
  }

  /** 测试/调试：立刻执行一轮 USB 探测（返回是否打开成功）。 */
  probeUsbForTest() {
    return this.#tryOpen();
  }

  /** 测试/调试：挂上与 start() 相同的链路监听（用假 link 时调用）。 */
  attachLinkForTest() {
    this.#attachLink();
  }

  /**
   * 请求一次握手。用户滑出再滑回电脑模式（PC 0 → PC 1）时走这里：
   * 与 USB 重连共用同一条路径（settime → HELLO×3 → MODE 1 → 整屏重发）。
   */
  #requestHandshake() {
    if (this.disposed || !this.link.state.connected) return;
    if (this.faceHold) return; // 表盘模式：不要自动回电脑模式
    if (this.handshaking) {
      this.handshakeQueued = true;
      return;
    }
    this.#handshake().catch((error) => this.logger.warn(`握手失败：${error.message}`));
  }

  async #handshake() {
    if (this.handshaking || this.disposed) return;
    this.handshaking = true;
    try {
      this.warnedNoDevice = false;
      /* 第一步就问身份（research/74 §2.3：shell `adminid` 必须在 pcmode 1 之前问）。
       * 绑定了 deviceId 却连到别的表 → 立刻放弃这个口，绝不进 PC 模式/下发设置。 */
      const identity = await this.#identifyDevice();
      if (identity?.admin) {
        this.portIdentity.set(this.link.path, identity.admin);
        if (identity.fw) this.firmwareVersion = identity.fw;
        if (identity.build) this.firmwareBuild = identity.build;
      } else {
        /* 问不出身份（旧固件/串口被占用）：清掉上一块表留下的编号，
         * 界面上显示"未识别"比显示错编号好（项目原则：不显示假数据）。 */
        this.portIdentity.set(this.link.path, '');
        if (this.deviceId) {
          this.logger.info('这个口没回 adminid：先不显示设备编号（旧固件或串口被占用）');
          this.deviceId = '';
          this.deviceSource = null;
        }
      }
      const verdict = this.#acceptIdentity(identity);
      if (verdict === 'mismatch' || verdict === 'unknown-skip') {
        // 身份已经记进 portIdentity（上面那段），关掉这个口，让下一轮挑别的候选
        await this.link.close().catch(() => {});
        return;
      }
      this.fb.invalidate(); // 重连后手表没有帧缓存：无论之前发过什么都整屏重发
      this.lastAppliedBrightness = null;
      this.lastAppliedAlwaysOn = null; // 重新进 PC 模式要重下发常亮策略
      // 注意：WatchLink.syncTime() 自己会发 settime 并返回命令行，不要再包进 #write
      await this.link.syncTime().catch(() => {});
      await sleep(150);
      const state = await this.link.queryState().catch(() => null);
      if (state) {
        this.logger.info(
          `手表状态 active=${state.active} transport=${state.transport} ` +
            `passthrough=${state.passthrough} usbraw=${state.usbraw}`,
        );
      }
      if (this.config.autoEnter && !this.faceHold && !this.link.state.pcMode) {
        await this.#enterPcModeWithRetry();
      }
      if (this.link.state.pcMode && !this.link.state.passthrough) {
        // 文本命令和二进制帧之间留空隙：合成一个 USB 包时固件会把整包交给 shell
        await sleep(220);
        for (let attempt = 1; attempt <= 3 && !this.link.state.passthrough; attempt += 1) {
          if (!this.link.state.connected || this.disposed) return; // 握手途中拔线：立即放弃，别空转 3 次
          await this.#command(() => this.link.sayHello()).catch(() => {});
          if (await this.#waitState('passthrough', true, 1200)) break;
          if (!this.link.state.connected) return;
          this.logger.warn(`HELLO 第 ${attempt} 次没打开透传，重发`);
          await sleep(400);
        }
      }
      if (!this.link.state.connected) return;
      if (this.link.state.passthrough) {
        this.fb.invalidate();
        await this.#applyBrightness({ force: true }).catch(() => {});
        await this.#applyAlwaysOn({ force: true }).catch(() => {});
        await this.#applyScreenOff().catch(() => {});
        this.#paintSoon(0);
        this.logger.info('透传就绪（MODE 1），整屏重发');
      } else if (this.link.state.pcMode) {
        this.logger.warn('已在电脑模式但透传没打开：不发像素，等下一次 HELLO');
      } else {
        this.logger.info('手表没进电脑模式（从表盘右滑进入，或打开 autoEnter）');
      }
    } finally {
      this.handshaking = false;
      if (this.handshakeQueued) {
        // 握手期间又滑回来/又收到 pc=1：补一次（最多补一轮，不递归排队）
        this.handshakeQueued = false;
        if (!this.disposed && this.link.state.connected) {
          const timer = setTimeout(() => this.#requestHandshake(), 50);
          timer.unref?.();
        }
      }
    }
  }

  async #waitState(key, want, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      if (this.link.state[key] === want) return true;
      await sleep(40);
    }
    return false;
  }

  /**
   * 让手表进电脑模式（shell `pcmode 1`），**带状态校验和重试**。
   *
   * 2026-11-21 实测（10 轮复刻同一场景）：`pcmode 0` 之后短时间内再发 `pcmode 1`，
   * 约 2/10 次不生效 —— 手表查询回报 `active=0`，命令像是被吞了，但串口写入是成功的。
   * 单发一次就放弃会让手表停在表盘，用户看到的就是"插件明明连上了，屏幕却没进电脑模式"。
   * 所以发完必须查 `pcMode`，没进就重发。
   */
  async #enterPcModeWithRetry(attempts = 3, settleMs = 350) {
    for (let attempt = 1; attempt <= attempts; attempt += 1) {
      if (this.disposed || !this.link.state.connected) return false;
      if (this.link.state.pcMode) return true;
      /* 用 shell 发（语义就是"发出去"），成败由 pcMode 判定。
       * 固件 2026-11-21 起还多回一行 `[pc] cmd pcmode 1 active=1` 的执行回执，
       * 那是给台架/诊断用的；这里不依赖它，老固件同样能跑。 */
      await this.#command(() => this.link.shell('pcmode 1')).catch(() => {});
      if (await this.#waitState('pcMode', true, 1500)) return true;
      if (attempt < attempts) {
        this.logger.warn(`pcmode 1 第 ${attempt} 次没生效，重发`);
        await sleep(settleMs);
      }
    }
    return this.link.state.pcMode;
  }

  /** 把设置里的默认亮度下发到固件（0x02）。设置页保存后下一次 tick 即时生效。 */
  /** 当前生效的显示参数（USB / BLE 分开；transport 省略时按当前链路推断）。 */
  #displayFor(transport = '') {
    const kind = transport || (this.link.state.connected ? 'usb' : this.ble.connected ? 'ble' : 'usb');
    const cfg = kind === 'ble' ? this.config.ble : this.config.usb;
    /* 设置页拖动/勾选后立即生效用的运行时覆盖（不必等保存/重启）。 */
    const override = this.displayOverride?.[kind];
    return {
      brightness: Math.round(override?.brightness ?? cfg?.brightness ?? this.config.display.brightness),
      alwaysOn: override?.alwaysOn ?? (cfg?.alwaysOn !== false),
      screenOffSec: Math.max(0, Math.round(override?.screenOffSec ?? cfg?.screenOffSec ?? 0)),
    };
  }

  /**
   * 设置页「显示（USB）」「显示（BLE）」拖动/勾选时立即下发（前端 debounce 调用）。
   * 这解决"改了滑杆但没点保存/配置未热更新时看起来设置不管用"的问题；
   * 持久化仍由 DSH 设置页的保存负责，重启后从配置读取。
   */
  async applyDisplaySettings(patch = {}) {
    const clean = (cfg) => {
      if (!cfg || typeof cfg !== 'object') return null;
      const out = {};
      if (Number.isFinite(Number(cfg.brightness))) out.brightness = Math.min(255, Math.max(32, Math.round(Number(cfg.brightness))));
      if (typeof cfg.alwaysOn === 'boolean') out.alwaysOn = cfg.alwaysOn;
      if (Number.isFinite(Number(cfg.screenOffSec))) out.screenOffSec = Math.min(600, Math.max(0, Math.round(Number(cfg.screenOffSec))));
      return out;
    };
    const usb = clean(patch.usb);
    const ble = clean(patch.ble);
    this.displayOverride = { ...(this.displayOverride ?? {}) };
    if (usb) this.displayOverride.usb = usb;
    if (ble) this.displayOverride.ble = ble;
    let applied = false;
    if (this.link.state.connected) {
      await this.#applyBrightness({ force: true }).catch(() => {});
      await this.#applyAlwaysOn({ force: true }).catch(() => {});
      await this.#applyScreenOff({ force: true }).catch(() => {});
      applied = true;
    }
    if (this.ble.connected) {
      this.#applyBleDisplay();
      applied = true;
    }
    return { ok: true, applied };
  }

  async #applyBrightness({ force = false } = {}) {
    if (!this.link.state.passthrough) return false;
    const level = this.#displayFor('usb').brightness;
    if (!force && this.lastAppliedBrightness === level) return false;
    this.lastAppliedBrightness = level;
    this.brightBase = level;
    await this.#write(bright(level)).catch(() => {});
    this.logger.info(`电脑模式亮度设为 ${level}`);
    return true;
  }

  /**
   * 把「PC 模式下常亮」下发到固件（0x15 ALWAYS_ON）。
   * 固件默认常亮；关掉后 PC 模式下也按正常 15 s 规则息屏。
   */
  async #applyAlwaysOn({ force = false } = {}) {
    if (!this.link.state.passthrough) return false;
    const on = this.#displayFor('usb').alwaysOn;
    if (!force && this.lastAppliedAlwaysOn === on) return false;
    this.lastAppliedAlwaysOn = on;
    await this.#write(frame([0x15, on ? 1 : 0])).catch(() => {});
    this.logger.info(`PC 模式常亮：${on ? '开' : '关'}`);
    return true;
  }

  /** PC 帧 0x16：电脑模式独立息屏超时（不写手表设置）。 */
  async #applyScreenOff({ force = false } = {}) {
    if (!this.link.state.passthrough) return false;
    const sec = this.#displayFor('usb').screenOffSec;
    if (!force && this.lastAppliedScreenOff === sec) return false;
    this.lastAppliedScreenOff = sec;
    await this.#write(frame([0x16, sec & 0xff, (sec >> 8) & 0xff])).catch(() => {});
    this.logger.info(`PC 模式息屏时间：${sec ? `${sec} s` : '固件默认'}`);
    return true;
  }

  /** BLE 数字通道连上后下发它自己的一套显示参数（和 USB/手表模式分开）。 */
  #applyBleDisplay() {
    if (!this.ble.connected) return false;
    const d = this.#displayFor('ble');
    this.ble.brightness?.(d.brightness);
    this.ble.alwaysOn?.(d.alwaysOn);
    this.ble.screenOff?.(d.screenOffSec);
    this.logger.info(`BLE 显示：亮度 ${d.brightness} · ${d.alwaysOn ? '常亮' : `息屏 ${d.screenOffSec || '默认'} s`}`);
    return true;
  }

  /** 「USB 探测注册」：枚举 + 逐口问 adminid + 自动登记（不改变当前连接）。 */
  async probeUsbDevices() {
    if (this.scanning) return this.scanning;
    this.scanning = (async () => {
      try {
        const ports = await this.discover();
        this.#rememberCandidates(ports);
        await this.#probeCandidates();
        return { devices: this.#knownDevicesView(), candidates: this.#candidatesView() };
      } finally {
        this.scanning = null;
      }
    })();
    return this.scanning;
  }

  /**
   * 「蓝牙探测注册」：只扫一轮广播、登记编号（不连接、不改当前连接）。
   *
   * 显式探测 = 用户想确认蓝牙通不通：先 `resetRuntime()` 清掉"永久禁用"缓存再探一次。
   * 扫不成时返回 ok:false + reason，界面必须显示；以前 disabled 直接返回空数组、
   * 界面还写"探测完成"，被误判成硬件坏（2026-11-22）。
   */
  async probeBleDevices(ms = 5000) {
    this.ble.resetRuntime?.();
    const result = await this.ble.probeOnce?.(ms, ({ admin }) => this.#registerSeen(admin, 'ble'))
      .catch?.((error) => ({ found: [], reason: error.message })) ?? { found: [], reason: '蓝牙通道不可用' };
    const found = Array.isArray(result) ? result : (result.found ?? []);
    const reason = Array.isArray(result) ? '' : String(result.reason ?? '');
    if (found.length) this.logger.info(`蓝牙探测注册：${found.map((item) => item.id).join(' / ')}`);
    return {
      ok: !reason,
      found,
      reason,
      error: reason ? `蓝牙不可用：${reason}` : '',
      devices: this.#knownDevicesView(),
    };
  }

  /** 供 HTTP 路由 / agent 工具读取的面板状态（只读，无副作用）。 */
  stateForApi() {
    const bound = normalizeAdminId(this.appliedDeviceId);
    const admin = normalizeAdminId(this.deviceId);
    return {
      connected: this.link.state.connected,
      port: this.link.path,
      pcMode: this.link.state.pcMode,
      passthrough: this.link.state.passthrough,
      battery: this.status.battery,
      bright: this.status.bright,
      page: this.status.page,
      fps: this.config.fps,
      ble: this.ble.connected,
      /** BLE 通道是否活着：已连接或正在扫描/启动都算（resume/.status 判据，2026-11-22）。 */
      bleActive: Boolean(this.ble.connected || this.ble.starting || this.ble.scanning || this.ble.timer),
      /** BLE 通道是否被永久停用（加载失败/unsupported/unauthorized）；设置页与 protorig_state 靠它显示。 */
      bleDisabled: this.ble.disabled === true,
      /** 停用原因码；空 = 没被停用。 */
      bleReason: String(this.ble.runtimeReason ?? ''),
      flashHold: this.flashHold,
      paused: this.manualPaused,
      /** 会话投影只读摘要（不含标题/提示词）：确认手表当前显示的任务状态。 */
      focus: {
        state: this.focus?.live ?? 'IDLE',
        tool: this.focus?.tool || '',
        steps: Number.isFinite(this.focus?.steps) ? this.focus.steps : 0,
        turnOpen: this.focus?.turnOpen === true,
      },
      /** 协议版本（HELLO 里的 ver；USB 下拿到过才有，不编造固件版本）。 */
      protocolVersion: Number.isFinite(this.link.state.hello?.ver) ? this.link.state.hello.ver : null,
      /** 固件版本（adminid fw=；旧固件没有则为 null）。 */
      firmwareVersion: this.firmwareVersion || null,
      /** 固件构建时间（"1.6.0 · Nov 14 2026 21:32" 用来区分同版本的不同构建）。 */
      firmwareBuild: this.firmwareBuild || null,
      /** 插件版本（客户端用来确认是否加载了最新 bundle）。 */
      pluginVersion: this.pluginVersion || null,
      balance: this.balance?.publicView?.() ?? null,
      /** 设备身份（P6）：同一块表在 Win/mac 是同一个 6 位编号。 */
      device: {
        id: admin,
        bound,
        source: this.deviceSource,
        label: deviceLabel({
          admin,
          transport: this.link.state.connected ? 'usb' : this.ble.connected ? 'ble' : '',
          port: this.link.path,
        }),
        /** 已绑定但当前连的不是它（设置页要能提示"设置不会下发到这块表"）。 */
        mismatch: Boolean(bound && admin && bound !== admin),
      },
      /** 已登记设备（自动发现；设置页从中选择连接目标，USB/BLE 都生效）。 */
      knownDevices: this.#knownDevicesView(),
      /** 平台能力（P3）：烧录只在 Windows 出现。 */
      platform: {
        os: this.platform,
        kind: osKind(this.platform),
        name: osLabel(this.platform),
        flash: flashSupported(this.platform),
      },
      /** 表盘（P8/P9）：当前 face id + 是否有图片安装任务在跑。 */
      faces: { current: this.faceId, installing: Boolean(this.faceJob?.running) },
      /** 默认表盘下半区（research/76 §2）：只报**已读到的缓存**，这里不发 shell（轮询不占串口）。 */
      panel: this.panelView(),
      /** 最近一次枚举的候选口（不在这里开串口；要探活用 listDevices）。 */
      candidates: this.#candidatesView(),
      candidatesAt: this.candidatesAt,
    };
  }

  /** 候选口 → 设置页「设备」分组的列表项（身份只取已探明/当前连接的）。 */
  #candidatesView() {
    const list = Array.isArray(this.candidates) ? [...this.candidates] : [];
    const connectedPath = this.link.path;
    // 固定了 port 设置时不会走枚举：当前连接口也要出现在候选列表里
    if (connectedPath && !list.some((port) => port.path === connectedPath)) {
      list.unshift({ path: connectedPath, vendorId: '349B', productId: '6160' });
    }
    return list.map((port) => {
      const known = this.portIdentity.get(port.path);
      const admin = normalizeAdminId(known) || (connectedPath === port.path ? normalizeAdminId(this.deviceId) : '');
      return {
        path: port.path,
        port: port.path,
        admin,
        weak: !port.vendorId && !port.productId,
        connected: connectedPath === port.path && this.link.state.connected,
        label: deviceLabel({ admin, transport: 'usb', port: port.path }),
      };
    });
  }

  /**
   * 列出候选设备（设置页「设备」分组 / P10）。
   *
   * @param {{refresh?:boolean, probe?:boolean}} opts
   *   refresh=true 重新枚举串口；probe=true 顺便逐个开串口问 adminid
   *   （当前已连接的口不再打开，身份直接取链路状态 —— 同一个口开两次会冲突）。
   */
  async listDevices({ refresh = false, probe = false } = {}) {
    if (this.scanning) return this.scanning;
    const fresh = refresh || Date.now() - this.candidatesAt > 3000 || !this.candidates.length;
    if (!fresh && !probe) return this.#deviceView();
    this.scanning = (async () => {
      try {
        if (fresh) {
          const ports = await this.discover();
          this.#rememberCandidates(ports);
        }
        if (probe) await this.#probeCandidates();
        return this.#deviceView();
      } finally {
        this.scanning = null;
      }
    })();
    return this.scanning;
  }

  /** 逐个候选口开串口问 adminid（P5 的逐口探活）；当前连接口跳过。 */
  async #probeCandidates() {
    const connectedPath = this.link.state.connected ? this.link.path : null;
    for (const port of this.candidates) {
      if (connectedPath && port.path === connectedPath) continue;
      const known = this.portIdentity.get(port.path);
      if (known) continue; // 上次已经问出来过，不重复开
      const admin = await this.probePort(port.path).catch(() => '');
      this.portIdentity.set(port.path, admin);
      if (admin) this.#registerSeen(admin, 'usb');
    }
    this.candidatesAt = Date.now();
  }

  #deviceView() {
    const bound = normalizeAdminId(this.appliedDeviceId);
    const admin = normalizeAdminId(this.deviceId);
    return {
      platform: {
        os: this.platform,
        kind: osKind(this.platform),
        name: osLabel(this.platform),
        flash: flashSupported(this.platform),
      },
      bound,
      current: admin,
      connected: this.link.state.connected,
      source: this.deviceSource,
      paused: this.manualPaused,
      candidates: this.#candidatesView(),
      knownDevices: this.#knownDevicesView(),
      scannedAt: this.candidatesAt,
    };
  }

  /* ------------------------------------------------------------ 设置页动作 */

  /**
   * 「重连」：清掉"哪些口是哪块表"的缓存，按当前绑定重新挑口。
   * （缓存清掉是必要的：用户可能刚换了线/换了表。）
   */
  async reconnectNow() {
    this.manualPaused = false;
    clearTimeout(this.pauseTimer);
    this.pauseTimer = null;
    this.lastSelection = '';
    this.lastIdentityWarn = '';
    this.portIdentity.clear();
    this.appliedDeviceId = normalizeAdminId(this.config.deviceId);
    clearTimeout(this.openTimer);
    this.openTimer = null;
    await this.link.close().catch(() => {});
    const opened = await this.#tryOpen();
    /* USB 打不开 ≠ 连不上表：没插线时 BLE 才是可用通道。这里清掉"永久禁用"缓存并
     * 顺手把桥拉起来，否则 resume/重连 会误报"没找到手表"（2026-11-22 用户反馈）。 */
    this.ble.resetRuntime?.();
    if (!opened && this.config.ble.enabled !== false) {
      this.ble.config = this.config;
      void this.ble.start().catch(() => {});
    }
    const bleActive = Boolean(this.ble.connected || this.ble.starting || this.ble.scanning || this.ble.timer);
    return {
      opened,
      ble: bleActive,
      bleReason: bleActive ? '' : String(this.ble.runtimeReason ?? ''),
      state: this.#deviceView(),
    };
  }

  /** 「断开」：关串口/停 BLE 并暂停自动重连（直到重连/改设置）。 */
  async manualDisconnect() {
    this.manualPaused = true;
    clearTimeout(this.openTimer);
    this.openTimer = null;
    await this.link.close().catch(() => {});
    await Promise.race([this.ble.stop(), sleep(1500)]).catch(() => {});
    this.logger.info('用户手动断开手表（暂停自动重连）');
    return { state: this.#deviceView() };
  }

  /**
   * 「把串口让出来 N 秒」——给台架脚本/串口助手用。
   *
   * 为什么需要：插件在宿主进程内常驻，会一直占着 /dev/tty.usbmodem*（或 BLE 连接）。
   * 想用 CLI 脚本、`ble-e2e`、串口助手或者自定义工具时，不先让开就会撞"口被占用"，
   * 而且插件每 1.5 s 还会自己重连一次，把刚空出来的口又抢回去。
   *
   * 到期自动恢复：脚本崩了/忘了恢复也不会把插件永久挂起。
   */
  async pauseForScript(seconds = 120) {
    const bounded = Math.min(3600, Math.max(5, Math.round(Number(seconds) || 120)));
    await this.manualDisconnect();
    clearTimeout(this.pauseTimer);
    this.pauseTimer = setTimeout(() => {
      this.pauseTimer = null;
      if (!this.manualPaused || this.disposed) return;
      this.logger.info(`台架暂停 ${bounded}s 到期，自动恢复连接`);
      void this.reconnectNow().catch(() => {});
    }, bounded * 1000);
    this.pauseTimer.unref?.();
    this.logger.info(`已让出串口/BLE，${bounded}s 后自动恢复（也可手动 resume）`);
    return { seconds: bounded, paused: true, state: this.#deviceView() };
  }

  /** 「震动识别」：让当前这块表震一下，确认在操作哪一块。 */
  async identify() {
    if (!this.link.state.connected && !this.ble.connected) {
      return { ok: false, error: '需要连接手表' };
    }
    await this.#buzz(320);
    return { ok: true, device: this.#deviceView() };
  }

  /** 「重置缓存」：调试用（换表保护已自动做，不要求用户手动点）。 */
  resetCaches() {
    this.lastAppliedBrightness = null;
    this.lastAppliedAlwaysOn = null;
    this.fb?.invalidate();
    this.#paintSoon(0);
    this.logger.info('已重置下发缓存：下个 tick 重发亮度/常亮/整屏');
    return { ok: true };
  }

  /**
   * 烧录前暂停面板：关串口、停 BLE、暂停 USB 重连。
   * 返回烧录后应等待重新出现的应用口路径。
   */
  /**
   * 烧录目标判定：已连应用口 → app；否则枚举串口找唯一的 ROM 下载口。
   * 全新区板子停在下载模式时，用户不需要先知道 COM 号。
   * @returns {Promise<{mode:'app'|'rom'|'none', port:string, error:string}>}
   */
  async resolveFlashTarget() {
    let ports;
    try {
      ports = await this.discover();
    } catch (error) {
      return { mode: 'none', port: '', error: `枚举串口失败：${error.message}` };
    }
    const list = (Array.isArray(ports) ? ports : [])
      .filter((p) => p && typeof p.path === 'string' && p.path);
    const isRom = (p) => !/MI_00/i.test(String(p?.pnpId ?? ''));
    const connectedPort = this.link.state.connected ? this.link.path : null;
    const connectedInfo = connectedPort ? list.find((p) => p.path === connectedPort) : null;
    if (connectedPort && connectedInfo && isRom(connectedInfo)) {
      /* 插件可能已经把 ROM 口当手表连上了（未绑定/未知身份容忍）；按 ROM 直烧，不能走 -AppPort。 */
      return { mode: 'rom', port: connectedPort, error: '' };
    }
    if (connectedPort && !connectedInfo && !this.link.state.adminId) {
      /* 连上了但枚举不到这条口、也没应答 adminid：更像 ROM 下载模式；先按 ROM 直烧。 */
      return { mode: 'rom', port: connectedPort, error: '' };
    }
    const roms = list.filter(isRom);
    if (connectedPort) {
      if (roms.length > 0) {
        return {
          mode: 'none',
          port: '',
          error: `同时检测到应用口 ${connectedPort} 和下载口 ${roms.map((p) => p.path).join(', ')}：请只插要烧的板子，或显式传 romPort`,
        };
      }
      return { mode: 'app', port: connectedPort, error: '' };
    }
    if (roms.length === 1) return { mode: 'rom', port: roms[0].path, error: '' };
    if (roms.length > 1) {
      return {
        mode: 'none',
        port: '',
        error: `检测到多个下载模式端口（${roms.map((p) => p.path).join(', ')}），请显式传 romPort`,
      };
    }
    return {
      mode: 'none',
      port: '',
      error: '没有检测到下载模式端口：请确认板子已进下载模式、只插了这一块，或显式传 romPort',
    };
  }

  /** 把"烧录中"落盘，宿主重启后据此只认原应用口。 */
  #persistFlashHold() {
    try {
      const file = flashHoldFile();
      fs.mkdirSync(path.dirname(file), { recursive: true });
      fs.writeFileSync(file, JSON.stringify({ appPort: this.appPortBeforeFlash ?? '', at: Date.now() }));
    } catch { /* 落盘失败不影响烧录本身 */ }
  }

  #dropFlashHoldFile() {
    try { fs.unlinkSync(flashHoldFile()); } catch { /* 没有就算了 */ }
  }

  /** 启动时接回上次未收尾的烧录状态（过期或没有记录就直接忽略）。 */
  #restoreFlashHold() {
    try {
      const raw = JSON.parse(fs.readFileSync(flashHoldFile(), 'utf8'));
      if (!raw || Date.now() - Number(raw.at ?? 0) > FLASH_HOLD_TTL_MS) {
        this.#dropFlashHoldFile();
        return;
      }
      this.flashHold = true;
      this.appPortBeforeFlash = raw.appPort || null;
      this.logger.info(`上次烧录未收尾：只等原应用口 ${this.appPortBeforeFlash ?? '（未记录）'} 回来`);
    } catch { /* 没有记录：正常启动 */ }
  }

  async suspendForFlash() {
    this.flashHold = true;
    this.appPortBeforeFlash = this.link.path || this.config.port || null;
    this.#persistFlashHold();
    this.logger.info(`烧录：暂停面板链路（原应用口 ${this.appPortBeforeFlash ?? '自动发现'}）`);
    await this.link.close().catch(() => {});
    clearTimeout(this.openTimer);
    this.openTimer = null;
    await Promise.race([this.ble.stop(), sleep(1500)]);
    return this.appPortBeforeFlash;
  }

  /**
   * 烧录结束（成功 / 失败 / 取消）：**一律保持 flashHold**，等原应用口回来再放开。
   *
   * 2026-11-21 实机踩过：失败后这里把 flashHold 清掉，重连循环立刻发现"有个手表口"
   * （BL616 的 ROM 下载口也是 VID 349B/PID 6160，和应用口**完全一样**，
   * `isWatchPort()` 分辨不出来），于是把 ROM 口打开占住 —— 下一次烧录直接
   * `Resource busy`，而且必须重启宿主才能释放。
   *
   * `#tryOpen()` 的 flashHold 分支是按 `appPortBeforeFlash` 精确路径匹配的，
   * 所以保持 flashHold 既不会误抓 ROM 口，也不会影响"烧录前就失败、板子还在应用模式"
   * 的情况（原路径就是当前路径，下一轮立刻重连）。
   */
  resumeAfterFlash() {
    this.warnedFlashWait = false;
    this.logger.info('烧录结束：保持等待原应用口（烧录后需手动拨动开关重启 SW2 ON→OFF→ON）');
    this.#tryOpen();
  }

  /* ------------------------------------------------------- 表盘（P8/P9） */

  /** shell 命令包装：和像素帧/亮度共用同一个写队列，保证不交错。 */
  #faceCommand(line, opts) {
    return this.#command(() => this.link.command(line, opts));
  }

  /** 表盘操作要先离开电脑模式，否则屏幕上还是电脑模式的像素画面。 */
  async #leavePcModeIfNeeded() {
    this.faceHold = true;
    if (!this.link.state.pcMode && !this.link.state.passthrough) return false;
    await this.#command(() => this.link.shell('pcmode 0')).catch(() => {});
    await this.#waitState('pcMode', false, 3000);
    this.logger.info('表盘操作：已退出电脑模式（手表模式）');
    return true;
  }

  /** 主动切回手表模式（幂等；设置页「表盘」操作前会调，也可由路由直接调）。 */
  async exitPcMode() {
    if (!this.link.state.connected) return { ok: false, error: '需要连接手表' };
    const changed = await this.#leavePcModeIfNeeded().catch(() => false);
    return { ok: true, changed, pcMode: this.link.state.pcMode, state: this.stateForApi() };
  }

  /** 读图片表盘槽状态（shell `faceimg`）。withCrc=true 时固件会读回 231 KB 重算 CRC（较慢）。 */
  async faceStatus({ withCrc = false } = {}) {
    const base = {
      supported: this.link.state.connected,
      current: this.faceId,
      valid: false,
      crc: '',
      layout: null,
      withCrc,
    };
    if (!this.link.state.connected) {
      return { ...base, error: '需要 USB 连接（BLE 不支持表盘安装）' };
    }
    const line = await this.#faceCommand(withCrc ? 'faceimg info' : 'faceimg', {
      match: /\[faceimg\]/,
      timeoutMs: withCrc ? 15000 : 3000,
    }).catch(() => null);
    if (!line) return { ...base, error: '手表没有回应 faceimg' };
    const validMatch = /\[faceimg\] valid=(\d+)/.exec(line);
    if (!validMatch) return { ...base, error: `无法解析 faceimg：${line}` };
    if (validMatch[1] !== '1') {
      const rc = /rc=(-?\d+)(?: \(([^)]+)\))?/.exec(line);
      return { ...base, valid: false, rc: rc ? Number(rc[1]) : null, reason: rc?.[2] ?? '', raw: line };
    }
    const info = /crc=([0-9A-Fa-f]+) len=(\d+) w=(\d+) h=(\d+) time=\((-?\d+),(-?\d+)\) font=(\d+) flags=(\d+) color=([0-9A-Fa-f]+)/.exec(line);
    return {
      ...base,
      valid: true,
      crc: info ? info[1].toUpperCase() : '',
      len: info ? Number(info[2]) : null,
      width: info ? Number(info[3]) : null,
      height: info ? Number(info[4]) : null,
      layout: info
        ? {
          timeX: Number(info[5]),
          timeY: Number(info[6]),
          fontId: Number(info[7]),
          flags: Number(info[8]),
          color: parseInt(info[9], 16),
        }
        : null,
      raw: line,
    };
  }

  /**
   * 切表盘：只支持 **0（默认）/ 3（图片）**（research/76 §1：1/2 已从固件删除，
   * 固件会直接拒绝；插件也拒绝，避免"点了没反应"）。
   */
  async faceSwitch(id) {
    const faceId = normalizeFaceId(id);
    if (faceId === null) return { ok: false, error: '只支持 0（默认）/ 3（图片）表盘' };
    if (!this.link.state.connected) return { ok: false, error: '需要 USB 连接' };
    /* 2026-11-14：USB 电脑模式下切表盘必须临时退出（face 重建会切页），
     * 切完自动回电脑模式，不再让用户"只能重新连接"。 */
    const wasPc = Boolean(this.link.state.pcMode || this.link.state.passthrough);
    await this.#leavePcModeIfNeeded().catch(() => {});
    /* 刚退出 PC 模式时文本/二进制包边界在切换，第一条 shell 命令可能丢；
     * 实测第一次点表盘偶发超时，这里留 250 ms 并重试一次。 */
    await sleep(250);
    let reply = await this.#faceCommand(`face ${faceId}`, {
      match: /face -> \d/,
      timeoutMs: 3000,
    }).catch(() => null);
    if (!reply) {
      reply = await this.#faceCommand(`face ${faceId}`, {
        match: /face -> \d/,
        timeoutMs: 3000,
      }).catch(() => null);
    }
    if (!reply) return { ok: false, error: '手表没有回应 face 命令' };
    this.faceId = faceId;
    this.fb?.invalidate();
    this.logger.info(`表盘切到 ${faceId}（0=默认 3=图片）`);
    await this.#restorePcAfterFaceOp(wasPc, '表盘切换完成');
    return { ok: true, id: faceId, reply, state: this.stateForApi() };
  }

  /** 图片表盘安装任务视图（设置页轮询）。 */
  faceInstallView() {
    const job = this.faceJob;
    if (!job) return { running: false, stage: 'idle', ok: false, percent: 0, error: null, supported: this.link.state.connected };
    return {
      id: job.id,
      running: job.running,
      stage: job.stage,
      ok: job.ok,
      percent: job.percent,
      error: job.error,
      warning: job.warning ?? null,
      seconds: job.seconds,
      crc: job.crc,
      apply: job.apply,
      supported: this.link.state.connected,
      logTail: job.logTail.slice(-6).join('\n'),
    };
  }

  /** 开始安装图片表盘（PFI = 256 B header + 231,424 B blob；路由负责 PNG→PFI）。 */
  async faceInstallStart(pfi, { apply = true } = {}) {
    if (!this.link.state.connected) {
      return { ok: false, error: '需要 USB 连接（BLE 不支持表盘安装）' };
    }
    if (this.faceJob?.running) return { ok: false, error: 'face-busy', job: this.faceInstallView() };
    /* 图片表盘和下半区面板共用一条 shell 链路：一次只能有一个在写，否则命令会交错。 */
    if (this.panelJob?.running) {
      return { ok: false, error: 'face-busy（默认表盘下半区正在写入）', job: this.faceInstallView() };
    }
    let parsed;
    try {
      parsed = parsePfi(Buffer.from(pfi));
    } catch (error) {
      return { ok: false, error: `PFI 无效：${error.message}` };
    }
    if (!parsed.ok) return { ok: false, error: 'PFI 校验失败（header/blob CRC 不一致）' };
    const job = {
      id: `face-${++this.faceSeq}-${Date.now().toString(36)}`,
      warning: null,
      running: true,
      stage: 'starting',
      ok: false,
      percent: 0,
      error: null,
      seconds: 0,
      startedAt: Date.now(),
      crc: parsed.imageCrc32.toString(16).toUpperCase().padStart(8, '0'),
      apply,
      cancelRequested: false,
      settle: null,
      logTail: [],
    };
    job.finished = new Promise((resolve) => {
      job.settle = resolve;
    });
    this.faceJob = job;
    void this.#faceInstallRun(job, Buffer.from(pfi), parsed, apply);
    return { ok: true, jobId: job.id, job: this.faceInstallView() };
  }

  /** 启动并等待安装结束（agent 工具用）。 */
  async faceInstallAndWait(pfi, { apply = true } = {}) {
    const started = await this.faceInstallStart(pfi, { apply });
    if (!started.ok) return { ...started, view: this.faceInstallView() };
    await this.faceJob.finished;
    return { ok: true, view: this.faceInstallView() };
  }

  /** 取消正在进行的图片安装（上传循环会在下一片前退出）。 */
  faceInstallCancel() {
    const job = this.faceJob;
    if (!job?.running) return false;
    job.cancelRequested = true;
    job.stage = 'cancelling';
    return true;
  }

  /** 图片表盘上传状态机（48 B/行，逐行等 W-OK —— shell 环形缓冲只有 512 B）。 */
  async #faceInstallRun(job, pfi, parsed, apply) {
    const IMAGE_LEN = 231424;
    const CHUNK = 48;
    const layout = parsed.layout;
    const colorHex = layout.color.toString(16).toUpperCase().padStart(6, '0');
    const crcHex = parsed.imageCrc32.toString(16).toUpperCase().padStart(8, '0');
    const blob = pfi.subarray(256);
    const wasPc = Boolean(this.link.state.pcMode || this.link.state.passthrough);
    try {
      job.stage = 'leaving-pc';
      await this.#leavePcModeIfNeeded().catch(() => {});
      job.stage = 'probe';
      /* 刚退出电脑模式时第一条 shell 命令可能被吞（faceSwitch、面板安装都踩过）。
       * 2026-11-21 实机：第一次装图片表盘就因为这个直接失败，第二次才成功。
       * 这里和面板安装保持一致：留 250 ms，问不到再重试一次。 */
      await sleep(250);
      let probe = await this.#faceCommand('faceimg', { match: /\[faceimg\]/, timeoutMs: 3000 });
      if (!probe) probe = await this.#faceCommand('faceimg', { match: /\[faceimg\]/, timeoutMs: 3000 });
      if (!probe) throw new Error('手表没有回应 faceimg');
      job.stage = 'erase';
      const erase = await this.#faceCommand('faceimg erase', { match: /erase /, timeoutMs: 15000 });
      if (!erase || !/erase ok/.test(erase)) throw new Error(erase ? `擦除失败：${erase}` : '擦除超时');
      job.stage = 'begin';
      const begin = await this.#faceCommand(`faceimg begin ${IMAGE_LEN} ${crcHex}`, {
        match: /begin len=/,
        timeoutMs: 6000,
      });
      if (!begin || !/ begin len=\d+ crc=[0-9A-Fa-f]+ ok/.test(begin)) {
        throw new Error(begin ? `begin 被拒：${begin}` : 'begin 超时（手表在下载模式？）');
      }
      job.stage = 'upload';
      for (let off = 0; off < IMAGE_LEN; off += CHUNK) {
        if (job.cancelRequested) throw new Error('用户取消安装');
        const chunk = blob.subarray(off, Math.min(off + CHUNK, IMAGE_LEN));
        const cmd = `faceimg w ${off} ${chunk.toString('hex').toUpperCase()}`;
        let line = await this.#faceCommand(cmd, { match: /W-OK|W-ERR/, timeoutMs: 2500 });
        if (!line) line = await this.#faceCommand(cmd, { match: /W-OK|W-ERR/, timeoutMs: 4000 });
        if (!line || line.includes('W-ERR')) throw new Error(line || `写 ${off} 字节超时`);
        job.percent = Math.round(((off + chunk.length) / IMAGE_LEN) * 1000) / 10;
        job.logTail.push(`w ${off}`);
        if (job.logTail.length > 20) job.logTail.shift();
        if ((off / CHUNK) % 64 === 0) await sleep(0);
      }
      job.stage = 'end';
      const endLine = await this.#faceCommand(
        `faceimg end ${layout.timeX} ${layout.timeY} ${layout.fontId} ${layout.flags} ${colorHex}`,
        { match: /install (OK|ERR)/, timeoutMs: 12000 },
      );
      if (!endLine || !/install OK/.test(endLine)) throw new Error(endLine || 'install end 超时');
      job.stage = 'verify';
      let info = null;
      for (let attempt = 1; attempt <= 3 && !info; attempt += 1) {
        info = await this.#faceCommand('faceimg info', {
          match: /\[faceimg\]/,
          timeoutMs: attempt === 1 ? 30000 : 45000,
        }).catch(() => null);
        if (!info) {
          job.stage = `verify-retry-${attempt}`;
          await sleep(1500);
        }
      }
      if (!info) {
        job.warning = '设备侧 CRC 复核超时（安装已写入，表盘应已更新）';
        this.logger.warn(`图片表盘安装：${job.warning}`);
        job.ok = true;
        job.stage = 'done';
        job.percent = 100;
        job.crc = crcHex;
        if (apply) this.faceId = 3;
        return;
      }
      if (!/valid=1/.test(info)) throw new Error(info);
      const deviceCrc = /crc=([0-9A-Fa-f]{8})/.exec(info);
      if (!deviceCrc || deviceCrc[1].toUpperCase() !== crcHex) {
        throw new Error(`设备 CRC ${deviceCrc?.[1] ?? '?'} 与本地 ${crcHex} 不一致`);
      }
      job.crc = crcHex;
      if (apply) {
        job.stage = 'apply';
        const applied = await this.#faceCommand('faceimg apply', { match: /apply queued/, timeoutMs: 5000 });
        if (!applied) throw new Error('apply 超时');
        this.faceId = 3;
      }
      job.ok = true;
      job.stage = 'done';
      job.percent = 100;
      this.logger.info(`图片表盘安装成功：crc=${crcHex}（${apply ? '已切到 face 3' : '未切换'}）`);
    } catch (error) {
      job.ok = false;
      job.stage = 'error';
      job.error = error instanceof Error ? error.message : String(error);
      this.logger.warn(`图片表盘安装失败：${job.error}`);
    } finally {
      job.running = false;
      job.seconds = Math.round((Date.now() - job.startedAt) / 100) / 10;
      job.settle?.();
      if (job.ok) await this.#restorePcAfterFaceOp(wasPc, '图片表盘安装完成');
    }
  }

  /* ------------------------------------ 默认表盘下半区（research/76 §2.3） */

  /** 表盘/面板安装是否占着串口（设置页据此把两边按钮都置灰）。 */
  faceInstallBusy() {
    const face = Boolean(this.faceJob?.running);
    const panel = Boolean(this.panelJob?.running);
    return { face, panel, busy: face || panel };
  }

  /**
   * 面板槽状态视图：只报**已读到的缓存**（/state、/faces 轮询不占串口）。
   * `known=false` 表示还没问过手表，设置页应去 `GET /facepanel` 拉一次。
   */
  panelView() {
    const info = this.panelInfo;
    return {
      known: Boolean(info),
      supported: this.link.state.connected,
      valid: info?.valid === true,
      crc: info?.crc ?? '',
      len: info?.len ?? null,
      width: info?.width ?? null,
      height: info?.height ?? null,
      y: info?.y ?? null,
      checkedAt: this.panelInfoAt || 0,
      installing: Boolean(this.panelJob?.running),
      error: info?.error ?? null,
    };
  }

  /** 发一条 facepanel 命令（与像素帧/亮度/表盘共用同一个写队列）。 */
  #panelCommand(line, opts) {
    return this.#command(() => this.link.command(line, opts));
  }

  /**
   * 读面板槽状态：shell `facepanel`（不重算 CRC）或 `facepanel info`（固件读回 112,384 B 重算）。
   *
   * 离线/无回包/尺寸不符都写进 `panelInfo` 并返回 `error`，不抛错 —— 设置页只读状态。
   */
  async panelStatus({ withCrc = false } = {}) {
    const base = {
      supported: this.link.state.connected,
      valid: false,
      crc: '',
      len: null,
      width: null,
      height: null,
      y: null,
      withCrc,
      checkedAt: Date.now(),
      error: null,
    };
    const remember = (value) => {
      this.panelInfo = { ...value, checkedAt: Date.now() };
      this.panelInfoAt = this.panelInfo.checkedAt;
      return { ...value, checkedAt: this.panelInfoAt };
    };
    if (!this.link.state.connected) {
      return remember({ ...base, error: '需要 USB 连接（BLE 不支持下半区面板）' });
    }
    /* 上传中不要插状态查询：`facepanel w` 失败的 W-ERR 行也带 `[facepanel]` 前缀，
     * 并发查询可能把 W-ERR 当成状态行读走。上传时直接回缓存（进度走 /install/state）。 */
    if (this.panelJob?.running) {
      return { ...this.panelView(), withCrc: false, cached: true, error: null };
    }
    /* 只认带 `valid=` 的状态行：固件在 install/clear 后会多打一行
     * `[facepanel] apply queued (rebuild face 0)`，不能让它被当成状态应答。 */
    const line = await this.#panelCommand(withCrc ? PANEL_CMD_INFO : PANEL_CMD, {
      match: /\[facepanel\] valid=\d/,
      timeoutMs: withCrc ? 15000 : 3000,
    }).catch(() => null);
    if (!line) {
      return remember({ ...base, error: '手表没有回应 facepanel（固件可能还没更新到 76 版）' });
    }
    const parsed = parsePanelStatus(line);
    if (!parsed) return remember({ ...base, error: `无法解析 facepanel：${line}`, raw: line });
    if (parsed.valid && !panelStatusMatchesContract(parsed)) {
      return remember({
        ...parsed,
        supported: true,
        withCrc,
        error: `面板尺寸不符合契约（len=${parsed.len} ${parsed.width}×${parsed.height} y=${parsed.y}；`
          + `期望 ${PANEL_IMAGE_LEN} 480×232 y=248）`,
      });
    }
    return remember({ ...parsed, supported: true, withCrc, error: null });
  }

  /** 面板安装任务视图（设置页轮询；字段与图片表盘保持同构）。 */
  panelInstallView() {
    const job = this.panelJob;
    if (!job) {
      return {
        running: false, stage: 'idle', ok: false, percent: 0, error: null, supported: this.link.state.connected,
      };
    }
    return {
      id: job.id,
      running: job.running,
      stage: job.stage,
      ok: job.ok,
      percent: job.percent,
      error: job.error,
      warning: job.warning ?? null,
      seconds: job.seconds,
      crc: job.crc,
      supported: this.link.state.connected,
      logTail: job.logTail.slice(-6).join('\n'),
    };
  }

  /**
   * 开始安装默认表盘下半区图（112,384 B I8 blob；路由负责 PNG → blob）。
   *
   * 与图片表盘安装**共用同一把串口锁**：任一方在跑时另一方直接 `face-busy`。
   */
  async panelInstallStart(blob) {
    if (!this.link.state.connected) {
      return { ok: false, error: '需要 USB 连接（BLE 不支持下半区面板）' };
    }
    if (this.panelJob?.running) return { ok: false, error: 'panel-busy', job: this.panelInstallView() };
    if (this.faceJob?.running) {
      return { ok: false, error: 'face-busy（图片表盘正在安装）', job: this.panelInstallView() };
    }
    const buffer = Buffer.isBuffer(blob) ? Buffer.from(blob) : Buffer.from(blob ?? []);
    if (buffer.length !== PANEL_IMAGE_LEN) {
      return { ok: false, error: `面板图长度不对（${buffer.length}，期望 ${PANEL_IMAGE_LEN}）` };
    }
    const crcValue = crc32(buffer);
    const crcHex = panelCrcHex(crcValue);
    const job = {
      id: `panel-${++this.panelSeq}-${Date.now().toString(36)}`,
      warning: null,
      running: true,
      stage: 'starting',
      ok: false,
      percent: 0,
      error: null,
      seconds: 0,
      startedAt: Date.now(),
      crc: crcHex,
      crc32: crcValue,
      cancelRequested: false,
      settle: null,
      logTail: [],
    };
    job.finished = new Promise((resolve) => {
      job.settle = resolve;
    });
    this.panelJob = job;
    void this.#panelInstallRun(job, buffer, crcHex);
    return { ok: true, jobId: job.id, job: this.panelInstallView() };
  }

  /** 启动并等待结束（agent 工具/CLI 用）。 */
  async panelInstallAndWait(blob) {
    const started = await this.panelInstallStart(blob);
    if (!started.ok) return { ...started, view: this.panelInstallView() };
    await this.panelJob.finished;
    return { ok: true, view: this.panelInstallView() };
  }

  /** 取消面板安装（上传循环在下一片前退出；已写的部分不影响已安装内容）。 */
  panelInstallCancel() {
    const job = this.panelJob;
    if (!job?.running) return false;
    job.cancelRequested = true;
    job.stage = 'cancelling';
    return true;
  }

  /**
   * 面板上传状态机（与图片表盘同一套 P9 模式：48 B/行、逐行等 W-OK、end 校验、info 复核）。
   *
   *   probe  → `facepanel`（确认固件有这条命令）
   *   begin  → `facepanel begin 112384 <crc>`
   *   upload → 2342 × `facepanel w <off> <hex>`
   *   end    → `facepanel end`（固件校验 CRC → 写 header → 重建 face 0）
   *   verify → `facepanel info`（设备侧读回重算 CRC，与本地比对）
   */
  async #panelInstallRun(job, blob, crcHex) {
    const wasPc = Boolean(this.link.state.pcMode || this.link.state.passthrough);
    try {
      job.stage = 'leaving-pc';
      await this.#leavePcModeIfNeeded().catch(() => {});
      job.stage = 'probe';
      /* 刚退出电脑模式时第一条 shell 命令可能丢（faceSwitch 踩过），这里留 250 ms 并重试一次。 */
      await sleep(250);
      let probe = await this.#panelCommand(PANEL_CMD, { match: /\[facepanel\] valid=\d/, timeoutMs: 3000 });
      if (!probe) probe = await this.#panelCommand(PANEL_CMD, { match: /\[facepanel\] valid=\d/, timeoutMs: 3000 });
      if (!probe) throw new Error('手表没有回应 facepanel（固件可能还没更新到 76 版）');
      const before = parsePanelStatus(probe);
      if (before?.valid) job.previousCrc = before.crc;
      job.stage = 'begin';
      const begin = await this.#panelCommand(panelBeginLine({ crcHex }), {
        match: /begin len=/,
        timeoutMs: 6000,
      });
      if (!begin || !/begin len=\d+ crc=[0-9A-Fa-f]+ ok/i.test(begin)) {
        throw new Error(begin ? `begin 被拒：${begin}` : 'begin 超时（手表在下载模式？）');
      }
      job.stage = 'upload';
      let rows = 0;
      for (const chunk of panelChunks(blob, { chunk: PANEL_UPLOAD_CHUNK })) {
        if (job.cancelRequested) throw new Error('用户取消安装');
        let line = await this.#panelCommand(chunk.line, { match: /W-OK|W-ERR/, timeoutMs: 2500 });
        if (!line) line = await this.#panelCommand(chunk.line, { match: /W-OK|W-ERR/, timeoutMs: 4000 });
        if (!line || line.includes('W-ERR')) throw new Error(line || `写 ${chunk.off} 字节超时`);
        job.percent = Math.round(((chunk.off + chunk.len) / PANEL_IMAGE_LEN) * 1000) / 10;
        job.logTail.push(`w ${chunk.off}`);
        if (job.logTail.length > 20) job.logTail.shift();
        rows += 1;
        if (rows % 64 === 0) await sleep(0);
      }
      job.stage = 'end';
      const endLine = await this.#panelCommand(PANEL_CMD_END, { match: /install (OK|ERR)/, timeoutMs: 12000 });
      if (!endLine || !/install OK/.test(endLine)) throw new Error(endLine || 'install end 超时');
      job.stage = 'verify';
      /* 2026-11-14 真机：facepanel end 会重建 face 0，紧接着 info 要读回 110 KB
       * 复算 CRC，偶发超过 20 s 才回（shell/UI 正在重建）。重试三次；
       * 若 end 已回 install OK 但 info 始终超时，按"写入成功、复核未完成"处理，
       * 不把成功安装误报成失败。 */
      let info = null;
      for (let attempt = 1; attempt <= 3 && !info; attempt += 1) {
        info = await this.#panelCommand(PANEL_CMD_INFO, {
          match: /\[facepanel\] valid=\d/,
          timeoutMs: attempt === 1 ? 30000 : 45000,
        }).catch(() => null);
        if (!info) {
          job.stage = `verify-retry-${attempt}`;
          await sleep(1500);
        }
      }
      const parsed = parsePanelStatus(info);
      if (parsed && parsed.valid === false) {
        throw new Error(`设备侧校验失败：${info}`);
      }
      if (parsed?.valid) {
        if (parsed.crc !== crcHex) throw new Error(`设备 CRC ${parsed.crc || '?'} 与本地 ${crcHex} 不一致`);
        if (!panelStatusMatchesContract(parsed)) throw new Error('设备回报的面板尺寸/位置不符合契约');
        this.panelInfo = { ...parsed, error: null };
        this.panelInfoAt = Date.now();
      } else if (info) {
        job.warning = '设备侧复核未完成（end 已通过，表盘应已更新）';
        this.logger.warn(`下半区安装：${job.warning}：${info}`);
      } else {
        job.warning = '设备侧 CRC 复核超时（写入已由 end 校验通过，表盘应已更新）';
        this.panelInfo = null;
        this.panelInfoAt = 0;
        this.logger.warn(`下半区安装：${job.warning}`);
      }
      job.ok = true;
      job.stage = 'done';
      job.percent = 100;
      job.crc = crcHex;
      this.logger.info(`默认表盘下半区已安装：crc=${crcHex}（480×232 @ y=248，face 0 已重建）`);
    } catch (error) {
      job.ok = false;
      job.stage = 'error';
      job.error = error instanceof Error ? error.message : String(error);
      this.panelInfo = null; // 状态未知：下次面板状态查询重新问手表
      this.panelInfoAt = 0;
      this.logger.warn(`默认表盘下半区安装失败：${job.error}`);
    } finally {
      job.running = false;
      job.seconds = Math.round((Date.now() - job.startedAt) / 100) / 10;
      job.settle?.();
      if (job.ok) await this.#restorePcAfterFaceOp(wasPc, '下半区安装完成');
    }
  }

  /**
   * 恢复内置 watch photo：shell `facepanel clear`（固件只擦 header，face 0 立刻回内置图）。
   * 不做 begin/end —— 这是"撤销安装"，不是"装一张空图"。
   */
  async panelClear() {
    if (!this.link.state.connected) return { ok: false, error: '需要 USB 连接' };
    if (this.panelJob?.running) return { ok: false, error: 'panel-busy', job: this.panelInstallView() };
    if (this.faceJob?.running) return { ok: false, error: 'face-busy（图片表盘正在安装）', job: this.panelInstallView() };
    const wasPc = Boolean(this.link.state.pcMode || this.link.state.passthrough);
    await this.#leavePcModeIfNeeded().catch(() => {});
    /* 与 faceSwitch 同样的经验：刚退出电脑模式时第一条 shell 命令可能丢，重试一次。 */
    await sleep(250);
    /* 只认 `clear` 那一行：固件成功后会跟一行 `clear queued (rebuild face 0)`。 */
    let line = await this.#panelCommand(PANEL_CMD_CLEAR, { match: /\[facepanel\] clear/, timeoutMs: 6000 }).catch(() => null);
    if (!line) line = await this.#panelCommand(PANEL_CMD_CLEAR, { match: /\[facepanel\] clear/, timeoutMs: 6000 }).catch(() => null);
    if (!line) return { ok: false, error: '恢复内置 watch photo 超时' };
    /* 固件只有成功才回 `clear ok`；失败是 `clear flash error`（同 faceimg erase 风格）。 */
    if (!/clear ok/i.test(line)) return { ok: false, error: `clear 被拒：${line}` };
    this.panelInfo = {
      valid: false, crc: '', len: null, width: null, height: null, y: null, raw: line, error: null,
    };
    this.panelInfoAt = Date.now();
    this.logger.info('默认表盘下半区已清除：face 0 回内置 watch photo');
    await this.#restorePcAfterFaceOp(wasPc, '下半区清除完成');
    return { ok: true, reply: line, panel: this.panelView() };
  }

  /** 表盘/下半区操作（会临时退出电脑模式）完成后，自动回到电脑模式。 */
  async #restorePcAfterFaceOp(wasPc, label) {
    if (!wasPc || !this.link.state.connected) return false;
    const restored = await this.enterPcMode().catch(() => null);
    this.logger.info(restored?.ok ? `${label}：已自动回到电脑模式` : `${label}：留在手表模式`);
    return Boolean(restored?.ok);
  }

  /** 从手表模式回到电脑模式（表盘操作后用户可能还想继续用副屏）。 */
  async enterPcMode() {
    if (!this.link.state.connected) return { ok: false, error: '需要连接手表' };
    this.faceHold = false;
    const entered = await this.#enterPcModeWithRetry();
    if (entered && !this.link.state.passthrough) {
      await sleep(220);
      for (let attempt = 1; attempt <= 3 && !this.link.state.passthrough; attempt += 1) {
        await this.#command(() => this.link.sayHello()).catch(() => {});
        if (await this.#waitState('passthrough', true, 1500)) break;
        await sleep(300);
      }
    }
    if (this.link.state.passthrough) {
      this.fb?.invalidate();
      await this.#applyBrightness({ force: true }).catch(() => {});
      this.#paintSoon(0);
    }
    return {
      ok: this.link.state.passthrough,
      pcMode: this.link.state.pcMode,
      passthrough: this.link.state.passthrough,
      state: this.stateForApi(),
    };
  }

  /* ------------------------------------------------------------ 刷新循环 */

  #scheduleNext() {
    if (this.disposed) return;
    clearTimeout(this.timer);
    const fps = Math.max(1, Math.min(5, this.config.fps));
    this.timer = setTimeout(() => this.#run(), Math.max(100, Math.round(1000 / fps)));
    this.timer.unref?.();
  }

  #run() {
    this.timer = null;
    if (this.disposed || this.busy) return;
    this.busy = true;
    this.#tick()
      .catch((error) => this.logger.debug?.(`面板刷新异常：${error.message}`))
      .finally(() => {
        this.busy = false;
        this.#scheduleNext();
      });
  }

  /** 测试/调试：跑一次面板 tick（验证 pc=0 时不发像素等）。 */
  tickForTest() {
    return this.#tick();
  }

  /** 状态变化时立刻补一帧（审批/问答/触摸不能等下一个 500 ms）。 */
  #paintSoon(delay = 0) {
    if (this.disposed) return;
    // 卡片/审批/问答状态变了：BLE 要在同一时刻看到（不能等下一个 500 ms tick）
    this.#blePublish(Date.now(), { force: true });
    clearTimeout(this.soonTimer);
    this.soonTimer = setTimeout(() => {
      this.soonTimer = null;
      if (!this.busy) this.#run();
    }, Math.max(0, delay));
    this.soonTimer.unref?.();
  }

  async #tick() {
    this.config = normalizeConfig(this.rawConfig); // 设置界面改了 volatile 字段要即时生效
    const now = Date.now();
    this.#refreshFocus(now);
    // BLE 卡片层：标题/步数/CTX/倒计时都在这里追平（USB 在时是空操作）
    this.#blePublish(now);

    // 设置页改了固定串口：关掉旧链路，按新设置重开
    if (this.appliedPort === undefined) this.appliedPort = this.config.port;
    else if (this.appliedPort !== this.config.port) {
      this.appliedPort = this.config.port;
      this.flashHold = false;
      this.manualPaused = false;
      this.logger.info(`串口设置变化 → ${this.config.port || '自动发现'}，重开链路`);
      await this.link.close().catch(() => {});
      this.#tryOpen();
    }

    // 设置页改了 USB 连接开关：关掉立刻断开串口（探测注册仍可用）
    const usbEnabled = this.config.usb.enabled !== false;
    if (usbEnabled !== this.appliedUsbEnabled) {
      this.appliedUsbEnabled = usbEnabled;
      this.logger.info(`USB 连接开关：${usbEnabled ? '开' : '关'}`);
      if (!usbEnabled) {
        await this.link.close().catch(() => {});
        this.lastSelection = '';
      } else {
        this.manualPaused = false;
        this.#scheduleOpen();
      }
    }

    /* 设置页改了蓝牙连接开关：关掉必须**立刻 stop()**，不能只是"下一次不再 start"。
     * 残留的 startScanning 会继续收广播，`#onDiscover` 命中后还会自己连上（2026-11-22 实测），
     * 用户看着开关是关的，射频却一直在扫。 */
    const bleEnabled = this.config.ble.enabled !== false;
    if (bleEnabled !== this.appliedBleEnabled) {
      this.appliedBleEnabled = bleEnabled;
      this.logger.info(`蓝牙连接开关：${bleEnabled ? '开' : '关'}`);
      if (!bleEnabled) {
        // 先把新配置交给 bridge，再停：避免这 500 ms 内残留广播按旧配置连上
        this.ble.config = this.config;
        await this.ble.stop().catch(() => {});
      } else {
        // 用户重新打开通道 = 想连：清掉「断开」暂停 + "永久禁用"缓存，让 tick 重新探测
        this.ble.resetRuntime?.();
        this.manualPaused = false;
      }
    }

    // 设置页改了设备绑定（P6/P7）：按新绑定重新挑口；连着的正是绑定表就不折腾
    const bound = normalizeAdminId(this.config.deviceId);
    if (bound !== this.appliedDeviceId) {
      const previous = this.appliedDeviceId;
      this.appliedDeviceId = bound;
      this.manualPaused = false;
      this.lastSelection = '';
      this.lastIdentityWarn = '';
      this.portIdentity.clear(); // 换绑后"哪个口是哪块表"的缓存不再可信
      this.ble.lastAddress = null; // BLE 直连缓存属于旧表
      this.logger.info(`设备绑定变化：${previous || '未绑定'} → ${bound || '未绑定'}`);
      const keepConnected = this.link.state.connected
        && (!bound || bound === normalizeAdminId(this.deviceId));
      if (!keepConnected) {
        await this.link.close().catch(() => {});
        this.#tryOpen();
      }
      /* BLE 只能连一个：绑定变了又正连着别的表 → 停掉，让它按新绑定重扫
       * （bridge 的广播名过滤读 config.deviceId，下一次 start() 会拿到新值）。 */
      if (bound && this.ble.connected
        && normalizeAdminId(this.deviceId) !== bound) {
        await this.ble.stop().catch(() => {});
      }
    }

    if (!this.link.state.connected) {
      if (this.manualPaused) {
        // 用户点了「断开」：不探测、不起 BLE，等「重连」或改设置
        // scanning 也算在内：否则"暂停"只是不再新起扫描，已经在跑的照旧收广播。
        if (this.ble.connected || this.ble.starting || this.ble.scanning) this.ble.stop();
        return;
      }
      // 断开期间：USB 开关开才探测；BLE 开关开才起桥（失败由 tick 重试）
      if (usbEnabled) this.#scheduleOpen();
      this.ble.config = this.config;
      if (!this.config.ble.enabled) {
        if (this.ble.connected || this.ble.starting || this.ble.scanning) this.ble.stop();
      } else if (now - this.startedAt > 1500 && now - (this.lastConnectedAt ?? 0) > 2500) {
        this.ble.start().catch((error) => this.logger.debug?.(`BLE 降级：${error.message}`));
      }
      return;
    }
    this.lastConnectedAt = now;
    this.deviceSource = 'usb';
    if (this.ble.connected || this.ble.child || this.ble.starting) this.ble.stop();

    // pcMode=0（用户左滑退出）时手表在本地页，发像素既没意义还会污染本地 UI；
    // passthrough=0 同理。两个条件都满足才允许 blit。
    if (!this.link.state.pcMode || !this.link.state.passthrough) return;

    // 设置页改了亮度：500 ms 内下发（0x02），不用重启
    if (this.lastAppliedBrightness !== this.config.display.brightness) {
      await this.#applyBrightness().catch(() => {});
    }
    if (this.lastAppliedAlwaysOn !== this.#displayFor('usb').alwaysOn) {
      await this.#applyAlwaysOn().catch(() => {});
    }
    if (this.lastAppliedScreenOff !== this.#displayFor('usb').screenOffSec) {
      await this.#applyScreenOff().catch(() => {});
    }

    if (this.notice && now >= this.notice.until) {
      this.notice = null;
      clearTimeout(this.noticeTimer);
      this.noticeTimer = null;
      this.fb.invalidate();
    }
    const image = this.#renderFrame(now);
    this.fb.setFromRGBA(image.rgba);
    const result = await this.#enqueue(async () => {
      if (!this.link.state.connected || !this.link.state.passthrough) return null;
      return this.flusher.flush();
    });
    if (result && result.bands > 0) {
      this.lastFrameAt = Date.now();
      this.logger.debug?.(`dirty=${result.bands} bytes=${result.bytes} ${result.ms.toFixed(1)} ms`);
    }
  }

  #panelState(now = Date.now()) {
    const focus = this.#sessionCardView(now);
    const date = new Date(now);
    const power = this.status;
    return {
      time: hhmm(date),
      date: ymd(date),
      battery: Number.isFinite(power.battery) ? power.battery : null,
      balance: this.balance?.publicView?.().text ?? null,
      footer: '',
      flashPct: -1,
      metrics: this.metrics ?? {},
      connection: 'usb',
      session: {
        state: focus.state,
        title: focus.title,
        tool: focus.tool,
        steps: focus.steps,
        ctx: focus.ctx,
      },
      approval: this.approval ? this.#approvalView() : null,
      question: this.question ? this.#questionView() : null,
    };
  }

  /** 当前屏幕帧：通知卡优先，否则监控面板。 */
  #renderFrame(now = Date.now()) {
    if (this.notice) return this.renderer.drawNotification(this.notice);
    return this.renderer.draw(this.#panelState(now), this.tokens);
  }

  /** 撤掉通知卡并让下一帧整屏重绘（审批/问答卡优先）。 */
  #dropNotice() {
    if (!this.notice) return;
    this.notice = null;
    clearTimeout(this.noticeTimer);
    this.noticeTimer = null;
    this.fb?.invalidate();
  }

  /**
   * 在手表上显示一条限时通知（USB 像素副屏）。
   * BLE 没有像素通道，只发震动；审批/问答卡在屏时直接返回忙，不抢卡。
   */
  async showNotification({ title = '', body = '', durationSec = 5, vibrate = true } = {}) {
    const textTitle = String(title ?? '').trim().slice(0, 24);
    const textBody = String(body ?? '').trim().slice(0, 100);
    if (!textTitle && !textBody) return { ok: false, error: '标题/正文不能都为空' };
    if (this.overlayBusy()) {
      return { ok: false, shown: false, via: 'usb', durationSec: 0, vibrated: false, error: '审批/问答卡正在显示，通知暂缓' };
    }
    if (!this.link.state.connected) {
      if (this.ble.connected) {
        if (vibrate) await this.vibrate({ ms: 120, times: 1 }).catch(() => {});
        return { ok: true, shown: false, via: 'ble', durationSec: 0, vibrated: !!vibrate, error: null, note: 'BLE 没有像素通道，只发震动' };
      }
      return { ok: false, shown: false, via: 'none', durationSec: 0, vibrated: false, error: '手表未连接' };
    }
    if (!this.link.state.pcMode || !this.link.state.passthrough) {
      return { ok: false, shown: false, via: 'usb', durationSec: 0, vibrated: false, error: '手表不在电脑模式' };
    }
    if (!this.renderer?.ready) {
      return { ok: false, shown: false, via: 'usb', durationSec: 0, vibrated: false, error: '渲染器未就绪' };
    }
    const secs = Math.min(30, Math.max(1, Math.round(Number(durationSec) || 5)));
    this.notice = { title: textTitle, body: textBody, until: Date.now() + secs * 1000 };
    clearTimeout(this.noticeTimer);
    this.fb?.invalidate();
    this.#paintSoon(0);
    this.noticeTimer = setTimeout(() => {
      if (this.notice && Date.now() >= this.notice.until) {
        this.#dropNotice();
        this.#paintSoon(0);
      }
    }, secs * 1000);
    this.noticeTimer.unref?.();
    if (vibrate) this.#buzz(120).catch(() => {});
    this.logger.info(`通知卡：${secs}s「${(textTitle || textBody).slice(0, 20)}」`);
    return { ok: true, shown: true, via: 'usb', durationSec: secs, vibrated: !!vibrate, error: null };
  }

  /** 让手表震动 n 次（USB 0x03 / BLE 桥）。 */
  async vibrate({ ms = 180, times = 1 } = {}) {
    if (!this.link.state.connected && !this.ble.connected) {
      return { ok: false, ms: 0, times: 0, via: 'none', error: '手表未连接' };
    }
    const duration = Math.min(2000, Math.max(20, Math.round(Number(ms) || 180)));
    const count = Math.min(5, Math.max(1, Math.round(Number(times) || 1)));
    for (let i = 0; i < count; i += 1) {
      await this.#buzz(duration);
      if (i < count - 1) await sleep(120);
    }
    return { ok: true, ms: duration, times: count, via: this.link.state.connected ? 'usb' : 'ble', error: null };
  }

  /** 把当前手表画面用同一 renderer 重渲成 PNG，返回本机路径。 */
  async screenshot() {
    /* BLE 连上也算"连着"，只是没有像素通道：别笼统说"手表未连接"，
     * 那会让人以为表掉了（2026-11-21 实机就踩过这个误导）。 */
    if (!this.link.state.connected) {
      const error = this.#bleActive() ? '需要 USB 连接（BLE 没有像素通道，截图拿不到）' : '手表未连接';
      return { ok: false, path: '', width: 0, height: 0, notice: false, pcMode: false, error };
    }
    if (!this.link.state.pcMode || !this.link.state.passthrough) return { ok: false, path: '', width: 0, height: 0, notice: false, pcMode: false, error: '手表不在电脑模式' };
    if (!this.renderer?.ready) return { ok: false, path: '', width: 0, height: 0, notice: false, pcMode: false, error: '渲染器未就绪' };
    this.#renderFrame(Date.now());
    const dir = path.join(DATA_DIR, 'screenshots');
    fs.mkdirSync(dir, { recursive: true });
    const file = path.join(dir, `protorig-${new Date().toISOString().replace(/[:.]/g, '-')}.png`);
    fs.writeFileSync(file, this.renderer.canvas.toBuffer('image/png'));
    return {
      ok: true, path: file, width: 480, height: 480,
      notice: Boolean(this.notice), pcMode: Boolean(this.link.state.pcMode), error: null,
    };
  }

  /* ------------------------------------------------------------ 写入队列 */

  /** 所有链路写入串行化：像素帧 / 文本命令 / 震动 / 亮度不能交错。 */
  #enqueue(fn) {
    const run = this.chain.then(fn, fn);
    this.chain = run.catch(() => {});
    return run;
  }

  #write(buffer) {
    return this.#enqueue(() => this.link.write(buffer));
  }

  /**
   * 执行一个"自己会写链路"的异步命令（shell/sayHello/requestStatus…）。
   * 注意不要写成 `#write(this.link.sayHello())`：那会把 Promise 当 Buffer 塞进写队列，
   * 而且 sayHello 自身的 rejection 没人接 —— 握手途中拔线会以未捕获 rejection 崩宿主。
   */
  #command(fn) {
    return this.#enqueue(async () => {
      /* 文本命令必须和像素帧在 USB 传输上分开（见 TEXT_GAP_MS 的实测记录）。 */
      if (this.lastFrameAt && Date.now() - this.lastFrameAt < TEXT_GAP_MS) {
        await sleep(TEXT_GAP_MS - (Date.now() - this.lastFrameAt));
      }
      return fn();
    });
  }

  /* ------------------------------------------------------------ 会话投影 */

  #sessionKey(session) {
    const id = session?.id ?? session?.header?.id;
    if (typeof id === 'string' && id) return id;
    if (typeof id === 'number' && Number.isFinite(id)) return String(id);
    let key = this.synthetic.get(session);
    if (!key) {
      key = `session-local-${++this.syntheticSeq}`;
      this.synthetic.set(session, key);
    }
    return key;
  }

  /** 由 index.mjs 的 `ctx.on('session/event')` 调用。 */
  onSessionEvent(session, event) {
    if (!session || typeof session !== 'object' || !event || typeof event.type !== 'string') return;
    try {
      const key = this.#sessionKey(session);
      const now = Date.now();
      if (isSubagentSession(session, { ...this.titleOptions, sessionId: key, now })) return;
      if (this.focus.sessionId !== key) this.focus = createFocusState(key);
      projectEvent(event, this.focus);
      if (event.type === 'tool/call') this.#rememberCommand(event.data);
      if (event.type === 'turn/end') {
        const kind = event.data?.reason?.kind;
        if (kind === 'completed') void this.remind('done');
        else if (FAILURE_KINDS.has(kind)) void this.remind('failed');
        else if (kind === 'blocked') void this.remind('needs');
      }
      if (event.type === 'approval/asked') void this.remind('needs');
      if (event.type === 'approval/decided') this.#clearComputerApproval();
      if (event.type === 'turn/start' || event.type === 'turn/end') {
        // 只记状态数字，不记标题/提示词（沿用桌面样例的隐私边界）
        const card = buildCards(this.focus, now)[0];
        const focus = card?.focus ?? {};
        this.logger.info(
          `会话 ${event.type}: state=${focus.state ?? '?'} steps=${focus.steps ?? 0} ` +
            `ctx=${focus.ctx ?? -1}${event.type === 'turn/end' ? ` kind=${event.data?.reason?.kind ?? '?'}` : ''}`,
        );
      }
      this.#paintSoon(0);
    } catch {
      // 事件格式异常绝不能打断宿主会话；面板停在上一帧即可
    }
  }

  #refreshFocus(now) {
    if (!this.focus.sessionId) return;
    const options = { ...this.titleOptions, now };
    if (!this.focus.titleAt || now - this.focus.titleAt >= 2000) {
      this.focus.title = readSessionTitle(this.focus.sessionId, options);
      this.focus.titleAt = now;
    }
    mergeContext(this.focus, readSessionContext(this.focus.sessionId, options));
  }

  /** 记下 tool/call 的命令文本，只用于审批卡显示；最多 16 条，不落盘。 */
  #rememberCommand(data) {
    if (!data || typeof data.callId !== 'string' || typeof data.arguments !== 'string') return;
    let text = null;
    try {
      const args = JSON.parse(data.arguments);
      text = typeof args?.command === 'string' ? args.command
        : typeof args?.cmd === 'string' ? args.cmd
          : typeof args?.script === 'string' ? args.script
            : null;
    } catch {
      /* 参数不一定是 JSON，退回原文 */
    }
    if (text === null) text = data.arguments;
    this.commands.set(data.callId, String(text).replace(/\s+/g, ' ').trim().slice(0, 200));
    while (this.commands.size > 16) this.commands.delete(this.commands.keys().next().value);
  }

  /** 审批 request 的 callId → 该工具调用的命令（拿不到就是空串）。 */
  commandFor(callId) {
    if (typeof callId !== 'string') return '';
    return this.commands.get(callId) ?? '';
  }

  /* ------------------------------------------------------------ 指标 */

  #attachMetrics() {
    try {
      const { MetricsCollector } = require(METRICS_PATH);
      this.metricsCollector = new MetricsCollector();
    } catch (error) {
      if (!this.metricsWarned) {
        this.metricsWarned = true;
        this.logger.warn(`指标采集不可用（${error.message}）；三环显示 --`);
      }
      return;
    }
    const sample = async () => {
      if (this.disposed) return;
      try {
        this.metrics = await this.metricsCollector.sample();
        this.ble.updateMetrics(this.metrics);
      } catch {
        /* 指标失败不显示假数据（PRD FR-6 精神） */
      }
    };
    /* 插件热重载会留下旧实例的计时器；同一进程只允许一个指标采样器。 */
    const previousMetrics = globalThis.__protorigMetricsTimer;
    if (previousMetrics) clearInterval(previousMetrics);
    this.metricsTimer = setInterval(sample, 1000);
    this.metricsTimer.unref?.();
    globalThis.__protorigMetricsTimer = this.metricsTimer;
    void sample();
  }

  /* ------------------------------------------------------------ 提醒（FR-7） */

  /**
   * 完成 / 需响应 / 失败三档提醒。
   * 用固件的震动执行器 0x03 + 亮度执行器 0x02 —— 绝不刷像素做闪屏（整屏 0.45 s，还打断面板）。
   */
  async remind(kind) {
    const pattern = REMIND_PATTERN[kind];
    if (!pattern) return;
    const reminder = this.config.reminder;
    // 事件开关（完成/需响应/失败）+ 两个独立通道（震动/亮度）
    if (!reminder[kind]) return;
    const vibrateOn = reminder.vibrate;
    const brightOn = reminder.bright;
    const mode = vibrateOn && brightOn ? 'both' : vibrateOn ? 'vibrate' : brightOn ? 'bright' : 'off';
    if (mode === 'off') return;
    const stamp = Date.now();
    if (stamp - (this.lastRemind[kind] ?? 0) < 2000) return; // 同一类事件 2 s 内只提醒一次
    this.lastRemind[kind] = stamp;
    this.logger.info(`提醒 ${kind}（${mode}${this.link.state.connected ? ' · USB' : this.ble.connected ? ' · BLE' : ''}）`);
    const jobs = [];
    if (vibrateOn) jobs.push(this.#vibratePattern(pattern.vibrate));
    if (brightOn) jobs.push(this.#brightPulse(pattern.pulse));
    await Promise.allSettled(jobs);
  }

  async #vibratePattern(durations) {
    const ble = this.ble;
    const reminder = this.config.reminder;
    const preset = reminder.vibratePreset;
    const scale = VIBRATE_SCALE[preset] ?? 1;
    for (let index = 0; index < durations.length; index += 1) {
      // custom：每一响都用用户给的时长；预设：按短/中/长缩放
      const ms = preset === 'custom'
        ? Math.max(20, Math.min(2000, Math.round(reminder.vibrateMs)))
        : Math.max(20, Math.min(2000, Math.round(durations[index] * scale)));
      if (this.link.state.connected) await this.#write(vibrate(ms)).catch(() => {});
      else if (ble?.connected) ble.vibrate(ms);
      if (index < durations.length - 1) await sleep(160);
    }
  }

  async #brightPulse(count) {
    const base = Number.isFinite(this.brightBase) ? this.brightBase : 128;
    const high = Math.min(255, Math.max(32, base + 70));
    if (this.pulsing) return;
    this.pulsing = true;
    try {
      for (let index = 0; index < count; index += 1) {
        if (this.link.state.connected) await this.#write(bright(high)).catch(() => {});
        else if (this.ble?.connected) this.ble.brightness(high);
        await sleep(140);
        if (this.link.state.connected) await this.#write(bright(base)).catch(() => {});
        else if (this.ble?.connected) this.ble.brightness(base);
        if (index < count - 1) await sleep(140);
      }
    } finally {
      this.pulsing = false;
      // 闪完必须恢复原亮度（PRD FR-7）；再补一次，防止最后一帧被丢掉
      if (this.link.state.connected) await this.#write(bright(base)).catch(() => {});
    }
  }

  /* ------------------------------------------------------------ BLE 卡片层 */

  /** 会话状态名 → 固件 PM_STATE_*（monitor_protocol.h）。 */
  static PM_STATE = Object.freeze({ IDLE: 0, RUNNING: 1, NEEDS: 2, DONE: 3, FAILED: 4 });

  /**
   * BLE 版热区兜底：与固件 pc_monitor.c 的版式一一对应。
   * 正常情况下用固件 0x58 自报的矩形（改版式只改固件一处），
   * 这份常量只在"卡刚出、热区帧还没到"的首 100 ms 起作用。
   */
  static BLE_HOTZONES = Object.freeze({
    /* = design/dsh_panel_v4/tokens.json 的 hotzones，与固件 pc_monitor.c 绘制一致 */
    deny: [106, 296, 128, 96],
    allow: [246, 296, 128, 96],
    option: [92, 218, 296, 96],
    next: [128, 314, 104, 96],
    submit: [248, 314, 104, 96],
  });

  /** 当前是否走 BLE 数字链路（USB 在线时空操作）。 */
  #bleActive() {
    return Boolean(this.ble?.connected) && !this.link.state.connected;
  }

  /**
   * 触摸链路是否可用（USB 透传 或 BLE 卡片链路）。
   * 注意：只看 pcMode+passthrough，不看 connected —— 握手/切模式的瞬间
   * 链路状态和这两个标志不同步，原来就是靠这两个标志判定的，别改语义。
   */
  #touchLive() {
    if (this.link.state.pcMode && this.link.state.passthrough) return true;
    return this.#bleActive();
  }

  /**
   * 卡片能不能真的显示出来：USB 透传中，或 BLE 数字通道已连。
   *
   * 看不到就必须**立刻**回落给电脑。否则插件会接管这张卡却没有任何地方能渲染它，
   * 用户手表和电脑两头都看不到（2026-11-21 实机踩过：`pcmode 0` 退出电脑模式后
   * 发问答卡，30 s 内两边都是空的，最后还超时了）。
   */
  #cardVisible() {
    if (this.link.state.connected && this.link.state.pcMode && this.link.state.passthrough) return true;
    return this.#bleActive();
  }

  #onBleEvent(event) {
    if (!event || this.disposed) return;
    if (event.type === 'touch') {
      // 与 USB 的 link 'touch' 事件同一个处理函数：判定逻辑只有一份
      this.#onTouch(event);
      return;
    }
    if (event.type === 'status') {
      // 0x91 状态帧：BLE 下原来没人接，所以电量/亮度一直不更新
      this.status = event;
      // page=4 是监控页（-1 屏）：刚切进来时之前的卡片帧会被固件丢弃，强制重发一次
      if (event.page === 4 && this.blePage !== 4) {
        this.bleCardKey = null;
        this.bleCardKind = null;
        this.bleBitmaps.clear();
        this.#paintSoon(0);
      }
      this.blePage = event.page;
      if (this.config.debug) {
        this.logger.debug?.(`BLE 状态 batt=${event.battery} bright=${event.bright} page=${event.page}`);
      }
      return;
    }
    if (event.type === 'zones') {
      this.bleZones = { kind: event.kind, list: event.list ?? [], at: Date.now() };
      if (this.config.debug) {
        const text = (event.list ?? []).map((z) => `${z.name}(${z.x},${z.y},${z.w}x${z.h})`).join(' ');
        this.logger.debug?.(`BLE 热区 kind=${event.kind} ${text || '(无热区)'}`);
      }
      return;
    }
    if (event.type === 'cardack') this.#onCardAck(event);
    if (event.type === 'redraw') {
      /* 0x92：固件本地弹窗（闹钟/计时器）结束，请主机整屏重发。
       * 手表没有帧缓存 —— 不重发的话，弹窗区域会留一块旧像素。 */
      this.logger.info?.(`手表请求整屏重发（reason=${event.reason}）`);
      this.fb?.invalidate();
      return;
    }
  }

  #onCardAck(event) {
    if (event.what === ACK.BMP_BAD) {
      // 固件那片没收下（丢包/乱序/超长/固件槽太矮）：整张图作废重发，绝不让它画半张
      const slot = Number(event.code) || 0;
      const prev = this.bleBitmapFails.get(slot);
      /* 上一次回执已经把待发键删掉了，所以这里要沿用 prev.key 才能连续计数。 */
      const key = this.bleBitmaps.get(slot) ?? prev?.key ?? '';
      const count = prev && prev.key === key ? prev.count + 1 : 1;
      this.bleBitmapFails.set(slot, { key, count });
      this.bleBitmaps.delete(slot);
      this.bleBitmapRetries = (this.bleBitmapRetries ?? 0) + 1;
      if (count >= 3) {
        this.logger.warn(
          `卡片位图连续 ${count} 次被手表拒收（slot=${slot}）：固件位图槽可能比插件期望的矮（旧固件？），`
          + '改用固件字库文本兜底，不再重发',
        );
        return;
      }
      this.logger.debug?.(`卡片位图被手表丢弃 slot=${slot}（第 ${count} 次），重发`);
      this.#paintSoon(0);
      return;
    }
    if (event.what === ACK.CARD_BAD) {
      this.logger.warn(`BLE 卡片帧被手表丢弃（kind=${event.code}）：字段长度或格式不合法`);
      return;
    }
    if (this.config.debug) this.logger.debug?.(`卡片回执 what=${event.what} code=${event.code}`);
  }

  /** 会话投影 → 会话卡字段（USB 像素版与 BLE 数字版共用这一份投影）。 */
  #sessionCardView(now = Date.now()) {
    const card = buildCards(this.focus, now)[0];
    const focus = card?.focus ?? { title: 'SESSION', state: 'IDLE', tool: '', steps: 0, ctx: -1 };
    const ctx = Number.isFinite(focus.ctx) && focus.ctx >= 0 ? Math.min(100, Math.round(focus.ctx)) : -1;
    /* 没有会话时不要编一个 "SESSION" 标题：交给固件画 v4 的空态
     * （当前没有会话 / 在电脑上启动 DSH 任务），同时清掉可能残留的标题位图。 */
    const live = Boolean(this.focus?.sessionId);
    return {
      state: live ? (focus.state ?? 'IDLE') : 'IDLE',
      title: live && typeof focus.title === 'string' ? focus.title : '',
      tool: live && typeof focus.tool === 'string' ? focus.tool : '',
      steps: live && Number.isFinite(focus.steps) ? focus.steps : 0,
      ctx: live ? ctx : -1,
      footer: this.#footerText(),
    };
  }

  /** v4 底部那一行：`电量 97%  ·  ¥91.38`（拿不到就少显示一段，绝不显示假数据）。 */
  #footerText() {
    const parts = [];
    const battery = Number.isFinite(this.status?.battery) ? this.status.battery : null;
    if (battery != null) parts.push(`电量 ${battery}%`);
    const balance = this.balance?.publicView?.().text;
    if (typeof balance === 'string' && balance) parts.push(balance);
    return parts.join('  ·  ');
  }

  /**
   * BLE 卡片下发（P1/P3/P4）：会话状态、审批、问答。
   * 只在内容变化时发（lead 要求：卡片不跟 5 Hz 数值走，数值仍走 0x52）；
   * 位图另按文本键去重，只在标题/命令/题目文字变化时重传。
   */
  #blePublish(now = Date.now()) {
    if (this.disposed) return;
    if (!this.#bleActive()) {
      this.bleCardKey = null;
      this.bleBitmaps.clear();
      return;
    }
    const view = this.#sessionCardView(now);
    let payload;
    let bitmaps;
    if (this.approval) {
      const approval = this.#approvalView();
      payload = cardApproval({
        canAllow: approval.canAnswer ? 1 : 0,
        computer: approval.canAnswer ? 0 : 1,
        countdown: approval.countdown,
        tool: approval.toolName,
        command: approval.cmd,
      });
      bitmaps = [{ slot: 0, text: approval.cmd }];
    } else if (this.question) {
      const question = this.#questionView();
      const overlay = this.question;
      const page = Math.min(overlay.page, Math.max(0, question.options.length - 1));
      /* 文本字段一律用**原文**：BLE 上固件自己画一行字作兜底，
       * 而 USB 侧的时间翻页会让 question.prompt 每 2.6 s 变一次 —— 一旦拿它拼 payload，
       * 每变一次就整包重发、整卡重画，实机上就是"监控页和问答页疯狂切换"（2026-11-21）。
       * 长文本改由位图槽承载（renderBlock 多行），payload 只留稳定原文。 */
      const cur = this.#currentQuestion();
      const promptText = String(cur?.question ?? '').trim();
      const optionText = String(question.options[page] ?? '').trim();
      payload = cardQuestion({
        index: overlay.qi,
        count: Math.max(1, overlay.questions.length),
        multi: question.multiSelect ? 1 : 0,
        selected: question.selected,
        page,
        optionCount: Math.max(1, question.options.length),
        prompt: promptText,
        option: optionText,
        computer: question.canAnswer ? 0 : 1,
      });
      bitmaps = [
        { slot: 0, text: promptText, block: { maxLines: 2, maxWidth: 300 } },
        { slot: 1, text: optionText, block: { maxLines: 2, maxWidth: 296 } },
      ];
    } else {
      payload = cardSession({
        state: PanelService.PM_STATE[view.state] ?? 0,
        steps: view.steps,
        ctx: view.ctx,
        tool: view.tool,
        title: view.title,
        footer: view.footer,
      });
      // 没有会话 = v4 空态：插件把「鲸鱼 + 探索未至之境」渲成一张 2-bit 位图
      const live = Boolean(this.focus?.sessionId);
      bitmaps = [{ slot: 0, text: view.title, whale: !live }];
    }
    // 换卡种时固件会丢掉旧位图（防止上一张卡的标题留在审批卡上），
    // 所以插件这边的位图去重键也必须一起清掉，否则新卡只剩文本兜底（缺字 '?'）。
    const kind = payload[0];
    if (kind !== this.bleCardKind) {
      this.bleCardKind = kind;
      this.bleBitmaps.clear();
    }
    const key = Buffer.from(payload).toString('hex');
    // 5 s 兜底重发：卡片帧同时是"电脑还活着"的租约（固件 5 s 过期），
    // 也覆盖"手表刚滑回电脑模式、之前的帧被丢掉"的情况。20 B/5 s 可忽略。
    /* 顺序很重要：**先位图，后卡片帧**。
     * 卡片帧一到，固件立刻出卡；这时若位图还没传完，它只能用自己的字库画一行截断文本兜底
     * —— 实机上就是"先看到一张带乱码/缺字的卡，一两秒后才变正常"（2026-11-21 用户反馈）。
     * 反过来先传位图（每张 ~2.5 KB，BLE 上要几百毫秒），用户最多看到卡晚一点出现，
     * 不会看到半成品。 */
    for (const { slot, text, whale, block } of bitmaps) this.#bleSyncBitmap(slot, text, 296, whale, block ?? null);

    const stale = now - (this.bleCardAt ?? 0) > 5000;
    if (key !== this.bleCardKey || stale) {
      this.bleCardKey = key;
      this.bleCardAt = now;
      this.ble.sendPcFrame(payload);
      if (this.config.debug) {
        this.logger.debug?.(`BLE 卡片下发 ${payload[0].toString(16).padStart(2, '0')}… ${payload.length} B`);
      }
    }
  }

  /** 文本 → 2-bit 位图 → 0x57 分片（文本没变就不发）；whale=true 时渲鲸鱼空态。 */
  #bleSyncBitmap(slot, text, maxWidth = 296, whale = false, block = null) {
    const clean = typeof text === 'string' ? text.replace(/\s+/g, ' ').trim() : '';
    if (whale) {
      const key = `${slot}\u0000whale`;
      if (this.bleBitmaps.get(slot) === key) return;
      const bitmap = this.cardBitmap?.renderEmpty?.();
      if (!bitmap) return;
      this.bleBitmaps.set(slot, key);
      for (const payload of cardBitmapChunks(slot, bitmap)) this.ble.sendPcFrame(payload);
      if (this.config.debug) {
        this.logger.debug?.(`BLE 鲸鱼位图 slot=${slot} ${bitmap.w}x${bitmap.h} ${bitmap.bits.length} B`);
      }
      return;
    }
    /* 文本变空（比如会话结束/空态）要显式清掉槽，否则旧标题会停在屏上 */
    if (!clean) {
      const emptyKey = `${slot}\u0000`;
      if (this.bleBitmaps.get(slot) === emptyKey) return;
      this.bleBitmaps.set(slot, emptyKey);
      this.ble.sendPcFrame(cardBitmapClear(slot));
      return;
    }
    if (!this.cardBitmap?.ready) return;
    /* 多行块和单行要分开去重：同一个文本在两种版式下的位图不一样。 */
    const key = `${slot}\u0000${block ? `blk${block.maxLines ?? 2}` : 'line'}\u0000${clean}`;
    /* 同一张图被固件连续拒收 3 次就放弃：典型场景是**新插件配旧固件**
     * （位图槽只有 30 px，2 行位图 34 px 会被 h > PC_CARD_BMP_H 直接拒掉）。
     * 没有这个闸门就会 BMP_BAD → 删缓存 → 重发 → 再被拒 的无限循环，
     * 把 BLE 带宽吃光而且卡永远画不出来。放弃后固件会退回自己的字库文本。 */
    const fail = this.bleBitmapFails.get(slot);
    if (fail && fail.key === key && fail.count >= 3) return;
    if (this.bleBitmaps.get(slot) === key) return;
    const bitmap = block
      ? this.cardBitmap.renderBlock(clean, { maxWidth, ...block })
      : this.cardBitmap.render(clean, { maxWidth });
    if (!bitmap) return;
    this.bleBitmaps.set(slot, key);
    for (const payload of cardBitmapChunks(slot, bitmap)) this.ble.sendPcFrame(payload);
    if (this.config.debug) {
      this.logger.debug?.(`BLE 位图 slot=${slot} ${bitmap.w}x${bitmap.h} ${bitmap.bits.length} B「${clean.slice(0, 18)}」`);
    }
  }

  /* ------------------------------------------------------------ 触摸 */

  /**
   * 当前叠加层用哪几个热区。
   * 设计稿的审批热区（deny/allow，y=296..392）和问答热区（next/submit，y=314..410）
   * 在坐标上重叠：不按叠加层过滤的话，"下一项"会先命中 deny、"提交"会先命中 allow，
   * 手表上就会"点了没反应"。所以命中测试必须只查当前卡片自己的热区。
   */
  #zoneNames() {
    if (this.approval) return ['deny', 'allow'];
    if (this.question) return ['option', 'next', 'submit'];
    return [];
  }

  /**
   * 一个热区的矩形。BLE 模式优先用固件自报的（0x58，版式在固件一处），
   * 报文没到之前用 BLE_HOTZONES 兜底；USB 仍用 v4 设计稿的 tokens。
   */
  #zoneBox(name) {
    // USB 透传优先（v4 版式）；只有"USB 没在透传、BLE 卡片链路在"时才用 BLE 版式
    const usbPassthrough = Boolean(this.link.state.pcMode && this.link.state.passthrough);
    if (!usbPassthrough && this.#bleActive()) {
      const reported = this.bleZones.list?.find((zone) => zone.name === name);
      if (reported) return [reported.x, reported.y, reported.w, reported.h];
      const fallback = PanelService.BLE_HOTZONES[name];
      if (fallback) return fallback;
    }
    return this.tokens?.hotzones?.[name];
  }

  #zoneAt(point, names = this.#zoneNames()) {
    for (const name of names) {
      const box = this.#zoneBox(name);
      if (!Array.isArray(box) || box.length < 4) continue;
      const [x, y, w, h] = box;
      if (point.x >= x && point.x < x + w && point.y >= y && point.y < y + h) return name;
    }
    return null;
  }

  /** 一次点击的判定参数（真机滑动误触的护栏）。 */
  static TAP = Object.freeze({ MIN_MS: 80, MAX_MS: 2000, MAX_MOVE_PX: 30, MODE_GUARD_MS: 500 });

  /**
   * 上下滑翻页的判定参数（与手表本地的左右滑导航分开）：
   *   · 垂直位移 ≥40 px、水平位移 <30 px、按下到抬起 <800 ms 才算翻页；
   *   · 判为滑动后绝不触发任何按钮点击；
   *   · 左右滑仍由手表本地处理（退出电脑模式），插件不拦截、不翻页。
   */
  static SWIPE = Object.freeze({ MIN_DY: 40, MAX_DX: 30, MAX_MS: 800, HORIZONTAL_MIN: 30 });

  /**
   * 触摸处理。护栏（research/62 §12）：
   *   1. 本地页（passthrough=false）根本没有我们的按钮 → 全部忽略；
   *   2. 模式切换（pc/mode 0↔1）后 MODE_GUARD_MS 内的残点全部丢弃；
   *   3. 上下滑 = 问答翻页（向上下一页 / 向下上一页）；
   *      左右滑 = 手表本地导航，插件既不翻页也不点击；
   *   4. 一次点击 = 同一热区按下→抬起，时长 80 ms..2 s，累计位移 ≤30 px。
   */
  #onTouch(touch) {
    if (!touch) return;
    const now = Date.now();
    // 本地页（USB 无透传 / BLE 卡片链路不可用）根本没有我们的按钮 → 全部忽略
    if (!this.#touchLive()) {
      this.touchDown = null;
      return;
    }
    if (now < this.touchGuardUntil) {
      this.touchDown = null;
      return;
    }
    if (touch.pressed) {
      if (!this.touchDown) {
        // 起点可以在热区外：上下滑翻页允许从卡面任意位置开始
        this.touchDown = { zone: this.#zoneAt(touch), at: now, x: touch.x, y: touch.y, moved: 0 };
        return;
      }
      // 滑动过程中固件会持续报 pressed：累计位移（点击判定用），但不因离开热区就作废
      this.touchDown.moved = Math.max(
        this.touchDown.moved,
        Math.abs(touch.x - this.touchDown.x) + Math.abs(touch.y - this.touchDown.y),
      );
      return;
    }
    const down = this.touchDown;
    this.touchDown = null;
    if (!down) return;
    const dx = touch.x - down.x;
    const dy = touch.y - down.y;
    const elapsed = now - down.at;

    // ③ 上下滑 → 问答翻页。判为滑动就 return，绝不再走点击分支
    if (
      elapsed <= PanelService.SWIPE.MAX_MS
      && Math.abs(dy) >= PanelService.SWIPE.MIN_DY
      && Math.abs(dx) < PanelService.SWIPE.MAX_DX
    ) {
      this.#onVerticalSwipe(dy < 0 ? 1 : -1);
      return;
    }
    // ④ 左右滑：交给手表本地（本地导航会退出电脑模式），插件不翻页也不点击
    if (Math.abs(dx) >= PanelService.SWIPE.HORIZONTAL_MIN) return;

    // ⑤ 点击：同一热区、80 ms..2 s、位移 ≤30 px
    const zone = this.#zoneAt(touch);
    if (!down.zone || zone !== down.zone) return;
    const moved = Math.max(down.moved, Math.abs(dx) + Math.abs(dy));
    if (elapsed < PanelService.TAP.MIN_MS || elapsed > PanelService.TAP.MAX_MS) return;
    if (moved > PanelService.TAP.MAX_MOVE_PX) return;
    this.#activate(zone);
  }

  /** 上下滑翻页：向上 = 下一页，向下 = 上一页（循环，与"下一项"按钮一致）。 */
  #onVerticalSwipe(direction) {
    const overlay = this.question;
    if (!overlay || !overlay.canAnswer) return false;
    const options = this.#questionOptions();
    if (options.length <= 1) return false;
    overlay.page = (overlay.page + (direction > 0 ? 1 : -1) + options.length) % options.length;
    this.#resetQuestionTimer(overlay); // 用户正在操作：延长手表独占时间
    this.logger.info(`问答手势翻页：${direction > 0 ? '上滑' : '下滑'} → ${overlay.page + 1}/${options.length}`);
    this.#paintSoon(0);
    return true;
  }

  /** 触摸入口的公开包装（测试与调试用；真实触摸来自 link 的 'touch' 事件）。 */
  handleTouch(touch) {
    this.#onTouch(touch);
  }

  /** 手表事件入口的公开包装（测试与调试用；BLE notify 与 USB 文本行共用这一个）。 */
  handleWatchEvent(event) {
    this.#onBleEvent(event);
  }

  #activate(zone) {
    if (this.approval) {
      if (!this.approval.canAnswer) return;
      if (zone === 'allow') this.#settleApproval('allowed-once');
      else if (zone === 'deny') this.#settleApproval('rejected');
      return;
    }
    if (this.question) {
      this.#activateQuestion(zone);
    }
  }

  /* ------------------------------------------------------------ 审批（FR-8） */

  /** 是否有叠加层在处理中（审批/问答同时只接一个，另一个交给电脑）。 */
  overlayBusy() {
    return Boolean(this.approval || this.question);
  }

  /* ------------------------------------------------------------ 叠加层队列 */

  /**
   * 审批/问答共用一条 FIFO 队列：表上同一时刻只显示一张卡。
   * 旧版第二张直接 fallback 给电脑；现在排队，排队超过 config.queueWaitSec
   * （10–60 s，最多 60 s）或队列超过 5 张才交回电脑。
   * 每张卡的手表计时从"真正显示"开始，排队时间单独计算。
   */
  #enqueueOverlay(kind, input, activate) {
    const signal = input?.signal;
    if (signal?.aborted) return Promise.resolve({ aborted: true, token: null });
    const entry = {
      kind,
      input,
      activate,
      sessionLabel: this.#overlaySessionLabel(),
      queuedAt: Date.now(),
      done: false,
      started: false,
      resolve: null,
      queueTimer: null,
      onAbort: null,
    };
    entry.promise = new Promise((resolve) => { entry.resolve = resolve; });
    entry.finish = (result) => {
      if (entry.done) return;
      entry.done = true;
      clearTimeout(entry.queueTimer);
      if (signal && entry.onAbort) signal.removeEventListener('abort', entry.onAbort);
      entry.resolve({ ...result, token: entry });
    };
    if (signal) {
      entry.onAbort = () => {
        this.#removeQueuedOverlay(entry);
        entry.finish({ aborted: true });
      };
      signal.addEventListener('abort', entry.onAbort, { once: true });
    }
    if (!this.overlayBusy() && !this.overlayQueue.length) {
      this.#startOverlay(entry);
      return entry.promise;
    }
    if (this.overlayQueue.length >= OVERLAY_QUEUE_MAX) {
      this.logger.info(`${kind} 卡排队已满，交给电脑`);
      entry.finish({ fallback: true, queueFull: true });
      return entry.promise;
    }
    this.overlayQueue.push(entry);
    const waitMs = Math.min(
      OVERLAY_QUEUE_WAIT_MAX_MS,
      Math.max(1000, (Number(this.config.queueWaitSec) || 60) * 1000),
    );
    entry.queueTimer = setTimeout(() => {
      this.#removeQueuedOverlay(entry);
      this.logger.info(`${kind} 卡排队超时，交给电脑`);
      entry.finish({ fallback: true, queueTimeout: true });
    }, waitMs);
    entry.queueTimer.unref?.();
    this.logger.info(`${kind} 卡排队（前面 ${this.overlayQueue.length} 张，最多 ${Math.round(waitMs / 1000)} s）`);
    return entry.promise;
  }

  #startOverlay(entry) {
    if (!entry || entry.done) return;
    if (this.overlayBusy()) {
      this.overlayQueue.unshift(entry);
      return;
    }
    entry.started = true;
    clearTimeout(entry.queueTimer);
    this.overlayQueue = this.overlayQueue.filter((item) => item !== entry);
    entry.activate(entry);
  }

  #pumpOverlay() {
    if (this.disposed || this.overlayBusy()) return;
    while (this.overlayQueue.length) {
      const next = this.overlayQueue.shift();
      if (!next || next.done) continue;
      this.#startOverlay(next);
      return;
    }
  }

  #removeQueuedOverlay(entry) {
    const index = this.overlayQueue.indexOf(entry);
    if (index >= 0) this.overlayQueue.splice(index, 1);
  }

  /** 卡片上的会话短标题（≤10 字）：多对话并发时区分"这是哪个任务"。 */
  #overlaySessionLabel() {
    const title = this.focus?.title;
    if (typeof title !== 'string' || !title.trim() || title === 'SESSION') return '';
    const clean = title.replace(/\s+/g, ' ').trim();
    return clean.length > 10 ? `${clean.slice(0, 10)}…` : clean;
  }

  /**
   * 在手表上出审批卡并等用户点击；忙时进队列而不是直接回电脑。
   * @returns {Promise<{outcome?: string, fallback?: boolean, aborted?: boolean, token?: object}>}
   */
  requestApproval(input = {}) {
    /* 同问答：显示不出来就不能接管审批，否则用户的电脑端也会被吊住。 */
    if (!this.#cardVisible()) return Promise.resolve({ fallback: true, token: null, reason: 'card-not-visible' });
    return this.#enqueueOverlay('approval', input, (entry) => this.#activateApproval(entry));
  }

  #activateApproval(entry) {
    this.#dropNotice();
    const { toolName, reason, command } = entry.input ?? {};
    const timeoutMs = this.config.approval.watchTimeoutMs;
    const overlay = {
      kind: 'approval',
      entry,
      sessionLabel: entry.sessionLabel,
      toolName: String(toolName ?? '工具'),
      reason: typeof reason === 'string' ? reason : '',
      command: typeof command === 'string' ? command : '',
      deadline: Date.now() + timeoutMs,
      canAnswer: true,
      settled: false,
      fallbackSince: 0,
      timer: null,
    };
    this.approval = overlay;
    // 卡出现时手指可能正按在屏幕上（比如滑动刚结束）：静默 350 ms 再接受点击
    this.touchDown = null;
    this.touchGuardUntil = Date.now() + 350;
    this.logger.info(`审批卡已出：${overlay.toolName}${overlay.sessionLabel ? `（${overlay.sessionLabel}）` : ''}（手表优先 ${timeoutMs / 1000} s）`);
    this.#paintSoon(0);
    overlay.timer = setTimeout(() => {
      if (entry.done || this.approval !== overlay) return;
      overlay.canAnswer = false;
      overlay.deadline = 0;
      overlay.fallbackSince = Date.now();
      this.logger.info(`审批 ${timeoutMs / 1000} s 无应答，移交给电脑`);
      this.#paintSoon(0);
      overlay.settled = true;
      entry.finish({ fallback: true });
    }, timeoutMs);
    overlay.timer.unref?.();
  }

  #settleApproval(outcome) {
    const overlay = this.approval;
    if (!overlay || overlay.settled) return;
    overlay.settled = true;
    clearTimeout(overlay.timer);
    this.logger.info(`手表审批：${outcome === 'allowed-once' ? '允许一次' : '拒绝'}`);
    overlay.entry?.finish({ outcome });
    this.#paintSoon(0);
  }

  clearApproval(token = null) {
    if (!this.approval) return;
    /* 排队超时/被取消的那一张不能误撤别人正在看的卡：认 token。 */
    if (token && this.approval.entry !== token) return;
    clearTimeout(this.approval.timer);
    this.approval = null;
    this.touchDown = null;
    this.logger.info('审批卡已撤');
    this.#paintSoon(0);
    this.#pumpOverlay();
  }

  /**
   * 电脑答完时 `approval/decided` 会进 session/event（PRD FR-8：撤卡不靠插件自己记状态）。
   * 只撤"已经移交给电脑"的那张卡，避免误伤刚出的新卡。
   */
  #clearComputerApproval() {
    const overlay = this.approval;
    if (!overlay || overlay.canAnswer) return;
    if (!overlay.fallbackSince || Date.now() - overlay.fallbackSince > 30000) return;
    this.logger.info('电脑已处理审批，撤下表面卡片');
    this.clearApproval();
  }

  #approvalView() {
    const overlay = this.approval;
    const remaining = overlay.canAnswer ? Math.max(0, overlay.deadline - Date.now()) : 0;
    const label = overlay.sessionLabel ? `【${overlay.sessionLabel}】` : '';
    return {
      toolName: overlay.toolName,
      cmd: `${label}${overlay.command || overlay.reason || ''}`,
      reason: overlay.reason,
      countdown: overlay.canAnswer ? Math.ceil(remaining / 1000) : 0,
      canAnswer: overlay.canAnswer,
    };
  }

  /** 测试/调试用：当前审批卡渲染视图（没有卡时 null）。 */
  approvalViewForTest() {
    return this.approval ? this.#approvalView() : null;
  }

  /** 测试/调试用：当前问答卡渲染视图（没有卡时 null）。 */
  questionViewForTest() {
    return this.question ? this.#questionView() : null;
  }

  /** 一段短震动：USB 走 0x03 帧，BLE 走桥的 vibrate，两条链路都要生效。 */
  #buzz(ms) {
    const duration = Math.max(20, Math.min(2000, Math.round(ms)));
    if (this.link.state.connected) return this.#write(vibrate(duration)).catch(() => {});
    this.ble?.vibrate?.(duration);
    return Promise.resolve();
  }

  /** 测试/调试用：某个坐标命中的热区名。 */
  zoneAtForTest(x, y) {
    return this.#zoneAt({ x, y });
  }

  /* ------------------------------------------------------------ 问答（FR-9） */

  /**
   * 在手表上出问答卡：每页一个选项（设计稿只有 option/next/submit 三个热区），
   * 多选累加，提交回写。多个问题时按顺序逐题提交；忙时进同一条 FIFO 队列。
   * @returns {Promise<{answer?: {answers: Array<{id:string, selected:string[]}>}, fallback?: boolean, token?: object}>}
   */
  requestQuestion(request = {}) {
    const questions = Array.isArray(request?.questions) ? request.questions : [];
    if (!questions.length) return Promise.resolve({ fallback: true, token: null });
    if (request?.signal?.aborted) return Promise.resolve({ aborted: true, token: null });
    /* 表上根本显示不出来就别接管：直接交给电脑，别让用户等 30 s 超时。 */
    if (!this.#cardVisible()) return Promise.resolve({ fallback: true, token: null, reason: 'card-not-visible' });
    return this.#enqueueOverlay('question', request, (entry) => this.#activateQuestionEntry(entry));
  }

  #activateQuestionEntry(entry) {
    this.#dropNotice();
    const request = entry.input ?? {};
    const questions = Array.isArray(request?.questions) ? request.questions : [];
    const overlay = {
      kind: 'question',
      entry,
      sessionLabel: entry.sessionLabel,
      questions,
      qi: 0,
      page: 0,
      selections: questions.map(() => new Set()),
      settled: false,
      canAnswer: true,
      fallbackSince: 0,
      shownAt: Date.now(),
      timer: null,
    };
    this.question = overlay;
    this.touchDown = null;
    this.touchGuardUntil = Date.now() + 350; // 同上：卡出现瞬间的残留触点不算点击
    const first = questions[0];
    this.logger.info(
      `问答卡已出：${questions.length} 题，首题“${String(first?.question ?? '').slice(0, 24)}” ` +
        `${Array.isArray(first?.options) ? first.options.length : 0} 个选项` +
        `${first?.multiSelect ? '（多选）' : '（单选）'}` +
        `${overlay.sessionLabel ? `（${overlay.sessionLabel}）` : ''}` +
        `${`，手表优先 ${this.config.questions.watchTimeoutMs / 1000} s`}`,
    );
    this.#resetQuestionTimer(overlay);
    this.#paintSoon(0);
  }

  /**
   * 手表独占问答卡的倒计时。超时后 `next()` 交给电脑（否则电脑端看不到卡）；
   * 卡留在表上显示"请到电脑查看"，直到电脑答完/请求撤销再撤卡。
   * 手表上有操作会重置倒计时（用户正在翻页时不能突然抢走）。
   */
  #resetQuestionTimer(overlay) {
    clearTimeout(overlay.timer);
    if (overlay.settled || !overlay.canAnswer) return;
    const timeoutMs = this.config.questions.watchTimeoutMs;
    overlay.timer = setTimeout(() => {
      if (overlay.settled || this.question !== overlay || !overlay.canAnswer) return;
      overlay.canAnswer = false;
      overlay.fallbackSince = Date.now();
      this.logger.info(`问答 ${timeoutMs / 1000} s 无操作，移交给电脑`);
      this.#paintSoon(0);
      overlay.settled = true;
      overlay.entry?.finish({ fallback: true });
    }, timeoutMs);
    overlay.timer.unref?.();
  }

  clearQuestion(token = null) {
    if (!this.question) return;
    if (token && this.question.entry !== token) return;
    clearTimeout(this.question.timer);
    this.question = null;
    this.touchDown = null;
    this.logger.info('问答卡已撤');
    this.#paintSoon(0);
    this.#pumpOverlay();
  }

  #currentQuestion() {
    const overlay = this.question;
    if (!overlay) return null;
    return overlay.questions[overlay.qi] ?? null;
  }

  #questionOptions() {
    const question = this.#currentQuestion();
    return Array.isArray(question?.options) ? question.options : [];
  }

  #activateQuestion(zone) {
    const overlay = this.question;
    const options = this.#questionOptions();
    if (!overlay || !overlay.canAnswer || !options.length) return;
    const question = this.#currentQuestion();
    this.#resetQuestionTimer(overlay); // 用户正在操作：延长手表独占时间
    if (zone === 'option') {
      const selected = overlay.selections[overlay.qi];
      if (question.multiSelect) {
        if (selected.has(overlay.page)) selected.delete(overlay.page);
        else selected.add(overlay.page);
      } else {
        selected.clear();
        selected.add(overlay.page);
      }
    } else if (zone === 'next') {
      overlay.page = (overlay.page + 1) % options.length;
    } else if (zone === 'submit') {
      if (!overlay.selections[overlay.qi].size) {
        // 至少选一项（与设计稿 canSubmit 一致）。点了没反应会让人以为卡死：
        // 用一段短震动明确告诉用户"还没选"，卡片不动，可继续操作。
        this.#buzz(80);
        this.logger.info('手表问答：还没选任何项，提交被忽略（已短震提示）');
        return;
      }
      if (overlay.qi < overlay.questions.length - 1) {
        overlay.qi += 1;
        overlay.page = 0;
      } else {
        overlay.settled = true;
        const answers = overlay.questions.map((item, index) => {
          const labels = Array.from(overlay.selections[index])
            .sort((a, b) => a - b)
            .map((optionIndex) => item.options?.[optionIndex]?.label)
            .filter((label) => typeof label === 'string');
          return { id: item.id, selected: labels };
        });
        this.logger.info(`手表问答：提交 ${answers.map((a) => a.selected.length).join('+')} 项`);
        overlay.entry?.finish({ answer: { answers } });
      }
    }
    this.#paintSoon(0);
  }

  #questionView() {
    const overlay = this.question;
    const question = this.#currentQuestion();
    const options = this.#questionOptions().map((option) => option?.label ?? '');
    const label = overlay.sessionLabel ? `【${overlay.sessionLabel}】` : '';
    return {
      multiSelect: question?.multiSelect === true,
      prompt: `${label}${String(question?.question ?? '')}`,
      options,
      index: Math.min(overlay.page, Math.max(0, options.length - 1)),
      selected: Array.from(overlay.selections[overlay.qi] ?? []),
      canAnswer: overlay.canAnswer,
      /** 出卡时刻：多行长选项的翻页从这一刻起算，保证总是先看到第 1 页。 */
      shownAt: overlay.shownAt ?? 0,
    };
  }
}
