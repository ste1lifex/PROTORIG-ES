/*
 * link.mjs —— 手表串口链路
 *
 * 负责：发现 Bouffalo USB Console（VID 349B / PID 6160，应用口 MI_00）、
 * 打开/关闭、带背压的写、把文本流解析成事件、断线自动重连、对时、设备身份。
 *
 * 平台差异在 platform.mjs：Windows 靠 pnpId 的 MI_00 认应用口；
 * macOS 是 `/dev/cu.usbmodem*`，没有 MI_00/pnpId 可用 —— 排序只是启发式，
 * 最终一定靠 `adminid` 探活确认是哪块表（research/73 §3.2、74 §2.3）。
 *
 * 事件：
 *   'open'    {path}
 *   'close'   {reason}
 *   'error'   Error
 *   'pc'      {value}        0/1 进出电脑模式
 *   'mode'    {value}        0/1 透传开关（1 = 可以发像素）
 *   'hello'   {ver,maxW,maxH,fmt}
 *   'status'  {link,battery,bright,page}
 *   'touch'   {pressed,x,y}
 *   'admin'   {admin,chip,chipOk}   shell `adminid` 的输出（设备身份）
 *   'line'    {text}        其它行（调试用，仅 debug=true 时发）
 *   'text'    {text}        每一行原始文本（始终发；shell `command()` 等应答用）
 */
import { EventEmitter } from 'node:events';
import { loadSerialPort } from './deps.mjs';
import { parseAdminLine } from './device-id.mjs';
import { sortWatchPorts } from './platform.mjs';
import { LineParser, parseLine, frame, hello, statusRequest } from './protocol.mjs';

export const VID = '349b';
export const PID = '6160';

export class WatchLink extends EventEmitter {
  constructor({ baudRate = 115200, autoReconnect = true, reconnectDelayMs = 1500, debug = false } = {}) {
    super();
    this.baudRate = baudRate;
    this.autoReconnect = autoReconnect;
    this.reconnectDelayMs = reconnectDelayMs;
    this.debug = debug;
    this.port = null;
    this.path = null;
    this.closing = false;
    this.reconnectTimer = null;
    this.parser = new LineParser((line) => this.#onLine(line));
    this.state = {
      connected: false,
      path: null,
      pcMode: false,
      passthrough: false,
      hello: null,
      status: null,
      adminId: null,
    };
  }

  /* ------------------------------------------------------------ 发现 */

  /**
   * 列出所有 Bouffalo 口，应用口（MI_00 / macOS cu.usbmodem）排前面。
   * 排序只是"先试哪个"，身份判定一律用 adminid（见 queryAdminId）。
   */
  static async discover(platform = process.platform) {
    const SerialPort = await loadSerialPort();
    const ports = await SerialPort.list();
    return sortWatchPorts(ports, platform);
  }

  /* ------------------------------------------------------------ 打开 */

  async open(path) {
    const SerialPort = await loadSerialPort();
    const target = path ?? (await WatchLink.discover())[0]?.path;
    if (!target) {
      throw new Error('没找到手表：期望 VID 349B / PID 6160 的 USB 串口（手表睡眠或没插线时没有）');
    }
    this.closing = false;
    return new Promise((resolve, reject) => {
      const port = new SerialPort({ path: target, baudRate: this.baudRate, autoOpen: false });
      port.open((err) => {
        if (err) {
          reject(err);
          return;
        }
        this.port = port;
        this.path = target;
        this.state.connected = true;
        this.state.path = target;
        this.state.adminId = null; // 换口/重连都是新身份，等 adminid 重新问
        port.on('data', (chunk) => this.parser.push(chunk));
        port.on('close', () => this.#onClose());
        port.on('error', (e) => this.emit('error', e));
        this.emit('open', { path: target });
        resolve(target);
      });
    });
  }

  async close() {
    this.closing = true;
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    const port = this.port;
    this.port = null;
    this.state.connected = false;
    if (!port) return;
    await new Promise((resolve) => port.close(() => resolve()));
  }

  #onClose() {
    this.state.connected = false;
    this.state.pcMode = false;
    this.state.passthrough = false;
    this.state.adminId = null;
    this.port = null;
    this.emit('close', { reason: this.closing ? 'requested' : 'device' });
    if (!this.closing && this.autoReconnect) {
      this.reconnectTimer = setTimeout(() => {
        this.reconnectTimer = null;
        this.open().catch((e) => this.emit('error', e));
      }, this.reconnectDelayMs);
    }
  }

  /* ------------------------------------------------------------ 写 */

  /** 写一帧，等 OS 真正收下（背压交给串口驱动）。 */
  async write(buf) {
    const port = this.port;
    if (!port || !port.isOpen) throw new Error('串口没打开');
    await new Promise((resolve, reject) => {
      port.write(buf, (err) => (err ? reject(err) : resolve()));
    });
    // 驱动缓冲区满时等一下 drain，避免把主机内存打爆
    if (port.writableNeedDrain) {
      await new Promise((resolve) => port.once('drain', resolve));
    }
  }

  /** 发一行 shell 文本命令（电脑模式下也有效：不以 0xA5 开头就会交给 shell）。 */
  async shell(line) {
    await this.write(Buffer.from(`${line}\r\n`, 'ascii'));
  }

  /**
   * 发一行 shell 命令并等它的应答行（P8/P9 的 face/faceimg 用）。
   *
   * 手表 shell 会回显命令行、提示符也会粘行，所以这里**按匹配器取第一个命中行**，
   * 不假设第几行。超时返回 null（collect=true 时返回期间收到的全部行）。
   *
   * @param {string} line 要发的命令（不含换行）
   * @param {{timeoutMs?:number, match?:RegExp|((text:string)=>boolean), collect?:boolean}} opts
   * @returns {Promise<string|null|string[]>}
   */
  async command(line, { timeoutMs = 3000, match = null, collect = false } = {}) {
    const predicate = typeof match === 'function'
      ? match
      : (text) => (match ? match.test(text) : true);
    const seen = [];
    const got = new Promise((resolve) => {
      let timer = null;
      const onText = (ev) => {
        const text = cleanShellLine(ev?.text ?? ev ?? '');
        if (!text) return;
        seen.push(text);
        if (collect) return;
        if (predicate(text)) {
          cleanup();
          resolve(text);
        }
      };
      const cleanup = () => {
        clearTimeout(timer);
        this.off('text', onText);
      };
      timer = setTimeout(() => {
        cleanup();
        resolve(collect ? seen : null);
      }, timeoutMs);
      timer.unref?.();
      this.on('text', onText);
    });
    await this.shell(line);
    return got;
  }

  /** 让手表 RTC 与主机时间同步（固件 cmd_settime：yy mm dd hh mm ss）。 */
  async syncTime(date = new Date()) {
    const p2 = (n) => String(n).padStart(2, '0');
    const cmd = [
      'settime',
      p2(date.getFullYear() % 100),
      p2(date.getMonth() + 1),
      p2(date.getDate()),
      p2(date.getHours()),
      p2(date.getMinutes()),
      p2(date.getSeconds()),
    ].join(' ');
    await this.shell(cmd);
    return cmd;
  }

  /** 发 HELLO，返回请求本身（回复走 'hello' 事件）。 */
  async sayHello() {
    await this.write(hello());
  }

  async requestStatus() {
    await this.write(statusRequest());
  }

  /**
   * 主动查询电脑模式状态。
   *
   * PC 1 / MODE 1 只在**跳变**时广播，重连后拿不到历史值，所以必须问一次。
   * 固件回 `[pc] active=.. transport=.. passthrough=.. usbraw=..`。
   */
  async queryState(timeoutMs = 1200) {
    const deadline = Date.now() + timeoutMs;
    const got = new Promise((resolve) => {
      const on = (ev) => {
        clearTimeout(timer);
        this.off('pcstate', on);
        resolve(ev);
      };
      const timer = setTimeout(() => {
        this.off('pcstate', on);
        resolve(null);
      }, timeoutMs);
      this.on('pcstate', on);
    });
    await this.shell('pcmode');
    return got;
  }

  /**
   * 问设备身份：shell `adminid` → `admin=2BEA4E chip=...`（research/74 §2.3）。
   *
   * 必须在 `pcmode 1` **之前**问（进 PC 模式后串口被二进制帧接管，
   * shell 不一定还应答）；握手的第一步就是它，所以挂在已经打开的口上问，
   * 不需要为了探活再开一个串口（少一次开关就少一次和串口助手抢口的机会）。
   *
   * @returns {Promise<{admin:string, chip:string, chipOk:boolean}|null>}
   */
  async queryAdminId(timeoutMs = 1200) {
    const got = new Promise((resolve) => {
      const on = (ev) => {
        clearTimeout(timer);
        this.off('admin', on);
        resolve(ev);
      };
      const timer = setTimeout(() => {
        this.off('admin', on);
        resolve(null);
      }, timeoutMs);
      this.on('admin', on);
    });
    await this.shell('adminid').catch(() => {});
    return got;
  }

  /* ------------------------------------------------------------ 解析 */

  #onLine(line) {
    // 始终保留最近若干行：调试与诊断（pcmode 计数）都要用
    this.tail = this.tail ?? [];
    this.tail.push(line);
    if (this.tail.length > 40) this.tail.shift();
    // shell 应答（W-OK / [faceimg] ...）需要稳定的行事件：始终发 'text'
    this.emit('text', { text: line });
    if (this.debug) this.emit('line', { text: line });
    const ev = parseLine(line);
    if (!ev) return;
    switch (ev.type) {
      case 'pc':
        this.state.pcMode = ev.value === 1;
        this.emit('pc', { value: ev.value });
        break;
      case 'mode':
        this.state.passthrough = ev.value === 1;
        this.emit('mode', { value: ev.value });
        break;
      case 'hello':
        this.state.hello = ev;
        this.emit('hello', ev);
        break;
      case 'status':
        this.state.status = ev;
        this.emit('status', ev);
        break;
      case 'touch':
        this.emit('touch', ev);
        break;
      case 'pcstate':
        this.state.pcMode = ev.active;
        this.state.passthrough = ev.passthrough;
        this.state.transport = ev.transport;
        this.emit('pcstate', ev);
        break;
      case 'admin':
        this.state.adminId = ev.admin;
        this.emit('admin', ev);
        break;
      case 'frame':
        this.emit('frame', ev);
        break;
      /* USB 文本行里的卡片层事件（0x58 热区 / 0x59 回执 / 0x92 整屏重发请求）。
       * 以前这几种事件解析出来没人转发；现在统一走 'event'，由 panel-service 消费
       * （语义与 BLE notify 完全一致，判定只有一份）。 */
      case 'zones':
      case 'cardack':
      case 'redraw':
        this.emit('event', ev);
        break;
      default:
        break;
    }
  }
}

/** 去掉 shell 提示符/ANSI/空白，供 command() 匹配。 */
export function cleanShellLine(raw) {
  return String(raw ?? '')
    .replace(/\x1b\[[0-9;]*[A-Za-z]/g, '')
    .replace(/bouffalolab\s*\/>/g, '')
    .replace(/\x00/g, '')
    .trim();
}

/**
 * 单独开一个串口问 `adminid`（P5 的逐口探活）。
 *
 * 只用于"这个口不是当前连接口"的场景（设置页扫描候选、另一块表）；
 * 当前连接的口身份由握手里的 `WatchLink.queryAdminId()` 提供，
 * 绝不对已经打开的口再开一次（Windows 会直接报占用）。
 *
 * 任何失败（占用/不是手表/没回包）都返回 ''，不抛错 —— 扫描要能继续下一口。
 *
 * @param {string} path 串口路径（COM8 / /dev/cu.usbmodem…）
 * @param {{baudRate?:number, timeoutMs?:number, SerialPort?:Function}} opts
 * @returns {Promise<string>} 6 位大写 admin_id，或 ''
 */
export async function probePortAdminId(path, { baudRate = 115200, timeoutMs = 1200, SerialPort: Ctor } = {}) {
  if (!path) return '';
  let SerialPort = Ctor;
  try {
    SerialPort = SerialPort ?? (await loadSerialPort());
  } catch {
    return '';
  }
  let admin = '';
  const parser = new LineParser((line) => {
    const found = parseAdminLine(line);
    if (found && !admin) admin = found.admin;
  });
  let port = null;
  try {
    port = new SerialPort({ path, baudRate, autoOpen: false });
  } catch {
    return '';
  }
  await new Promise((resolve) => {
    let settled = false;
    let timer = null;
    const closePort = () => {
      try {
        if (port.isOpen) port.close(() => resolve());
        else resolve();
      } catch {
        resolve();
      }
    };
    const stop = () => {
      if (settled) return;
      settled = true;
      if (timer) clearTimeout(timer);
      closePort();
    };
    timer = setTimeout(stop, timeoutMs);
    timer.unref?.();
    port.on('data', (chunk) => {
      parser.push(chunk);
      if (admin) stop();
    });
    port.on('error', stop);
    port.open((err) => {
      if (err) {
        stop();
        return;
      }
      try {
        port.write('adminid\r\n', () => {});
      } catch {
        stop();
      }
    });
  });
  return admin;
}
