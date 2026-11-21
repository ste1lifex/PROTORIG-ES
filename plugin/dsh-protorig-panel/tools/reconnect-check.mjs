#!/usr/bin/env node
/*
 * reconnect-check.mjs —— 真机回归：USB 掉线 → BLE 降级 → USB 回来 → 切回像素面板
 *
 * 背景（research/62 §11）：旧版 WatchLink 的自动重连只在 close 后再试一次，
 * USB 拔掉后 1.5 s 那次重开如果手表还没枚举出来，链路就永久停在断开，BLE 也一直挂着。
 *
 * 这个脚本用真实手表、真实串口、真实 BLE 桥跑一遍完整迁移，只把"串口枚举结果"
 * 短暂 mock 成空（模拟拔线后 4 s 内系统还没枚举出手表），其余全是生产代码路径：
 *
 *   1. 正常连接 → 像素面板；
 *   2. 链路 close + 枚举为空 → 应启动 BLE 数字通道桥（扫描中或已连接）；
 *   3. 恢复枚举 → 应停 BLE 桥、重开 COM 口、对时/HELLO、整屏重发；
 *   4. 滑出电脑模式 → 滑回 → 应自动重新握手并整屏重发。
 *
 * 第 2 步的边界（2026-11-21 实测，见 local/docs/research/pc-dsh/89）：
 *   关闭 tty **不等于**物理拔出 USB，设备仍 enumerated，固件的主机存活看门狗
 *   （PC_HOST_GONE_MS = 3000）会在 3 s 后退出电脑模式并停广播，BLE 因此连不上。
 *   所以本步只断言"插件起了 BLE 桥并在扫描"；要验 BLE 真连，请物理拔掉 USB 后
 *   把手表右滑进电脑模式，用 tools/ble-e2e.mjs p1/p2。
 *
 * 用法（先停 3100，否则 COM8 被占用）：
 *   node tools/reconnect-check.mjs [--port COM8] [--empty-ms 4000]
 *
 * 退出码 0 = 四步全过；非 0 = 有一步超时。
 */
import { WatchLink } from '../src/link.mjs';
import { PanelService } from '../src/panel-service.mjs';

const argv = process.argv.slice(2);
const flag = (name) => {
  const index = argv.indexOf(name);
  return index >= 0 ? argv[index + 1] : undefined;
};
const port = flag('--port') ?? '';
const emptyMs = Number(flag('--empty-ms') ?? 4000);
if (port) {
  console.warn('[warn] 指定了 --port：固定串口会绕过"设备不存在"的模拟，本脚本的 STEP 2 将不成立');
}
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const logger = {
  info: (...args) => console.log(`[info] ${args.join(' ')}`),
  warn: (...args) => console.warn(`[warn] ${args.join(' ')}`),
  error: (...args) => console.error(`[error] ${args.join(' ')}`),
  debug: () => {},
};

async function waitFor(label, check, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (check()) {
      console.log(`[PASS] ${label}（${((Date.now() - (deadline - timeoutMs)) / 1000).toFixed(1)} s）`);
      return true;
    }
    await sleep(50);
  }
  console.error(`[FAIL] ${label} 超时（${timeoutMs} ms）`);
  return false;
}

/** 静默轮询（不打印 PASS/FAIL），给"重试"场景用，免得每次重试都刷一条 FAIL。 */
async function poll(check, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (check()) return true;
    await sleep(50);
  }
  return false;
}

const service = new PanelService({ config: { port, fps: 2, autoEnter: true }, logger });
let failed = false;
try {
  await service.start();
  failed |= !(await waitFor(
    '1. USB 像素面板就绪（MODE 1）',
    () => service.link.state.passthrough && service.handshaking === false,
    20000,
  ));
  if (failed) throw new Error('第 1 步失败，后续没有意义');

  // --- 模拟拔线：之后的 4 s 里"系统枚举不到手表" ---
  let discovers = 0;
  service.discover = async () => {
    discovers += 1;
    return [];
  };
  const connectedAt = Date.now();
  await service.link.close();
  console.log('[step] 已关闭串口（模拟 USB 拔出），断开 4 s 后恢复枚举');
  /* 关闭 tty ≠ 物理拔出 USB：USB 设备仍是 enumerated/configured 状态。
   * 固件 2026-11-14 为此加了主机存活看门狗（svc_pc.c 的 pc_host_watch，
   * PC_HOST_GONE_MS = 3000，见 local/docs/research/pc-dsh/85），3 s 收不到
   * USB IN 完成就 svc_pc_leave() → 退出电脑模式 → ble_link_stop() 停广播。
   * 而插件的 BLE 降级要等 lastConnectedAt + 2500 ms 才起桥，再扫描/连接，
   * 天然晚于那 3 s 窗口 —— 所以**这里连不上 BLE 是固件现状决定的，不是插件 bug**。
   * 物理拔线时 transport 会切到 NONE，看门狗不触发，电脑模式保活 60 s，
   * BLE 有充足窗口；纯 BLE 卡片层由 tools/ble-e2e.mjs p1/p2 在真机覆盖。
   * 本步只断言"插件确实起了 BLE 桥并在扫描"，连上则额外报喜。 */
  failed |= !(await waitFor(
    '2. USB 不在时启动 BLE 数字通道桥（扫描中或已连接）',
    () => service.ble.connected || service.ble.starting || service.ble.connecting || service.ble.scanning,
    20000,
  ));
  const bleWait = Date.now() - connectedAt;
  console.log(`[info] BLE 等待 ${bleWait} ms，期间探测 ${discovers} 次（期望约每 1.5 s 一次）`);
  if (service.ble.connected) {
    console.log('[info] BLE 已连上 —— 本窗口内手表仍在广播（真机链路正常）');
  } else {
    console.log('[skip] BLE 未连上属预期：关闭 tty 触发固件 3 s host-gone，手表已退出电脑模式并停广播');
  }

  // 断电期间不应留下多个桥（纯 Node 实现没有子进程：ble-bridge 的 child 恒为 null，
  // 旧文案"子进程数（期望 1）"已过期，这里改成看桥的真实状态）
  await sleep(2000);
  const bleState = service.ble.connected
    ? '已连接'
    : (service.ble.starting || service.ble.connecting || service.ble.scanning ? '扫描中' : '未启动');
  console.log(`[info] BLE 桥状态=${bleState}`);

  // --- USB 回来 ---
  const emptyUntil = connectedAt + emptyMs;
  while (Date.now() < emptyUntil) await sleep(50);
  service.discover = () => WatchLink.discover();
  await sleep(1200); // 让出正在跑的那轮空探测，下一轮就能看到设备
  failed |= !(await waitFor('3. USB 回来并重新进入透传（整屏重发）', () => service.link.state.passthrough, 25000));
  await sleep(1500);
  console.log(`[info] 最终状态 connected=${service.link.state.connected} passthrough=${service.link.state.passthrough} path=${service.link.path}`);

  // --- 模拟"左滑退出 → 滑回电脑模式"：PC 0 → PC 1，插件必须重新握手 ---
  const invalidatedBefore = service.fb?.invalidations ?? 0;
  await service.link.shell('pcmode 0');
  const left = await waitFor(
    '4a. 退出电脑模式（PC 0 / MODE 0，本地页）',
    () => service.link.state.pcMode === false && service.link.state.passthrough === false,
    6000,
  );
  failed |= !left;
  /* 用 shell 模拟"右滑回电脑模式"：`pcmode 1` 偶尔不生效 —— 写入成功，但手表没进
   * 电脑模式（随后查询回报 `active=0`）。2026-11-21 实测单发失效率约 2/10，成因未定；
   * 真实用户是右滑，由固件自己走 svc_pc_enter()，不经过 shell。
   * 产品侧的"自动进电脑模式"已改成发完查状态、没进就重发（#enterPcModeWithRetry），
   * 台架这里同样重试，别把这个偶发当成产品缺陷。 */
  await sleep(600); // 本地页滑动收尾
  let back = false;
  for (let attempt = 1; attempt <= 4 && !back; attempt += 1) {
    await service.link.shell('pcmode 1');
    back = await poll(
      () => service.link.state.pcMode && service.link.state.passthrough && service.handshaking === false,
      4000,
    );
    if (!back) {
      console.log(`[info] 第 ${attempt} 次 pcmode 1 未生效（已知偶发约 2/10，成因未定），重试`);
      await sleep(1200);
    }
  }
  if (back) {
    console.log('[PASS] 4b. 滑回电脑模式后自动重新握手（settime/HELLO/整屏重发）');
  } else {
    console.error('[FAIL] 4b. 滑回电脑模式后自动重新握手（settime/HELLO/整屏重发） 重试 4 次仍未生效');
  }
  failed |= !back;
  console.log(
    `[info] 重新进入后整屏重发次数增加=${(service.fb?.invalidations ?? 0) - invalidatedBefore}（应 ≥2：握手+MODE 1）`,
  );
} catch (error) {
  failed = true;
  console.error('[fatal]', error.message);
} finally {
  await Promise.race([service.dispose(), sleep(4000)]);
}
process.exit(failed ? 1 : 0);
