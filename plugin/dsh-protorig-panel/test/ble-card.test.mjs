/*
 * ble-card.test.mjs —— BLE 卡片层（P1..P4）单测
 *
 * 覆盖三件在真机上最容易翻车的事：
 *   1. notify 帧解析（A5 5A cmd len payload）—— 之前插件把 evt 整条丢掉；
 *   2. 卡片载荷/位图分片的字节布局与上限——固件按死长度解析，多一位就丢帧；
 *   3. BLE 下的触摸命中（没有 USB 链路）——判定必须走 USB 同一套 hit-test 与叠加层。
 */
import './_env.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { PanelService } from '../src/panel-service.mjs';
import {
  ACK,
  CMD,
  OP,
  SYNC0,
  SYNC1,
  cardApproval,
  cardBitmapChunks,
  cardClear,
  cardQuestion,
  cardSession,
  decodeFrame,
  parseBleFrame,
  utf8Clamp,
} from '../src/protocol.mjs';
import { CardBitmapRenderer } from '../src/card-bitmap.mjs';

const silent = { info() {}, warn() {}, error() {}, debug() {} };
const settle = (ms = 20) => new Promise((resolve) => setTimeout(resolve, ms));

/** 与固件 pc_monitor.c 的版式一致（热区自报没到之前的兜底，应当是同一组数）。 */
const BLE_CENTER = {
  deny: { x: 168, y: 358 },
  allow: { x: 312, y: 358 },
  option: { x: 240, y: 305 },
  next: { x: 168, y: 359 },
  submit: { x: 312, y: 359 },
};

function bleFrame(cmd, payload) {
  return Buffer.from([SYNC0, SYNC1, cmd, payload.length, ...payload]);
}

function makeBleService() {
  const service = new PanelService({ config: {}, logger: silent });
  service.tokens = { layout: 'monitor-v4', hotzones: {} };
  service.ble.start = async () => {};
  service.ble.stop = () => {};
  service.discover = async () => [];
  service.config.reconnectDelayMs = 60000;
  // 模拟"USB 不在、BLE 在线"：判定逻辑不能依赖 USB 链路
  service.ble.connected = true;
  service.link.state.connected = false;
  service.cardBitmap = { ready: false, render: () => null };
  const sent = [];
  service.ble.sendPcFrame = (payload) => {
    sent.push(Buffer.from(payload));
    return true;
  };
  return { service, sent };
}

async function bleTap(service, zone, holdMs = 100) {
  service.touchGuardUntil = 0;
  const point = BLE_CENTER[zone];
  service.handleTouch({ pressed: true, x: point.x, y: point.y });
  await settle(holdMs);
  service.handleTouch({ pressed: false, x: point.x, y: point.y });
  await settle(10);
}

const ops = (sent) => sent.map((payload) => payload[0]);

/* ------------------------------------------------------------ 帧解析 */

test('BLE notify：触摸/状态/热区/回执都有解析，半截帧返回 null', () => {
  const touch = parseBleFrame(bleFrame(0x50, [0x04, 1, 0x70, 0x01, 0x26, 0x01]));
  assert.deepEqual(touch, { type: 'touch', pressed: true, x: 368, y: 294 });

  const status = parseBleFrame(bleFrame(0x91, [2, 88, 200, 4]));
  assert.deepEqual(status, { type: 'status', link: 2, battery: 88, bright: 200, page: 4 });

  const zones = parseBleFrame(bleFrame(0x58, [2, 2, 1, 104, 0, 322, 1, 128, 0, 72, 0, 2, 248, 0, 322, 1, 128, 0, 72, 0]));
  assert.equal(zones.type, 'zones');
  assert.equal(zones.kind, 2);
  assert.deepEqual(zones.list.map((z) => z.name), ['deny', 'allow']);
  assert.deepEqual([zones.list[0].x, zones.list[0].y, zones.list[0].w, zones.list[0].h], [104, 322, 128, 72]);

  const ack = parseBleFrame(bleFrame(0x59, [ACK.BMP_OK, 0]));
  assert.deepEqual(ack, { type: 'cardack', what: ACK.BMP_OK, code: 0 });

  assert.equal(parseBleFrame(Buffer.from('a55a5006', 'hex')), null, '长度不足要丢弃');
  assert.equal(parseBleFrame(Buffer.from('00000000', 'hex')), null, '帧头不对要丢弃');
  assert.equal(parseBleFrame(Buffer.from('a55a58', 'hex')), null, '只剩帧头要丢弃');
  assert.equal(decodeFrame(0x77, [1]).type, 'frame');
});

/* ------------------------------------------------------------ 载荷布局 */

test('卡片载荷：字段顺序/上限与固件解析一致', () => {
  const session = cardSession({ state: 1, steps: 3, ctx: 42, tool: 'bash', title: 'BLE 对齐 USB', footer: '电量 97%  ·  ¥91.38' });
  assert.equal(session[0], OP.SESSION);
  assert.equal(session[1], 1);
  assert.equal(session[2], 3);
  assert.equal(session[3], 42);
  assert.equal(session[4], 4, 'tool 长度前缀');
  assert.equal(Buffer.from(session.subarray(5, 9)).toString(), 'bash');
  const titleLen = session[9];
  assert.equal(Buffer.from(session.subarray(10, 10 + titleLen)).toString(), 'BLE 对齐 USB');
  const footAt = 10 + titleLen;
  const footLen = session[footAt];
  assert.equal(Buffer.from(session.subarray(footAt + 1, footAt + 1 + footLen)).toString(), '电量 97%  ·  ¥91.38');
  assert.ok(session.length <= 240);

  const noCtx = cardSession({ state: 0, steps: 0, ctx: -1, tool: '', title: '' });
  assert.equal(noCtx[3], 0xff, 'ctx=-1 用 0xFF 表示未知');

  const tooLong = cardSession({ title: '很'.repeat(200), tool: 'x'.repeat(40) });
  assert.ok(tooLong.length <= 240, '超长文本必须被截断');
  assert.equal(tooLong[4], 11, 'tool 上限 11 字节');

  const approval = cardApproval({ canAllow: 1, computer: 0, countdown: 9, tool: 'bash', command: 'rm -rf build' });
  assert.deepEqual([...approval.subarray(0, 4)], [OP.APPROVAL, 1, 0, 9]);
  assert.equal(Buffer.from(approval.subarray(5, 9)).toString(), 'bash');

  const question = cardQuestion({
    index: 1, count: 3, multi: 1, selected: [0, 2], page: 2, optionCount: 5, prompt: '选哪个', option: 'A',
  });
  assert.deepEqual([...question.subarray(0, 8)], [OP.QUESTION, 1, 3, 1, 0b101, 0, 2, 5]);
  assert.equal(question[8], 9, 'prompt 长度前缀（中文 3 字 9 字节）');

  assert.deepEqual([...cardClear()], [OP.CARD_CLEAR]);
});

test('位图分片：头 9 B（含 bpp）+ 数据 ≤231 B，总数/序号从 0 连续', () => {
  const width = 296;
  const height = 30;
  // 2-bit（4 级灰）：每像素 2 bit
  const bits = Buffer.alloc((width * 2 / 8) * height, 0x5a);
  const chunks = [...cardBitmapChunks(0, { w: width, h: height, bpp: 2, bits })];
  assert.equal(chunks.length, Math.ceil(bits.length / 231));
  chunks.forEach((payload, index) => {
    assert.ok(payload.length <= 240, '单片不得超过固件 240 B 上限');
    assert.equal(payload[0], OP.CARD_BMP);
    assert.equal(payload[1], 0, 'slot');
    assert.equal(payload[2] | (payload[3] << 8), width);
    assert.equal(payload[4] | (payload[5] << 8), height);
    assert.equal(payload[6], index, '序号必须从 0 连续');
    assert.equal(payload[7], chunks.length, '每片都带总数');
    assert.equal(payload[8], 2, 'bpp=2 表示 4 级灰');
  });
  const joined = Buffer.concat(chunks.map((payload) => payload.subarray(9)));
  assert.deepEqual(joined.subarray(0, bits.length), bits);

  // 1-bit 老格式仍然支持（bpp=1）
  const mono = Buffer.alloc((width / 8) * height, 0xff);
  const monoChunks = [...cardBitmapChunks(1, { w: width, h: height, bpp: 1, bits: mono })];
  assert.equal(monoChunks[0][8], 1);
  assert.equal(monoChunks.length, Math.ceil(mono.length / 231));
});

test('utf8Clamp 不切断半个汉字', () => {
  assert.equal(utf8Clamp('对齐', 2), '', '2 字节装不下一个汉字');
  assert.equal(utf8Clamp('对齐', 3), '对');
  assert.equal(utf8Clamp('对齐', 5), '对', '剩下的 2 字节不能塞半个"齐"');
  assert.equal(utf8Clamp('对齐', 6), '对齐');
  assert.equal(utf8Clamp('ab对齐', 5), 'ab对');
});

/* ------------------------------------------------------------ 触摸（BLE 无 USB） */

test('BLE 触摸：点/翻页/提交走 USB 同一套 hit-test，且没有 USB 链路也能生效', async () => {
  const { service, sent } = makeBleService();
  const promise = service.requestQuestion({
    questions: [{ id: 'q1', question: '先做哪个？', options: [{ label: 'A' }, { label: 'B' }, { label: 'C' }] }],
  });
  await settle();
  assert.ok(ops(sent).includes(OP.QUESTION), 'BLE 下必须把问答卡下发到手表');

  await bleTap(service, 'option'); // 选 A
  assert.equal(service.question.selections[0].size, 1);

  await bleTap(service, 'next'); // 翻到 B
  assert.equal(service.question.page, 1);
  await bleTap(service, 'option'); // 单选：B 替换 A
  assert.deepEqual([...service.question.selections[0]], [1]);

  await bleTap(service, 'submit');
  const result = await promise;
  assert.deepEqual(result.answer.answers, [{ id: 'q1', selected: ['B'] }]);
  // 真实路径里 questions.mjs 拿到 answer 后调 clearQuestion() → 手表回到会话卡
  service.clearQuestion();
  await settle();
  assert.ok(ops(sent).includes(OP.SESSION), '撤掉问答卡后要回到会话卡');
  await service.dispose();
});

test('BLE 热区以固件自报为准（0x58 覆盖内置兜底）', async () => {
  const { service } = makeBleService();
  const promise = service.requestQuestion({
    questions: [{ id: 'q1', question: '选一个', options: [{ label: 'A' }, { label: 'B' }] }],
  });
  await settle();
  // 固件说"选项区"搬到了 200..300 这一带：点在旧位置就不该命中
  service.ble.onEvent({
    type: 'zones',
    kind: 3,
    list: [
      { id: 3, name: 'option', x: 200, y: 330, w: 80, h: 40 },
      { id: 4, name: 'next', x: 104, y: 330, w: 80, h: 40 },
      { id: 5, name: 'submit', x: 248, y: 330, w: 80, h: 40 },
    ],
  });
  service.touchGuardUntil = 0;
  service.handleTouch({ pressed: true, x: 240, y: 305 }); // 旧兜底位置
  await settle(100);
  service.handleTouch({ pressed: false, x: 240, y: 305 });
  assert.equal(service.question.selections[0].size, 0, '旧位置不该命中');

  service.touchGuardUntil = 0;
  service.handleTouch({ pressed: true, x: 240, y: 350 }); // 固件自报的新位置
  await settle(100);
  service.handleTouch({ pressed: false, x: 240, y: 350 });
  assert.equal(service.question.selections[0].size, 1, '新位置必须命中');
  service.clearQuestion();   // 不 await promise：clearQuestion 不结算它（和真实卸载路径一致）
  await service.dispose();
});

/* ------------------------------------------------------------ 审批 / 会话卡下发 */

test('BLE 审批卡：先发 0x54（可答+倒计时），移交给电脑后变成 computer=1', async () => {
  const { service, sent } = makeBleService();
  service.config.approval.watchTimeoutMs = 60;
  const promise = service.requestApproval({ toolName: 'bash', command: 'rm -rf build' });
  await settle();
  const first = sent.find((payload) => payload[0] === OP.APPROVAL);
  assert.ok(first, '审批卡必须下发');
  assert.equal(first[1], 1, 'canAllow');
  assert.equal(first[2], 0, 'computer');
  assert.ok(first[3] >= 0, 'countdown 秒数');

  const result = await promise;
  assert.equal(result.fallback, true);
  await settle();
  const last = sent.filter((payload) => payload[0] === OP.APPROVAL).at(-1);
  assert.equal(last[1], 0, '超时后不能再在表上答');
  assert.equal(last[2], 1, '标记"到电脑确认"');
  service.clearApproval();
  await service.dispose();
});

test('BLE 会话卡：状态/步数变化才下发，内容不变不重发', async () => {
  const { service, sent } = makeBleService();
  const session = { id: 'test-session-ble' };
  // 走真实投影路径：turn/start → RUNNING；tool/call → 工具+步数
  service.onSessionEvent(session, { type: 'turn/start', data: {} });
  await settle();
  const first = sent.filter((payload) => payload[0] === OP.SESSION);
  assert.ok(first.length >= 1, 'BLE 下必须下发会话卡');
  assert.equal(first[0][1], 1, 'turn/start 后是 RUNNING');

  const before = sent.filter((payload) => payload[0] === OP.SESSION).length;
  await service.tickForTest();
  assert.equal(
    sent.filter((payload) => payload[0] === OP.SESSION).length,
    before,
    '内容没变不重发（卡片不跟 5 Hz 数值走）',
  );

  service.onSessionEvent(session, { type: 'turn/end', data: { reason: { kind: 'completed' } } });
  await settle();
  const last = sent.filter((payload) => payload[0] === OP.SESSION).at(-1);
  assert.equal(last[1], 3, 'turn/end completed 后是 DONE');
  await service.dispose();
});

/* ------------------------------------------------------------ 位图渲染 */

test('位图渲染：宽是 8 的倍数、高 20、有实际笔画', async () => {
  const renderer = new CardBitmapRenderer();
  try {
    await renderer.init();
  } catch (error) {
    // 没装 @napi-rs/canvas 的环境跳过（生产插件里是硬依赖）
    test.skip(`canvas 不可用：${error.message}`);
    return;
  }
  const bitmap = renderer.render('对齐 USB');
  assert.ok(bitmap, '应当能渲染');
  assert.equal(bitmap.h, 30);
  assert.equal(bitmap.bpp, 2, '2-bit 灰度（v4 抗锯齿）');
  assert.equal(bitmap.w % 8, 0);
  assert.ok(bitmap.w <= 296);
  assert.equal(bitmap.bits.length, (bitmap.w * 2 / 8) * 30);
  const lit = [...bitmap.bits].reduce((sum, byte) => sum + (byte ? 1 : 0), 0);
  assert.ok(lit > 30, `笔画太少（${lit} 个亮点），标题可能是空图`);
  assert.equal(renderer.render('   '), null, '空文本不渲染');
});

test('多行位图：长文本折成 2 行、高 = 行数 × 行高（BLE 长文本，2026-11-21）', async () => {
  /* BLE 卡片层的槽高从 30 抬到 44 之后，长题干/长选项改由多行位图承载。
   * 这里锁住：行数正确、宽是 8 的倍数、高度随行数变化。 */
  const renderer = new CardBitmapRenderer();
  try {
    await renderer.init();
  } catch (error) {
    test.skip(`canvas 不可用：${error.message}`);
    return;
  }
  const long = '这是一段很长很长的题干文字，用来验证它会被折成两行完整显示，而不是像以前那样被截断';
  const two = renderer.renderBlock(long, { maxLines: 2, maxWidth: 300, fontSize: 16, lineHeight: 17 });
  assert.ok(two, '长文本应当渲染成功');
  assert.equal(two.h, 34, '两行 = 2 × 17');
  assert.equal(two.w % 8, 0, '宽必须是 8 的倍数');
  assert.ok(two.w <= 300);
  assert.equal(two.bits.length, (two.w * 2 / 8) * two.h, '位图字节数要与宽高一致');
  const one = renderer.renderBlock('短', { maxLines: 2, maxWidth: 300, fontSize: 16, lineHeight: 17 });
  assert.equal(one.h, 17, '一行就是 17 高');
  assert.equal(renderer.renderBlock('   '), null, '空文本不渲染');
});

test('0x92 REDRAW：固件请主机整屏重发（reason 透传，空 payload 不炸）', () => {
  assert.equal(CMD.REDRAW, 0x92);
  assert.deepEqual(decodeFrame(CMD.REDRAW, [1]), { type: 'redraw', reason: 1 });
  assert.deepEqual(decodeFrame(CMD.REDRAW, []), { type: 'redraw', reason: 0 });
  assert.equal(decodeFrame(0x77, [1]).type, 'frame', '未知 cmd 仍按 frame 处理');
});
