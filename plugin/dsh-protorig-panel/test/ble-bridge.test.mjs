/*
 * ble-bridge.test.mjs —— 纯 npm（@stoprocent/noble）BLE 桥的单元测试
 *
 * 不碰真实蓝牙：注入一个假 noble 实例，验证扫描→连接→GATT→帧收发的状态机。
 * 协议（A5 5A cmd len payload）必须与固件 ble_svc 一致。
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import {
  BleBridge,
  SVC_UUID,
  CHR_CMD_UUID,
  CHR_EVT_UUID,
  CMD_METRICS,
  CMD_PC_FRAME,
  CMD_PC_MODE,
  bleFrame,
  matchesAdvName,
  metricsPayloadForBle,
} from '../src/ble-bridge.mjs';
import { normalizeConfig } from '../src/config.mjs';

const silent = { info() {}, warn() {}, error() {}, debug() {} };
const settle = (ms = 25) => new Promise((resolve) => setTimeout(resolve, ms));
const waitFor = async (fn, timeoutMs = 1500) => {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (fn()) return true;
    await settle(10);
  }
  return false;
};

test('GATT UUID 常量：32 hex，与固件 ble_svc.c 一致（2026-11-09 防"多打两个 0"回归）', () => {
  const all = { SVC_UUID, CHR_CMD_UUID, CHR_EVT_UUID };
  for (const [name, uuid] of Object.entries(all)) {
    assert.match(uuid, /^[0-9a-f]{32}$/, `${name} 必须是 32 位小写 hex（实际 ${uuid.length} 位：${uuid}）`);
  }
  /* 固件：BT_UUID_128_ENCODE(0x50524F54, 0x4F52, 0x4947, 0x4553, 0x00000000000N) */
  assert.equal(SVC_UUID, '50524f544f5249474553000000000001');
  assert.equal(CHR_CMD_UUID, '50524f544f5249474553000000000002');
  assert.equal(CHR_EVT_UUID, '50524f544f5249474553000000000003');
});

test('帧构造：A5 5A + cmd + len + payload', () => {
  assert.deepEqual([...bleFrame(0x51, Buffer.from([1]))], [0xa5, 0x5a, 0x51, 1, 1]);
  assert.deepEqual([...bleFrame(0x50, Buffer.alloc(3))], [0xa5, 0x5a, 0x50, 3, 0, 0, 0]);
  // 长度按 u8 截断（固件单帧上限 255）
  assert.equal(bleFrame(0x52, Buffer.alloc(300))[3], 44);
});

test('0x52 metrics payload：15 B 小端、vram 未知=0xFFFF、brand 0..3', () => {
  const payload = metricsPayloadForBle({ cpu: 123, gpu: 456, ram: 789, vram: -1, brand: 9, rx: 1000, tx: 2000 });
  assert.equal(payload.length, 15);
  assert.equal(payload.readUInt16LE(0), 123);
  assert.equal(payload.readUInt16LE(2), 456);
  assert.equal(payload.readUInt16LE(4), 789);
  assert.equal(payload.readUInt16LE(6), 0xffff, 'vram<0 用 0xFFFF');
  assert.equal(payload.readUInt8(8), 3, 'brand clamp 0..3');
  assert.equal(payload.readUInt32LE(9), 1000);
  assert.equal(payload.readUInt16LE(13), 2000, 'tx 是 u16（固件只读 2 B）');
  // clamp 到 0..1000
  const clamped = metricsPayloadForBle({ cpu: -5, gpu: 5000, ram: 500, vram: 528, brand: 1, rx: -1, tx: 999999 });
  assert.equal(clamped.readUInt16LE(0), 0);
  assert.equal(clamped.readUInt16LE(2), 1000);
  assert.equal(clamped.readUInt16LE(6), 528);
  assert.equal(clamped.readUInt16LE(13), 0xffff, 'tx clamp u16');
});

test('广播名匹配：前缀 / 绑定 id', () => {
  assert.equal(matchesAdvName('PROTORIG ES 2BEA4E', {}), true);
  assert.equal(matchesAdvName('PROTORIG ES', {}), true);
  assert.equal(matchesAdvName('PROTORIG ES 657837', { deviceId: '2BEA4E' }), false);
  assert.equal(matchesAdvName('PROTORIG ES 2BEA4E', { deviceId: '2bea4e' }), true);
  assert.equal(matchesAdvName('Other Device', {}), false);
  assert.equal(matchesAdvName('', {}), false);
});

function makeFakeEnvironment() {
  const noble = new EventEmitter();
  noble.state = 'poweredOn';
  noble.scanning = false;
  noble.startScanning = () => { noble.scanning = true; };
  noble.stopScanning = () => { noble.scanning = false; };

  const writes = [];
  const cmdChar = {
    uuid: CHR_CMD_UUID,
    write(buffer, _withoutResponse, cb) { writes.push(Buffer.from(buffer)); if (cb) cb(null); },
    writeAsync: async (buffer) => writes.push(Buffer.from(buffer)),
  };
  const evtChar = new EventEmitter();
  evtChar.uuid = CHR_EVT_UUID;
  evtChar.subscribe = (cb) => cb?.(null);
  evtChar.subscribeAsync = async () => {};
  evtChar.on('data', () => {});

  const peripheral = new EventEmitter();
  peripheral.id = 'fake-peripheral';
  peripheral.address = 'fake-peripheral';
  peripheral.advertisement = {};
  peripheral.connect = (cb) => cb?.(null);
  peripheral.connectAsync = async () => {};
  peripheral.discoverAllServicesAndCharacteristics = (cb) => cb?.(null, [], [cmdChar, evtChar]);
  peripheral.discoverAllServicesAndCharacteristicsAsync = async () => ({ services: [], characteristics: [cmdChar, evtChar] });
  peripheral.disconnect = (cb) => cb?.(null);
  peripheral.disconnectAsync = async () => {};

  return { noble, peripheral, cmdChar, evtChar, writes };
}

test('状态机：扫描 → 连接 → 发 PC_MODE/metrics/PC 帧 → notify → 断开重扫', async () => {
  const env = makeFakeEnvironment();
  const states = [];
  const events = [];
  const bridge = new BleBridge({
    config: normalizeConfig({ deviceId: '2BEA4E', ble: { metricsHz: 10 } }),
    logger: silent,
    onState: (value) => states.push(value),
    onEvent: (event) => events.push(event),
    nobleFactory: () => env.noble,
  });
  try {
    assert.equal(await bridge.start(), true, 'poweredOn 时应开始扫描');
    assert.equal(env.noble.scanning, true);

    // 广播名命中 → 连接
    env.peripheral.advertisement.localName = 'PROTORIG ES 2BEA4E';
    env.noble.emit('discover', env.peripheral);
    assert.ok(await waitFor(() => bridge.connected), '应连上 GATT');
    assert.deepEqual(states, [true]);
    assert.equal(env.noble.scanning, false, '连接时停扫描');

    // 0.4.2 起连接后不再主动发 0x51（USB 拔出竞态时会把表从表盘拉回电脑模式）
    await settle(60);
    assert.ok(!env.writes.some((buf) => buf[2] === CMD_PC_MODE), '不应主动发 PC_MODE=1');

    // metrics 推送（200 ms 周期，改数值触发一帧）
    bridge.updateMetrics({ cpuPercent: 12.3, gpuPercent: 4.5, memoryPercent: 67.8, gpuMemoryPercent: 9.9, gpuVendor: 'intel', networkRxBps: 100, networkTxBps: 50 });
    assert.ok(await waitFor(() => env.writes.some((buf) => buf[2] === CMD_METRICS), 1200), '应推送 0x52');
    const metrics = env.writes.find((buf) => buf[2] === CMD_METRICS);
    assert.equal(metrics[3], 15, 'metrics payload 15 B');

    // 任意 PC 帧（卡片层 0x53）
    assert.equal(bridge.sendPcFrame([0x53, 1, 2, 3]), true);
    assert.ok(await waitFor(() => env.writes.some((buf) => buf[2] === CMD_PC_FRAME && buf[4] === 0x53)), '应写 0x50 通道');
    assert.equal(bridge.sendPcFrame([]), false, '空帧拒绝');
    assert.equal(bridge.sendPcFrame(Buffer.alloc(241)), false, '>240 B 拒绝');

    // 手表 notify：A5 5A 50 <len> + PC 帧；op=0x04 触摸（pressed,x u16,y u16）
    const touchFrame = [0xa5, 0x5a, 0x50, 6, 0x04, 1, 0x10, 0x01, 0xe0, 0x0f];
    env.evtChar.emit('data', Buffer.from(touchFrame));
    await settle(20);
    assert.ok(events.length >= 1, 'notify 应转成事件');
    assert.equal(events[0].type, 'touch');

    // 断开 → onState(false) + 退避重扫
    env.peripheral.emit('disconnect');
    assert.equal(bridge.connected, false);
    assert.deepEqual(states, [true, false]);
  } finally {
    await bridge.stop();
  }
});

test('WinRT 式发现不完整：scoped 第一次缺 EVT，重试后应连上', async () => {
  const env = makeFakeEnvironment();
  let calls = 0;
  env.peripheral.discoverSomeServicesAndCharacteristicsAsync = async () => {
    calls += 1;
    return { services: [], characteristics: calls === 1 ? [env.cmdChar] : [env.cmdChar, env.evtChar] };
  };
  const bridge = new BleBridge({
    config: normalizeConfig({ deviceId: '2BEA4E' }),
    logger: silent,
    nobleFactory: () => env.noble,
  });
  try {
    assert.equal(await bridge.start(), true);
    env.peripheral.advertisement.localName = 'PROTORIG ES 2BEA4E';
    env.noble.emit('discover', env.peripheral);
    assert.ok(await waitFor(() => bridge.connected, 2000), '第二次发现后应连上');
    assert.equal(calls, 2, '第一次不完整 → 重试一次');
  } finally {
    await bridge.stop();
  }
});

test('发现持续不完整：3 次后放弃，错误信息可读', async () => {
  const env = makeFakeEnvironment();
  const warnings = [];
  const logger = { ...silent, warn: (msg) => warnings.push(String(msg)) };
  let calls = 0;
  env.peripheral.discoverSomeServicesAndCharacteristicsAsync = async () => {
    calls += 1;
    return { services: [], characteristics: [env.evtChar] };
  };
  const bridge = new BleBridge({
    config: normalizeConfig({ deviceId: '2BEA4E' }),
    logger,
    nobleFactory: () => env.noble,
  });
  try {
    await bridge.start();
    env.peripheral.advertisement.localName = 'PROTORIG ES 2BEA4E';
    env.noble.emit('discover', env.peripheral);
    assert.ok(
      await waitFor(() => warnings.some((w) => w.includes('没找到 PROTORIG GATT 特征值')), 3000),
      '应报可读错误',
    );
    assert.equal(bridge.connected, false);
    assert.equal(calls, 3, '重试 3 次');
  } finally {
    await bridge.stop();
  }
});

test('checkRuntime：蓝牙被拒（unauthorized）→ 禁用且只警告一次', async () => {
  const noble = new EventEmitter();
  noble.state = 'unauthorized';
  noble.startScanning = () => {};
  noble.stopScanning = () => {};
  const warnings = [];
  const logger = { ...silent, warn: (msg) => warnings.push(String(msg)) };
  const bridge = new BleBridge({
    config: normalizeConfig({ ble: { enabled: true } }),
    logger,
    nobleFactory: () => noble,
  });
  assert.equal(await bridge.checkRuntime(), false);
  assert.equal(await bridge.checkRuntime(), false, '缓存结果');
  assert.equal(warnings.length, 1, '只警告一次');
  assert.match(warnings[0], /蓝牙不可用|unauthorized|授权/);
  assert.equal(await bridge.start(), false, '禁用后 start 直接 false');
});

test('BLE 亮度/震动必须包在 0x50 里（固件只把 0x50 交给 PC 帧解析）', async () => {
  const env = makeFakeEnvironment();
  const bridge = new BleBridge({
    config: normalizeConfig({ deviceId: '2BEA4E' }),
    logger: silent,
    nobleFactory: () => env.noble,
  });
  try {
    await bridge.start();
    env.peripheral.advertisement.localName = 'PROTORIG ES 2BEA4E';
    env.noble.emit('discover', env.peripheral);
    assert.ok(await waitFor(() => bridge.connected), '应连上');
    bridge.vibrate(320);
    bridge.brightness(200);
    await settle(50);
    const frames = env.writes.map((buf) => [...buf]);
    assert.ok(
      frames.some((f) => f[0] === 0xa5 && f[1] === 0x5a && f[2] === 0x50 && f[4] === 0x03),
      '震动应为 A5 5A 50 len 03 msLe msHi',
    );
    assert.ok(
      frames.some((f) => f[0] === 0xa5 && f[1] === 0x5a && f[2] === 0x50 && f[4] === 0x02),
      '亮度应为 A5 5A 50 len 02 level',
    );
  } finally {
    await bridge.stop();
  }
});

test('BLE 只连指定编号：别的表广播不连，指定编号才连', async () => {
  const env = makeFakeEnvironment();
  const bridge = new BleBridge({
    config: normalizeConfig({ deviceId: '2BEA4E' }),
    logger: silent,
    nobleFactory: () => env.noble,
  });
  try {
    await bridge.start();
    env.peripheral.advertisement.localName = 'PROTORIG ES 657837';
    env.noble.emit('discover', env.peripheral);
    await settle(40);
    assert.equal(bridge.connected, false, '别的表不应该被连上');
    env.peripheral.advertisement.localName = 'PROTORIG ES 2BEA4E';
    env.noble.emit('discover', env.peripheral);
    assert.ok(await waitFor(() => bridge.connected), '指定编号应该连上');
  } finally {
    await bridge.stop();
  }
});

test('connecting 期间 start() 不再重启扫描（WinRT 连接中重置 watcher）', async () => {
  const env = makeFakeEnvironment();
  const bridge = new BleBridge({
    config: normalizeConfig({}),
    logger: silent,
    nobleFactory: () => env.noble,
  });
  bridge.scanning = false;
  bridge.connecting = true;
  bridge.starting = false;
  bridge.timer = null;
  bridge.disabled = false;
  const ok = await bridge.start();
  assert.equal(ok, false, '连接中不能重新 startScanning');
  assert.equal(bridge.connecting, true, '不应影响已有连接流程');
});
