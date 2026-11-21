/*
 * face-panel.test.mjs —— 默认表盘下半区（research/76 §2.3）
 *
 * 三块：
 *   1. 打包：cover 480×232 → 256 色 → I8（1024 B palette + 111,360 B 索引 = 112,384 B）→ CRC32；
 *   2. 上传命令序列：`facepanel begin <len> <crc>` → 2342 × `facepanel w <off> <hex>` → `facepanel end`；
 *   3. 面板服务状态机 + 路由（注入替身，**不碰真串口/真手表**）。
 *
 * 固件回包字符串来自 `research/76 §2.2` 与固件侧（panel-fw）确认的字面量，见 research/78 §2。
 */
import './_env.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { createCanvas } from '@napi-rs/canvas';
import { crc32 } from '../src/face-pack.mjs';
import {
  PANEL_H,
  PANEL_HEADER_LEN,
  PANEL_IMAGE_LEN,
  PANEL_MAGIC,
  PANEL_PALETTE_BYTES,
  PANEL_UPLOAD_CHUNK,
  PANEL_UPLOAD_LINES,
  PANEL_W,
  PANEL_Y,
  packPanelImageBuffer,
  packPanelRgba,
  panelBeginLine,
  panelBlobCrc,
  panelChunks,
  panelCrcHex,
  panelStatusMatchesContract,
  parsePanelStatus,
} from '../src/face-panel.mjs';
import { FACE_IDS, faceLabel, normalizeFaceId } from '../src/face-ids.mjs';
import { PanelService } from '../src/panel-service.mjs';

const silent = { info() {}, warn() {}, error() {}, debug() {} };
const settle = (ms = 10) => new Promise((resolve) => setTimeout(resolve, ms));

/* ------------------------------------------------------------------ 打包 */

const solidRgba = (r, g, b) => {
  const rgba = new Uint8Array(PANEL_W * PANEL_H * 4);
  for (let i = 0; i < rgba.length; i += 4) {
    rgba[i] = r;
    rgba[i + 1] = g;
    rgba[i + 2] = b;
    rgba[i + 3] = 255;
  }
  return rgba;
};

test('面板常量：480×232 @ y=248，I8 = 1024 B palette + 111,360 B 索引 = 112,384 B', () => {
  assert.equal(PANEL_W, 480);
  assert.equal(PANEL_H, 232);
  assert.equal(PANEL_Y, 248);
  assert.equal(PANEL_PALETTE_BYTES, 1024);
  assert.equal(PANEL_IMAGE_LEN, 112384);
  assert.equal(PANEL_IMAGE_LEN, PANEL_PALETTE_BYTES + PANEL_W * PANEL_H);
  assert.equal(PANEL_UPLOAD_CHUNK, 48);
  /* 112,384 = 2341×48 + 16 → 2342 行 */
  assert.equal(PANEL_UPLOAD_LINES, 2342);
});

test('packPanelRgba：纯色图 → 1 色调色板（B,G,R,A）+ 全 0 索引，blob 尺寸/CRC 正确', () => {
  const packed = packPanelRgba(solidRgba(255, 128, 0), { dither: false });
  assert.equal(packed.size, PANEL_IMAGE_LEN);
  assert.equal(packed.blob.length, PANEL_IMAGE_LEN);
  assert.equal(packed.width, PANEL_W);
  assert.equal(packed.height, PANEL_H);
  /* 纯色无法继续切分 → 只剩 1 个调色板项，其余 255 项留 0 */
  assert.equal(packed.palette.length, 1);
  assert.deepEqual([...packed.blob.subarray(0, 4)], [0, 128, 255, 255], 'palette 内存序 B,G,R,A + A=FF');
  assert.equal(packed.blob[PANEL_PALETTE_BYTES], 0, '索引全 0');
  assert.equal(packed.blob[PANEL_IMAGE_LEN - 1], 0);
  assert.equal(packed.crc32, crc32(packed.blob));
  assert.match(packed.crcHex, /^[0-9A-F]{8}$/);
});

test('packPanelRgba：渐变图 → 256 色，索引不越界，尺寸/CRC 自洽', () => {
  const rgba = new Uint8Array(PANEL_W * PANEL_H * 4);
  for (let y = 0; y < PANEL_H; y += 1) {
    for (let x = 0; x < PANEL_W; x += 1) {
      const i = (y * PANEL_W + x) * 4;
      rgba[i] = Math.round((x / PANEL_W) * 255);
      rgba[i + 1] = Math.round((y / PANEL_H) * 255);
      rgba[i + 2] = 128;
      rgba[i + 3] = 255;
    }
  }
  const packed = packPanelRgba(rgba, { dither: true });
  assert.equal(packed.palette.length, 256);
  let maxIndex = 0;
  for (let i = PANEL_PALETTE_BYTES; i < PANEL_IMAGE_LEN; i += 1) maxIndex = Math.max(maxIndex, packed.blob[i]);
  assert.ok(maxIndex < 256, `索引 ${maxIndex} 越界`);
  assert.equal(panelBlobCrc(packed.blob).crcHex, packed.crcHex, '两次 CRC 一致');
});

test('packPanelRgba：RGBA 长度不对 → 报错（不静默截断）', () => {
  assert.throws(() => packPanelRgba(new Uint8Array(16), { dither: false }), /RGBA 长度/);
});

test('packPanelImageBuffer：任意尺寸 PNG → cover 480×232 + 480×232 预览 PNG', async () => {
  const canvas = createCanvas(900, 300);
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#224466';
  ctx.fillRect(0, 0, 900, 300);
  const png = canvas.toBuffer('image/png');
  const packed = await packPanelImageBuffer(png, { dither: false });
  assert.equal(packed.blob.length, PANEL_IMAGE_LEN);
  assert.ok(Buffer.isBuffer(packed.previewPng) && packed.previewPng.length > 100, '应有 previewPng');
  const { loadImage } = await import('@napi-rs/canvas');
  const img = await loadImage(packed.previewPng);
  assert.equal(img.width, 480);
  assert.equal(img.height, 232);
});

test('packPanelImageBuffer：空数据 → 报错', async () => {
  await assert.rejects(() => packPanelImageBuffer(Buffer.alloc(0)), /为空/);
});

/* -------------------------------------------------------- 上传命令序列 */

test('上传序列：begin 行 + 2342 行 48 B + end，覆盖整个 blob 且每行 ≤ 120 字符', () => {
  const blob = Buffer.alloc(PANEL_IMAGE_LEN);
  for (let i = 0; i < blob.length; i += 1) blob[i] = i & 0xff;
  const packed = panelBlobCrc(blob);

  const begin = panelBeginLine({ crcHex: packed.crcHex });
  assert.equal(begin, `facepanel begin ${PANEL_IMAGE_LEN} ${packed.crcHex}`);

  const rows = [...panelChunks(blob, { chunk: PANEL_UPLOAD_CHUNK })];
  assert.equal(rows.length, PANEL_UPLOAD_LINES);
  assert.deepEqual(rows[0], {
    off: 0, len: 48, hex: blob.subarray(0, 48).toString('hex').toUpperCase(), line: rows[0].line,
  });
  assert.equal(rows.at(-1).off, 112368);
  assert.equal(rows.at(-1).len, 16);
  assert.equal(rows.at(-1).hex, blob.subarray(112368).toString('hex').toUpperCase());

  /* 每行都以 facepanel w 开头、hex 大写、整行不超过 shell 120 字符上限 */
  const rebuilt = [];
  for (const row of rows) {
    assert.match(row.line, new RegExp(`^facepanel w ${row.off} [0-9A-F]+$`));
    assert.ok(row.line.length <= 120, `行太长：${row.line.length}`);
    assert.equal(row.line.split(' ')[3].length, row.len * 2);
    rebuilt.push(Buffer.from(row.hex, 'hex'));
  }
  assert.deepEqual(Buffer.concat(rebuilt), blob, '逐行拼回来必须等于原 blob');
});

test('上传序列：非法长度/非法分片大小直接拒绝', () => {
  assert.throws(() => panelBeginLine({ crcHex: '00000000', length: 1234 }), /len 必须是/);
  assert.throws(() => [...panelChunks(Buffer.alloc(10))], /长度不对/);
  assert.throws(() => [...panelChunks(Buffer.alloc(PANEL_IMAGE_LEN), { chunk: 64 })], /最多 48 B/);
  assert.throws(() => panelBlobCrc(Buffer.alloc(10)), /长度不对/);
});

test('panelCrcHex：8 位大写 hex', () => {
  assert.equal(panelCrcHex(0x04f565b1), '04F565B1');
  assert.equal(panelCrcHex(0xdeadbeef), 'DEADBEEF');
});

test('固件自测向量（panel-fw 2026-11-08）：header 字段与 zlib CRC32 跨实现一致', () => {
  /* 固件 RAM 仿 NOR 自测产出的前 24 B（research/77 §5）：PC 侧用它证明
   * ①zlib.crc32 与固件 CRC 实现等价（begin/end 的 CRC 才可能对上）；
   * ②480×232/y=248/112,384 这几个常量与固件 header 一致。 */
  const hdr = Buffer.from('5046504E0101E001E800F80000B7010039FAC7485EE798ED', 'hex');
  assert.equal(hdr.length, 24);
  assert.equal(hdr.readUInt32LE(0), PANEL_MAGIC, "magic 'PFPN'");
  assert.equal(hdr[4], 1, 'version');
  assert.equal(hdr[5], 1, 'fmt I8');
  assert.equal(hdr.readUInt16LE(6), PANEL_W, 'width');
  assert.equal(hdr.readUInt16LE(8), PANEL_H, 'height');
  assert.equal(hdr.readUInt16LE(10), PANEL_Y, 'y');
  assert.equal(hdr.readUInt32LE(12), PANEL_IMAGE_LEN, 'image_len');
  assert.equal(hdr.readUInt32LE(16), 0x48C7FA39, 'image_crc32（固件自测值）');
  assert.equal(hdr.readUInt32LE(20), 0xED98E75E, 'header_crc32');
  /* 固件 header_crc32 只覆盖前 20 B（research/77 §5）—— PC 侧同样算得出来 */
  assert.equal(crc32(hdr.subarray(0, 20)), 0xED98E75E);
  assert.equal(PANEL_HEADER_LEN, 256, '面板 header 预留 256 B（0x1000 扇区里的前 256 B）');
});

/* ------------------------------------------------------------ 状态解析 */

test('parsePanelStatus：valid=1 / valid=0 / crc-checked / 非 facepanel 行', () => {
  const ok = parsePanelStatus('[facepanel] valid=1 crc=04F565B1 len=112384 w=480 h=232 y=248');
  assert.equal(ok.valid, true);
  assert.equal(ok.crc, '04F565B1');
  assert.equal(ok.len, 112384);
  assert.equal(ok.width, 480);
  assert.equal(ok.height, 232);
  assert.equal(ok.y, 248);
  assert.equal(ok.crcChecked, false);
  assert.equal(panelStatusMatchesContract(ok), true);

  const checked = parsePanelStatus('[facepanel] valid=1 crc=04F565B1 len=112384 w=480 h=232 y=248 crc-checked');
  assert.equal(checked.crcChecked, true);

  const none = parsePanelStatus('[facepanel] valid=0 rc=-5 (bad header format)');
  assert.equal(none.valid, false);
  assert.equal(none.rc, -5);
  assert.equal(none.reason, 'bad header format');
  assert.equal(none.crc, '');
  assert.equal(panelStatusMatchesContract(none), false);

  assert.equal(parsePanelStatus('W-OK'), null);
  assert.equal(parsePanelStatus('[faceimg] valid=1 crc=04F565B1'), null);
  /* 固件在 install/clear 之后会多打一行重建提示：不能被当成状态行 */
  assert.equal(parsePanelStatus('[facepanel] apply queued (rebuild face 0)'), null);
  assert.equal(parsePanelStatus('[facepanel] clear queued (rebuild face 0)'), null);
  assert.equal(parsePanelStatus(''), null);
});

test('panelStatusMatchesContract：尺寸/位置被固件改了要能发现', () => {
  const base = parsePanelStatus('[facepanel] valid=1 crc=04F565B1 len=112384 w=480 h=232 y=248');
  assert.equal(panelStatusMatchesContract({ ...base, width: 480, height: 240 }), false);
  assert.equal(panelStatusMatchesContract({ ...base, len: 231424 }), false);
  assert.equal(panelStatusMatchesContract({ ...base, y: 240 }), false);
  assert.equal(panelStatusMatchesContract({ ...base, y: null }), true, 'y 缺失容忍（老固件）');
});

/* --------------------------------------------------------- 表盘 id 常量 */

test('face id：只允许 0/3；1/2（已删）与其它值一律拒绝', () => {
  assert.deepEqual([...FACE_IDS], [0, 3]);
  assert.equal(normalizeFaceId(0), 0);
  assert.equal(normalizeFaceId('3'), 3);
  assert.equal(normalizeFaceId(1), null);
  assert.equal(normalizeFaceId(2), null);
  assert.equal(normalizeFaceId(9), null);
  assert.equal(normalizeFaceId(''), null);
  assert.equal(normalizeFaceId(undefined), null);
  assert.equal(faceLabel(0), '默认');
  assert.equal(faceLabel(3), '图片');
  assert.equal(faceLabel(1), '?');
});

/* --------------------------------------------------- 面板服务状态机（替身） */

function makeFakeLink({ connected = true, pcMode = false, passthrough = false } = {}) {
  const link = new EventEmitter();
  link.state = {
    connected, pcMode, passthrough, hello: null, path: '/dev/cu.FAKE', adminId: '2BEA4E',
  };
  link.calls = [];
  link.handler = () => null;
  link.command = async (line, opts = {}) => {
    link.calls.push({ type: 'command', line, opts });
    const out = await link.handler(line, opts);
    if (out === undefined || out === null) return null;
    return out;
  };
  link.shell = async (line) => {
    link.calls.push({ type: 'shell', line });
    if (line === 'pcmode 0') link.state.pcMode = false;
    if (line === 'pcmode 1') link.state.pcMode = true;
  };
  link.sayHello = async () => {
    link.calls.push({ type: 'hello' });
    link.state.passthrough = true;
  };
  link.write = async () => {};
  return link;
}

function makeService(link) {
  const service = new PanelService({ config: {}, logger: silent });
  service.ble.start = async () => {};
  service.ble.stop = () => {};
  service.discover = async () => [];
  service.config.reconnectDelayMs = 60000;
  service.link = link;
  return service;
}

const blob = () => {
  const buffer = Buffer.alloc(PANEL_IMAGE_LEN);
  for (let i = 0; i < buffer.length; i += 1) buffer[i] = (i * 7) & 0xff;
  return buffer;
};

const panelStatusLine = (crc, { valid = true } = {}) => (valid
  ? `[facepanel] valid=1 crc=${crc} len=112384 w=480 h=232 y=248`
  : '[facepanel] valid=0 rc=-5 (bad header format)');

test('panelStatus：解析 valid=1 并缓存进 panelView；离线说明需要 USB', async () => {
  const link = makeFakeLink();
  link.handler = (line) => (line === 'facepanel' ? panelStatusLine('04F565B1') : null);
  const service = makeService(link);
  const status = await service.panelStatus();
  assert.equal(status.valid, true);
  assert.equal(status.crc, '04F565B1');
  assert.equal(status.width, PANEL_W);
  assert.equal(status.height, PANEL_H);
  assert.equal(status.y, PANEL_Y);
  assert.equal(status.error, null);
  const view = service.panelView();
  assert.equal(view.known, true);
  assert.equal(view.valid, true);
  assert.equal(view.crc, '04F565B1');

  const offline = makeService(makeFakeLink({ connected: false }));
  const off = await offline.panelStatus();
  assert.equal(off.supported, false);
  assert.match(off.error, /USB/);
  assert.equal((await offline.panelStatus({ withCrc: true })).error.includes('USB'), true);
});

test('panelStatus：withCrc 用 facepanel info；没回包/尺寸不符都只报错不抛', async () => {
  const link = makeFakeLink();
  link.handler = (line) => (line === 'facepanel info'
    ? `${panelStatusLine('04F565B1')} crc-checked`
    : null);
  const service = makeService(link);
  const status = await service.panelStatus({ withCrc: true });
  assert.equal(status.crcChecked, true);

  const silentLink = makeFakeLink();
  silentLink.handler = () => null;
  const quiet = makeService(silentLink);
  const noReply = await quiet.panelStatus();
  assert.equal(noReply.valid, false);
  assert.match(noReply.error, /没有回应 facepanel/);

  const wrongSize = makeFakeLink();
  wrongSize.handler = () => '[facepanel] valid=1 crc=04F565B1 len=231424 w=480 h=480 y=0';
  const mismatch = makeService(wrongSize);
  const bad = await mismatch.panelStatus();
  assert.match(bad.error, /不符合契约/);
});

test('panelInstallStart：完整状态机（begin → 2342×w → end → info），CRC/进度正确', async () => {
  const image = blob();
  const { crcHex } = panelBlobCrc(image);
  const link = makeFakeLink({ pcMode: true, passthrough: true });
  link.handler = (line) => {
    if (line === 'facepanel') return panelStatusLine('', { valid: false });
    if (line.startsWith('facepanel begin')) return `[facepanel] begin len=${PANEL_IMAGE_LEN} crc=${crcHex} ok`;
    if (line.startsWith('facepanel w ')) return 'W-OK';
    if (line === 'facepanel end') return `[facepanel] install OK crc=${crcHex}`;
    if (line === 'facepanel info') return `${panelStatusLine(crcHex)} crc-checked`;
    return null;
  };
  const service = makeService(link);
  const started = await service.panelInstallStart(image);
  assert.equal(started.ok, true, started.error);
  await service.panelJob.finished;
  const view = service.panelInstallView();
  assert.equal(view.ok, true, `安装应成功：${view.error}`);
  assert.equal(view.stage, 'done');
  assert.equal(view.percent, 100);
  assert.equal(view.crc, crcHex);

  const lines = link.calls.filter((call) => call.type === 'command').map((call) => call.line);
  assert.equal(lines[0], 'facepanel');           // probe（先确认固件有这条命令）
  assert.equal(lines[1], `facepanel begin ${PANEL_IMAGE_LEN} ${crcHex}`);
  const writes = lines.filter((line) => line.startsWith('facepanel w '));
  assert.equal(writes.length, PANEL_UPLOAD_LINES, '2342 行（112,384 = 2341×48 + 16）');
  assert.equal(writes[0], `facepanel w 0 ${image.subarray(0, 48).toString('hex').toUpperCase()}`);
  assert.equal(
    writes.at(-1),
    `facepanel w 112368 ${image.subarray(112368).toString('hex').toUpperCase()}`,
  );
  assert.equal(lines.at(-2), 'facepanel end');
  assert.equal(lines.at(-1), 'facepanel info');
  assert.ok(link.calls.some((call) => call.type === 'shell' && call.line === 'pcmode 0'), '先退出电脑模式');

  /* 安装完成后缓存里就是"已安装" */
  const cached = service.panelView();
  assert.equal(cached.valid, true);
  assert.equal(cached.crc, crcHex);
});

test('panelInstallStart：设备侧 CRC 不一致 → 失败，状态缓存清空', async () => {
  const image = blob();
  const { crcHex } = panelBlobCrc(image);
  const link = makeFakeLink();
  link.handler = (line) => {
    if (line === 'facepanel') return panelStatusLine('', { valid: false });
    if (line.startsWith('facepanel begin')) return `[facepanel] begin len=${PANEL_IMAGE_LEN} crc=${crcHex} ok`;
    if (line.startsWith('facepanel w ')) return 'W-OK';
    if (line === 'facepanel end') return `[facepanel] install OK crc=${crcHex}`;
    if (line === 'facepanel info') return panelStatusLine('DEADBEEF');
    return null;
  };
  const service = makeService(link);
  await service.panelInstallStart(image);
  await service.panelJob.finished;
  const view = service.panelInstallView();
  assert.equal(view.ok, false);
  assert.equal(view.stage, 'error');
  assert.match(view.error, /CRC/);
  assert.equal(service.panelView().known, false, '失败后不能显示上一张的"已安装"');
});

test('panelInstallStart：W-ERR / begin 被拒 / 长度不对 都是失败路径', async () => {
  const image = blob();
  const crcHex = panelBlobCrc(image).crcHex;

  const werr = makeFakeLink();
  werr.handler = (line) => {
    if (line === 'facepanel') return panelStatusLine('', { valid: false });
    if (line.startsWith('facepanel begin')) return `[facepanel] begin len=${PANEL_IMAGE_LEN} crc=${crcHex} ok`;
    if (line.startsWith('facepanel w ')) return '[facepanel] W-ERR off=48 rc=-1 (flash error)';
    return null;
  };
  const service = makeService(werr);
  await service.panelInstallStart(image);
  await service.panelJob.finished;
  assert.match(service.panelInstallView().error, /W-ERR/);

  const refused = makeFakeLink();
  refused.handler = (line) => (line.startsWith('facepanel begin') || line === 'facepanel'
    ? '[facepanel] begin len=112384 crc=00000000 bad args/len/align'
    : null);
  const refusedService = makeService(refused);
  await refusedService.panelInstallStart(image);
  await refusedService.panelJob.finished;
  assert.match(refusedService.panelInstallView().error, /begin 被拒/);

  const short = makeService(makeFakeLink());
  const bad = await short.panelInstallStart(Buffer.alloc(10));
  assert.equal(bad.ok, false);
  assert.match(bad.error, /长度不对/);

  const offline = makeService(makeFakeLink({ connected: false }));
  assert.equal((await offline.panelInstallStart(image)).ok, false);
});

test('panelInstallStart：可取消；取消后不写 end', async () => {
  const image = blob();
  const crcHex = panelBlobCrc(image).crcHex;
  const link = makeFakeLink();
  link.handler = (line) => {
    if (line === 'facepanel') return panelStatusLine('', { valid: false });
    if (line.startsWith('facepanel begin')) return `[facepanel] begin len=${PANEL_IMAGE_LEN} crc=${crcHex} ok`;
    if (line.startsWith('facepanel w ')) return 'W-OK';
    return null;
  };
  const service = makeService(link);
  const started = service.panelInstallStart(image);
  assert.equal(service.panelInstallCancel(), true);
  await started;
  await service.panelJob.finished;
  assert.match(service.panelInstallView().error, /取消/);
  assert.equal(link.calls.some((call) => call.line === 'facepanel end'), false);
});

test('串口互斥：图片表盘与下半区面板共用一把锁（同一时刻只能一个在写）', async () => {
  const image = blob();
  const crcHex = panelBlobCrc(image).crcHex;
  const link = makeFakeLink();
  link.handler = (line) => {
    if (line === 'facepanel') return panelStatusLine('', { valid: false });
    if (line.startsWith('facepanel begin')) return `[facepanel] begin len=${PANEL_IMAGE_LEN} crc=${crcHex} ok`;
    if (line.startsWith('facepanel w ')) return 'W-OK';
    return null;
  };
  const service = makeService(link);
  const started = service.panelInstallStart(image);
  await started;
  assert.deepEqual(service.faceInstallBusy(), { face: false, panel: true, busy: true });

  const faceTry = await service.faceInstallStart(Buffer.alloc(231680).fill(1));
  assert.equal(faceTry.ok, false);
  assert.match(faceTry.error, /下半区正在写入/);
  assert.equal((await service.panelClear()).ok, false, '同一把锁也挡住 clear');

  service.panelInstallCancel();
  await service.panelJob.finished;

  /* 反向：图片表盘在跑时，面板安装 / clear 都被同一把锁挡住 */
  const busyService = makeService(makeFakeLink());
  busyService.faceJob = { running: true };
  const blocked = await busyService.panelInstallStart(image);
  assert.equal(blocked.ok, false);
  assert.match(blocked.error, /face-busy/);
  assert.equal((await busyService.panelClear()).ok, false);
  assert.deepEqual(busyService.faceInstallBusy(), { face: true, panel: false, busy: true });
});

test('panelClear：facepanel clear ok → 回内置 watch photo；ERR 被拒', async () => {
  const link = makeFakeLink();
  link.handler = (line) => (line === 'facepanel clear' ? '[facepanel] clear ok' : null);
  const service = makeService(link);
  service.panelInfo = { valid: true, crc: '04F565B1' };
  const cleared = await service.panelClear();
  assert.equal(cleared.ok, true);
  assert.equal(service.panelView().valid, false);
  assert.equal(service.panelView().crc, '');
  assert.ok(link.calls.some((call) => call.line === 'facepanel clear'));

  const failing = makeFakeLink();
  failing.handler = (line) => (line === 'facepanel clear'
    ? '[facepanel] clear flash error'
    : null);
  const failingService = makeService(failing);
  const refused = await failingService.panelClear();
  assert.equal(refused.ok, false);
  assert.match(refused.error, /clear 被拒/);

  const timeout = makeService(makeFakeLink());
  timeout.link.handler = () => null;
  const lost = await timeout.panelClear();
  assert.equal(lost.ok, false);
  assert.match(lost.error, /超时/);
});

/* ------------------------------------------------------------------ 路由 */

test('路由：/facepanel 状态、预览、安装（真实打包）、清除、/faces 带 panel', async () => {
  const { registerRoutes } = await import('../src/routes.mjs');
  const { cachePanelThumb, panelThumbPath } = await import('../src/face-cache.mjs');
  const fs = await import('node:fs');
  const os = await import('node:os');
  const path = await import('node:path');

  const cacheDir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-panel-route-'));
  process.env.PROTORIG_FACE_CACHE = cacheDir;
  try {
    let startedBlob = null;
    const panel = {
      stateForApi: () => ({ connected: true }),
      faceStatus: async () => ({ supported: true, valid: false, crc: '' }),
      faceInstallView: () => ({ running: false }),
      faceInstallBusy: () => ({ face: false, panel: false, busy: false }),
      panelView: () => ({ known: true, valid: false, crc: '' }),
      panelStatus: async () => ({ supported: true, valid: true, crc: '04F565B1', width: 480, height: 232, len: 112384, y: 248 }),
      panelInstallView: () => ({ running: false, stage: 'idle' }),
      panelInstallStart: async (blob) => {
        startedBlob = blob;
        return { ok: true, jobId: 'panel-1', job: { running: true, stage: 'starting', percent: 0 } };
      },
      panelInstallCancel: () => true,
      panelClear: async () => ({ ok: true, reply: '[facepanel] clear ok' }),
      faceSwitch: async () => ({ ok: true }),
      faceInstallStart: async () => ({ ok: true }),
      faceInstallCancel: () => true,
      enterPcMode: async () => ({ ok: true }),
      exitPcMode: async () => ({ ok: true }),
    };
    const entries = [];
    const ctx = {
      inject: (deps, fn) => fn({ webServer: { register: (entry) => { entries.push(entry); return () => {}; } } }),
    };
    registerRoutes(ctx, { panel, flash: { view: () => ({}) }, logger: silent, balance: { publicView: () => null } });

    const call = async (method, pathname, url, body) => {
      const entry = entries.find((e) => e.path === pathname);
      assert.ok(entry, `应注册 ${pathname}`);
      let text = null;
      const res = { writeHead: () => {}, end: (chunk) => { text = JSON.parse(chunk); } };
      const req = { method, url, on: (name, fn) => {
        if (name === 'end') fn();
      } };
      if (body !== undefined) {
        const raw = Buffer.from(JSON.stringify(body));
        req.on = (name, fn) => {
          if (name === 'data') fn(raw);
          if (name === 'end') fn();
        };
      }
      entry.handler(req, res);
      await settle(20);
      return text;
    };

    /* 状态：真实缓存里有 panel-<CRC>.png → thumbCached=true */
    cachePanelThumb('04F565B1', Buffer.from('89504e470d0a1a0a', 'hex'));
    const status = await call('GET', '/api/dsh-protorig-panel/facepanel', '/api/dsh-protorig-panel/facepanel');
    assert.equal(status.ok, true);
    assert.equal(status.valid, true);
    assert.equal(status.thumbCached, true);
    assert.deepEqual(status.busy, { face: false, panel: false, busy: false });

    const thumb = await call('GET', '/api/dsh-protorig-panel/facepanel/thumb', '/api/dsh-protorig-panel/facepanel/thumb?crc=04f565b1');
    assert.equal(thumb.ok, true);
    assert.match(thumb.dataUrl, /^data:image\/png;base64,/);
    const miss = await call('GET', '/api/dsh-protorig-panel/facepanel/thumb', '/api/dsh-protorig-panel/facepanel/thumb?crc=00000000');
    assert.equal(miss.error, 'no-cache');

    /* 安装：真跑 packPanelImageBuffer（480×232 PNG），断言交给 service 的 blob 长度 */
    const canvas = createCanvas(800, 600);
    const context = canvas.getContext('2d');
    context.fillStyle = '#135724';
    context.fillRect(0, 0, 800, 600);
    const dataUrl = `data:image/png;base64,${canvas.toBuffer('image/png').toString('base64')}`;
    const installed = await call('POST', '/api/dsh-protorig-panel/facepanel/install', '/api/dsh-protorig-panel/facepanel/install', { image: dataUrl });
    assert.equal(installed.ok, true, installed.error);
    assert.equal(startedBlob.length, PANEL_IMAGE_LEN);
    assert.equal(installed.packed.size, PANEL_IMAGE_LEN);
    assert.equal(installed.packed.width, 480);
    assert.equal(installed.packed.height, 232);
    assert.equal(installed.thumbCached, true);
    assert.ok(fs.existsSync(panelThumbPath(installed.packed.crc)), '应缓存 panel-<CRC>.png');

    const cleared = await call('POST', '/api/dsh-protorig-panel/facepanel/clear', '/api/dsh-protorig-panel/facepanel/clear', {});
    assert.equal(cleared.ok, true);

    const faces = await call('GET', '/api/dsh-protorig-panel/faces', '/api/dsh-protorig-panel/faces');
    assert.ok(faces.panel, '/faces 应带面板缓存视图');
    assert.deepEqual(faces.busy, { face: false, panel: false, busy: false });

    const missing = await call('POST', '/api/dsh-protorig-panel/facepanel/install', '/api/dsh-protorig-panel/facepanel/install', {});
    assert.equal(missing.ok, false);
    assert.match(missing.error, /缺少 image/);
  } finally {
    delete process.env.PROTORIG_FACE_CACHE;
    fs.rmSync(cacheDir, { recursive: true, force: true });
  }
});

test('本地预览缓存：panel-<CRC>.png 往返，且不进图片表盘的缩略图列表', async () => {
  const {
    cachePanelThumb, cacheThumb, hasPanelThumb, listThumbs, panelThumbPath, readPanelThumb, removePanelThumb,
  } = await import('../src/face-cache.mjs');
  const fs = await import('node:fs');
  const os = await import('node:os');
  const path = await import('node:path');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-panel-cache-'));
  process.env.PROTORIG_FACE_CACHE = dir;
  try {
    const png = Buffer.from('89504e470d0a1a0a', 'hex');
    assert.equal(hasPanelThumb('04F565B1'), false);
    const file = cachePanelThumb('04f565b1', png);
    assert.equal(path.basename(file), 'panel-04F565B1.png');
    assert.equal(panelThumbPath('04F565B1'), path.join(dir, 'panel-04F565B1.png'));
    assert.deepEqual(readPanelThumb('04F565B1'), png);
    assert.equal(hasPanelThumb('04F565B1'), true);
    assert.equal(cacheThumb('04F565B1', png) !== '', true);
    assert.equal(listThumbs().length, 1, 'listThumbs 只列 480×480 表盘缩略图');
    assert.equal(listThumbs()[0].crc, '04F565B1');
    assert.equal(removePanelThumb('04F565B1'), true);
    assert.equal(hasPanelThumb('04F565B1'), false);
    assert.equal(cachePanelThumb('bad', png), '', '非法 CRC 不落盘');
    assert.equal(cachePanelThumb('AABBCCDD', Buffer.alloc(0)), '');
  } finally {
    delete process.env.PROTORIG_FACE_CACHE;
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
