#!/usr/bin/env node
/*
 * ble-e2e.mjs —— BLE 卡片层端到端台架（P1..P4）
 *
 * 这个脚本**不经过 DSH 宿主**，直接把插件真正在用的 PanelService 跑起来：
 * 同一条 BleBridge → @stoprocent/noble（npm 原生绑定）→ 真手表。所以它验证的是：
 *   · 会话投影 → 0x53/0x57 的载荷（P1）
 *   · 手表 0x04 触摸 → 插件 hit-test（P2）
 *   · 审批卡 0x54 + 俩按钮 + 10 s 回落（P3）
 *   · 问答卡 0x55 + 翻页/多选/提交（P4）
 * 唯一不覆盖的是 DSH 宿主的事件接线（session/event → onSessionEvent），
 * 那部分用 3100 开发实例的 logs/panel.log 对照（见 research/66）。
 *
 * 用法（先拔掉 USB，让插件只能走 BLE）：
 *   node tools/ble-e2e.mjs p1            # 会话卡：RUNNING→NEEDS→DONE
 *   node tools/ble-e2e.mjs p2 --hold 30  # 会话卡 + 收手表触摸
 *   node tools/ble-e2e.mjs p3 --hold 20  # 审批卡，等人点 拒绝/允许一次
 *   node tools/ble-e2e.mjs p4 --hold 60  # 问答卡，等人点 选项/下一项/提交
 *   node tools/ble-e2e.mjs all
 *
 * 退出码：0 全 PASS，1 有 FAIL。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { PanelService } from '../src/panel-service.mjs';
import { createFocusState } from '../src/focus.mjs';
import { OP, ACK } from '../src/protocol.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const PM_STATE = { IDLE: 0, RUNNING: 1, NEEDS: 2, DONE: 3, FAILED: 4 };
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const log = {
  info: (...args) => console.log('[info]', ...args),
  warn: (...args) => console.log('[warn]', ...args),
  error: (...args) => console.log('[error]', ...args),
  debug: (...args) => {
    if (process.env.PROTORIG_DEBUG) console.log('[debug]', ...args);
  },
};

const results = [];
function check(ok, text) {
  results.push({ ok: Boolean(ok), text });
  console.log(`  ${ok ? 'PASS' : 'FAIL'} ${text}`);
}

/* 默认指向仓库里的本地开发实例（local/，不发布）；其他机器用 DSH_HOME 覆盖。 */
const DEFAULT_DSH_HOME = path.resolve(here, '../../../local/dsh-dev/home');

/**
 * 从 DSH 的 projcache 里挑一个"有中文标题"的会话，用来验证 P1 的标题位图。
 * 这和插件生产路径读的是同一个文件（focus.mjs 的 readSessionTitle）。
 */
function pickTitleSession(dshHome, wanted) {
  const dir = path.join(dshHome, 'storages', 'session_projcache', 'sessions');
  let names = [];
  try {
    names = fs.readdirSync(dir).filter((name) => name.endsWith('.json'));
  } catch {
    return null;
  }
  if (wanted) {
    for (const name of names) {
      if (name.includes(wanted)) {
        const id = name.replace(/^session-/, '').replace(/\.json$/, '');
        return { id, title: readTitle(path.join(dir, name)) ?? '(空标题)' };
      }
    }
  }
  for (const name of names) {
    const title = readTitle(path.join(dir, name));
    if (title && /[\u4e00-\u9fa5]/.test(title)) {
      return { id: name.replace(/^session-/, '').replace(/\.json$/, ''), title };
    }
  }
  return null;
}

function readTitle(file) {
  try {
    const record = JSON.parse(fs.readFileSync(file, 'utf8'));
    const value = record?.record?.rows?.title?.val;
    return typeof value === 'string' && value.trim() ? value.trim() : null;
  } catch {
    return null;
  }
}

function parseArgs(argv) {
  const args = {
    stage: 'all',
    hold: 0,
    name: 'PROTORIG ES',
    address: '',
    dshHome: process.env.DSH_HOME || DEFAULT_DSH_HOME,
    session: '',
  };
  for (let i = 0; i < argv.length; i += 1) {
    const item = argv[i];
    if (item === '--hold') args.hold = Number(argv[++i] ?? 0);
    else if (item === '--name') args.name = argv[++i];
    else if (item === '--address') args.address = argv[++i];
    else if (item === '--dsh-home') args.dshHome = argv[++i];
    else if (item === '--session') args.session = argv[++i];
    else if (!item.startsWith('--')) args.stage = item;
  }
  if (!['p1', 'p2', 'p3', 'p4', 'empty', 'all'].includes(args.stage)) {
    throw new Error(`未知阶段 ${args.stage}`);
  }
  return args;
}

async function waitFor(checkFn, timeoutMs, label) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (checkFn()) return true;
    await sleep(100);
  }
  console.log(`  .. 等待超时：${label}`);
  return false;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const service = new PanelService({
    config: {
      ble: { enabled: true, name: args.name, address: args.address, metricsHz: 2 },
      reconnectDelayMs: 60000,
      // 台架用最长的表上独占时间（真机上这两个值来自设置页）
      approval: { watchTimeoutMs: 20000 },
      questions: { watchTimeoutMs: 60000 },
      debug: Boolean(process.env.PROTORIG_DEBUG),
    },
    logger: log,
  });
  // 只走 BLE：不探测 USB 串口（台架时手表应该已经拔掉）
  service.discover = async () => [];
  // 标题读真实 DSH projcache（和插件生产路径同一个文件）
  service.titleOptions = { dshHome: args.dshHome };
  const titleSession = pickTitleSession(args.dshHome, args.session);

  /* 观察点：卡片下发 / 触摸 / 热区 / 回执 */
  const seen = { card: [], touch: [], zones: [], ack: [], status: [] };
  const originalSend = service.ble.sendPcFrame.bind(service.ble);
  service.ble.sendPcFrame = (payload) => {
    const ok = originalSend(payload);
    if (ok) seen.card.push({ at: Date.now(), op: payload[0], size: payload.length });
    return ok;
  };
  const originalEvent = service.ble.onEvent;
  service.ble.onEvent = (event) => {
    if (event?.type === 'touch') {
      seen.touch.push(event);
      console.log(`  <- 手表触摸 pressed=${event.pressed} x=${event.x} y=${event.y}`);
    } else if (event?.type === 'zones') {
      seen.zones.push(event);
      const list = (event.list ?? []).map((z) => `${z.name}(${z.x},${z.y},${z.w}x${z.h})`).join(' ');
      console.log(`  <- 热区 kind=${event.kind} ${list || '(无)'}`);
    } else if (event?.type === 'cardack') {
      seen.ack.push(event);
      console.log(`  <- 回执 what=${event.what} code=${event.code}`);
    } else if (event?.type === 'status') {
      seen.status.push(event);
      if (seen.status.length <= 2) {
        console.log(`  <- 状态 batt=${event.battery} bright=${event.bright} page=${event.page}`);
      }
    }
    originalEvent?.(event);
  };

  console.log('=== 启动 PanelService（只走 BLE，USB 探测已关闭）===');
  await service.start();
  const connected = await waitFor(() => service.ble.connected, 30000, 'BLE 连接');
  check(connected, 'BLE 数字通道已连接');
  if (!connected) {
    console.log('\n连不上手表：确认手表已拔 USB 且在广播、蓝牙开着、没有被手机占用。');
    await service.dispose();
    process.exit(1);
  }
  await sleep(1500); // 等固件切到监控页（-1 屏）

  const stageWants = (name) => args.stage === name || args.stage === 'all';

  /* ---------------------------------------------------------------- P1 */
  if (stageWants('p1') || stageWants('p2')) {
    console.log('\n=== P1 会话卡 ===');
    const session = { id: titleSession?.id ?? 'bench-session-ble' };
    if (titleSession) {
      console.log(`  用真实会话 ${session.id.slice(0, 18)}… 标题「${titleSession.title}」（手表上应显示同样的字）`);
    } else {
      console.log('  没读到 DSH projcache，标题会是 SESSION（不影响卡片链路验证）');
    }
    service.onSessionEvent(session, { type: 'turn/start', data: {} });
    await sleep(1500);
    const sessionCards = seen.card.filter((c) => c.op === OP.SESSION);
    check(sessionCards.length > 0, '下发会话卡 0x53');
    check(seen.card.some((c) => c.op === OP.CARD_BMP), '下发标题位图 0x57');
    check(seen.ack.some((a) => a.what === ACK.CARD), '手表回执 0x59 card');
    check(seen.ack.some((a) => a.what === ACK.BMP_OK), '手表回执 0x59 bmp-ok');
    check(seen.zones.some((z) => z.kind === 1 && !z.list.length), '会话卡热区自报 kind=1 count=0');

    console.log('  … 3 s 后转 NEEDS（看表：状态应变黄）');
    await sleep(3000);
    service.onSessionEvent(session, { type: 'approval/asked', data: {} });
    await sleep(3000);
    console.log('  … 3 s 后转 DONE（看表：状态应变蓝）');
    service.onSessionEvent(session, { type: 'turn/end', data: { reason: { kind: 'completed' } } });
    await sleep(2000);
  }

  /* ---------------------------------------------------------------- 空态 */
  if (stageWants('empty')) {
    console.log('\n=== 空态（v4：鲸鱼 + 探索未至之境 / DSH 等待任务）===');
    service.focus = createFocusState();          // 没有会话
    await service.tickForTest();
    await sleep(1500);
    check(seen.card.some((c) => c.op === OP.SESSION), '下发空态卡 0x53');
    check(seen.card.some((c) => c.op === OP.CARD_BMP), '下发鲸鱼位图 0x57');
    console.log('  手表上应为：鲸鱼 + 探索未至之境 / DSH 等待任务 / 底部电量·余额');
    await sleep((args.hold || 8) * 1000);
  }

  /* ---------------------------------------------------------------- P2 */
  if (stageWants('p2')) {
    console.log('\n=== P2 触摸回传 ===');
    const hold = args.hold || 20;
    console.log(`  请在手表上点几下 / 上下滑几下（${hold} s）…`);
    const before = seen.touch.length;
    await sleep(hold * 1000);
    check(seen.touch.length > before, '收到手表回传的 0x04 触摸帧');
    const press = seen.touch.find((t) => t.pressed);
    const release = seen.touch.find((t) => !t.pressed);
    check(Boolean(press && release), '同时收到按下与抬起（点击判定需要这一对）');
    if (press) console.log(`  坐标范围示例：按下 (${press.x},${press.y})`);
  }

  /* ---------------------------------------------------------------- P3 */
  if (stageWants('p3')) {
    console.log('\n=== P3 审批卡 ===');
    const hold = args.hold || 20;
    console.log(`  卡片出现后请在 ${hold} s 内点手表上的 [拒绝] 或 [允许一次]`);
    const promise = service.requestApproval({
      toolName: 'bash',
      command: 'rm -rf firmware/lvgl_test/build',
    });
    await sleep(500);
    check(seen.card.some((c) => c.op === OP.APPROVAL), '下发审批卡 0x54');
    const zonesAfter = seen.zones.filter((z) => z.kind === 2).at(-1);
    check(Boolean(zonesAfter && zonesAfter.list.length === 2), '审批热区自报 2 个按钮');
    if (zonesAfter) {
      console.log(`  热区：${zonesAfter.list.map((z) => `${z.name}@(${z.x},${z.y},${z.w}x${z.h})`).join('  ')}`);
    }
    const touchesBefore = seen.touch.length;
    const outcome = await promise;
    check(seen.touch.length > touchesBefore, '审批期间收到手表触摸');
    check(Boolean(outcome.outcome), `手表作答：${outcome.outcome ?? '(无，可能超时移交电脑)'}`);
    if (outcome.fallback) {
      console.log('  （超时 ↑ 属于设计行为：表上变成"到电脑确认"）');
      check(seen.card.filter((c) => c.op === OP.APPROVAL).length >= 2, '超时后重发审批卡（computer=1）');
    }
    service.clearApproval();
    await sleep(800);
  }

  /* ---------------------------------------------------------------- P4 */
  if (stageWants('p4')) {
    console.log('\n=== P4 问答卡 ===');
    const hold = args.hold || 60;
    console.log(`  请点 [选项] 选一项，点 [下一项] 翻页，再点 [提交]（${hold} s）`);
    const promise = service.requestQuestion({
      questions: [
        {
          id: 'bench-q1',
          question: '手表上先做哪一项？',
          options: [{ label: '固件卡片层' }, { label: '插件触摸路由' }, { label: 'BLE 位图' }],
          multiSelect: false,
        },
      ],
    });
    await sleep(500);
    check(seen.card.some((c) => c.op === OP.QUESTION), '下发问答卡 0x55');
    const zonesAfter = seen.zones.filter((z) => z.kind === 3).at(-1);
    check(Boolean(zonesAfter && zonesAfter.list.length === 3), '问答热区自报 3 个（选项/下一项/提交）');
    if (zonesAfter) {
      console.log(`  热区：${zonesAfter.list.map((z) => `${z.name}@(${z.x},${z.y},${z.w}x${z.h})`).join('  ')}`);
    }
    const pageTurns = () => seen.card.filter((c) => c.op === OP.QUESTION).length;
    const before = pageTurns();
    const result = await promise;
    check(Boolean(result.answer), '手表提交成功');
    if (result.answer) {
      const picked = result.answer.answers?.[0]?.selected ?? [];
      console.log(`  回写：${JSON.stringify(picked)}`);
      check(picked.length > 0, '回写的选项非空');
    }
    check(pageTurns() > before, '翻页/选择过程中重发过问答卡（状态在跟着手指标）');
    service.clearQuestion();
    await sleep(800);
  }

  /* ---------------------------------------------------------------- 收尾 */
  console.log('\n=== 收尾：撤卡 ===');
  service.clearApproval();
  service.clearQuestion();
  await sleep(500);
  console.log(`卡片帧 ${seen.card.length} 条 · 触摸 ${seen.touch.length} 条 · 热区 ${seen.zones.length} 条 · 回执 ${seen.ack.length} 条`);

  const failed = results.filter((r) => !r.ok);
  console.log(`\n=== ${results.length - failed.length} PASS / ${failed.length} FAIL ===`);
  if (failed.length) {
    for (const item of failed) console.log(`  FAIL ${item.text}`);
  }
  await service.dispose();
  process.exit(failed.length ? 1 : 0);
}

main().catch(async (error) => {
  console.error('台架异常：', error);
  process.exit(1);
});
