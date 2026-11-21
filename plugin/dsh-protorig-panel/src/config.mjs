/*
 * config.mjs —— 插件配置（默认值 + DSH 设置界面 schema）
 *
 * DSH 的设置表单只渲染 Config schema 里标了 `.volatile()` 的字段
 * （见 @deepseek-ai/dsh-settings README：forms expose only volatile fields）。
 * 用户会调的项全部标 volatile；改完保存后 Cordis 会更新引用值，插件在下一次
 * tick/事件里即时生效，不需要重启宿主。
 *
 * 所有默认值必须与 `apply()` 拿到的 config 一致：宿主会用 schema 补齐默认值，
 * 但本文件仍做一次 normalize，保证插件在旧宿主/手写 YAML 下也不会读到 undefined。
 */
import z from '@deepseek-ai/schemastery';
import { normalizeAdminId } from './device-id.mjs';

const REMIND_LEGACY = ['both', 'vibrate', 'bright', 'off'];

function unionOf(values) {
  return z.union(values.map((value) => z.const(value)));
}

/** 提醒三档：布尔开关（新版）或旧版字符串（兼容既有 patch）。 */
const reminderEvent = () =>
  z.union([z.boolean(), ...REMIND_LEGACY.map((value) => z.const(value))]).default(true).volatile();

export const Config = z.object({
  /** 串口号；空字符串 = 自动发现 VID 349B / PID 6160 的手表。 */
  port: z.string().default('').volatile(),
  /**
   * 设备身份：6 位十六进制管理员编号（admin_id，默认表盘上显示的那个）。
   * 空 = 不绑定（多块表时自动选第一个，与升级前行为一致）。
   * Win/mac 存同一个值 —— BLE 广播名后缀、USB `adminid` 都是它（research/73 §1）。
   */
  deviceId: z.string().default('').volatile(),
  /** 面板刷新率（fps）。链路实测整屏 1.7 fps，局部刷新才允许到 5。 */
  fps: z.union([z.const(1), z.const(2), z.const(5)]).default(2).volatile(),
  /** 插线后是否自动发 `pcmode 1` 进入电脑模式。 */
  autoEnter: z.boolean().default(true).volatile(),
  /**
   * 审批/问答排队上限（秒）：多张卡同时到达时，后一张最多在队列里等多久。
   * 上限固定 60 s（手表上等太久没有意义，超过就直接交回电脑）。
   */
  queueWaitSec: z.number().min(10).max(60).default(60).volatile(),
  /** 断线重连间隔（ms）。 */
  reconnectDelayMs: z.number().min(500).max(10000).default(1500),
  /** 设计稿 tokens 目录；留空用包内 design/，再回退仓库 local/design/dsh_panel_v4。 */
  design: z.string().default(''),

  /** 提醒：三档事件 + 震动/亮度两个独立通道 + 振动模式。 */
  reminder: z.object({
    done: reminderEvent(),
    needs: reminderEvent(),
    failed: reminderEvent(),
    /** 震动通道开关。 */
    vibrate: z.boolean().default(true).volatile(),
    /** 亮度脉冲通道开关。 */
    bright: z.boolean().default(true).volatile(),
    /** 振动模式：短/中/长/自定义。 */
    vibratePreset: z.union([z.const('short'), z.const('medium'), z.const('long'), z.const('custom')])
      .default('medium').volatile(),
    /** 自定义振动时长（ms），仅 vibratePreset=custom 时使用。 */
    vibrateMs: z.number().min(20).max(2000).default(180).volatile(),
  }).default({}),

  /** USB 连接开关 + 该模式显示参数（与手表模式分开）。 */
  usb: z.object({
    /** 关 = 不打开串口；仍可用「USB 探测注册」扫描登记。 */
    enabled: z.boolean().default(true).volatile(),
    brightness: z.number().min(32).max(255).default(201).volatile(),
    alwaysOn: z.boolean().default(true).volatile(),
    /** 息屏时间（秒）；0 = 跟随手表自己的全局息屏（固件默认 15 s，用户可在终端设置里改）。仅 alwaysOn=false 时生效。 */
    screenOffSec: z.number().min(0).max(600).default(0).volatile(),
  }).default({}),

  /** 旧字段（v0.2）：兼容保留，normalize 时作为 usb/ble 的兜底。 */
  display: z.object({
    /** 进入电脑模式后的默认亮度 32..255。 */
    brightness: z.number().min(32).max(255).default(128).volatile(),
    /**
     * 旧版（v0.2）兜底字段：PC 模式是否常亮。现在设置页按通道写
     * usb.alwaysOn / ble.alwaysOn（PC 帧 0x15），这个字段只在旧 patch 缺新字段时兜底。
     */
    alwaysOn: z.boolean().default(true).volatile(),
  }).default({}),

  /** 审批：手表优先，超时交给电脑。 */
  approval: z.object({
    enabled: z.boolean().default(true).volatile(),
    watchTimeoutMs: z.union([z.const(5000), z.const(10000), z.const(20000)])
      .default(10000).volatile(),
  }).default({}),

  /** 问答（ask_user_question）。 */
  questions: z.object({
    enabled: z.boolean().default(true).volatile(),
    /**
     * 手表独占问答卡的时长：超时后交给电脑（否则电脑端永远看不到这张卡）。
     * 手表上有操作会重置计时。
     */
    watchTimeoutMs: z.union([z.const(15000), z.const(30000), z.const(60000)])
      .default(30000).volatile(),
  }).default({}),

  /** BLE 降级：USB 不在时用 @stoprocent/noble（npm 原生绑定）推数字通道。 */
  ble: z.object({
    /** 蓝牙探测/连接开关。关 = 不扫描不连接；仍可用「蓝牙探测注册」扫一轮。 */
    enabled: z.boolean().default(true).volatile(),
    name: z.string().default('PROTORIG ES'),
    address: z.string().default(''),
    metricsHz: z.number().min(1).max(10).default(5),
    brightness: z.number().min(32).max(255).default(201).volatile(),
    alwaysOn: z.boolean().default(true).volatile(),
    screenOffSec: z.number().min(0).max(600).default(0).volatile(),
  }).default({}),

  /** 余额（FR-6）：已接 DeepSeek /user/balance；凭据走 credentials seam 或 DEEPSEEK_API_KEY。 */
  balance: z.object({
    enabled: z.boolean().default(true).volatile(),
    intervalSec: z.number().min(30).max(600).default(60).volatile(),
  }).default({}),
  /** 旧字段（v0.1 配置）；normalize 时映射到 balance.enabled。 */
  showBalance: z.boolean().default(true),

  /** 高级：烧录。 */
  flash: z.object({
    /** 默认 bin 绝对路径（设置页可改）。 */
    bin: z.string().default('').volatile(),
    /** ROM 烧录波特率。 */
    baud: z.union([z.const(1000000), z.const(2000000), z.const(3000000)])
      .default(2000000).volatile(),
  }).default({}),

  /** 打印手表原始行、链接调试信息。 */
  debug: z.boolean().default(false),
  /** 可选：把插件日志追加到这个文件（开发副本用；默认关闭）。 */
  logFile: z.string().default(''),
});

const FPS = new Set([1, 2, 5]);
const TIMEOUTS = new Set([5000, 10000, 20000]);
const QUESTION_TIMEOUTS = new Set([15000, 30000, 60000]);
const PRESETS = new Set(['short', 'medium', 'long', 'custom']);
const BAUDS = new Set([1000000, 2000000, 3000000]);

/**
 * `.volatile()` 字段在 Cordis 里是运行时引用（{ get(), [write]() }），不是字面值：
 * 宿主设置界面改完值会调用引用上的 write，get() 才能拿到最新值。
 * 所以每次 normalize 都要把引用解开，而不是在启动时快照一次。
 */
function value(raw) {
  return raw && typeof raw === 'object' && typeof raw.get === 'function' && !Array.isArray(raw)
    ? raw.get()
    : raw;
}

const pick = (raw, allowed, fallback) => (allowed.has(value(raw)) ? value(raw) : fallback);
const clamp = (raw, lo, hi, fallback) =>
  (Number.isFinite(value(raw)) ? Math.min(hi, Math.max(lo, value(raw))) : fallback);
const text = (raw, fallback = '') => {
  const v = value(raw);
  return typeof v === 'string' ? v : fallback;
};
/** 布尔开关；兼容旧版 'off' 字符串。 */
const enabled = (raw, fallback = true) => {
  const v = value(raw);
  if (v === undefined || v === null) return fallback;
  if (typeof v === 'boolean') return v;
  if (typeof v === 'string') return v !== 'off' && v !== 'false' && v !== '0';
  return fallback;
};

/** 把任意（可能缺字段、含 volatile 引用的）配置补全成插件内部使用的形状。 */
export function normalizeConfig(raw) {
  const config = raw && typeof raw === 'object' ? raw : {};
  const reminder = config.reminder && typeof config.reminder === 'object' ? config.reminder : {};
  const display = config.display && typeof config.display === 'object' ? config.display : {};
  const approval = config.approval && typeof config.approval === 'object' ? config.approval : {};
  const questions = config.questions && typeof config.questions === 'object' ? config.questions : {};
  const ble = config.ble && typeof config.ble === 'object' ? config.ble : {};
  const usb = config.usb && typeof config.usb === 'object' ? config.usb : {};
  const displayBrightness = Math.round(clamp(display.brightness, 32, 255, 201));
  const balance = config.balance && typeof config.balance === 'object' ? config.balance : {};
  const flash = config.flash && typeof config.flash === 'object' ? config.flash : {};
  const bleAddress = text(ble.address).trim();
  const bleName = text(ble.name);
  return {
    port: text(config.port).trim(),
    deviceId: normalizeAdminId(text(config.deviceId)),
    fps: pick(config.fps, FPS, 2),
    autoEnter: value(config.autoEnter) !== false,
    queueWaitSec: Math.round(clamp(config.queueWaitSec, 10, 60, 60)),
    reconnectDelayMs: clamp(config.reconnectDelayMs, 500, 10000, 1500),
    design: text(config.design).trim(),
    reminder: {
      done: enabled(reminder.done),
      needs: enabled(reminder.needs),
      failed: enabled(reminder.failed),
      vibrate: enabled(reminder.vibrate),
      bright: enabled(reminder.bright),
      vibratePreset: pick(reminder.vibratePreset, PRESETS, 'medium'),
      vibrateMs: Math.round(clamp(reminder.vibrateMs, 20, 2000, 180)),
    },
    display: {
      brightness: displayBrightness,
      alwaysOn: enabled(display.alwaysOn),
    },
    usb: {
      enabled: enabled(usb.enabled),
      brightness: Math.round(clamp(usb.brightness, 32, 255, displayBrightness)),
      alwaysOn: enabled(usb.alwaysOn),
      screenOffSec: Math.round(clamp(usb.screenOffSec, 0, 600, 0)),
    },
    approval: {
      enabled: value(approval.enabled) !== false,
      watchTimeoutMs: pick(approval.watchTimeoutMs, TIMEOUTS, 10000),
    },
    questions: {
      enabled: value(questions.enabled) !== false,
      watchTimeoutMs: pick(questions.watchTimeoutMs, QUESTION_TIMEOUTS, 30000),
    },
    ble: {
      enabled: value(ble.enabled) !== false,
      name: bleName || 'PROTORIG ES',
      address: bleAddress,
      metricsHz: clamp(ble.metricsHz, 1, 10, 5),
      brightness: Math.round(clamp(ble.brightness, 32, 255, displayBrightness)),
      alwaysOn: enabled(ble.alwaysOn),
      screenOffSec: Math.round(clamp(ble.screenOffSec, 0, 600, 0)),
    },
    balance: {
      // 旧字段 showBalance 作为兜底
      enabled: value(balance.enabled) === undefined ? value(config.showBalance) === true : enabled(balance.enabled),
      intervalSec: Math.round(clamp(balance.intervalSec, 30, 600, 60)),
    },
    flash: {
      bin: text(flash.bin).trim(),
      baud: pick(flash.baud, BAUDS, 2000000),
    },
    showBalance: value(config.showBalance) === true,
    debug: value(config.debug) === true,
    logFile: text(config.logFile).trim(),
  };
}

/** 设置页用的默认值（客户端另有一份，保持一致）。 */
export const CONFIG_DEFAULTS = Object.freeze({
  port: '',
  deviceId: '',
  fps: 2,
  autoEnter: true,
  queueWaitSec: 60,
  reminder: { done: true, needs: true, failed: true, vibrate: true, bright: true, vibratePreset: 'medium', vibrateMs: 180 },
  usb: { enabled: true, brightness: 201, alwaysOn: true, screenOffSec: 0 },
  display: { brightness: 201, alwaysOn: true },
  approval: { enabled: true, watchTimeoutMs: 10000 },
  questions: { enabled: true, watchTimeoutMs: 30000 },
  ble: { enabled: true, brightness: 201, alwaysOn: true, screenOffSec: 0 },
  balance: { enabled: true, intervalSec: 60 },
  flash: { bin: '', baud: 2000000 },
});
