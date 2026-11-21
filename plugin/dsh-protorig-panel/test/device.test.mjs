/*
 * device.test.mjs —— 跨平台设备身份（admin_id）与设置页设备逻辑的纯逻辑测试
 *
 * 覆盖（research/73 §3.2 的 P3–P7、P10/P11）：
 *   · platform.mjs：Windows MI_00 排序 / macOS cu-tty 去重与「不看 pnpId」/ 烧录后端选择
 *   · device-id.mjs：`adminid` 输出与广播名后缀解析、绑定选择规则
 *   · protocol.parseLine：`admin=...` 行进事件流
 *   · WatchLink.queryAdminId：等到/等不到的两种结果
 *   · PanelService：绑定后挑对口、换表保护清缓存、未绑定单表不退化、断开/重连、
 *     BLE 名字身份、候选扫描（探活用注入替身，绝不碰真实 COM8）
 *   · FlashService：Windows 后端在 macOS 上拒绝
 *
 * 运行：npm test（node --test）
 */
import './_env.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import {
  flashBackend,
  flashSupported,
  flashUnsupportedReason,
  isWatchPort,
  osKind,
  osLabel,
  rankPort,
  sortWatchPorts,
} from '../src/platform.mjs';
import {
  advNameFor,
  describeSelection,
  deviceLabel,
  normalizeAdminId,
  parseAdminLine,
  parseAdvName,
  selectCandidate,
} from '../src/device-id.mjs';
import { parseLine } from '../src/protocol.mjs';
import { WatchLink } from '../src/link.mjs';
import fs from 'node:fs';
import { normalizeConfig } from '../src/config.mjs';
import { PanelService, resolveDesignDir } from '../src/panel-service.mjs';
import { FlashService } from '../src/flash.mjs';

const silent = { info() {}, warn() {}, error() {}, debug() {} };
const settle = (ms = 15) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor(check, ms = 3000) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (check()) return true;
    await settle(20);
  }
  return false;
}

/* ------------------------------------------------------------------ 平台 */

test('平台：osKind/osLabel/flashSupported —— Windows/macOS 都有烧录后端', () => {
  assert.equal(osKind('win32'), 'windows');
  assert.equal(osKind('darwin'), 'macos');
  assert.equal(osKind('linux'), 'linux');
  assert.equal(osKind('freebsd'), 'other');
  assert.equal(osLabel('darwin'), 'macOS');
  assert.equal(flashSupported('win32'), true);
  assert.equal(flashSupported('darwin'), true, 'macOS 用 BLFlashCommand-macos 后端');
  assert.equal(flashSupported('linux'), false);
  assert.equal(flashBackend('win32'), 'powershell');
  assert.equal(flashBackend('darwin'), 'blflashcommand');
  assert.equal(flashBackend('linux'), null);
  assert.match(flashUnsupportedReason('linux'), /Windows \/ macOS/);
});

test('平台：Windows 仍按 VID 349B / PID 6160 过滤，MI_00 应用口排第一', () => {
  const ports = [
    { path: 'COM5', vendorId: '349B', productId: '6160', pnpId: 'USB\\VID_349B&PID_6160&MI_02\\A' },
    { path: 'COM8', vendorId: '349b', productId: '6160', pnpId: 'USB\\VID_349B&PID_6160&MI_00\\B' },
    { path: 'COM3', vendorId: '1A86', productId: '7523', pnpId: 'USB\\VID_1A86&PID_7523' },
    { path: 'COM9', vendorId: '349B', productId: '6160' },
  ];
  const sorted = sortWatchPorts(ports, 'win32');
  assert.deepEqual(sorted.map((port) => port.path), ['COM8', 'COM5', 'COM9']);
  assert.equal(isWatchPort(ports[2], 'win32'), false, 'CH340 不是手表');
  assert.equal(rankPort(ports[0], 'win32'), 5);
  assert.equal(rankPort(ports[3], 'win32'), 9);
});

test('平台：macOS 只留 cu.*（tty.* 是同一设备），不依赖 pnpId，usbmodem 兜底', () => {
  const ports = [
    { path: '/dev/tty.usbmodem14201', vendorId: '349B', productId: '6160' },
    { path: '/dev/cu.usbmodem14201', vendorId: '349B', productId: '6160' },
    // 某些驱动不给 VID/PID：macOS 上退回 usbmodem 名字兜底（weak）
    { path: '/dev/cu.usbmodem201', manufacturer: 'Bouffalo Lab' },
    // 无关设备：没有 VID/PID 也不是 usbmodem，必须排除
    { path: '/dev/cu.Bluetooth-Incoming-Port' },
    { path: '/dev/cu.usbserial-0001', vendorId: '0403', productId: '6001' },
  ];
  const sorted = sortWatchPorts(ports, 'darwin');
  assert.deepEqual(
    sorted.map((port) => port.path),
    ['/dev/cu.usbmodem14201', '/dev/cu.usbmodem201'],
    'cu 优先、tty 去重、弱候选排后面、无关设备排除',
  );
  // 没有 pnpId 也不影响排序结果（macOS 上根本不看它）
  assert.equal(rankPort({
    path: '/dev/cu.usbmodem14201', vendorId: '349B', productId: '6160',
    pnpId: 'MI_00', serialNumber: '00000001',
  }, 'darwin'), 0);
  /* ROM 下载口与应用口 VID/PID 完全一样（349b:6160），唯一区分是 iSerialNumber：
   * 应用口有、ROM 口没有。有序列号的必须排前面，否则烧录中途重启宿主会抓错口。 */
  assert.equal(rankPort({ path: '/dev/cu.usbmodem101', vendorId: '349b', productId: '6160' }, 'darwin'), 2,
    '没有序列号（典型是 ROM 下载口）要排在应用口之后');
  // Linux 不放宽：没有 VID/PID 的 usbmodem 不算手表
  assert.equal(isWatchPort({ path: '/dev/ttyACM0' }, 'linux'), false);
});

test('平台：macOS 没有 MI_00 时也能列出表（不靠排序，最终靠 adminid）', () => {
  const ports = [
    { path: '/dev/cu.usbmodem1101', vendorId: '349B', productId: '6160' },
    { path: '/dev/cu.usbmodem1103', vendorId: '349B', productId: '6160' },
  ];
  // 两块表 rank 相同 → 保持枚举顺序（顺序本身不可信，身份由握手时的 adminid 决定）
  assert.deepEqual(sortWatchPorts(ports, 'darwin').map((port) => port.path), [
    '/dev/cu.usbmodem1101',
    '/dev/cu.usbmodem1103',
  ]);
});

/* ------------------------------------------------------------- 设备身份 */

test('admin_id：6 位大写十六进制，其它一律视为未识别', () => {
  assert.equal(normalizeAdminId('2BEA4E'), '2BEA4E');
  assert.equal(normalizeAdminId(' 2bea4e '), '2BEA4E');
  assert.equal(normalizeAdminId('0x2BEA4E'), '2BEA4E');
  assert.equal(normalizeAdminId(0x2bea4e), '2BEA4E');
  assert.equal(normalizeAdminId('2BEA4'), '', '少一位');
  assert.equal(normalizeAdminId('2BEA4E1'), '', '多一位');
  assert.equal(normalizeAdminId('ZZZZZZ'), '');
  assert.equal(normalizeAdminId(''), '');
  assert.equal(normalizeAdminId(undefined), '');
  assert.equal(normalizeAdminId(null), '');
});

test('解析 adminid 输出：常规/带 shell 提示符/小写/无 chip/efuse 回落', () => {
  const plain = parseAdminLine('admin=2BEA4E chip=0123456789ABCDEF');
  assert.equal(plain.admin, '2BEA4E');
  assert.equal(plain.chip, '0123456789ABCDEF');
  assert.equal(plain.chipOk, true);

  assert.equal(parseAdminLine('bouffalolab />admin=2BEA4E chip=0123456789ABCDEF').admin, '2BEA4E');
  assert.equal(parseAdminLine('admin=2bea4e chip=0123').admin, '2BEA4E');
  assert.equal(parseAdminLine('admin=657837').chipOk, false, '没有 chip 字段');
  assert.equal(parseAdminLine('admin=7C3E82 chip=0000000000000000').chipOk, false, 'efuse 读失败全 0');
  const withFw = parseAdminLine('admin=2BEA4E chip=0123456789ABCDEF fw=1.6.0 build=Nov 14 2026');
  assert.equal(withFw.fw, '1.6.0', '固件版本从 adminid 的 fw= 解析');
  assert.equal(withFw.build, 'Nov 14 2026');
  assert.equal(parseAdminLine('admin=657837').fw, '', '旧固件没有 fw 字段');
  assert.equal(parseAdminLine('admin=2BEA4E1'), null, '多一位不算');
  assert.equal(parseAdminLine('[face] admin_id = 2BEA4E'), null, '表盘日志不是 adminid 输出');
  assert.equal(parseAdminLine('admin=ZZZZZZ'), null);
  assert.equal(parseAdminLine(''), null);
});

test('解析广播名：PROTORIG ES <ADMIN>，旧固件无后缀返回空（兼容）', () => {
  assert.equal(parseAdvName('PROTORIG ES 2BEA4E'), '2BEA4E');
  assert.equal(parseAdvName('PROTORIG ES 657837'), '657837');
  assert.equal(parseAdvName('protorig es 2bea4e'), '2BEA4E');
  assert.equal(parseAdvName('  PROTORIG ES  2BEA4E  '), '2BEA4E');
  assert.equal(parseAdvName('PROTORIG ES'), '', '旧固件没有后缀：不认身份，但前缀匹配仍可用');
  assert.equal(parseAdvName('PROTORIG ES2BEA4E'), '');
  assert.equal(parseAdvName('PROTORIG ES 2BEA4E2'), '');
  assert.equal(parseAdvName('MIPROTORIG ES 2BEA4E'), '');
  assert.equal(parseAdvName(''), '');
  assert.equal(advNameFor('2bea4e'), 'PROTORIG ES 2BEA4E');
  assert.equal(advNameFor(''), 'PROTORIG ES');
});

test('设备标签：PROTORIG ES · 编号 · 端口，未识别时不编编号', () => {
  assert.equal(deviceLabel({ admin: '2BEA4E', port: 'COM8' }), 'PROTORIG ES · 2BEA4E · USB COM8');
  assert.equal(deviceLabel({ admin: '2BEA4E', transport: 'ble' }), 'PROTORIG ES · 2BEA4E · BLE');
  assert.equal(deviceLabel({}), 'PROTORIG ES · 未识别');
  assert.equal(deviceLabel({ port: '/dev/cu.usbmodem14201' }), 'PROTORIG ES · 未识别 · USB /dev/cu.usbmodem14201');
});

test('候选选择：未绑定选第一个（P11）；绑定后只认编号；先试没问出口', () => {
  const two = [
    { path: 'COM5', admin: '657837' },
    { path: 'COM8', admin: '2BEA4E' },
  ];
  assert.deepEqual(selectCandidate(two, '2BEA4E'), { candidate: two[1], reason: 'bound-known' });
  assert.deepEqual(selectCandidate(two, ''), { candidate: two[0], reason: 'unbound-multiple' });
  assert.deepEqual(selectCandidate([two[1]], ''), { candidate: two[1], reason: 'unbound-single' });
  assert.deepEqual(
    selectCandidate([{ path: 'COM5', admin: '657837' }, { path: 'COM7', admin: '' }], '2BEA4E'),
    { candidate: { path: 'COM7', admin: '' }, reason: 'bound-unknown' },
  );
  assert.deepEqual(selectCandidate(two, 'ABCDEF'), { candidate: null, reason: 'bound-mismatch' });
  assert.deepEqual(selectCandidate([], '2BEA4E'), { candidate: null, reason: 'no-device' });
  assert.match(describeSelection('bound-mismatch', { bound: '2BEA4E' }), /未找到绑定的手表 2BEA4E/);
  assert.match(describeSelection('unbound-single'), /自动连接/);
});

/* ------------------------------------------------------- 协议：admin 行 */

test('parseLine：adminid 行变成 admin 事件，不影响其它行', () => {
  const ev = parseLine('bouffalolab />admin=2BEA4E chip=0123456789ABCDEF');
  assert.equal(ev.type, 'admin');
  assert.equal(ev.admin, '2BEA4E');
  assert.equal(ev.chipOk, true);
  assert.equal(parseLine('@PCM1 PC 1').type, 'pc');
  assert.equal(parseLine('[face] admin_id = 2BEA4E').type, 'other');
});

test('WatchLink.queryAdminId：等到回包返回身份，超时返回 null', async () => {
  const link = new WatchLink({ debug: false });
  link.state.connected = true;
  const sent = [];
  link.shell = async (line) => { sent.push(line); };

  const pending = link.queryAdminId(200);
  await settle(10);
  link.parser.push(Buffer.from('bouffalolab />admin=2BEA4E chip=0123456789ABCDEF\r\n'));
  const found = await pending;
  assert.equal(found.admin, '2BEA4E');
  assert.deepEqual(sent, ['adminid']);
  assert.equal(link.state.adminId, '2BEA4E', '状态里也记一份');

  // 问不出来：等满超时返回 null（握手要能继续，不能卡死）
  const missed = await link.queryAdminId(60);
  assert.equal(missed, null);
});

/* ---------------------------------------------------- PanelService 设备层 */

function makeFakeLink({ identities = {} } = {}) {
  const link = new EventEmitter();
  link.state = { connected: false, pcMode: false, passthrough: false, status: null, hello: null, adminId: null };
  link.path = null;
  link.openCount = 0;
  link.opened = [];
  link.writes = [];
  link.identity = identities;
  link.open = async (path) => {
    link.openCount += 1;
    link.opened.push(path);
    link.path = path;
    link.state.connected = true;
    link.emit('open', { path });
    return path;
  };
  link.write = async (buffer) => { link.writes.push(Buffer.from(buffer)); };
  link.syncTime = async () => {
    const cmd = 'settime 26 11 08 12 00 00';
    link.writes.push(Buffer.from(`${cmd}\r\n`));
    return cmd;
  };
  link.queryState = async () => ({ active: false, transport: 1, passthrough: false, usbraw: false });
  link.queryAdminId = async () => {
    const admin = link.identity[link.path] ?? '';
    if (admin) link.state.adminId = admin;
    return admin ? { admin, chip: '', chipOk: false } : null;
  };
  link.requestStatus = async () => {};
  link.shell = async (line) => {
    link.writes.push(Buffer.from(`${line}\r\n`));
    if (line === 'pcmode 1') {
      link.state.pcMode = true;
      link.emit('pc', { value: 1 });
    }
  };
  link.sayHello = async () => {
    link.state.passthrough = true;
    link.emit('mode', { value: 1 });
  };
  link.close = async () => {
    if (!link.state.connected) return;
    link.state.connected = false;
    link.emit('close', { reason: 'requested' });
  };
  return link;
}

function makeDeviceService(config = {}, { identities = {}, platform = 'win32', portPaths = ['COM8'] } = {}) {
  const service = new PanelService({ config, logger: silent, platform });
  service.link = makeFakeLink({ identities });
  service.discover = async () => portPaths.map((path) => ({ path, vendorId: '349B', productId: '6160' }));
  service.probePort = async (path) => identities[path] ?? '';
  service.ble.start = async () => {};
  service.ble.stop = () => {};
  service.config.reconnectDelayMs = 20;
  service.fb = { invalidated: 0, invalidate() { this.invalidated += 1; }, setFromRGBA() {} };
  service.flusher = { flush: async () => ({ bands: 0, bytes: 0, ms: 0 }) };
  service.renderer = { draw: () => ({ rgba: new Uint8ClampedArray(480 * 480 * 4) }) };
  service.tokens = { layout: 'monitor-v4', hotzones: {} };
  service.attachLinkForTest();
  return service;
}

test('绑定后挑对口（P5）：先开错表 → 关掉换下一口 → 只对绑定表进 PC 模式', async () => {
  const service = makeDeviceService(
    { deviceId: '2BEA4E' },
    { identities: { COM5: '657837', COM8: '2BEA4E' }, portPaths: ['COM5', 'COM8'] },
  );
  assert.equal(await service.probeUsbForTest(), true);
  // 换了口之后还要等下一轮重连监督器挑到 COM8
  assert.ok(await waitFor(() => service.link.path === 'COM8' && service.link.state.passthrough), '应连上绑定的那块');
  assert.equal(service.deviceId, '2BEA4E');
  assert.equal(service.portIdentity.get('COM5'), '657837', '错口的身份要被记住，下一轮跳过');
  const pcmode = service.link.writes.filter((buf) => buf.toString('utf8').startsWith('pcmode 1'));
  assert.equal(pcmode.length, 1, '只对绑定表发了一次 pcmode 1');
  assert.equal(service.link.state.connected, true);
  await service.dispose();
});

test('绑定但所有候选都不是它（P5）：不连接，也不下发任何设置', async () => {
  const service = makeDeviceService(
    { deviceId: '2BEA4E' },
    { identities: { COM5: '657837', COM7: '111111' }, portPaths: ['COM5', 'COM7'] },
  );
  // 第一轮先开 COM5（身份还不知道）→ 握手问出 657837 与绑定不符 → 关掉换下一个口
  const first = await service.probeUsbForTest();
  assert.equal(first, true, '第一轮先开第一个候选口（身份这时还不知道）');
  assert.ok(await waitFor(() => service.portIdentity.get('COM5') === '657837' && !service.link.state.connected));
  assert.ok(await waitFor(() => service.link.opened.length >= 2), '应换到第二个候选口');
  assert.ok(await waitFor(() => service.portIdentity.get('COM7') === '111111' && !service.link.state.connected));
  await settle(80); // 等重连监督器排的那一轮跑完

  assert.equal(await service.probeUsbForTest(), false, '两个候选口都已知不匹配 → 不再开');
  assert.equal(service.link.state.connected, false);
  assert.equal(service.link.opened.length, 2, '每个候选口只试一次');
  assert.equal(service.deviceId, '');
  assert.equal(service.link.writes.filter((buf) => buf.toString('utf8').startsWith('pcmode')).length, 0);
  await service.dispose();
});

test('未绑定单表（P11）：行为与升级前一致，直接连第一个口', async () => {
  const service = makeDeviceService({}, { identities: { COM8: '2BEA4E' }, portPaths: ['COM8'] });
  assert.equal(await service.probeUsbForTest(), true);
  assert.ok(await waitFor(() => service.link.state.passthrough));
  assert.equal(service.link.openCount, 1, '只开一次，不因为探活反复开关');
  assert.equal(service.deviceId, '2BEA4E', '顺手把身份记下来（设置页要显示）');
  assert.equal(service.stateForApi().device.label, 'PROTORIG ES · 2BEA4E · USB COM8');
  await service.dispose();
});

test('未绑定单表 + 旧固件问不出 adminid：照样连接（不能因为探活失败就不连）', async () => {
  const service = makeDeviceService({}, { identities: {}, portPaths: ['COM8'] });
  assert.equal(await service.probeUsbForTest(), true);
  assert.ok(await waitFor(() => service.link.state.passthrough));
  assert.equal(service.deviceId, '');
  await service.dispose();
});

test('换表保护（P7）：admin_id 变化 → 清缓存、整屏重发、重置指标与焦点、重发亮度/常亮', async () => {
  const service = makeDeviceService({}, { identities: { COM8: '657837' }, portPaths: ['COM8'] });
  await service.probeUsbForTest();
  assert.ok(await waitFor(() => service.deviceId === '657837' && service.link.state.passthrough));
  // 模拟"旧表上已经下发过"的状态
  service.lastAppliedBrightness = 128;
  service.lastAppliedAlwaysOn = true;
  service.metrics = { cpuPercent: 42 };
  service.focus.sessionId = 'session-old';
  service.focus.steps = 7;
  service.focus.tool = 'bash';
  service.ble.lastAddress = '74:88:A8:C7:83:20';
  const invalidatedBefore = service.fb.invalidated;
  const frameCount = (op) => service.link.writes
    .filter((buf) => buf.length > 3 && buf[0] === 0xA5 && buf[1] === 0x5A && buf[3] === op).length;
  const brightBefore = frameCount(0x02);
  const alwaysOnBefore = frameCount(0x15);

  // 换表 = 拔掉再插上另一块（同一个 COM 号）：close 会清掉"已验证"记录，重连必须重新问
  await service.link.close();
  service.link.identity = { COM8: '2BEA4E' };
  assert.ok(await waitFor(() => service.deviceId === '2BEA4E'), '重连后应识别出新编号');
  assert.ok(await waitFor(() => service.link.state.passthrough), '新表要重新握手');

  assert.ok(service.fb.invalidated > invalidatedBefore, '必须整屏重发');
  assert.equal(service.metrics, null, '指标属于旧表');
  assert.equal(service.focus.steps, 0, '焦点状态重置');
  assert.equal(service.focus.sessionId, 'session-old', '会话绑定保留，换表后还跟当前会话');
  assert.equal(service.ble.lastAddress, null, 'BLE 直连缓存属于旧表');
  assert.ok(
    await waitFor(() => frameCount(0x02) > brightBefore),
    '亮度要重新下发（0x02）',
  );

  // 常亮（0x15）由换表后触发的那次 tick 补发（握手结束会 #paintSoon(0)）
  assert.ok(
    await waitFor(() => frameCount(0x15) > alwaysOnBefore),
    '常亮要重新下发（0x15）',
  );
  assert.equal(service.lastAppliedAlwaysOn, true, '常亮重新下发后缓存恢复为已下发');
  await service.dispose();
});

test('BLE 广播名身份（P6）：名字后缀定身份，换名触发换表保护', async () => {
  const service = makeDeviceService({}, { identities: {}, portPaths: [] });
  service.ble.onDevice({ name: 'PROTORIG ES 2BEA4E' });
  assert.equal(service.deviceId, '2BEA4E');
  assert.equal(service.deviceSource, 'ble');
  assert.equal(service.stateForApi().device.label, 'PROTORIG ES · 2BEA4E');

  service.lastAppliedBrightness = 200;
  service.focus.steps = 3;
  service.ble.onDevice({ name: 'PROTORIG ES 657837' });
  assert.equal(service.deviceId, '657837');
  assert.equal(service.lastAppliedBrightness, null, '换表要重下发亮度');
  assert.equal(service.focus.steps, 0);
  await service.dispose();
});

test('设备动作：断开暂停重连，重连接回来（P10 概览按钮）', async () => {
  const service = makeDeviceService({}, { identities: { COM8: '2BEA4E' }, portPaths: ['COM8'] });
  await service.probeUsbForTest();
  assert.ok(await waitFor(() => service.link.state.connected));

  await service.manualDisconnect();
  assert.equal(service.link.state.connected, false);
  assert.equal(service.manualPaused, true, '断开后暂停自动重连');
  assert.equal(service.stateForApi().paused, true);
  // 暂停期间探测是空操作，不会又把口打开
  assert.equal(await service.probeUsbForTest(), false);
  assert.equal(service.link.state.connected, false);

  const result = await service.reconnectNow();
  assert.equal(result.opened, true);
  assert.equal(service.manualPaused, false);
  assert.equal(service.link.state.connected, true);
  await service.dispose();
});

test('候选扫描（P10）：逐个问 adminid 并给标签；已连接口不再重复打开', async () => {
  const service = makeDeviceService(
    {},
    { identities: { COM5: '657837', COM8: '2BEA4E' }, portPaths: ['COM5', 'COM8'] },
  );
  await service.probeUsbForTest(); // 连上 COM5
  assert.ok(await waitFor(() => service.link.state.connected));
  service.link.identity = { COM5: '657837', COM8: '2BEA4E' };

  const view = await service.listDevices({ refresh: true, probe: true });
  assert.equal(view.candidates.length, 2);
  const byPath = Object.fromEntries(view.candidates.map((item) => [item.path, item]));
  assert.equal(byPath.COM5.admin, '657837', '当前连接口的身份直接取链路状态');
  assert.equal(byPath.COM8.admin, '2BEA4E', '另一个口靠逐口探活问出来');
  assert.equal(byPath.COM8.label, 'PROTORIG ES · 2BEA4E · USB COM8');
  assert.equal(byPath.COM5.connected, true);
  assert.equal(view.platform.flash, true);
  assert.equal(view.bound, '');
  assert.equal(view.current, '657837');

  // 第二次 probe 不重复开同一个口（身份已缓存）
  let probes = 0;
  service.probePort = async () => { probes += 1; return ''; };
  await service.listDevices({ refresh: true, probe: true });
  assert.equal(probes, 0, '已探明的口不再开');
  await service.dispose();
});

test('设备绑定配置：规范化成 6 位大写；非法值当作未绑定', () => {
  assert.equal(normalizeConfig({ deviceId: '2bea4e' }).deviceId, '2BEA4E');
  assert.equal(normalizeConfig({ deviceId: ' 0x2BEA4E ' }).deviceId, '2BEA4E');
  assert.equal(normalizeConfig({ deviceId: 'nope' }).deviceId, '');
  assert.equal(normalizeConfig({}).deviceId, '');
});

test('平台能力（P3→macOS 后端）：macOS 的 PanelService 状态里 flash=true', () => {
  const service = new PanelService({ config: {}, logger: silent, platform: 'darwin' });
  const state = service.stateForApi();
  assert.equal(state.platform.os, 'darwin');
  assert.equal(state.platform.name, 'macOS');
  assert.equal(state.platform.flash, true, 'macOS 现在也能烧录（BLFlashCommand 后端）');
  assert.equal(typeof service.listDevices, 'function');
});

test('Windows 烧录后端在 macOS 上拒绝（macOS 走 MacFlashService）', async () => {
  const service = new FlashService({ panel: { stateForApi: () => ({}) }, logger: silent, platform: 'darwin' });
  assert.equal(service.supported, false);
  assert.equal(service.view().supported, false);
  const started = await service.start('C:/nope/demo.bin');
  assert.equal(started.ok, false);
  assert.match(started.error, /macOS|Windows/);
  const pre = await service.preflight('C:/nope/demo.bin');
  assert.equal(pre.ok, false);
  assert.match(pre.error, /macOS|Windows/);
  assert.equal(service.job, null, '不应创建烧录任务');
});

/* ------------------------------------------------- 设计稿（包内 tokens） */

test('设计稿：解析顺序 = 配置 > 包内 design > PROTORIG_DESIGN_DIR > 仓库路径', () => {
  const packaged = 'C:/work/pkg/design/dsh_panel_v4';
  const env = 'C:/work/repo-env/design/dsh_panel_v4';
  const repo = 'C:/work/repo/design/dsh_panel_v4';
  const all = { packaged, env, repo };

  assert.equal(resolveDesignDir({ ...all, exists: () => true }), packaged, '包内副本优先');
  assert.equal(
    resolveDesignDir({ ...all, exists: (dir) => dir === env }),
    env,
    '包内没有时才用 PROTORIG_DESIGN_DIR',
  );
  assert.equal(resolveDesignDir({ ...all, exists: () => false }), repo, '最后回退仓库路径');
  assert.equal(
    resolveDesignDir({ ...all, configured: 'D:/my-design', exists: () => true }),
    'D:/my-design',
    '显式配置最优先（即使包里也有）',
  );
  assert.equal(resolveDesignDir({ ...all, configured: '  ', exists: () => true }), packaged, '空白配置视为没配');

  // 生产默认解析出来的目录里必须真的有 tokens.json
  assert.ok(fs.existsSync(new URL('../design/dsh_panel_v4/tokens.json', import.meta.url)));
});

test('设计稿：包内 tokens 与内部仓库副本一致（防漂移），layout=monitor-v4', () => {
  const read = (rel) => JSON.parse(fs.readFileSync(new URL(rel, import.meta.url), 'utf8'));
  const packaged = read('../design/dsh_panel_v4/tokens.json');
  const repoUrl = new URL('../../../local/design/dsh_panel_v4/tokens.json', import.meta.url);
  assert.equal(packaged.layout, 'monitor-v4');
  /* 公开导出树不含 local/：没有内部副本时跳过防漂移比较，包内 tokens 仍是唯一运行时来源。 */
  if (!fs.existsSync(repoUrl)) return;
  const repo = JSON.parse(fs.readFileSync(repoUrl, 'utf8'));
  assert.deepEqual(
    packaged,
    repo,
    '包内 design/dsh_panel_v4/tokens.json 与仓库副本不一致：改设计稿后请同步复制到插件 design/',
  );
});
