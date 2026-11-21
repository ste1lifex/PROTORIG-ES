/* pc_host_loss_test.mjs —— 复现/验证「PC 端进程被杀后手表卡死」的看门狗。
 *
 * 用法（手表已插 USB，先 shell 进电脑模式 `pcmode 1`）：
 *   node tools/pc_host_loss_test.mjs COM8 5000
 *
 * 流程：HELLO（打开透传）→ CLEAR 红 → 保持 N 秒（主机在读，不该误判）→
 *      进程 process.exit(0)（等价于 3100 被关掉，端口由系统关闭）。
 * 预期（2026-11-14 修复后）：约 3 s 后手表自动退出电脑模式回表盘，
 *      串口 `pcmode` 显示 active=0 … host_gone=1。
 *
 * 依赖插件里的 serialport（与 3100 插件同一套原生绑定，不额外装包）。
 */
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '../../..');
const serialportEntry = path.join(repo, 'plugin/dsh-protorig-panel/node_modules/serialport/dist/index.js');
const { SerialPort } = await import(pathToFileURL(serialportEntry).href);

const portPath = process.argv[2] || 'COM8';
const holdMs = Number(process.argv[3] || 5000);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);

const port = new SerialPort({ path: portPath, baudRate: 115200, autoOpen: false });
await new Promise((resolve, reject) => port.open((e) => (e ? reject(e) : resolve())));
log('opened', portPath);
await sleep(1200);

const drain = (ms) => new Promise((resolve) => {
  const onData = (d) => process.stdout.write(d);
  port.on('data', onData);
  setTimeout(() => { port.off('data', onData); resolve(); }, ms);
});

// HELLO: A5 5A len=1 payload=0x01 -> 固件 passthrough ON + HELLO_RSP(0x81)
port.write(Buffer.from([0xa5, 0x5a, 0x01, 0x01]));
await drain(800);

// CLEAR red: A5 5A len=4 [0x13, FF, 00, 00]
port.write(Buffer.from([0xa5, 0x5a, 0x04, 0x13, 0xff, 0x00, 0x00]));
await drain(1200);

log(`holding ${holdMs} ms with PC pixels on screen; then process.exit(0) (no cleanup)`);
await sleep(holdMs);
log('EXIT NOW (port closed by OS) — 手表应在 ~3 s 后自动回表盘');
process.exit(0);
