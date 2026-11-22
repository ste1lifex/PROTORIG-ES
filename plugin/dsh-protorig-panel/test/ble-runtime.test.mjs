/*
 * ble-runtime.test.mjs —— BLE 运行时探测的「永久失败 vs 暂时失败」
 *
 * 2026-11-22 真机事故：checkRuntime() 把 #waitPoweredOn 的 8 s 超时也当成永久失败，
 * 置 disabled=true 后没有任何代码清掉；之后 start()/重试/探测全在门口 return，
 * 手表永远连不上，只能重载插件。这里锁住新的语义：
 *   1. 超时（蓝牙还没就绪）= 暂时失败：不缓存、不 disabled，下次还能再试；
 *   2. unsupported / unauthorized / 加载失败 = 永久失败：落 disabled，但 resetRuntime() 可清；
 *   3. 适配器 stateChange → poweredOn 自动清掉禁用；
 *   4. probeOnce 失败要返回原因，不再静默空数组。
 */
import './_env.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { BleBridge } from '../src/ble-bridge.mjs';
import { PanelService } from '../src/panel-service.mjs';
import { normalizeConfig } from '../src/config.mjs';

const silent = { info() {}, warn() {}, error() {}, debug() {} };

function makeNoble(state = 'poweredOn') {
  const noble = new EventEmitter();
  noble.state = state;
  noble.scanning = false;
  noble.startScanning = () => { noble.scanning = true; };
  noble.stopScanning = () => { noble.scanning = false; };
  return noble;
}

function makeBridge(noble, extra = {}) {
  const bridge = new BleBridge({
    config: normalizeConfig({ deviceId: '2BEA4E', ...extra }),
    logger: silent,
    nobleFactory: () => noble,
  });
  bridge.poweredOnTimeoutMs = 40; // 别真等 8 s
  return bridge;
}

test('蓝牙还没就绪（超时）不算永久失败，之后还能起来', async () => {
  const noble = makeNoble('poweredOff');
  const bridge = makeBridge(noble);
  assert.equal(await bridge.start(), false, '蓝牙没开时当次起不来');
  assert.equal(bridge.disabled, false, '暂时失败不能永久禁用通道');
  assert.equal(bridge.runtimeOk, null, '暂时失败不缓存运行时结论');

  noble.state = 'poweredOn';
  assert.equal(await bridge.start(), true, '蓝牙可用后应能重新起来');
  assert.equal(noble.scanning, true);
  bridge.stop();
});

test('unsupported 是永久失败，但 resetRuntime 后能重试', async () => {
  const noble = makeNoble('unsupported');
  const bridge = makeBridge(noble);
  assert.equal(await bridge.start(), false);
  assert.equal(bridge.disabled, true);
  assert.equal(bridge.runtimeReason, 'unsupported');

  const probe = await bridge.probeOnce(20);
  assert.deepEqual(probe.found, []);
  assert.equal(probe.reason, 'unsupported', '探测要给出原因而不是空数组');

  noble.state = 'poweredOn';
  bridge.resetRuntime();
  assert.equal(await bridge.start(), true, '显式重置后应能重新探测');
  bridge.stop();
});

test('适配器恢复 poweredOn 时自动清掉禁用状态', async () => {
  const noble = makeNoble('unsupported');
  const bridge = makeBridge(noble);
  assert.equal(await bridge.start(), false);
  assert.equal(bridge.disabled, true);

  noble.state = 'poweredOn';
  noble.emit('stateChange', 'poweredOn');
  assert.equal(bridge.disabled, false, 'stateChange→poweredOn 应清账');

  assert.equal(await bridge.start(), true);
  bridge.stop();
});

test('探测能扫到广播时 found 有值、reason 为空', async () => {
  const noble = makeNoble('poweredOn');
  const bridge = makeBridge(noble);
  const pending = bridge.probeOnce(60);
  /* probeOnce 里先 await checkRuntime() 再挂 discover 监听，广播要等监听挂上后再发。 */
  await new Promise((resolve) => setTimeout(resolve, 10));
  noble.emit('discover', {
    id: 'fake', address: 'fake', advertisement: { localName: 'PROTORIG ES 2BEA4E' },
  });
  const probe = await pending;
  assert.equal(probe.reason, '');
  assert.deepEqual(probe.found.map((item) => item.id), ['2BEA4E']);
  assert.equal(noble.scanning, false, '探测结束要停扫描');
});

test('蓝牙探测路由失败时返回 ok:false 和原因（不再静默）', async () => {
  const service = new PanelService({ config: {}, logger: silent });
  let resets = 0;
  service.ble = {
    resetRuntime() { resets += 1; },
    probeOnce: async () => ({ found: [], reason: '蓝牙尚未就绪（state=poweredOff）' }),
  };
  const result = await service.probeBleDevices(10);
  assert.equal(resets, 1, '显式探测先清掉永久禁用缓存');
  assert.equal(result.ok, false);
  assert.match(result.error, /蓝牙不可用/);
  assert.match(result.error, /poweredOff/);
});

test('重连时 USB 没口也要把 BLE 拉起来并回报通道可用', async () => {
  const service = new PanelService({
    config: { usb: { enabled: true }, ble: { enabled: true } }, logger: silent, platform: 'darwin',
  });
  let startCalls = 0;
  let resets = 0;
  service.ble = {
    connected: false, starting: false, scanning: false, runtimeReason: '', config: null,
    resetRuntime() { resets += 1; },
    start() { startCalls += 1; this.starting = true; return Promise.resolve(true); },
    stop() { this.starting = false; return Promise.resolve(); },
  };
  service.discover = async () => [];       // 模拟没插线
  service.probePort = async () => '';

  const result = await service.reconnectNow();
  assert.equal(result.opened, false, '没有 USB 口');
  assert.equal(startCalls, 1, 'USB 不可用时应主动拉起 BLE');
  assert.equal(resets, 1, '重连也要清永久禁用缓存');
  assert.equal(result.ble, true, 'BLE 正在启动 = 通道可用');
});

test('被停用时状态接口与 protorig_state 都要带原因（可见性）', async () => {
  const service = new PanelService({ config: {}, logger: silent });
  service.ble = {
    connected: false, starting: false, scanning: false, timer: null,
    disabled: true, runtimeReason: 'unsupported',
  };
  const state = service.stateForApi();
  assert.equal(state.bleDisabled, true);
  assert.equal(state.bleReason, 'unsupported');

  const { stateTool } = await import('../src/tools.mjs');
  const tool = stateTool({ panel: service });
  const out = await tool.execute({});
  assert.equal(out.bleDisabled, true);
  assert.equal(out.bleReason, 'unsupported');
  const rendered = tool.output.render({}, out).map((block) => block.text).join('');
  assert.match(rendered, /BLE已停用\(unsupported\)/);
});
