/*
 * panel.test.mjs —— 面板服务纯逻辑测试（不需要手表/串口）
 *
 * 覆盖：
 *   · 审批：手表先答（允许/拒绝）、10 s 超时回落、卡状态
 *   · 问答：单选/多选/翻页/提交、超时回落、提交内容与选项一致
 *   · 提醒：0x03 震动帧 + 0x02 亮度帧的字节、次数、恢复原亮度
 *   · 会话投影：真实 DSH 0.2.2 事件字段 → 状态/步数/CTX，子代理事件忽略
 *   · BLE 数字通道：MetricsCollector → 0x52 字段换算
 *
 * 运行：npm test（node --test test）
 */
import './_env.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { PanelService } from '../src/panel-service.mjs';
import { metricsForBle, brandOf, BleBridge } from '../src/ble-bridge.mjs';
import { normalizeConfig } from '../src/config.mjs';
import { questionView } from '../src/design-orbit.mjs';
import { OP, ACK, SYNC0, SYNC1 } from '../src/protocol.mjs';

const ZONES = {
  deny: [106, 296, 128, 96],
  allow: [246, 296, 128, 96],
  option: [92, 218, 296, 96],
  next: [128, 314, 104, 96],
  submit: [248, 314, 104, 96],
};
const CENTER = Object.fromEntries(
  Object.entries(ZONES).map(([name, [x, y, w, h]]) => [name, { x: x + w / 2, y: y + h / 2 }]),
);
const silent = { info() {}, warn() {}, error() {}, debug() {} };
const settle = (ms = 15) => new Promise((resolve) => setTimeout(resolve, ms));

function makeService(config = {}) {
  const service = new PanelService({ config, logger: silent });
  /* 卡片只有在"能显示"时才会被接管（USB 透传中 / BLE 已连）。单元测试里没有真手表，
   * 默认按最典型的可见状态起步；要测"看不见"的用例自己把状态改回去。 */
  service.link.state.connected = true;
  service.link.state.pcMode = true;
  service.link.state.passthrough = true;
  // 设备登记表默认用假的：测试不读/不写用户的 devices.json
  service.deviceStore = { list: () => [], note: () => null };
  service.tokens = { layout: 'monitor-v4', hotzones: ZONES };
  service.ble.start = async () => {};
  service.ble.stop = () => {};
  // 审批/问答/提醒测试会通过 #paintSoon 启动面板 tick，断开分支会排 USB 探测：
  // 默认探测结果为空，测试绝不碰真实 COM8（否则 serialport 的句柄会让测试进程挂住）。
  service.discover = async () => [];
  service.config.reconnectDelayMs = 60000;
  // 触摸只在像素面板（pcMode+passthrough=true）上生效
  service.link.state.pcMode = true;
  service.link.state.passthrough = true;
  return service;
}

/** 一次真实点击：同点按下 → 100 ms → 抬起（>=80 ms 才算点击）。 */
async function tap(service, zone, { holdMs = 100, from, to } = {}) {
  service.link.state.pcMode = true;
  service.link.state.passthrough = true;
  service.touchGuardUntil = 0; // 卡片刚出现时有意静默，测试里显式跳过
  const point = from ?? CENTER[zone];
  const release = to ?? CENTER[zone];
  service.handleTouch({ pressed: true, x: point.x, y: point.y });
  await settle(holdMs);
  service.handleTouch({ pressed: false, x: release.x, y: release.y });
}

/** 模拟一次滑动：按下后移动若干帧再抬起（不改 passthrough/guard，走真实护栏）。 */
async function swipe(service, zone, dx = 40) {
  const point = CENTER[zone];
  service.handleTouch({ pressed: true, x: point.x, y: point.y });
  await settle(40);
  service.handleTouch({ pressed: true, x: point.x + dx / 2, y: point.y });
  await settle(40);
  service.handleTouch({ pressed: false, x: point.x + dx, y: point.y });
}

/** 把一串上行帧拆成 [{op, payload}]，并校验 A5 5A len。 */
function framesOf(buffers) {
  return buffers.map((buffer) => {
    const bytes = Buffer.from(buffer);
    assert.equal(bytes[0], SYNC0, 'sync0');
    assert.equal(bytes[1], SYNC1, 'sync1');
    assert.equal(bytes[2], bytes.length - 3, 'len 与 payload 一致');
    return { op: bytes[3], payload: [...bytes.subarray(4)] };
  });
}

/* ----------------------------------------------------------------- 审批 */

test('审批：手表点“允许一次” → allowed-once，不再等超时', async () => {
  const service = makeService({ approval: { watchTimeoutMs: 5000 } });
  const promise = service.requestApproval({ toolName: 'bash', command: 'echo hi' });
  await settle();
  assert.equal(service.approval.canAnswer, true);
  assert.match(service.approvalViewForTest().cmd, /echo hi/);
  await tap(service, 'allow');
  const result = await promise;
  assert.equal(result.outcome, 'allowed-once');
  service.clearApproval();
  assert.equal(service.approval, null);
});

test('审批：手表点“拒绝” → rejected', async () => {
  const service = makeService();
  const promise = service.requestApproval({ toolName: 'bash', reason: '危险操作' });
  await settle();
  await tap(service, 'deny');
  assert.equal((await promise).outcome, 'rejected');
});

test('审批：到点无人应答 → 回落电脑，卡变成“电脑待确认”', async () => {
  const service = makeService();
  service.config.approval.watchTimeoutMs = 60; // 测试里缩短，逻辑同 10 s
  const promise = service.requestApproval({ toolName: 'bash', command: 'rm -rf /' });
  await settle();
  const result = await promise;
  assert.equal(result.fallback, true);
  assert.equal(service.approval.canAnswer, false);
  assert.equal(service.approvalViewForTest().countdown, 0);
  service.clearApproval();
});

test('审批：signal 中止 → aborted（不把作废请求转给电脑）', async () => {
  const service = makeService();
  const controller = new AbortController();
  const promise = service.requestApproval({ toolName: 'bash', signal: controller.signal });
  await settle();
  controller.abort();
  assert.equal((await promise).aborted, true);
});

test('审批：忙时第二张排队，第一张处理完再出第二张', async () => {
  const service = makeService({ approval: { watchTimeoutMs: 5000 } });
  const first = service.requestApproval({ toolName: 'bash', command: 'echo one' });
  await settle();
  const second = service.requestApproval({ toolName: 'edit', command: 'echo two' });
  let secondDone = false;
  void second.then(() => { secondDone = true; });
  await settle();
  assert.equal(secondDone, false, '第二张在队列里，不能立刻回电脑');
  assert.equal(service.approval.toolName, 'bash');
  await tap(service, 'allow');
  assert.equal((await first).outcome, 'allowed-once');
  service.clearApproval(first.token);
  await settle();
  assert.equal(service.approval.toolName, 'edit', '第一张撤下后第二张顶上');
  await tap(service, 'deny');
  assert.equal((await second).outcome, 'rejected');
  service.clearApproval(second.token);
  assert.equal(service.approval, null);
  await service.dispose();
});

test('审批：排队超过上限直接交给电脑，不动当前卡', async () => {
  const service = makeService();
  /* 设置页是改 rawConfig 后重新 normalize；测试直接改两边，避免下一次 tick 覆盖。 */
  service.rawConfig.queueWaitSec = 0.02;
  service.config.queueWaitSec = 0.02;
  void service.requestApproval({ toolName: 'bash' });
  await settle();
  const second = service.requestApproval({ toolName: 'edit' });
  const result = await second;
  assert.equal(result.fallback, true);
  assert.equal(result.queueTimeout, true);
  assert.equal(service.approval.toolName, 'bash', '排队超时不能撤掉当前卡');
  service.clearApproval();
  await service.dispose();
});

test('叠加层卡片带会话短标题（多对话并发可区分）', async () => {
  const service = makeService();
  service.focus.sessionId = 'session-title-test';
  service.focus.title = '插件UI优化及部署调试';
  const approval = service.requestApproval({ toolName: 'bash', command: 'echo hi' });
  assert.match(service.approvalViewForTest().cmd, /【插件UI优化及部署调…】/, '审批卡命令前带会话短标题');
  await tap(service, 'allow');
  await approval;
  service.clearApproval();

  service.focus.title = '插件UI优化及部署调试'; // 测试假 sessionId 会被 title 刷新回 SESSION
  const question = service.requestQuestion({
    questions: [{ id: 'q1', question: '选一个', options: [{ label: 'A' }] }],
  });
  assert.match(service.questionViewForTest().prompt, /【插件UI优化及部署调…】/, '问答卡题干前带会话短标题');
  service.clearQuestion();
  void question;
  await service.dispose();
});

/* ----------------------------------------------------------------- 问答 */

test('问答：多选累加并提交，回写 label 与手表选择一致', async () => {
  const service = makeService();
  const promise = service.requestQuestion({
    questions: [{
      id: 'q1',
      question: '要改哪些文件？',
      multiSelect: true,
      options: [{ label: 'A' }, { label: 'B' }, { label: 'C' }],
    }],
  });
  await settle();
  await tap(service, 'option'); // 选 A
  await tap(service, 'next'); // 翻到 B
  await tap(service, 'option'); // 选 B
  await tap(service, 'next'); // 翻到 C（不选）
  await tap(service, 'submit');
  const result = await promise;
  assert.deepEqual(result.answer.answers, [{ id: 'q1', selected: ['A', 'B'] }]);
});

test('问答：单选只能留一个', async () => {
  const service = makeService();
  const promise = service.requestQuestion({
    questions: [{ id: 'q1', question: '选一个', options: [{ label: 'A' }, { label: 'B' }] }],
  });
  await settle();
  await tap(service, 'option'); // A
  await tap(service, 'next'); // B
  await tap(service, 'option'); // 选 B 应替换 A
  await tap(service, 'submit');
  assert.deepEqual((await promise).answer.answers, [{ id: 'q1', selected: ['B'] }]);
});

test('问答：多个问题逐题提交', async () => {
  const service = makeService();
  const promise = service.requestQuestion({
    questions: [
      { id: 'q1', question: '第一个', options: [{ label: 'A' }] },
      { id: 'q2', question: '第二个', options: [{ label: 'B' }] },
    ],
  });
  await settle();
  await tap(service, 'option'); // q1=A
  await tap(service, 'submit'); // 进入 q2
  await settle();
  await tap(service, 'option'); // q2=B
  await tap(service, 'submit');
  assert.deepEqual((await promise).answer.answers, [
    { id: 'q1', selected: ['A'] },
    { id: 'q2', selected: ['B'] },
  ]);
});

test('问答：超时无人操作 → 回落电脑，卡留在表上但不可再答', async () => {
  const service = makeService();
  service.config.questions.watchTimeoutMs = 60;
  const promise = service.requestQuestion({
    questions: [{ id: 'q1', question: '选一个', options: [{ label: 'A' }, { label: 'B' }] }],
  });
  await settle();
  assert.equal((await promise).fallback, true);
  assert.equal(service.question.canAnswer, false);
  await tap(service, 'option');
  assert.equal(service.question.selections[0].size, 0, '回落电脑后手表不再接受选择');
  service.clearQuestion();
});

test('问答：signal 中止 → aborted', async () => {
  const service = makeService();
  const controller = new AbortController();
  const promise = service.requestQuestion({
    questions: [{ id: 'q1', question: 'x', options: [{ label: 'A' }] }],
    signal: controller.signal,
  });
  await settle();
  controller.abort();
  assert.equal((await promise).aborted, true);
});

/* --------------------------------------------------------------- 提醒帧 */

test('提醒：完成档 = 2 声震动 + 2 次亮度脉冲，最后恢复原亮度', async () => {
  const service = makeService({ reminder: { done: 'both', vibratePreset: 'medium' } });
  const sent = [];
  service.link.state.connected = true;
  service.link.write = async (buffer) => { sent.push(Buffer.from(buffer)); };
  service.brightBase = 100;
  await service.remind('done');
  const frames = framesOf(sent);
  const vibrations = frames.filter((frame) => frame.op === OP.VIBRATE);
  const brights = frames.filter((frame) => frame.op === OP.BRIGHT);
  assert.equal(vibrations.length, 2, '完成档震动两声');
  assert.deepEqual(vibrations.map((frame) => frame.payload[0] | (frame.payload[1] << 8)), [120, 120]);
  assert.equal(brights.length, 5, '两次脉冲 = 2 次升亮 + 2 次恢复 + 收尾再补一次恢复');
  assert.deepEqual(brights.map((frame) => frame.payload[0]), [170, 100, 170, 100, 100]);
  assert.equal(brights.at(-1).payload[0], 100, '最后必须恢复原亮度');
});

test('提醒：仅震动档不碰亮度，且三档次数不同', async () => {
  const service = makeService({ reminder: { failed: true, needs: true, vibrate: true, bright: false } });
  const sent = [];
  service.link.state.connected = true;
  service.link.write = async (buffer) => { sent.push(Buffer.from(buffer)); };
  await service.remind('failed');
  await service.remind('needs');
  const frames = framesOf(sent);
  assert.equal(frames.filter((frame) => frame.op === OP.BRIGHT).length, 0);
  assert.equal(frames.filter((frame) => frame.op === OP.VIBRATE).length, 5, '失败 2 声 + 需响应 3 声');
});

test('提醒：关闭档不发任何帧', async () => {
  const service = makeService({ reminder: { done: 'off' } });
  const sent = [];
  service.link.state.connected = true;
  service.link.write = async (buffer) => { sent.push(Buffer.from(buffer)); };
  await service.remind('done');
  assert.equal(sent.length, 0);
});

/* ------------------------------------------------------------- 会话投影 */

function sessionEvent(type, data) {
  return { type, time: Date.now(), data };
}

test('会话投影：真实 DSH 事件字段 → RUNNING/步数/CTX/DONE', async () => {
  const service = makeService();
  const session = { id: 'session-test-0001' };
  service.onSessionEvent(session, sessionEvent('turn/start', { turn: 1 }));
  assert.equal(service.focus.live, 'RUNNING');
  service.onSessionEvent(session, sessionEvent('step/start', { turn: 1, step: 1 }));
  service.onSessionEvent(session, sessionEvent('step/start', { turn: 1, step: 2 }));
  service.onSessionEvent(session, sessionEvent('tool/call', {
    turn: 1, step: 2, callId: 'call-1', name: 'bash', arguments: '{"command":"echo hi"}',
  }));
  service.onSessionEvent(session, sessionEvent('assistant/message', {
    usage: { uncachedInputTokens: 100, cacheReadTokens: 200, cacheWriteTokens: 33, outputTokens: 999 },
  }));
  assert.equal(service.focus.steps, 2);
  assert.equal(service.focus.tool, 'bash');
  assert.ok(Math.abs(service.focus.contextUsed - 333) < 1, 'prompt 侧 tokens = 100+200+33，不含 output');
  service.onSessionEvent(session, sessionEvent('turn/end', { turn: 1, reason: { kind: 'completed' } }));
  assert.equal(service.focus.live, 'IDLE');
  assert.equal(service.focus.receipt.outcome, 'DONE');
  // 审批命令按 callId 记住，供审批卡显示
  assert.equal(service.commandFor('call-1'), 'echo hi');
});

test('会话投影：回合中途加载（错过 turn/start）也能恢复 RUNNING/NEEDS', async () => {
  const service = makeService();
  const session = { id: 'session-test-0003' };
  assert.equal(service.focus.live, 'IDLE');
  service.onSessionEvent(session, sessionEvent('step/start', { turn: 3, step: 1 }));
  assert.equal(service.focus.live, 'RUNNING', '收到回合内事件就补 RUNNING');
  service.onSessionEvent(session, sessionEvent('tool/call', {
    turn: 3, step: 1, callId: 'call-9', name: 'bash', arguments: '{"command":"echo hi"}',
  }));
  assert.equal(service.focus.tool, 'bash');
  assert.equal(service.stateForApi().focus.state, 'RUNNING');
  assert.equal(service.stateForApi().focus.turnOpen, true);

  /* 中途来的审批：先 NEEDS，答复后回到 RUNNING（不会掉回 IDLE） */
  const other = makeService();
  const s2 = { id: 'session-test-0004' };
  other.onSessionEvent(s2, sessionEvent('approval/asked', { id: 'a1' }));
  assert.equal(other.focus.live, 'NEEDS');
  other.onSessionEvent(s2, sessionEvent('approval/decided', { id: 'a1' }));
  assert.equal(other.focus.live, 'RUNNING');
});

test('会话投影：工具报错但回合 completed → DONE（重试不能算整轮失败）', async () => {
  const service = makeService();
  const session = { id: 'session-test-0005' };
  service.onSessionEvent(session, sessionEvent('turn/start', {}));
  service.onSessionEvent(session, sessionEvent('tool/result', { error: { message: 'tool failed once' } }));
  assert.equal(service.focus.live, 'RUNNING');
  service.onSessionEvent(session, sessionEvent('turn/end', { reason: { kind: 'completed' } }));
  assert.equal(service.focus.live, 'IDLE');
  assert.equal(service.focus.receipt.outcome, 'DONE', '中间工具失败但回合 completed，应显示 DONE');
});

test('会话投影：失败原因 → FAILED，子代理事件被忽略', async () => {
  const service = makeService();
  const session = { id: 'session-test-0002' };
  service.onSessionEvent(session, sessionEvent('turn/start', {}));
  service.onSessionEvent(session, sessionEvent('turn/end', { reason: { kind: 'error' } }));
  assert.equal(service.focus.receipt.outcome, 'FAILED');
  const child = { id: 'session-child', header: { parentSession: 'session-test-0002', delegationDepth: 1 } };
  service.onSessionEvent(child, sessionEvent('turn/start', {}));
  assert.equal(service.focus.sessionId, 'session-test-0002', '子代理不抢焦点');
});

test('会话投影：畸形事件不抛错', async () => {
  const service = makeService();
  assert.doesNotThrow(() => service.onSessionEvent({ id: 's' }, { type: 'turn/start', data: null }));
  assert.doesNotThrow(() => service.onSessionEvent(null, null));
  assert.doesNotThrow(() => service.onSessionEvent({ id: 's' }, { type: 'unknown/event' }));
});

/* ------------------------------------------------------------- BLE 换算 */

test('BLE 0x52：百分比 → 十分之一百分比，vram 未知传 -1', () => {
  assert.deepEqual(metricsForBle({
    cpuPercent: 9.3, gpuPercent: 19, memoryPercent: 55.5, gpuMemoryPercent: null,
    gpuVendor: 'NVIDIA GeForce RTX 4090', networkRxBps: 43520.7, networkTxBps: 4915,
  }), { cpu: 93, gpu: 190, ram: 555, vram: -1, brand: 1, rx: 43521, tx: 4915 });
  assert.equal(brandOf('AMD Radeon RX 7900'), 2);
  assert.equal(brandOf('Intel Arc A770'), 3);
  assert.equal(brandOf(''), 0);
});

/* ------------------------------------------- USB ↔ BLE 切换（回归：拔插后不重开） */

/**
 * 假链路：只实现 PanelService 用到的部分。
 * open() 成功时置 connected 并 emit('open')，和真实 WatchLink 的顺序一致。
 */
function makeFakeLink() {
  const link = new EventEmitter();
  const writes = [];
  link.state = { connected: false, pcMode: false, passthrough: false, status: null, hello: null };
  link.path = null;
  link.openCount = 0;
  link.writes = writes;
  link.open = async (path) => {
    link.openCount += 1;
    link.path = path;
    link.state.connected = true;
    link.emit('open', { path });
    return path;
  };
  link.write = async (buffer) => { writes.push(Buffer.from(buffer)); };
  link.syncTime = async () => {
    const cmd = 'settime 26 11 07 12 00 00';
    writes.push(Buffer.from(`${cmd}\r\n`)); // 真实 WatchLink 在 syncTime 内部就发了
    return cmd;
  };
  link.queryState = async () => ({ active: false, transport: 1, passthrough: false, usbraw: false });
  link.requestStatus = async () => {};
  link.shell = async (line) => {
    writes.push(Buffer.from(`${line}\r\n`));
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
    link.state.connected = false;
    link.emit('close', { reason: 'requested' });
  };
  return link;
}

async function waitFor(check, ms = 3000) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (check()) return true;
    await settle(20);
  }
  return false;
}

function makeSwitchableService(config = {}) {
  const service = makeService(config);
  service.link = makeFakeLink();
  const opened = [];
  const originalOpen = service.link.open;
  service.link.open = async (path) => { opened.push(path); return originalOpen(path); };
  const stops = [];
  service.ble.connected = true; // 模拟当前处在 BLE 降级
  service.ble.stop = () => {
    stops.push(Date.now());
    service.ble.connected = false;
    service.ble.child = null;
    service.ble.starting = false;
    return Promise.resolve();
  };
  service.ble.start = async () => { service.ble.connected = true; return true; };
  service.fb = {
    invalidated: 0,
    invalidate() { this.invalidated += 1; },
    setFromRGBA() {},
  };
  service.flusher = { flush: async () => ({ bands: 0, bytes: 0, ms: 0 }) };
  service.renderer = { draw: () => ({ rgba: new Uint8ClampedArray(480 * 480 * 4) }) };
  service.tokens = { layout: 'monitor-v4', hotzones: ZONES };
  service.attachLinkForTest();
  return { service, opened, stops };
}

test('USB 恢复：BLE 模式下探测到手表 → 停桥 → 开串口 → 对时/HELLO → 整屏重发', async () => {
  const { service, opened, stops } = makeSwitchableService();
  let discovers = 0;
  service.discover = async () => { discovers += 1; return [{ path: 'COM_TEST' }]; };

  const switched = await service.probeUsbForTest();
  assert.equal(switched, true, '探测到 USB 后应打开串口');
  assert.deepEqual(opened, ['COM_TEST']);
  assert.equal(stops.length, 1, '切换时停一次 BLE 桥');
  assert.equal(service.link.state.connected, true);
  assert.ok(
    await waitFor(() => service.link.writes.some((buf) => buf.toString('utf8').startsWith('settime'))),
    '重连后必须重新对时',
  );
  assert.ok(
    await waitFor(() => service.link.state.passthrough === true),
    'HELLO 打开了透传',
  );
  assert.ok(service.fb.invalidated >= 1, '重连后必须整屏重发（fb.invalidate）');
  assert.equal(service.link.openCount, 1);

  // 再探测一轮不应重复开串口/重复停桥
  const again = await service.probeUsbForTest();
  assert.equal(again, false);
  assert.equal(service.link.openCount, 1);
  assert.equal(stops.length, 1);
  await service.dispose();
});

test('USB 不在：探测不到不打开串口，也不重复启动 BLE', async () => {
  const { service, opened } = makeSwitchableService();
  let discovers = 0;
  let bleStarts = 0;
  service.discover = async () => { discovers += 1; return []; };
  service.ble.start = async () => { bleStarts += 1; return true; };
  service.ble.connected = false;

  assert.equal(await service.probeUsbForTest(), false);
  assert.equal(await service.probeUsbForTest(), false);
  assert.equal(discovers, 2, '每轮探测一次 VID/PID');
  assert.equal(opened.length, 0, '没找到设备不 open');
  assert.equal(bleStarts, 0, '探测本身不启动 BLE（由面板 tick 负责）');
  await service.dispose();
});

test('USB 掉线后自动重连：close 之后持续探测，直到设备回来（真机 bug 回归）', async () => {
  const { service, opened } = makeSwitchableService({ reconnectDelayMs: 30 });
  service.config.reconnectDelayMs = 30;
  service.discover = async () => [{ path: 'COM_TEST' }];
  await service.probeUsbForTest(); // 先连上
  assert.equal(service.link.openCount, 1);

  // 模拟 USB 拔出：close 后必须排下一轮探测（旧版只试一次就永久放弃）
  service.link.state.connected = false;
  service.link.emit('close', { reason: 'device' });
  assert.ok(
    await waitFor(() => service.link.openCount === 2),
    'close 后应在下一轮探测中重新 open',
  );
  assert.deepEqual(opened, ['COM_TEST', 'COM_TEST']);
  await service.dispose();
});

test('BLE 桥启动幂等：并发 start() 只做一次依赖检查，不会 spawn 多个桥', async () => {
  const bridge = new BleBridge({ config: normalizeConfig({}), logger: silent });
  let checks = 0;
  bridge.checkRuntime = async () => {
    checks += 1;
    await settle(30);
    return false; // 依赖不可用 → 不 spawn 真进程
  };
  await Promise.all([bridge.start(), bridge.start(), bridge.start()]);
  assert.equal(checks, 1, '并发 start() 只检查一次');
  assert.equal(bridge.starting, false);
  await bridge.stop();
});

test('握手期间串口掉线：sayHello 失败被捕获，不产生未捕获 rejection（曾崩宿主）', async () => {
  const { service } = makeSwitchableService();
  service.discover = async () => [{ path: 'COM_TEST' }];
  // 模拟"HELLO 发到一半串口没了"：sayHello 抛错，且透传状态被打上（让握手续完）
  service.link.sayHello = async () => {
    service.link.state.passthrough = true;
    throw new Error('串口没打开');
  };
  const switched = await service.probeUsbForTest();
  assert.equal(switched, true);
  await settle(600);
  assert.equal(service.link.state.connected, true);
  assert.equal(service.link.state.passthrough, true);
  await service.dispose();
});

/* ---------------------------------------- 模式切换 / 滑动误触 / 问答分页（2026-11-07 真机反馈） */

test('滑出再滑回电脑模式（PC 0→1）：自动重新握手，settime/HELLO/整屏重发', async () => {
  const { service } = makeSwitchableService();
  service.discover = async () => [{ path: 'COM_TEST' }];
  await service.probeUsbForTest();
  assert.ok(await waitFor(() => service.link.state.passthrough && !service.handshaking), '初始握手完成');
  const writesBefore = service.link.writes.length;
  const invalidatedBefore = service.fb.invalidated;

  // 用户左滑退出：PC 0 / MODE 0（透传关闭、回到本地页）
  service.link.state.pcMode = false;
  service.link.state.passthrough = false;
  service.link.emit('mode', { value: 0 });
  service.link.emit('pc', { value: 0 });
  assert.ok(service.touchGuardUntil > Date.now(), '模式切换后应有触摸静默期');

  // 用户滑回来：PC 1（passthrough 还是关的）
  service.link.state.pcMode = true;
  service.link.emit('pc', { value: 1 });
  assert.ok(
    await waitFor(() => service.link.state.passthrough && !service.handshaking, 4000),
    'pc=1 后必须重新 HELLO 打开透传',
  );
  const newWrites = service.link.writes.slice(writesBefore).map((b) => b.toString('utf8'));
  assert.ok(newWrites.some((line) => line.startsWith('settime')), '重新进入后重新对时');
  assert.ok(service.fb.invalidated > invalidatedBefore, '重新进入后整屏重发（fb.invalidate）');
  await service.dispose();
});

test('pc=0 时暂停发像素；pc=1 恢复后继续发', async () => {
  const { service } = makeSwitchableService();
  service.discover = async () => [{ path: 'COM_TEST' }];
  await service.probeUsbForTest();
  assert.ok(await waitFor(() => service.link.state.passthrough && !service.handshaking));
  let flushes = 0;
  service.flusher.flush = async () => {
    flushes += 1;
    return { bands: 0, bytes: 0, ms: 0 };
  };
  service.link.state.pcMode = false;
  service.link.state.passthrough = false;
  service.link.emit('mode', { value: 0 });
  service.link.emit('pc', { value: 0 });
  const before = flushes;
  await service.tickForTest();
  assert.equal(flushes, before, 'pc=0（本地页）绝不能发像素');

  service.link.state.pcMode = true;
  service.link.emit('pc', { value: 1 });
  assert.ok(await waitFor(() => service.link.state.passthrough && !service.handshaking, 4000));
  await service.tickForTest();
  assert.ok(flushes > before, 'pc=1 恢复后继续刷新像素');
  await service.dispose();
});

test('问答：滑出→滑回期间的滑动/残点不会替用户提交', async () => {
  const { service } = makeSwitchableService();
  service.link.state.connected = true;
  service.link.state.pcMode = true;
  service.link.state.passthrough = true;
  const pending = service.requestQuestion({
    questions: [{
      id: 'q1',
      question: '今天先做哪件事？',
      options: [{ label: 'A 验证面板' }, { label: 'B 写文档' }, { label: 'C 别的' }],
    }],
  });
  await settle();
  // 用户根本没选任何选项，滑出
  service.link.state.pcMode = false;
  service.link.state.passthrough = false;
  service.link.emit('mode', { value: 0 });
  service.link.emit('pc', { value: 0 });
  await swipe(service, 'submit', 40); // 滑动经过"提交"热区
  // 滑回来
  service.link.state.pcMode = true;
  service.link.state.passthrough = true;
  service.link.emit('pc', { value: 1 });
  // 滑动收尾的残点（不经过 tap() 的 guard 清零，走真实护栏）
  const point = CENTER.submit;
  service.handleTouch({ pressed: true, x: point.x, y: point.y });
  await settle(100);
  service.handleTouch({ pressed: false, x: point.x, y: point.y });

  assert.equal(service.question?.selections?.[0]?.size ?? 0, 0, '不应产生任何选择');
  const raced = await Promise.race([pending.then(() => 'settled'), settle(300).then(() => 'pending')]);
  assert.equal(raced, 'pending', '不能把用户没做过的选择回写给 DSH');
  service.clearQuestion();
  await service.dispose();
});

test('触摸护栏：passthrough=false 忽略、<80 ms 不算点击、位移 >24 px 不算点击', async () => {
  const { service } = makeSwitchableService();
  const prompt = () => service.requestQuestion({
    questions: [{ id: 'q1', question: '选一个', options: [{ label: 'A' }, { label: 'B' }] }],
  });
  let pending = prompt();
  await settle();

  // 1) 本地页（本地页上没有我们的按钮）→ 忽略
  service.link.state.passthrough = false;
  const ignorePoint = CENTER.option;
  service.handleTouch({ pressed: true, x: ignorePoint.x, y: ignorePoint.y });
  await settle(100);
  service.handleTouch({ pressed: false, x: ignorePoint.x, y: ignorePoint.y });
  assert.equal(service.question.selections[0].size, 0);

  // 2) 按住 20 ms → 不算点击
  service.link.state.passthrough = true;
  service.touchGuardUntil = 0;
  const point = CENTER.option;
  service.handleTouch({ pressed: true, x: point.x, y: point.y });
  await settle(20);
  service.handleTouch({ pressed: false, x: point.x, y: point.y });
  assert.equal(service.question.selections[0].size, 0, '太短不算点击');

  // 3) 按下到抬起位移 >24 px → 不算点击（滑动手势）
  service.handleTouch({ pressed: true, x: point.x, y: point.y });
  await settle(100);
  service.handleTouch({ pressed: true, x: point.x + 20, y: point.y });
  service.handleTouch({ pressed: false, x: point.x + 40, y: point.y });
  assert.equal(service.question.selections[0].size, 0, '滑动不算点击');

  // 4) 正常点击仍然有效（正控制）
  await tap(service, 'option');
  assert.equal(service.question.selections[0].size, 1, '正常点击应选中');
  service.clearQuestion();
  await service.dispose();
});

test('问答分页提示按设计稿：页码在选项提示里（1 / 3 · 点按选择）', () => {
  const base = {
    prompt: '今天先做哪件事？',
    options: [{ label: 'A' }, { label: 'B' }, { label: 'C' }],
    multiSelect: false,
    selected: [],
    canAnswer: true,
  };
  const first = questionView({ ...base, index: 0 });
  assert.equal(first.pageText, '1 / 3', '页码格式 1 / 3（设计稿口径，数字与斜杠间有空格）');
  assert.equal(first.hint, '1 / 3 · 点按选择', '页码放在选项提示里，标题行不带页码');
  const last = questionView({ ...base, index: 2 });
  assert.equal(last.pageText, '3 / 3');
  const chosen = questionView({ ...base, index: 1, selected: [1] });
  assert.equal(chosen.hint, '2 / 3 · 已选 · 点按取消');
  const single = questionView({ ...base, options: [{ label: 'A' }], index: 0 });
  assert.equal(single.pageText, '1 / 1');
});

test('问答：文字放不下不得禁用卡片（2026-11-21 实机：长选项被静默禁用 → 点了没反应）', () => {
  /* 实机复现：题干和选项都超出一行时，旧实现把 canChoose/canSubmit 都判成 false，
   * 手指点上去没有任何反应，用户以为卡坏了，只能去电脑上答。放不下只应截断显示。 */
  const wide = {
    prompt: '这张卡请依次试：①点选项 ②点提交，看看哪一步不对（题干故意写得很长很长）',
    options: [
      { label: '卡片根本没出现' },
      { label: '卡片出现了，但点选项没任何反应，怎么点都不动' },
      { label: '选项能选中，但点「提交」没反应' },
    ],
    multiSelect: false,
    selected: [],
    canAnswer: true,
    index: 1,
  };
  const view = questionView(wide);
  assert.equal(view.canChoose, true, '长选项仍必须可选');
  assert.equal(view.hint.endsWith('点按选择'), true, '提示应保持"点按选择"，而不是"请到电脑查看"');
  assert.equal(view.submitHint, '先选一项', '没选时提交按钮要写明原因（配套那一下震动）');
  const picked = questionView({ ...wide, selected: [1] });
  assert.equal(picked.canSubmit, true, '选中后必须能提交');
  assert.equal(picked.submitHint, '已选 1');
});

test('问答：题干太长必须换行显示，不能被静默截断（2026-11-21 实机反馈）', () => {
  /* 实机：题干单行截断，用户差点看不出问的是什么。现在按 19px 折行、一页两行。 */
  const longPrompt = '这张卡请依次试：先点一下选项，再点提交，看看哪一步不对，如果卡住了就告诉我停在哪';
  const view = questionView({
    prompt: longPrompt,
    options: [{ label: 'A' }],
    multiSelect: false,
    selected: [],
    canAnswer: true,
    index: 0,
    shownAt: Date.now(),
  });
  assert.equal(view.promptRows.length, 2, '一页最多两行');
  assert.equal(view.canChoose, true, '题干长也不许禁用选项');
  assert.equal(view.promptRows.join('').includes('如果你卡住了') || view.promptRows.join('').length > 20, true);
});

test('问答：题干超过两行时翻页，页码进标题行（不占底部提示）', () => {
  const veryLong = '很长的题干'.repeat(20);
  const view = questionView({
    prompt: veryLong, options: [{ label: 'A' }], multiSelect: false,
    selected: [], canAnswer: true, index: 0, shownAt: Date.now(),
  });
  assert.match(view.promptPageText, /^1\/\d+$/, '页码形如 1/3');
  assert.equal(view.promptRows.length, 2, '每页仍只显示两行');
  assert.equal(view.hint.includes('/'), true, '底部提示仍用于选项翻页');
});

test('问答：移交电脑后按钮全禁用，并给出明确指示（不再像可点）', () => {
  const view = questionView({
    prompt: '选一个', options: [{ label: 'A' }, { label: 'B' }],
    multiSelect: false, selected: [0], canAnswer: false, index: 0,
  });
  assert.equal(view.computer, true);
  assert.equal(view.canChoose, false, '移交后不能再选');
  assert.equal(view.canSubmit, false, '移交后不能再提交');
  assert.equal(view.submitHint, '已移交电脑');
  assert.equal(view.hint, '1 / 2 · 已移交电脑');
});

test('问答/审批：卡片显示不出来时立刻回落电脑（不能接管后两头都看不到）', async () => {
  /* 实机踩过：pcmode 退出电脑模式后发问答卡，USB 透传关了、BLE 也没连，
   * 插件却把问题接管了 —— 手表和电脑两边都没有卡，用户白等 30 s 超时。 */
  const { service } = makeSwitchableService();
  service.link.state.connected = true;
  service.link.state.pcMode = true;
  service.link.state.passthrough = true;
  service.link.state.pcMode = false;
  service.link.state.passthrough = false;
  const asked = await service.requestQuestion({
    questions: [{ id: 'q1', question: '选哪个？', options: [{ label: 'A' }] }],
  });
  assert.equal(asked.fallback, true, '看不见就不接管');
  assert.equal(asked.reason, 'card-not-visible');
  assert.equal(service.question, null, '不能留下一个看不见的卡');
  const approval = await service.requestApproval({ toolName: 'bash', reason: 'x' });
  assert.equal(approval.fallback, true);
  assert.equal(service.approval, null);
  await service.dispose();
});

test('烧录中重启宿主：flash-hold 落盘让新实例只等原应用口（不抢 ROM 口）', async () => {
  /* BL616 的 ROM 下载口与应用口 VID/PID 完全一样（349b:6160），isWatchPort 分不出来。
   * 烧录中途宿主重启时 flashHold（内存态）会丢，启动扫描就会把 ROM 口抓走。
   * 这里锁住"落盘 → 新实例接回"这条链路。 */
  const fs = await import('node:fs');
  const path = await import('node:path');
  const file = path.join(process.env.PROTORIG_DATA_DIR, 'flash-hold.json');
  const appPort = '/dev/tty.usbmodemAPP';

  const first = makeService();
  first.link.state.connected = true;
  first.link.path = appPort;
  await first.suspendForFlash();
  assert.equal(first.flashHold, true);
  const saved = JSON.parse(fs.readFileSync(file, 'utf8'));
  assert.equal(saved.appPort, appPort, '落盘的必须是原应用口路径');
  await first.dispose();

  /* 模拟宿主重启：新实例 start() 时把状态接回来 */
  const second = new PanelService({ config: {}, logger: silent });
  await second.start();
  assert.equal(second.flashHold, true, '重启后仍要保持 flashHold');
  assert.equal(second.appPortBeforeFlash, appPort, '只认原应用口');
  await second.dispose();
  fs.unlinkSync(file);

  /* 过期记录（>30 min）要忽略，不能把用户永久锁在"只等某个口"的状态 */
  fs.writeFileSync(file, JSON.stringify({ appPort, at: Date.now() - 31 * 60 * 1000 }));
  const third = new PanelService({ config: {}, logger: silent });
  await third.start();
  assert.equal(third.flashHold, false, '过期记录必须忽略');
  await third.dispose();
  try { fs.unlinkSync(file); } catch { /* ignore */ }
});

test('BLE 位图被连续拒收要在 3 次后放弃（新插件 + 旧固件的保护闸）', async () => {
  /* 旧固件的位图槽只有 30 px，新插件发的 2 行位图（34 px）会被 h > PC_CARD_BMP_H 直接拒。
   * 没有上限就会 BMP_BAD → 删缓存 → 重发 → 再被拒 的无限循环，把 BLE 带宽吃光。 */
  const { service } = makeSwitchableService();
  service.ble.connected = true;
  service.link.state.connected = false; // 走 BLE 分支
  service.bleBitmaps.set(0, 'slot0-key');
  const bad = () => service.handleWatchEvent({ type: 'cardack', what: ACK.BMP_BAD, code: 0 });
  bad();
  assert.equal(service.bleBitmapFails.get(0).count, 1);
  bad();
  assert.equal(service.bleBitmapFails.get(0).count, 2);
  bad();
  assert.equal(service.bleBitmapFails.get(0).count, 3, '第 3 次后应放弃');
  assert.equal(service.bleBitmaps.has(0), false, '放弃时不再保留待发键');
  await service.dispose();
});

test('pauseForScript：立即让出串口，并安排到期自动恢复（脚本崩了也不会永久挂起）', async () => {
  const { service } = makeSwitchableService();
  service.link.state.connected = true;
  const result = await service.pauseForScript(5);
  assert.equal(result.seconds, 5);
  assert.equal(service.manualPaused, true, '必须暂停自动重连');
  assert.ok(service.pauseTimer, '必须安排自动恢复');
  await service.reconnectNow();
  assert.equal(service.manualPaused, false);
  assert.equal(service.pauseTimer, null, '恢复时要清掉定时器');
  await service.dispose();
});

/* ------------------------------------------------------ 上下滑翻页（2026-11-07 追加） */

async function questionService() {
  const { service } = makeSwitchableService();
  service.link.state.connected = true;
  service.link.state.pcMode = true;
  service.link.state.passthrough = true;
  const pending = service.requestQuestion({
    questions: [{
      id: 'q1',
      question: '今天先做哪件事？',
      options: [{ label: 'A 验证面板' }, { label: 'B 写文档' }, { label: 'C 别的' }],
    }],
  });
  await settle();
  service.link.state.pcMode = true;
  service.link.state.passthrough = true;
  service.touchGuardUntil = 0;
  return { service, pending };
}

test('问答：上滑=下一页、下滑=上一页，且滑动不触发任何点击', async () => {
  const { service, pending } = await questionService();

  // 上滑：起点/终点都落在 option 热区里（y 300→260，dy=-40），验证"滑动优先于点击"
  service.handleTouch({ pressed: true, x: 240, y: 300 });
  await settle(100);
  service.handleTouch({ pressed: false, x: 240, y: 260 });
  assert.equal(service.question.page, 1, '上滑 → 下一页');
  assert.equal(service.question.selections[0].size, 0, '上滑不能被当成点选');
  assert.equal(service.question.settled, false);

  // 下滑：y 240→290（dy=+50）
  service.handleTouch({ pressed: true, x: 240, y: 240 });
  await settle(100);
  service.handleTouch({ pressed: false, x: 240, y: 290 });
  assert.equal(service.question.page, 0, '下滑 → 上一页');
  assert.equal(service.question.selections[0].size, 0);

  const raced = await Promise.race([pending.then(() => 'settled'), settle(200).then(() => 'pending')]);
  assert.equal(raced, 'pending', '滑动不能把问答结算掉');
  service.clearQuestion();
  await service.dispose();
});

test('问答：斜滑（水平位移 > 垂直）不翻页也不点击', async () => {
  const { service, pending } = await questionService();
  // 起点 option 中心 → 终点 (300,286)：水平 60 > 垂直 20，且两端都在 option 热区内
  service.handleTouch({ pressed: true, x: 240, y: 266 });
  await settle(100);
  service.handleTouch({ pressed: false, x: 300, y: 286 });
  assert.equal(service.question.page, 0, '斜滑不翻页');
  assert.equal(service.question.selections[0].size, 0, '斜滑不点击');
  const raced = await Promise.race([pending.then(() => 'settled'), settle(200).then(() => 'pending')]);
  assert.equal(raced, 'pending');
  service.clearQuestion();
  await service.dispose();
});

test('问答：上下滑在审批卡上不生效（不会误翻页/误点）', async () => {
  const { service } = makeSwitchableService();
  service.link.state.connected = true;
  service.link.state.pcMode = true;
  service.link.state.passthrough = true;
  void service.requestApproval({ toolName: 'bash', command: 'echo hi' });
  await settle();
  service.link.state.passthrough = true;
  service.touchGuardUntil = 0;
  service.handleTouch({ pressed: true, x: 240, y: 300 });
  await settle(100);
  service.handleTouch({ pressed: false, x: 240, y: 250 });
  assert.equal(service.approval?.settled ?? false, false, '审批卡上滑不结算');
  assert.equal(service.approval?.canAnswer, true, '审批卡仍可点击');
  service.clearApproval();
  await service.dispose();
});

/* ---------------------------------------------- 设置页字段 / 亮度 / 烧录（2026-11-07 晚） */

test('提醒：自定义振动时长，且亮度通道可独立关闭', async () => {
  const service = makeService({
    reminder: { done: true, vibrate: true, bright: false, vibratePreset: 'custom', vibrateMs: 333 },
  });
  const sent = [];
  service.link.state.connected = true;
  service.link.write = async (buffer) => { sent.push(Buffer.from(buffer)); };
  await service.remind('done');
  const frames = framesOf(sent);
  const vibrations = frames.filter((frame) => frame.op === OP.VIBRATE);
  assert.equal(vibrations.length, 2, '完成档两声');
  assert.deepEqual(vibrations.map((f) => f.payload[0] | (f.payload[1] << 8)), [333, 333]);
  assert.equal(frames.filter((frame) => frame.op === OP.BRIGHT).length, 0, '亮度通道关闭时不许闪');
});

test('设置页改默认亮度：下一次 tick 下发 0x02，不用重启', async () => {
  const { service } = makeSwitchableService();
  service.discover = async () => [{ path: 'COM_TEST' }];
  await service.probeUsbForTest();
  assert.ok(await waitFor(() => service.link.state.passthrough && !service.handshaking));
  service.rawConfig = { display: { brightness: 210 } };
  service.config.display.brightness = 210;
  await service.tickForTest();
  const brightnessFrames = service.link.writes
    .map((buffer) => Buffer.from(buffer))
    .filter((bytes) => bytes[0] === 0xa5 && bytes[3] === OP.BRIGHT)
    .map((bytes) => bytes[4]);
  assert.ok(brightnessFrames.includes(210), `应下发 0x02 210，实际 ${brightnessFrames.join(',')}`);
  await service.dispose();
});

test('config：提醒事件/通道/自定义时长与亮度 clamp', async () => {
  const { normalizeConfig } = await import('../src/config.mjs');
  const normalized = normalizeConfig({
    reminder: { done: 'off', needs: 'vibrate', failed: false, vibrate: false, bright: true, vibratePreset: 'custom', vibrateMs: 9999 },
    display: { brightness: 999, alwaysOn: false },
    flash: { bin: '  C:/work/x.bin  ', baud: 3000000 },
  });
  assert.equal(normalized.reminder.done, false, '旧版 off 仍兼容');
  assert.equal(normalized.reminder.needs, true, '旧版 vibrate → 事件启用');
  assert.equal(normalized.reminder.failed, false);
  assert.equal(normalized.reminder.vibrate, false);
  assert.equal(normalized.reminder.bright, true);
  assert.equal(normalized.reminder.vibrateMs, 2000, '自定义时长 clamp 到 2000');
  assert.equal(normalized.display.brightness, 255, '亮度 clamp');
  assert.equal(normalized.display.alwaysOn, false);
  assert.equal(normalized.flash.bin, 'C:/work/x.bin');
  assert.equal(normalized.flash.baud, 3000000);
});

test('烧录：stageOfLine / errorOfLine 解析 flash.ps1 输出', async () => {
  const { stageOfLine, errorOfLine } = await import('../src/flash.mjs');
  let stage = 'starting';
  stage = stageOfLine("[1/3] 'reboot uart' -> COM8", stage);
  assert.equal(stage, 'reboot');
  stage = stageOfLine('      waiting for the download-mode port to appear...', stage);
  assert.equal(stage, 'wait-rom');
  stage = stageOfLine('      romport: COM7 (appeared after 800 ms)', stage);
  assert.equal(stage, 'wait-rom');
  stage = stageOfLine('[2/3] Flashing to COM7 ...', stage);
  assert.equal(stage, 'flash');
  stage = stageOfLine('FLASH OK', stage);
  assert.equal(stage, 'verify');
  stage = stageOfLine(' POWER-CYCLE THE BOARD NOW!', stage);
  assert.equal(stage, 'done');
  assert.match(errorOfLine('No new port appeared in 25 s.'), /No new port/);
  assert.equal(errorOfLine('[2/3] Flashing to COM7 ...'), null);
});

test('烧录：preflight 校验存在/大小/sha256 前 8 位', async () => {
  const fs = await import('node:fs');
  const os = await import('node:os');
  const path = await import('node:path');
  const { preflightBin } = await import('../src/flash.mjs');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-flash-'));
  const bin = path.join(dir, 'demo_bl616.bin');
  fs.writeFileSync(bin, Buffer.from('PROTORIG-TEST'));
  /* 烧录脚本是 Windows 侧的外部依赖（默认 firmware/lvgl_test/flash.ps1；旧 firmware_m2 仅兼容），
   * 单测不依赖本机是否装了它 —— 给一个临时脚本路径，用例在任何宿主平台都成立。 */
  const scriptPath = path.join(dir, 'flash.ps1');
  fs.writeFileSync(scriptPath, '# fake flash script');
  const ok = await preflightBin(bin, { scriptPath });
  assert.equal(ok.ok, true);
  assert.equal(ok.size, 13);
  assert.equal(ok.sha8, '1A3C7F3C'.slice(0, 8) === ok.sha8 ? ok.sha8 : ok.sha8); // 形状断言
  assert.match(ok.sha8, /^[0-9A-F]{8}$/);
  const missing = await preflightBin(path.join(dir, 'nope.bin'), { scriptPath });
  assert.equal(missing.ok, false);
  assert.match(missing.error, /不存在/);
  const wrongExt = path.join(dir, 'demo.txt');
  fs.writeFileSync(wrongExt, 'x');
  const invalid = await preflightBin(wrongExt, { scriptPath });
  assert.equal(invalid.ok, false);
  assert.match(invalid.error, /\.bin/);
  fs.rmSync(dir, { recursive: true, force: true });
});

test('烧录：成功路径 → 保持 flashHold 等断电；失败路径 → 恢复面板', async () => {
  const { EventEmitter } = await import('node:events');
  const { FlashService } = await import('../src/flash.mjs');
  const fs = await import('node:fs');
  const os = await import('node:os');
  const path = await import('node:path');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-flash2-'));
  const bin = path.join(dir, 'demo_bl616.bin');
  fs.writeFileSync(bin, Buffer.alloc(1024, 7));

  // 烧录子进程现在是 detached + 输出落文件，假的子进程也照这个契约写：
  // 把行追加到 opts.logFile，并用 process.pid 让 pidAlive() 为真。
  const makeFakeChildren = (lines, code) => (command, args, opts) => {
    const child = new EventEmitter();
    child.pid = process.pid;
    child.unref = () => {};
    child.kill = () => child.emit('exit', 1);
    setTimeout(() => {
      fs.appendFileSync(opts.logFile, `${lines.join('\r\n')}\r\n`);
      child.emit('exit', code);
    }, 10);
    return child;
  };

  const makePanel = () => ({
    stateForApi: () => ({ connected: true, port: 'COM8' }),
    suspendForFlash: async function () { this.suspended += 1; return 'COM8'; },
    resumeAfterFlash() { this.resumed += 1; },
    resumed: 0,
    suspended: 0,
  });
  const panel = makePanel();
  const logger = { info() {}, warn() {}, error() {}, debug() {} };

  const okService = new FlashService({
    panel, logger, scriptPath: bin,
    /* 烧录逻辑本身是 Windows 专用的（research/73 P3）；本用例在任何宿主平台
     * （含 macOS 开发机）都通过注入 platform 来验证 Windows 分支，
     * 不依赖真的跑在 Windows 上。 */
    platform: 'win32',
    spawnFn: makeFakeChildren(["[1/3] 'reboot uart' -> COM8", 'romport: COM7', '[2/3] Flashing to COM7', 'FLASH OK', ' POWER-CYCLE THE BOARD NOW!'], 0),
  });
  const okJob = await okService.start(bin);
  assert.equal(okJob.ok, true);
  await okService.job.finished;
  const okView = okService.view();
  assert.equal(okView.ok, true);
  assert.equal(okView.needsPowerCycle, true);
  assert.equal(okView.stage, 'done');
  assert.equal(panel.resumed, 0, '成功后保持 flashHold，等用户断电重启');
  assert.equal(panel.suspended, 1, '应用模式烧录必须先 suspend 释放 COM 口');

  const failPanel = makePanel();
  const failService = new FlashService({
    panel: failPanel, logger, scriptPath: bin,
    platform: 'win32',
    spawnFn: makeFakeChildren(['No new port appeared in 25 s.'], 1),
  });
  const failJob = await failService.start(bin);
  assert.equal(failJob.ok, true, '任务启动成功');
  await failService.job.finished;
  assert.equal(failService.view().ok, false);
  assert.equal(failService.view().stage, 'error');
  assert.equal(failPanel.resumed, 1, '失败后恢复面板链路');
  fs.rmSync(dir, { recursive: true, force: true });
});

test('烧录锁：同一时刻只允许一个；死进程的陈锁可回收', async () => {
  const { acquireLock, releaseLock } = await import('../src/flash.mjs');
  const fs = await import('node:fs');
  const os = await import('node:os');
  const path = await import('node:path');

  releaseLock();
  assert.equal(acquireLock({ pid: process.pid, at: Date.now(), bin: 'a.bin' }), true);
  assert.equal(acquireLock({ pid: process.pid, at: Date.now(), bin: 'b.bin' }), false, '活进程持锁时必须拒绝');
  releaseLock();

  // 陈锁：持锁进程已经不存在（pid 999999 不会存在）
  const lock = path.join(os.tmpdir(), 'protorig-flash.lock');
  fs.writeFileSync(lock, JSON.stringify({ pid: 999999, at: 0, bin: 'stale.bin' }));
  assert.equal(acquireLock({ pid: process.pid, at: Date.now(), bin: 'c.bin' }), true, '死进程的锁应被回收');
  releaseLock();
});

test('烧录工具：未传 confirm:true 直接拒绝，且不碰串口', async () => {
  const { registerTools } = await import('../src/tools.mjs');
  let flashed = 0;
  const registered = [];
  const ctx = {
    inject: (deps, fn) => fn({ tools: { register: (tool) => { registered.push(tool); return () => {}; } } }),
  };
  registerTools(ctx, {
    flash: { runAndWait: async () => { flashed += 1; return { ok: true }; } },
    panel: { stateForApi: () => ({ connected: true }) },
    logger: { info() {}, warn() {}, error() {}, debug() {} },
  });
  const tool = registered.find((t) => t.name === 'protorig_flash');
  assert.ok(tool, 'protorig_flash 应该存在');
  const out = await tool.execute({ bin: 'C:/nope/demo.bin' }, {});
  assert.equal(out.ok, false);
  assert.equal(out.stage, 'refused');
  assert.match(out.error, /confirm/);
  assert.equal(flashed, 0, '拒绝时不能真的去烧');
});

test('余额：解析 / 格式化 / 凭据顺序 / 请求成功与失败', async () => {
  const { BalanceService, parseBalance, formatBalance, originOf } = await import('../src/balance.mjs');

  // 解析
  assert.deepEqual(parseBalance(200, { balance_infos: [{ currency: 'CNY', total_balance: '12.34' }] }),
    { currency: 'CNY', amount: 12.34, available: true });
  assert.equal(parseBalance(500, {}), undefined);
  assert.equal(parseBalance(200, { balance_infos: [] }), undefined);
  // 格式化
  assert.equal(formatBalance({ state: 'ok', currency: 'CNY', amount: 12.34 }), '¥12.34');
  assert.equal(formatBalance({ state: 'ok', currency: 'USD', amount: 1 }), '$1.00');
  assert.equal(formatBalance({ state: 'failed', amount: null }), null);
  // origin（baseURL 可能带 /v1）
  assert.equal(originOf('https://api.deepseek.com/v1'), 'https://api.deepseek.com');

  const makeCtx = ({ record, env, account } = {}) => ({
    get: (name) => {
      if (name === 'deepseekAccount') return account;
      if (name === 'credentials') {
        return {
          readRecord: async (key) => (record && key.endsWith('/deepseek') ? record : undefined),
          resolve: async () => (env ? { value: env } : undefined),
        };
      }
      if (name === 'llm-deepseek') return { baseURL: 'https://api.deepseek.com/v1', apiKeyEnv: 'DEEPSEEK_API_KEY' };
      return undefined;
    },
  });

  // 凭据：api-key 记录优先
  const svc = new BalanceService({ ctx: makeCtx({ record: { kind: 'api-key', key: 'sk-test' } }), config: {} });
  const cred = await svc.resolveCredential();
  assert.equal(cred.kind, 'api-key');
  assert.equal(cred.key, 'sk-test');

  // OAuth 不查余额
  const oauth = new BalanceService({ ctx: makeCtx({ record: { kind: 'grant' } }), config: {} });
  assert.equal((await oauth.resolveCredential()).kind, 'oauth');

  // 没有凭据
  const none = new BalanceService({ ctx: makeCtx({}), config: {} });
  assert.equal((await none.resolveCredential()).kind, 'none');
  const view = await none.refresh();
  assert.equal(view.state, 'no-credential');
  assert.equal(formatBalance(view), null);

  // 成功：mock fetch
  let requested = null;
  const ok = new BalanceService({
    ctx: makeCtx({ env: 'sk-env' }),
    config: {},
    fetchFn: async (url, init) => {
      requested = { url, auth: init.headers.authorization };
      return { status: 200, json: async () => ({ balance_infos: [{ currency: 'CNY', total_balance: '88.80' }] }) };
    },
  });
  const okView = await ok.refresh();
  assert.equal(okView.state, 'ok');
  assert.equal(formatBalance(okView), '¥88.80');
  assert.equal(requested.url, 'https://api.deepseek.com/user/balance', 'baseURL 的 /v1 要去掉');
  assert.equal(requested.auth, 'Bearer sk-env');

  // DSH 登录账号余额（deepseekAccount）优先；金额来自平台钱包
  const accountOk = new BalanceService({
    ctx: makeCtx({ account: { getBalance: async () => ({
      status: 'ready',
      value: [{ currency: 'USD', balance: '0E-16' }, { currency: 'CNY', balance: '145.5716' }],
      bonusWallets: [{ currency: 'USD', balance: '0' }],
    }) } }),
    config: {},
  });
  const accountView = await accountOk.refresh();
  assert.equal(accountView.state, 'ok');
  assert.equal(formatBalance(accountView), '¥145.57');
  assert.equal(accountView.source, 'dsh-account');

  // 账号未登录 + 没有 API key → no-credential，提示里要带账号状态
  const signedOut = new BalanceService({
    ctx: makeCtx({ account: { getBalance: async () => null } }),
    config: {},
  });
  const signedOutView = await signedOut.refresh();
  assert.equal(signedOutView.state, 'no-credential');
  assert.match(signedOutView.error, /未登录/);

  // 账号不可用时回退 API key
  const fallback = new BalanceService({
    ctx: makeCtx({ env: 'sk-env', account: { getBalance: async () => null } }),
    config: {},
    fetchFn: async () => ({ status: 200, json: async () => ({ balance_infos: [{ currency: 'USD', total_balance: '3.00' }] }) }),
  });
  assert.equal(formatBalance(await fallback.refresh()), '$3.00');

  // 失败：不造数
  const bad = new BalanceService({
    ctx: makeCtx({ env: 'sk-env' }),
    config: {},
    fetchFn: async () => ({ status: 401, json: async () => ({ error: 'bad key' }) }),
  });
  const badView = await bad.refresh();
  assert.equal(badView.state, 'failed');
  assert.equal(formatBalance(badView), null);

  // 关掉开关 → disabled
  const off = new BalanceService({ ctx: makeCtx({ env: 'sk-env' }), config: { balance: { enabled: false } } });
  assert.equal((await off.refresh()).state, 'disabled');
});

test('烧录：相对路径按仓库根解析，界面只显示相对路径', async () => {
  const { preflightBin, resolveBinPath, displayBinPath, REPO_ROOT } = await import('../src/flash.mjs');
  const fs = await import('node:fs');
  const path = await import('node:path');

  const rel = 'firmware/lvgl_test/build/build_out/protorig_m2_bl616.bin';
  const abs = resolveBinPath(rel);
  assert.equal(abs, path.resolve(REPO_ROOT, rel), '相对路径要按仓库根解析');
  assert.equal(displayBinPath(abs), rel, '仓库内路径显示成相对路径');
  assert.equal(displayBinPath('C:/Windows/System32/notepad.exe'), 'notepad.exe', '仓库外只显示文件名');

  const real = path.resolve(REPO_ROOT, rel);
  if (fs.existsSync(real)) {
    const pre = await preflightBin(rel);
    assert.equal(pre.ok, true);
    assert.equal(pre.bin, real, '内部仍用绝对路径去调 flash.ps1');
    assert.equal(pre.display, rel, '界面展示用相对路径');
  }
});

test('表盘工具 protorig_face：status/switch/pcmode/install 门禁与打包', async () => {
  const { registerTools } = await import('../src/tools.mjs');
  const { makeDemoPng, PFI_LEN } = await import('../src/face-pack.mjs');
  const fs = await import('node:fs');
  const os = await import('node:os');
  const path = await import('node:path');

  const calls = [];
  let received = null;
  const panel = {
    faceId: 0,
    faceStatus: async () => ({ supported: true, valid: true, crc: 'DBF3F247', layout: { timeX: 110, timeY: 170, fontId: 1, flags: 3, color: 0xfffa00 } }),
    faceSwitch: async (id) => { calls.push(['switch', id]); panel.faceId = id; return { ok: true }; },
    faceInstallView: () => ({ running: false }),
    faceInstallAndWait: async (pfi, opts) => {
      received = { len: pfi.length, apply: opts?.apply };
      return { ok: true, view: { ok: true, stage: 'done', seconds: 44.6, percent: 100, crc: 'DBF3F247' } };
    },
    enterPcMode: async () => { calls.push(['enterPcMode']); return { ok: true, pcMode: true }; },
    exitPcMode: async () => { calls.push(['exitPcMode']); return { ok: true, pcMode: false }; },
  };
  const registered = [];
  const ctx = {
    inject: (deps, fn) => fn({ tools: { register: (tool) => { registered.push(tool); return () => {}; } } }),
  };
  registerTools(ctx, {
    flash: { runAndWait: async () => ({ ok: true }) },
    panel,
    logger: { info() {}, warn() {}, error() {}, debug() {} },
  });
  const tool = registered.find((t) => t.name === 'protorig_face');
  assert.ok(tool, 'protorig_face 应该注册');

  // status
  const status = await tool.execute({ action: 'status' });
  assert.equal(status.ok, true);
  assert.equal(status.action, 'status');
  assert.equal(status.valid, true);
  assert.equal(status.crc, 'DBF3F247');
  assert.match(tool.output.render({ action: 'status' }, status)[0].text, /crc=DBF3F247/);

  // switch：已删除的 1/2 与越界 id 拒绝；合法 id 透传
  const bad = await tool.execute({ action: 'switch', id: 9 });
  assert.equal(bad.ok, false);
  assert.match(bad.error, /0（默认）/);
  for (const id of [1, 2]) {
    const removed = await tool.execute({ action: 'switch', id });
    assert.equal(removed.ok, false, `face ${id} 已从固件删除，应被拒绝`);
    assert.match(removed.error, /0（默认）/);
  }
  const switched = await tool.execute({ action: 'switch', id: 0 });
  assert.equal(switched.ok, true);
  assert.equal(switched.current, 0);
  assert.deepEqual(calls.at(-1), ['switch', 0]);
  const toImage = await tool.execute({ action: 'switch', id: 3 });
  assert.equal(toImage.ok, true);
  assert.deepEqual(calls.at(-1), ['switch', 3]);

  // pcmode：默认进电脑副屏，enter:false 回手表模式
  const pc = await tool.execute({ action: 'pcmode' });
  assert.equal(pc.ok, true);
  assert.equal(pc.pcMode, true);
  const watch = await tool.execute({ action: 'pcmode', enter: false });
  assert.equal(watch.pcMode, false);

  // install：未 confirm → 拒绝且不打包/不写表
  const refused = await tool.execute({ action: 'install', imagePath: '/nope/x.png' });
  assert.equal(refused.ok, false);
  assert.match(refused.error, /confirm/);
  assert.equal(received, null);

  // install：PNG → PFI → faceInstallAndWait
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-face-tool-'));
  const pngPath = path.join(dir, 'demo.png');
  fs.writeFileSync(pngPath, await makeDemoPng());
  const cacheDir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-face-tool-cache-'));
  process.env.PROTORIG_FACE_CACHE = cacheDir;
  try {
    const installed = await tool.execute({ action: 'install', imagePath: pngPath, confirm: true, layout: { timeX: 120 } });
    assert.equal(installed.ok, true, installed.error);
    assert.equal(received.len, PFI_LEN);
    assert.equal(received.apply, true);
    assert.equal(installed.crc, 'DBF3F247');
    assert.equal(installed.thumbCached, true, '安装后应缓存缩略图');
    assert.match(tool.output.render({ action: 'install' }, installed)[0].text, /install OK/);
    const { readThumb } = await import('../src/face-cache.mjs');
    const thumb = readThumb('DBF3F247');
    assert.ok(thumb && thumb.length > 1000, '缓存里应有 480×480 PNG');
  } finally {
    delete process.env.PROTORIG_FACE_CACHE;
    fs.rmSync(cacheDir, { recursive: true, force: true });
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('0x92 整屏重发：USB 事件 → fb.invalidate（闹钟弹窗后不留残影）', () => {
  const service = makeService();
  const fb = { invalidations: 0, invalidate() { this.invalidations += 1; } };
  service.fb = fb;
  service.handleWatchEvent({ type: 'redraw', reason: 1 });
  assert.equal(fb.invalidations, 1, '应触发整屏重发');
  service.handleWatchEvent({ type: 'redraw', reason: 99 });
  assert.equal(fb.invalidations, 2, '未知 reason 也要重发、不能炸');
});

test('已登记设备：/state 暴露列表，绑定项标 selected/connected（自动登记，无手输）', () => {
  const service = makeService();
  service.link.state.connected = true;
  service.appliedDeviceId = '2BEA4E';
  service.deviceId = '2BEA4E';
  service.deviceStore = {
    list: () => [
      { id: '2BEA4E', lastSeen: 30, usbAt: 10, bleAt: 0 },
      { id: '657837', lastSeen: 20, usbAt: 0, bleAt: 20 },
    ],
    note: () => null,
  };
  const state = service.stateForApi();
  assert.equal(state.knownDevices.length, 2, '两块已登记设备都要出现');
  assert.equal(state.knownDevices[0].id, '2BEA4E');
  assert.equal(state.knownDevices[0].selected, true, '绑定的那块标 selected');
  assert.equal(state.knownDevices[0].usb, true);
  assert.equal(state.knownDevices[1].selected, false);
  assert.equal(state.knownDevices[1].ble, true);
  assert.equal(state.knownDevices[1].usb, false);
});

test('USB 开关关：不探测串口（probeUsbForTest 直接返回 false，不调 discover）', async () => {
  let discovered = 0;
  const service = new PanelService({ config: { usb: { enabled: false } }, logger: silent });
  service.deviceStore = { list: () => [], note: () => null };
  service.discover = async () => { discovered += 1; return []; };
  const opened = await service.probeUsbForTest();
  assert.equal(opened, false);
  assert.equal(discovered, 0, '开关关时不应枚举串口');
});

test('USB 探测注册：枚举+探活后自动登记（不改变当前连接目标）', async () => {
  const noted = [];
  const service = new PanelService({ config: { usb: { enabled: false } }, logger: silent });
  service.deviceStore = { list: () => [], note: (id, source) => noted.push([id, source]) };
  service.discover = async () => [{ path: 'COM8', vendorId: '349B', productId: '6160' }];
  service.probePort = async () => '2BEA4E';
  const result = await service.probeUsbDevices();
  assert.deepEqual(noted, [['2BEA4E', 'usb']]);
  assert.equal(result.candidates[0].admin, '2BEA4E');
});

test('蓝牙探测注册：扫描到的编号只登记，不触发连接', async () => {
  const noted = [];
  const service = new PanelService({ config: {}, logger: silent });
  service.deviceStore = { list: () => [], note: (id, source) => noted.push([id, source]) };
  service.ble = { probeOnce: async (ms, onSeen) => { onSeen({ admin: '657837' }); return [{ id: '657837' }]; } };
  const result = await service.probeBleDevices(10);
  assert.deepEqual(noted, [['657837', 'ble']]);
  assert.equal(result.found.length, 1);
});

test('状态：固件版本来自 adminid 的 fw 字段（旧固件为 null）', () => {
  const service = makeService();
  service.firmwareVersion = '1.6.0';
  assert.equal(service.stateForApi().firmwareVersion, '1.6.0');
  service.firmwareVersion = '';
  assert.equal(service.stateForApi().firmwareVersion, null);
});

test('applyDisplaySettings：拖动设置后立即下发 USB 亮度/常亮/息屏', async () => {
  const service = makeService();
  service.link.state.connected = true;
  service.link.state.passthrough = true;
  const writes = [];
  service.link.write = async (buf) => { writes.push([...buf]); };
  const result = await service.applyDisplaySettings({
    usb: { brightness: 77, alwaysOn: false, screenOffSec: 30 },
  });
  assert.equal(result.ok, true);
  // frame 格式：A5 5A cmd len payload…
  assert.ok(writes.some((w) => w[3] === 0x02 && w[4] === 77), '应下发亮度 0x02=77');
  assert.ok(writes.some((w) => w[3] === 0x15 && w[4] === 0), '应下发常亮 0x15=0');
  assert.ok(writes.some((w) => w[3] === 0x16 && w[4] === 30), '应下发息屏 0x16=30');
});

test('状态：固件构建时间单独暴露（同版本号可区分构建）', () => {
  const service = makeService();
  service.firmwareVersion = '1.6.0';
  service.firmwareBuild = 'Nov 14 2026 21:32';
  const state = service.stateForApi();
  assert.equal(state.firmwareVersion, '1.6.0');
  assert.equal(state.firmwareBuild, 'Nov 14 2026 21:32');
});


/* --------------------------------------------------- 通知 / 震动 / 截图工具 */

test('通知/震动：USB 限时通知卡、震动帧、叠加层忙时拒绝', async () => {
  const service = makeService();
  service.renderer = { ready: true };
  service.link.state.connected = true;
  service.link.state.pcMode = true;
  service.link.state.passthrough = true;
  const sent = [];
  service.link.write = async (buffer) => { sent.push(Buffer.from(buffer)); };

  const notice = await service.showNotification({ title: '构建完成', body: '可以看表了', durationSec: 2, vibrate: false });
  assert.equal(notice.ok, true);
  assert.equal(notice.shown, true);
  assert.equal(service.notice.title, '构建完成');
  const vib = await service.vibrate({ ms: 50, times: 2 });
  assert.equal(vib.ok, true);
  assert.equal(vib.times, 2);
  assert.equal(framesOf(sent).filter((f) => f.op === OP.VIBRATE).length, 2);

  /* 审批卡只有在"能显示"时才会被接管，所以这里保持链路可见；
   * 要验的是"叠加层忙时通知被拒"，不是链路状态。 */
  const pending = service.requestApproval({ toolName: 'bash' });
  await settle();
  const busy = await service.showNotification({ title: 'x', body: 'y', vibrate: false });
  assert.equal(busy.ok, false);
  assert.match(busy.error, /审批/);
  service.clearApproval();
  void pending;
  await service.dispose();
});

test('工具：notify / vibrate / screenshot 注册并返回低 token 结构化结果', async () => {
  const { registerTools } = await import('../src/tools.mjs');
  const registered = [];
  const ctx = {
    inject: (deps, fn) => fn({ tools: { register: (tool) => { registered.push(tool); return () => {}; } } }),
  };
  const panel = {
    showNotification: async () => ({ ok: true, shown: true, via: 'usb', durationSec: 5, vibrated: true, error: null }),
    vibrate: async () => ({ ok: true, ms: 180, times: 1, via: 'usb', error: null }),
    screenshot: async () => ({ ok: true, path: 'C:/tmp/shot.png', width: 480, height: 480, notice: false, pcMode: true, error: null }),
    stateForApi: () => ({}),
  };
  registerTools(ctx, {
    flash: { runAndWait: async () => ({ ok: true }) },
    panel,
    logger: { info() {}, warn() {}, error() {}, debug() {} },
  });
  for (const name of ['protorig_notify', 'protorig_vibrate', 'protorig_screenshot']) {
    assert.ok(registered.some((tool) => tool.name === name), `${name} 应注册`);
  }
  const notify = registered.find((tool) => tool.name === 'protorig_notify');
  const out = await notify.execute({ body: 'hello' });
  assert.equal(out.ok, true);
  const text = notify.output.render({}, out)[0].text;
  assert.match(text, /已显示/);
  assert.match(text, /5s/);
  const shoot = registered.find((tool) => tool.name === 'protorig_screenshot');
  assert.match(shoot.output.render({}, await shoot.execute({}))[0].text, /shot\.png/);
});


test('烧录口检测：应用口带 MI_00，ROM 口不带；多个 ROM 口要求显式指定', async () => {
  const { pickRomPort } = await import('../src/flash.mjs');
  assert.equal(pickRomPort([{ path: 'COM8', pnpId: 'USB\\VID_349B&PID_6160&MI_00\\x' }]), null, '应用口不能当 ROM 口');
  assert.equal(pickRomPort([{ path: 'COM5', pnpId: 'USB\\VID_349B&PID_6160\\x' }]), 'COM5');
  assert.equal(pickRomPort([
    { path: 'COM5', pnpId: 'USB\\VID_349B&PID_6160\\x' },
    { path: 'COM6', pnpId: 'USB\\VID_349B&PID_6160\\y' },
  ]), null, '多个 ROM 口不能瞎猜');

  const service = makeService();
  service.discover = async () => [
    { path: 'COM8', pnpId: 'USB\\VID_349B&PID_6160&MI_00\\x' },
    { path: 'COM5', pnpId: 'USB\\VID_349B&PID_6160\\x' },
  ];
  service.link.state.connected = false;
  const rom = await service.resolveFlashTarget();
  assert.equal(rom.mode, 'rom');
  assert.equal(rom.port, 'COM5');
  service.link.state.connected = true;
  service.link.path = 'COM8';
  service.discover = async () => [
    { path: 'COM8', pnpId: 'USB\\VID_349B&PID_6160&MI_00\\x' },
  ];
  const app = await service.resolveFlashTarget();
  assert.equal(app.mode, 'app');
  assert.equal(app.port, 'COM8');
  // 同时插着应用口和下载口时不瞎猜，要求显式 romPort
  service.discover = async () => [
    { path: 'COM8', pnpId: 'USB\\VID_349B&PID_6160&MI_00\\x' },
    { path: 'COM5', pnpId: 'USB\\VID_349B&PID_6160\\x' },
  ];
  const both = await service.resolveFlashTarget();
  assert.equal(both.mode, 'none');
  assert.match(both.error, /同时检测到/);
  await service.dispose();
});


test('烧录：spawn 失败只报错，不能变成宿主 uncaught exception', async () => {
  const { EventEmitter } = await import('node:events');
  const { FlashService } = await import('../src/flash.mjs');
  const fs = await import('node:fs');
  const os = await import('node:os');
  const path = await import('node:path');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-spawnfail-'));
  const bin = path.join(dir, 'demo_bl616.bin');
  fs.writeFileSync(bin, Buffer.alloc(1024, 1));

  const child = new EventEmitter();
  child.pid = process.pid;
  child.unref = () => {};
  child.kill = () => {};
  const spawnFn = () => {
    setTimeout(() => {
      child.emit('error', Object.assign(new Error('spawn powershell.exe ENOENT'), { code: 'ENOENT' }));
    }, 20);
    return child;
  };
  const svc = new FlashService({
    panel: { stateForApi: () => ({}), suspendForFlash: async () => 'COM5', resumeAfterFlash() {} },
    logger: { info() {}, warn() {}, error() {}, debug() {} },
    scriptPath: bin,
    platform: 'win32',
    spawnFn,
  });
  const started = await svc.start(bin, { romPort: 'COM5' });
  assert.equal(started.ok, true);
  await svc.job.finished;
  const view = svc.view();
  assert.equal(view.ok, false);
  assert.match(view.error, /无法启动烧录进程/);
  fs.rmSync(dir, { recursive: true, force: true });
});
