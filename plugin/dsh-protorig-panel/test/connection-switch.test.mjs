/*
 * connection-switch.test.mjs —— 「USB/蓝牙连接」开关 与「断开/重连」的语义
 *
 * 2026-11-22 修的真问题：开关关掉以前只是"下一次 tick 不再 start()"，
 * 正在跑的 BLE 扫描不会停；残留扫描扫到表还会自己连上（用户看着开关是关的，
 * 射频却一直在扫）。这里把三条语义锁住：
 *   1. 关开关 = 立刻 stop()，含正在跑的扫描；
 *   2. 「断开」= 暂停全部，也要把扫描停掉；
 *   3. 重新打开任一通道 = 用户想连接，清掉「断开」暂停。
 */
import './_env.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { PanelService } from '../src/panel-service.mjs';
import { BleBridge, CHR_CMD_UUID, CHR_EVT_UUID } from '../src/ble-bridge.mjs';
import { normalizeConfig } from '../src/config.mjs';

const silent = { info() {}, warn() {}, error() {}, debug() {} };
const settle = (ms = 60) => new Promise((resolve) => setTimeout(resolve, ms));

function fakeBle(overrides = {}) {
  return {
    connected: false, child: null, starting: false, scanning: false,
    startCalls: 0, stopCalls: 0, config: null, metrics: null, lastAddress: null,
    start() { this.startCalls += 1; this.scanning = true; return Promise.resolve(true); },
    stop() { this.stopCalls += 1; this.scanning = false; return Promise.resolve(); },
    updateMetrics() {}, alwaysOn() {}, publishCard() { return false; },
    sendPcFrame() {}, sendPcFrames() {}, setPcMode() {}, blit() {},
    ...overrides,
  };
}

function makePanel(config, ble) {
  const service = new PanelService({ config, logger: silent, platform: 'darwin' });
  service.ble = ble;
  service.startedAt = Date.now() - 5000; // 跳过启动后 1.5 s 的静默期
  service.discover = async () => [];
  service.probePort = async () => '';
  return service;
}

test('关掉蓝牙开关：正在跑的扫描要立刻停', async () => {
  const ble = fakeBle({ scanning: true });
  const panel = makePanel({ usb: { enabled: false }, ble: { enabled: false } }, ble);
  await panel.tickForTest();
  assert.equal(ble.stopCalls, 1, '关开关必须 stop()，不能只靠下一次不再 start');
  assert.equal(ble.scanning, false);
});

test('「断开」暂停态：扫描也要停', async () => {
  const ble = fakeBle({ scanning: true });
  const panel = makePanel({ usb: { enabled: false }, ble: { enabled: true } }, ble);
  panel.manualPaused = true;
  await panel.tickForTest();
  assert.equal(ble.stopCalls, 1, '暂停不该留下仍在收广播的扫描');
});

test('已连接时关开关：要断开', async () => {
  const ble = fakeBle({ connected: true });
  const panel = makePanel({ usb: { enabled: false }, ble: { enabled: false } }, ble);
  await panel.tickForTest();
  assert.equal(ble.stopCalls, 1);
});

test('重新打开任一通道：清掉「断开」暂停', async () => {
  const ble = fakeBle();
  const panel = makePanel({ usb: { enabled: false }, ble: { enabled: true } }, ble);
  panel.manualPaused = true;
  panel.appliedUsbEnabled = false;
  panel.rawConfig = { usb: { enabled: true }, ble: { enabled: true } };
  await panel.tickForTest();
  assert.equal(panel.manualPaused, false, '打开通道 = 想连接，清掉暂停');
});

function makeBridgeEnv() {
  const noble = new EventEmitter();
  noble.state = 'poweredOn';
  noble.scanning = false;
  noble.startScanning = () => { noble.scanning = true; };
  noble.stopScanning = () => { noble.scanning = false; };

  const cmdChar = {
    uuid: CHR_CMD_UUID,
    write(_buffer, _withoutResponse, cb) { cb?.(null); },
    writeAsync: async () => {},
  };
  const evtChar = new EventEmitter();
  evtChar.uuid = CHR_EVT_UUID;
  evtChar.subscribe = (cb) => cb?.(null);
  evtChar.subscribeAsync = async () => {};

  const peripheral = new EventEmitter();
  peripheral.id = 'fake-peripheral';
  peripheral.address = 'fake-peripheral';
  peripheral.advertisement = { localName: 'PROTORIG ES 2BEA4E' };
  peripheral.connect = (cb) => cb?.(null);
  peripheral.connectAsync = async () => {};
  peripheral.discoverAllServicesAndCharacteristics = (cb) => cb?.(null, [], [cmdChar, evtChar]);
  peripheral.discoverAllServicesAndCharacteristicsAsync = async () => ({
    services: [], characteristics: [cmdChar, evtChar],
  });
  peripheral.disconnect = (cb) => cb?.(null);
  peripheral.disconnectAsync = async () => {};
  return { noble, peripheral };
}

test('蓝牙开关关掉：不起扫描，残留广播也不连', async () => {
  const env = makeBridgeEnv();
  const bridge = new BleBridge({
    config: normalizeConfig({ deviceId: '2BEA4E', ble: { enabled: false } }),
    logger: silent,
    nobleFactory: () => env.noble,
  });
  assert.equal(await bridge.start(), false, '开关关着时 start() 必须拒绝');
  assert.equal(env.noble.scanning, false);

  /* 关开关之前起的那轮扫描如果还残留：命中广播也不许连。 */
  bridge.scanning = true;
  env.noble.emit('discover', env.peripheral);
  await settle();
  assert.equal(bridge.connected, false, '开关关掉后扫到表也不能自动连');
  bridge.stop();
});
