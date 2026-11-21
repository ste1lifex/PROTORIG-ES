/*
 * platform.mjs —— 宿主平台差异（Windows / macOS）集中在这里
 *
 * 为什么单独一层：插件要在 Win/mac 上用同一套代码连同一块手表（research/73 §1、§3.2），
 * 但两边看串口的方式不一样：
 *
 *   · Windows：`SerialPort.list()` 给 pnpId（`USB\VID_349B&PID_6160&MI_00\...`），
 *     BL616 复合设备有 MI_00（应用 CDC）和 MI_02（其它接口）两个口 —— 应用口靠 MI_00 认，
 *     这个逻辑保持原样（research/56 §9、63 已真机验证）。
 *   · macOS：`/dev/cu.usbmodem*`（callout 口）才是能直接写的口，`/dev/tty.usbmodem*`
 *     会等 DCD；两者其实是同一台设备的两条路径，必须去重，否则逐口探活会重复打开。
 *     pnpId 在 macOS 上不可靠，**不能靠排序决定连哪块表** ——
 *     所以默认走 admin_id 逐口探活（见 device-id.mjs / link.mjs）；
 *     这里的排序只是"少开一个口"的启发式。
 *
 * 烧录：Windows = `flash.ps1` + BLFlashCommand.exe（PowerShell 后端），
 * macOS = SDK 自带的 BLFlashCommand-macos（serialport 后端，需要 Rosetta 2）。
 * 两者由 index.mjs 按 flashBackend() 二选一；其它平台不渲染入口（research/73 §3.1、§5）。
 */

/** Bouffalo BL616 USB Console（应用 CDC 口）。 */
export const WATCH_USB = Object.freeze({ vendorId: '349b', productId: '6160' });

const hex = (value) => String(value ?? '').trim().toLowerCase().replace(/^0x/, '');

/** 'windows' | 'macos' | 'linux' | 'other'（未知平台按 other 处理，不假装支持）。 */
export function osKind(platform = process.platform) {
  if (platform === 'win32') return 'windows';
  if (platform === 'darwin') return 'macos';
  if (platform === 'linux') return 'linux';
  return 'other';
}

/** 给设置页/日志看的中文平台名。 */
export function osLabel(platform = process.platform) {
  switch (osKind(platform)) {
    case 'windows': return 'Windows';
    case 'macos': return 'macOS';
    case 'linux': return 'Linux';
    default: return String(platform ?? '未知平台');
  }
}

/** 烧录能力。
 *  · Windows：`flash.ps1` + `BLFlashCommand.exe`（PowerShell 后端）
 *  · macOS  ：`BLFlashCommand-macos` + Rosetta 2（serialport 后端，2026-11-08 新增）
 *  其它平台没有后端。 */
export function flashSupported(platform = process.platform) {
  const kind = osKind(platform);
  return kind === 'windows' || kind === 'macos';
}

/** 该平台用哪个烧录后端（null = 不支持）。 */
export function flashBackend(platform = process.platform) {
  const kind = osKind(platform);
  if (kind === 'windows') return 'powershell';
  if (kind === 'macos') return 'blflashcommand';
  return null;
}

/** 烧录不可用时的说明（设置页与 HTTP 路由共用同一句）。 */
export function flashUnsupportedReason(platform = process.platform) {
  if (flashSupported(platform)) return '';
  return `烧录只在 Windows / macOS 上可用（当前 ${osLabel(platform)}）。`;
}

const macPortPattern = /usbmodem/i;
/** macOS 上 tty.* 与 cu.* 是同一台设备的两条路径：只保留 cu.*（能直接写、不等 DCD）。 */
function dedupeMacPorts(ports) {
  const cuPaths = new Set(
    ports.filter((port) => /\/cu\./.test(String(port.path ?? ''))).map((port) => String(port.path).replace(/\/cu\./, '/tty.')),
  );
  return ports.filter((port) => {
    const path = String(port.path ?? '');
    if (!/\/tty\./.test(path)) return true;
    return !cuPaths.has(path);
  });
}

/**
 * 是否是手表的候选串口。
 *
 * Windows/Linux：必须 VID 349B + PID 6160（保持原行为，绝不放宽 —— 放宽会打开无关串口）。
 * macOS：同样优先 VID/PID；VID/PID 拿不到时接受 `/dev/cu.usbmodem*`
 *        （BL616 是 USB CDC，macOS 上就叫 usbmodem），这类口标 weak=true，
 *        排序靠后，最终仍要靠 `adminid` 探活确认是不是手表。
 */
export function isWatchPort(port, platform = process.platform) {
  if (!port || !port.path) return false;
  const vendorId = hex(port.vendorId);
  const productId = hex(port.productId);
  if (vendorId && productId) {
    return vendorId === WATCH_USB.vendorId && productId === WATCH_USB.productId;
  }
  if (osKind(platform) === 'macos') {
    // 没有 VID/PID 时才走名字兜底（weak）；有 VID/PID 但不是手表的口一律排除
    return !vendorId && !productId && macPortPattern.test(String(port.path));
  }
  return false;
}

/** 这个口是不是"仅靠名字猜的"（macOS 没有 VID/PID 时）。 */
export function isWeakPort(port, platform = process.platform) {
  if (!isWatchPort(port, platform)) return false;
  const vendorId = hex(port.vendorId);
  const productId = hex(port.productId);
  return !(vendorId && productId);
}

/**
 * 候选口排序权重（只用于"先试哪个"，不用于身份判定）。
 *
 * Windows：MI_00 应用口 0 / 其它接口 5 / 没有 pnpId 9（原逻辑，保持不变）。
 * macOS：**不看 pnpId**（那边不可靠），cu.usbmodem 0 / 其它 cu 3 / tty 8。
 */
export function rankPort(port, platform = process.platform) {
  if (osKind(platform) === 'windows') {
    const pnpId = String(port?.pnpId ?? '');
    if (/MI_00/i.test(pnpId)) return 0;
    if (/MI_/i.test(pnpId)) return 5;
    return 9;
  }
  if (osKind(platform) === 'macos') {
    const path = String(port?.path ?? '');
    if (isWeakPort(port, platform)) return 3;
    if (/\/cu\.usbmodem/.test(path)) {
      /* ROM 下载口与应用口的 VID/PID 完全一样（349b:6160），只能靠 iSerialNumber 区分：
       * 应用口有（实测 00000001），ROM 口没有。有序列号的排前面，
       * 这样即使 flashHold 丢了也不会优先抓到 ROM 口。 */
      return String(port?.serialNumber ?? '') ? 0 : 2;
    }
    if (/\/cu\./.test(path)) return 1;
    if (/\/tty\./.test(path)) return 8;
    return 9;
  }
  return 9;
}

/**
 * 过滤 + 稳定排序候选串口。
 * 排序只在 rank 不同时生效，rank 相同保持 `SerialPort.list()` 的原顺序
 * （热插拔时顺序本来就不稳定，最终判定一律靠 admin_id 探活）。
 */
export function sortWatchPorts(ports, platform = process.platform) {
  const list = Array.isArray(ports) ? ports.filter((port) => port && port.path) : [];
  const filtered = list.filter((port) => isWatchPort(port, platform));
  const deduped = osKind(platform) === 'macos' ? dedupeMacPorts(filtered) : filtered;
  return deduped
    .map((port, index) => ({ port, index, rank: rankPort(port, platform) }))
    .sort((a, b) => a.rank - b.rank || a.index - b.index)
    .map((entry) => entry.port);
}

/** 界面上显示的端口名（Windows 就是 COM8；macOS 给完整 /dev 路径，短名会撞车）。 */
export function portLabel(port, platform = process.platform) {
  const value = typeof port === 'string' ? port : port?.path;
  return String(value ?? '');
}
