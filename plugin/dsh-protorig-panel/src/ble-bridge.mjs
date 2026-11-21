/*
 * ble-bridge.mjs —— BLE 数字通道降级（无 USB 时），**纯 npm 实现**
 *
 * 2026-11-09 重写：原实现 spawn 一个外部脚本桥（需要宿主另装运行时）——
 * 那是 npm 之外的外部依赖，发行版不能接受。现在改用 @stoprocent/noble：
 *   · npm 包，自带 darwin-x64+arm64 / win32-x64 预编译（node-gyp-build 选预编译）；
 *   · macOS 走 CoreBluetooth，Windows 走 WinRT 原生绑定；
 *   · 不需要用户另装任何运行时脚本，也不 spawn 子进程。
 *
 * 协议不变（与固件 ble_svc 一致）：A5 5A <cmd> <len> <payload...>
 *   0x51 PC_MODE  payload=[on]
 *   0x52 METRICS  payload=cpu u16, gpu u16, ram u16, vram u16, brand u8,
 *                          rx u32, tx u16（15 B 小端；与固件 svc_ble.c 一致）
 *   0x50 PC 帧通道（卡片层 0x53..0x57 也走这里）
 *   0x03 震动 / 0x02 亮度
 * 手表 → 电脑的 notify 用 protocol.parseBleFrame() 解码（touch/status/zones/cardack）。
 *
 * 生命周期：USB 断开 → start()；USB 恢复 → stop()；断开 → 退避重扫。
 * 任何失败只警告一次并禁用，**绝不影响 USB 面板**。
 */
import { parseBleFrame } from './protocol.mjs';
import { ADV_PREFIX, advNameFor, normalizeAdminId, parseAdvName } from './device-id.mjs';

/** GATT UUID（与固件 ble_svc.c 的 BT_UUID_128_ENCODE 完全一致；32 hex、无连字符小写）。
 *  2026-11-09 修：这三个常量原来多写了两个 0（34 hex），比较永远不相等，
 *  于是每次连接都被误判成"没找到 PROTORIG GATT 特征值"。 */
export const SVC_UUID = '50524f544f5249474553000000000001';
export const CHR_CMD_UUID = '50524f544f5249474553000000000002';
export const CHR_EVT_UUID = '50524f544f5249474553000000000003';

export const CMD_PC_FRAME = 0x50;
export const CMD_PC_MODE = 0x51;
export const CMD_METRICS = 0x52;
export const CMD_BRIGHT = 0x02;
export const CMD_VIBRATE = 0x03;

const normUuid = (uuid) => String(uuid ?? '').toLowerCase().replace(/-/g, '');

/** 组一帧 A5 5A cmd len payload。 */
export function bleFrame(cmd, payload = Buffer.alloc(0)) {
  const body = Buffer.from(payload ?? []);
  return Buffer.concat([Buffer.from([0xa5, 0x5a, cmd & 0xff, body.length & 0xff]), body]);
}

/** 0x52 的 payload（cpu/gpu/ram/vram 是十分之一个百分比；vram=-1 表示未知）。 */
export function metricsPayloadForBle(metrics) {
  const payload = Buffer.alloc(15);
  const clamp1000 = (v) => Math.max(0, Math.min(1000, Number.isFinite(v) ? Math.round(v) : 0));
  payload.writeUInt16LE(clamp1000(metrics?.cpu), 0);
  payload.writeUInt16LE(clamp1000(metrics?.gpu), 2);
  payload.writeUInt16LE(clamp1000(metrics?.ram), 4);
  const vram = Number.isFinite(metrics?.vram) && metrics.vram >= 0 ? clamp1000(metrics.vram) : 0xffff;
  payload.writeUInt16LE(vram, 6);
  payload.writeUInt8(Math.max(0, Math.min(3, Math.round(metrics?.brand ?? 0))), 8);
  payload.writeUInt32LE(Math.max(0, Math.round(metrics?.rx ?? 0)) >>> 0, 9);
  /* 固件 svc_ble.c 按 15 B 解析：rx 是 u32（9..12），tx 只读 u16（13..14）。 */
  payload.writeUInt16LE(Math.max(0, Math.min(0xffff, Math.round(metrics?.tx ?? 0))), 13);
  return payload;
}

/** GPU 厂商字符串 → 固件 brand 枚举（0 默认 / 1 NVIDIA / 2 AMD / 3 Intel）。 */
export function brandOf(vendor) {
  const text = String(vendor ?? '').toLowerCase();
  if (/nvidia|geforce|quadro/.test(text)) return 1;
  if (/amd|radeon|advanced micro devices/.test(text)) return 2;
  if (/intel/.test(text)) return 3;
  return 0;
}

/** MetricsCollector.sample() 的输出 → 0x52 payload 字段（cpu/gpu/ram/vram 是十分之一个百分比）。 */
export function metricsForBle(metrics) {
  const permille = (value) => (Number.isFinite(value) ? Math.max(0, Math.min(1000, Math.round(value * 10))) : 0);
  const vram = Number.isFinite(metrics.gpuMemoryPercent) ? permille(metrics.gpuMemoryPercent) : -1;
  return {
    cpu: permille(metrics.cpuPercent),
    gpu: permille(metrics.gpuPercent),
    ram: permille(metrics.memoryPercent),
    vram,
    brand: brandOf(metrics.gpuVendor ?? metrics.gpuName),
    rx: Number.isFinite(metrics.networkRxBps) ? Math.max(0, Math.round(metrics.networkRxBps)) : 0,
    tx: Number.isFinite(metrics.networkTxBps) ? Math.max(0, Math.round(metrics.networkTxBps)) : 0,
  };
}

/** 广播名是否命中（前缀匹配；地址命中时也接受）。纯函数，单测覆盖。 */
export function matchesAdvName(localName, { prefix = ADV_PREFIX, deviceId = '' } = {}) {
  const name = String(localName ?? '');
  const wanted = advNameFor(normalizeAdminId(deviceId), prefix);
  return name.startsWith(wanted);
}

const promiseWithTimeout = (promiseFactory, timeoutMs, label) => new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error(`${label} 超时（${timeoutMs} ms）`)), timeoutMs);
  timer.unref?.();
  Promise.resolve()
    .then(promiseFactory)
    .then((value) => {
      clearTimeout(timer);
      resolve(value);
    })
    .catch((error) => {
      clearTimeout(timer);
      reject(error);
    });
});

export class BleBridge {
  constructor({ config, logger, onState, onEvent, onDevice, nobleFactory } = {}) {
    this.config = config;
    this.logger = logger;
    this.onState = onState;
    this.onEvent = onEvent;
    this.onDevice = onDevice;
    /** 单测注入：返回一个 noble 实例（默认 @stoprocent/noble 的 withBindings('default')）。 */
    this.nobleFactory = nobleFactory ?? null;
    this.noble = null;
    this.foundName = null;
    this.child = null; // 兼容旧字段：纯 Node 实现没有子进程，恒为 null
    this.connected = false;
    this.disabled = false;
    this.metrics = null;
    this.timer = null;
    this.pushTimer = null;
    this.retryMs = 2000;   // 2026-11-14：15 s 太长，用户滑回 -1 后要等很久才能重连
    this.restarts = 0;
    this.starting = false;
    this.stopping = false;
    this.runtimeOk = null;
    this.stopPromise = null;
    this.lastAddress = null;
    this.zones = null;
    this.lastTouchAt = 0;
    this.badFrames = 0;
    this.connecting = false;
    /* 扫描是幂等的：面板每 0.5 s 调一次 start()，不能再重复 startScanning（WinRT 会重置 watcher）。 */
    this.scanning = false;
    this.scanStartedAt = 0;
    /** 只扫描登记、不自动连接（「蓝牙探测注册」用）。 */
    this.probeOnly = false;
    this.peripheral = null;
    this.cmdChar = null;
    this.evtChar = null;
    this.writeChain = Promise.resolve();
  }

  /** 加载 noble（懒加载：原生绑定缺失时只禁用 BLE，不影响插件其它功能）。 */
  async #loadNoble() {
    if (this.noble) return this.noble;
    if (this.nobleFactory) {
      this.noble = this.nobleFactory();
    } else {
      const pkg = await import('@stoprocent/noble');
      const factory = pkg.withBindings ?? pkg.default?.withBindings ?? pkg.default;
      this.noble = factory('default');
    }
    this.#attachNoble();
    return this.noble;
  }

  #attachNoble() {
    const noble = this.noble;
    noble.on('stateChange', (state) => {
      if (this.config?.debug) this.logger.debug?.(`BLE 状态：${state}`);
      if (state === 'poweredOff' && this.connected) this.#onDisconnected('poweredOff');
    });
    noble.on('discover', (peripheral) => this.#onDiscover(peripheral));
    noble.on('warning', (message) => {
      if (this.config?.debug) this.logger.debug?.(`BLE 警告：${message}`);
    });
  }

  /** 等待蓝牙可用（macOS 首次会弹权限；被拒绝时 state=unauthorized）。 */
  async #waitPoweredOn(timeoutMs = 8000) {
    const noble = this.noble;
    if (noble.state === 'poweredOn') return true;
    return new Promise((resolve) => {
      let settled = false;
      const finish = (ok) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        noble.removeListener('stateChange', onChange);
        resolve(ok);
      };
      const onChange = (state) => {
        if (state === 'poweredOn') finish(true);
        else if (state === 'unsupported' || state === 'unauthorized') finish(false);
      };
      const timer = setTimeout(() => finish(false), timeoutMs);
      timer.unref?.();
      noble.on('stateChange', onChange);
      /* 监听挂上后再查一次，避免状态在挂监听前就变了 */
      if (noble.state === 'poweredOn') finish(true);
      else if (noble.state === 'unsupported' || noble.state === 'unauthorized') finish(false);
    });
  }

  /**
   * 检查运行环境：noble 能加载 + 蓝牙可用。不可用则禁用降级（只警告一次）。
   * 结果缓存，避免每次重连都重新探测。
   */
  checkRuntime() {
    if (this.runtimeOk !== null) return Promise.resolve(this.runtimeOk);
    return (async () => {
      let noble;
      try {
        noble = await this.#loadNoble();
      } catch (error) {
        this.runtimeOk = false;
        this.disabled = true;
        this.logger.warn(
          `BLE 降级未启用：无法加载 @stoprocent/noble（${error.message}）。` +
            ' 该包带预编译，一般 npm install 后即可用；USB 像素面板不受影响。',
        );
        return false;
      }
      const state = String(noble.state ?? 'unknown');
      if (state === 'unsupported') {
        this.runtimeOk = false;
        this.disabled = true;
        this.logger.warn('BLE 降级未启用：系统/硬件不支持蓝牙低功耗（noble state=unsupported）。USB 不受影响。');
        return false;
      }
      const powered = await this.#waitPoweredOn();
      if (!powered) {
        this.runtimeOk = false;
        this.disabled = true;
        const finalState = String(this.noble?.state ?? state);
        this.logger.warn(
          `BLE 降级未启用：蓝牙不可用（state=${finalState}）。` +
            (finalState === 'unauthorized'
              ? ' macOS 请在 系统设置 → 隐私与安全性 → 蓝牙 给 DSH 授权后重启宿主。'
              : ' 请确认蓝牙已打开。') +
            ' USB 像素面板不受影响。',
        );
        return false;
      }
      this.runtimeOk = true;
      return true;
    })();
  }

  /**
   * 启动数字通道。幂等：并发/重复调用只会有一轮扫描（面板循环每 0.5 s 会调一次）。
   * @returns {Promise<boolean>} 是否（已经/正在）启动
   */
  async start() {
    /* 扫描未超时的重复调用直接忽略：本方法被面板循环每 0.5 s 调一次，
     * 重复 startScanning 会让 WinRT 反复重置 watcher（2026-11-09 实测刷屏 + 丢广播）。 */
    const scanStale = this.scanning && Date.now() - this.scanStartedAt > 120000;
    /* connecting 期间绝不能再 startScanning：WinRT 会在 GATT 连接中途重置
     * watcher，表现为"有时连上、有时一直连不上"（2026-11-14 用户实测）。 */
    if (this.disabled || this.connected || this.connecting || this.timer || this.starting) return false;
    if (this.scanning && !scanStale) return false;
    if (scanStale) {
      this.logger.warn?.('BLE 扫描 120 s 仍没发现手表，重启扫描');
      this.scanning = false;
    }
    this.starting = true;
    this.stopping = false;
    try {
      if (!(await this.checkRuntime())) return false;
      if (this.stopping) return false;
      if (this.noble.state !== 'poweredOn') {
        this.#scheduleRetry();
        return false;
      }
      this.logger.info(`BLE 数字通道扫描中（${advNameFor(normalizeAdminId(this.config?.deviceId), this.config?.ble?.name || ADV_PREFIX)}）`);
      this.noble.startScanning([], true);
      this.scanning = true;
      this.scanStartedAt = Date.now();
      return true;
    } catch (error) {
      this.logger.warn(`BLE 降级启动失败：${error.message}`);
      this.#scheduleRetry();
      return false;
    } finally {
      this.starting = false;
    }
  }

  #onDiscover(peripheral) {
    if (this.probeOnly) return;   // probeOnce() 自己收集广播，不自动连
    if (this.connected || this.connecting || this.stopping) return;
    const advertisement = peripheral?.advertisement ?? {};
    const localName = advertisement.localName ?? peripheral?.advertisement?.serviceLocalName ?? '';
    const address = peripheral?.id || peripheral?.address || '';
    const prefix = this.config?.ble?.name || ADV_PREFIX;
    const deviceId = normalizeAdminId(this.config?.deviceId);
    const addressHit = Boolean(this.config?.ble?.address && String(this.config.ble.address).toLowerCase() === String(address).toLowerCase());
    if (!addressHit && !matchesAdvName(localName, { prefix, deviceId })) return;
    /* 广播名后缀就是设备身份（research/73 §1）：交给 PanelService 做换表保护。 */
    if (localName) {
      this.foundName = localName;
      const admin = parseAdvName(localName);
      const bound = normalizeAdminId(this.config?.deviceId);
      if (admin && bound && admin !== bound) {
        this.logger.warn(`BLE 扫到的名字是「${localName}」，绑定的是 ${bound}：先继续，请检查绑定`);
      }
      this.lastAddress = address || null;
      this.onDevice?.({ name: localName, admin, address });
    }
    void this.#connect(peripheral);
  }

  async #connect(peripheral) {
    if (this.connecting || this.connected || this.stopping) return;
    this.connecting = true;
    this.scanning = false;
    try { this.noble.stopScanning(); } catch { /* ignore */ }
    const address = peripheral?.id || peripheral?.address || '';
    this.logger.info(`BLE 连接 ${this.foundName ?? address} …`);
    try {
      await promiseWithTimeout(
        () => (typeof peripheral.connectAsync === 'function'
          ? peripheral.connectAsync()
          : new Promise((resolve, reject) => peripheral.connect((error) => (error ? reject(error) : resolve())))),
        12000,
        'BLE connect',
      );
      this.peripheral = peripheral;
      const { cmd, evt } = await this.#discoverChars(peripheral);
      this.cmdChar = cmd;
      this.evtChar = evt;
      this.evtChar.on('data', (data) => this.#onNotify(data));
      await promiseWithTimeout(
        () => (typeof this.evtChar.subscribeAsync === 'function'
          ? this.evtChar.subscribeAsync()
          : new Promise((resolve, reject) => this.evtChar.subscribe((error) => (error ? reject(error) : resolve())))),
        8000,
        'BLE 订阅 notify',
      );
      peripheral.once?.('disconnect', () => this.#onDisconnected('disconnect'));
      this.connected = true;
      this.logger.info(`BLE 数字通道已连接（${address || '?'}）`);
      this.onState?.(true);
      /* 2026-11-14：不再在连接后主动发 0x51 PC_MODE=1。
       * 手表现在只在用户滑到 -1 屏时才广播，连上时它本来就已在电脑模式；
       * 而 USB 拔出瞬间存在竞态：手表刚被主机消失看门狗拉回表盘，插件这边
       * 才连上 BLE，若还发 0x51 就会把表"强行拉回"电脑模式。
       * 固件侧也已在手表模式忽略远端进入请求，这里再省掉一次多余命令。 */
      this.#startPush();
    } catch (error) {
      this.logger.warn(`BLE 连接失败：${error.message}`);
      await this.#disconnectPeripheral().catch(() => {});
      this.#scheduleRetry();
    } finally {
      this.connecting = false;
    }
  }

  /**
   * 发现私有 GATT 特征值（0x...0002 cmd / 0x...0003 evt）。
   *
   * 优先 scoped 发现（只查 SVC_UUID）：比全量查 6 个 service 快（约 0.2–2.6 s），
   * 也避免 WinRT 并发查多个 service。仍保留最多 3 次重试作为兜底 ——
   * 2026-11-09 实测 WinRT 第一次全量发现偶发只回 7/10 个特征值（缺 CTS 与私有），
   * 重试即恢复。注意：当天真正的连接失败根因是上面三个 UUID 常量写错（34 hex），
   * 不是发现不完整；`test/ble-bridge.test.mjs` 已加寄存器级别的格式断言防回归。
   */
  async #discoverChars(peripheral) {
    const attempts = 3;
    let lastCount = 0;
    const pick = (list) => ({
      cmd: list.find((char) => normUuid(char.uuid) === CHR_CMD_UUID) ?? null,
      evt: list.find((char) => normUuid(char.uuid) === CHR_EVT_UUID) ?? null,
    });
    for (let i = 0; i < attempts; i += 1) {
      if (i > 0) await new Promise((resolve) => setTimeout(resolve, 250 * i));
      let list = [];
      /* ① scoped：只查私有 service 的特征值 */
      if (typeof peripheral.discoverSomeServicesAndCharacteristicsAsync === 'function') {
        try {
          const result = await promiseWithTimeout(
            () => peripheral.discoverSomeServicesAndCharacteristicsAsync([SVC_UUID], []),
            15000,
            'BLE 发现服务（私有）',
          );
          list = Array.isArray(result?.characteristics) ? result.characteristics : [];
        } catch (error) {
          if (this.config?.debug) this.logger.debug?.(`BLE 私有服务发现失败（第 ${i + 1}/${attempts} 次）：${error?.message ?? JSON.stringify(error)}`);
        }
      }
      /* ② 全量兜底：只在上面没拿到东西时试一次（老绑定/测试替身） */
      if (!list.length && typeof peripheral.discoverAllServicesAndCharacteristics === 'function') {
        try {
          const result = await promiseWithTimeout(
            () => new Promise((resolve, reject) => peripheral.discoverAllServicesAndCharacteristics((error, services, chars) => (error ? reject(error) : resolve({ services, characteristics: chars })))),
            15000,
            'BLE 发现服务（全量）',
          );
          list = Array.isArray(result?.characteristics) ? result.characteristics : [];
        } catch (error) {
          if (this.config?.debug) this.logger.debug?.(`BLE 全量发现失败（第 ${i + 1}/${attempts} 次）：${error.message}`);
        }
      }
      lastCount = Math.max(lastCount, list.length);
      const { cmd, evt } = pick(list);
      if (cmd && evt) return { cmd, evt };
      if (this.config?.debug && i < attempts - 1) {
        const uuids = list.map((char) => char?.uuid).join(',') || '空';
        this.logger.debug?.(`BLE 特征值不完整（第 ${i + 1}/${attempts} 次只拿到 ${list.length} 个：${uuids}），重试…`);
      }
    }
    throw new Error(`没找到 PROTORIG GATT 特征值（重试 ${attempts} 次，最多发现 ${lastCount} 个；固件 BLE 服务没开？）`);
  }

  #onNotify(data) {
    const event = parseBleFrame(Buffer.from(data ?? []));
    if (!event) {
      this.badFrames += 1;
      if (this.config?.debug) this.logger.debug?.(`BLE 帧解析失败（第 ${this.badFrames} 帧）`);
      return;
    }
    if (event.type === 'touch') this.lastTouchAt = Date.now();
    if (event.type === 'zones') this.zones = event;
    this.onEvent?.(event);
  }

  #writeFrame(bytes) {
    if (!this.connected || !this.cmdChar) return false;
    this.writeChain = this.writeChain
      .then(async () => {
        if (!this.connected || !this.cmdChar) return;
        await promiseWithTimeout(
          () => (typeof this.cmdChar.writeAsync === 'function'
            ? this.cmdChar.writeAsync(bytes, true)
            : new Promise((resolve, reject) => this.cmdChar.write(bytes, true, (error) => (error ? reject(error) : resolve())))),
          5000,
          'BLE 写入',
        );
      })
      .catch((error) => {
        if (this.config?.debug) this.logger.debug?.(`BLE 写入失败：${error.message}`);
      });
    return true;
  }

  async #disconnectPeripheral() {
    const peripheral = this.peripheral;
    this.peripheral = null;
    this.cmdChar = null;
    this.evtChar = null;
    this.scanning = false;
    if (!peripheral) return;
    try {
      if (typeof peripheral.disconnectAsync === 'function') await peripheral.disconnectAsync();
      else await new Promise((resolve) => peripheral.disconnect(() => resolve()));
    } catch {
      /* 已经断开 */
    }
  }

  #onDisconnected(reason) {
    if (!this.connected) return;
    const wasConnected = this.connected;
    this.connected = false;
    this.zones = null;
    this.#stopPush();
    if (wasConnected) this.logger.info(`BLE 数字通道已断开（${reason}）`);
    this.onState?.(false);
    void this.#disconnectPeripheral().catch(() => {});
    if (!this.disabled && !this.stopping) this.#scheduleRetry(600);
  }

  #scheduleRetry(delay = this.retryMs) {
    if (this.timer || this.disabled || this.stopping) return;
    this.timer = setTimeout(() => {
      this.timer = null;
      if (!this.stopping) {
        if (this.noble?.state === 'poweredOn' && !this.connected) {
          try {
            this.noble.startScanning([], true);
            this.scanning = true;
            this.scanStartedAt = Date.now();
          } catch {
            this.scanning = false;
            this.#scheduleRetry();
          }
        } else {
          this.#scheduleRetry();
        }
      }
    }, Math.max(200, Number(delay) || this.retryMs));
    this.timer.unref?.();
  }

  #startPush() {
    this.#stopPush();
    const hz = Math.max(1, Math.min(10, this.config?.ble?.metricsHz ?? 5));
    const period = Math.max(100, Math.round(1000 / hz));
    let last = null;
    let lastAt = 0;
    const push = () => {
      if (!this.connected || !this.metrics) return;
      const next = this.metrics;
      const now = Date.now();
      // 数值没变就不发（省链路），但每 2 s 至少补一帧：固件"链路活着"的租约是 3 s。
      if (last && JSON.stringify(next) === JSON.stringify(last) && now - lastAt < 2000) return;
      last = next;
      lastAt = now;
      this.#writeFrame(bleFrame(CMD_METRICS, metricsPayloadForBle(next)));
    };
    this.pushTimer = setInterval(push, period);
    this.pushTimer.unref?.();
    push();
  }

  #stopPush() {
    if (this.pushTimer) {
      clearInterval(this.pushTimer);
      this.pushTimer = null;
    }
  }

  /** 服务每秒采到的 metrics（MetricsCollector.sample() 的原始对象）。 */
  updateMetrics(metrics) {
    this.metrics = metrics ? metricsForBle(metrics) : null;
  }

  /** 发送一帧 PC op（0x50 通道）：payload[0] = op。 */
  sendPcFrame(payload) {
    if (!this.connected) return false;
    const bytes = Buffer.from(payload ?? []);
    if (!bytes.length || bytes.length > 240) return false;
    return this.#writeFrame(bleFrame(CMD_PC_FRAME, bytes));
  }

  /* 注意：亮度和震动必须包在 0x50（PC_FRAME）里。
   * 固件 ble_write_cmd() 只把 **cmd==0x50** 的帧交给 svc_pc_stage()；裸 0x02/0x03
   * 会被当成旧 BLE 命令（GET_INFO / SET_TIME）静默丢掉 —— 2026-11-09 实机发现：
   * BLE 下提醒的震动/亮度其实一直没生效（USB 路径正常，因为 USB 没有外层 cmd）。 */
  vibrate(ms) {
    if (!this.connected) return;
    const value = Buffer.alloc(2);
    value.writeUInt16LE(Math.max(0, Math.min(2000, Math.round(ms))));
    return this.sendPcFrame(Buffer.concat([Buffer.from([CMD_VIBRATE]), value]));
  }

  brightness(level) {
    if (!this.connected) return;
    return this.sendPcFrame(Buffer.from([CMD_BRIGHT, Math.max(32, Math.min(255, Math.round(level)))]));
  }

  /** PC 帧 0x15：0 = PC 模式也会息屏，1 = 常亮。 */
  alwaysOn(on) {
    if (!this.connected) return false;
    return this.sendPcFrame(Buffer.from([0x15, on ? 1 : 0]));
  }

  /** PC 帧 0x16：电脑模式专用息屏秒数（u16 小端；0 = 固件默认）。 */
  screenOff(sec) {
    if (!this.connected) return false;
    const value = Math.max(0, Math.min(65535, Math.round(Number(sec) || 0)));
    const payload = Buffer.alloc(3);
    payload[0] = 0x16;
    payload.writeUInt16LE(value, 1);
    return this.sendPcFrame(payload);
  }

  /**
   * 只扫描 + 收集广播名（「蓝牙探测注册」按钮）：不连接、不改当前连接。
   * @returns {Promise<Array<{id:string,name:string,address:string}>>}
   */
  async probeOnce(ms = 5000, onSeen = null) {
    if (this.disabled || this.connected || this.connecting || this.probeOnly) return [];
    if (!(await this.checkRuntime())) return [];
    if (this.noble?.state !== 'poweredOn') return [];
    const seen = new Map();
    const handler = (peripheral) => {
      const advertisement = peripheral?.advertisement ?? {};
      const name = advertisement.localName ?? advertisement.serviceLocalName ?? '';
      const admin = parseAdvName(name);
      if (!admin) return;
      const address = peripheral?.id || peripheral?.address || '';
      seen.set(admin, { id: admin, name, address });
      onSeen?.({ admin, name, address });
    };
    this.probeOnly = true;
    this.noble.on('discover', handler);
    try {
      this.noble.startScanning([], true);
    } catch { /* ignore */ }
    await new Promise((resolve) => { const timer = setTimeout(resolve, ms); timer.unref?.(); });
    try { this.noble.stopScanning(); } catch { /* ignore */ }
    this.noble.removeListener?.('discover', handler);
    this.probeOnly = false;
    return [...seen.values()];
  }

  /** 停止数字通道：停止扫描、断开 GATT。可重复调用。 */
  stop() {
    if (this.stopPromise) return this.stopPromise;
    this.stopping = true;
    this.#stopPush();
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    this.connected = false;
    this.scanning = false;
    this.stopPromise = (async () => {
      try {
        this.noble?.stopScanning?.();
      } catch {
        /* ignore */
      }
      await this.#disconnectPeripheral().catch(() => {});
      this.onState?.(false);
    })().finally(() => {
      this.stopPromise = null;
    });
    return this.stopPromise;
  }
}
