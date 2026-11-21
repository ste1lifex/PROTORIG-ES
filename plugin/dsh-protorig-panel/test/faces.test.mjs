/*
 * faces.test.mjs —— 表盘切换（P8）与图片表盘安装（P9）的状态机测试
 *
 * 不碰真串口/真手表：把 PanelService.link 换成 EventEmitter 替身，
 * 按固件 shell 的真实应答格式回包（main.c 的 cmd_face / cmd_faceimg）。
 */
import './_env.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { PanelService } from '../src/panel-service.mjs';
import { IMAGE_LEN } from '../src/face-pack.mjs';
import { PANEL_W, PANEL_H, PANEL_IMAGE_LEN, applyPanelVignette, loadPanelMask, packPanelRgba } from '../src/face-panel.mjs';
import { FACE_IMAGE_W, FACE_IMAGE_H, packRgba } from '../src/face-pack.mjs';

const silent = { info() {}, warn() {}, error() {}, debug() {} };
const settle = (ms = 10) => new Promise((resolve) => setTimeout(resolve, ms));

function makeFakeLink({ connected = true, pcMode = false, passthrough = false } = {}) {
  const link = new EventEmitter();
  link.state = { connected, pcMode, passthrough, hello: null, path: '/dev/cu.FAKE', adminId: '2BEA4E' };
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
    if (line === 'pcmode 0') { link.state.pcMode = false; link.state.passthrough = false; }
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

const solidPfi = () => {
  const rgba = new Uint8Array(FACE_IMAGE_W * FACE_IMAGE_H * 4);
  for (let i = 0; i < rgba.length; i += 4) {
    rgba[i] = 20;
    rgba[i + 1] = 40;
    rgba[i + 2] = 60;
    rgba[i + 3] = 255;
  }
  return packRgba(rgba, {}, { dither: false });
};

const statusLine = (crc, { valid = true } = {}) => (valid
  ? `[faceimg] valid=1 crc=${crc} len=${IMAGE_LEN} w=480 h=480 time=(110,170) font=1 flags=3 color=FFFA00`
  : '[faceimg] valid=0 rc=-4 (state error (begin? received?))');

test('faceStatus：解析 faceimg 有效状态行', async () => {
  const link = makeFakeLink();
  link.handler = (line) => (line === 'faceimg' ? statusLine('0A1B2C3D') : null);
  const service = makeService(link);
  const status = await service.faceStatus();
  assert.equal(status.supported, true);
  assert.equal(status.valid, true);
  assert.equal(status.crc, '0A1B2C3D');
  assert.equal(status.len, IMAGE_LEN);
  assert.deepEqual(status.layout, { timeX: 110, timeY: 170, fontId: 1, flags: 3, color: 0xfffa00 });
});

test('faceStatus：无图时给出 rc/原因；离线时说明需要 USB', async () => {
  const link = makeFakeLink();
  link.handler = (line) => (line === 'faceimg' ? statusLine('', { valid: false }) : null);
  const service = makeService(link);
  const status = await service.faceStatus();
  assert.equal(status.valid, false);
  assert.equal(status.rc, -4);
  assert.match(status.reason, /begin/);

  const offline = makeService(makeFakeLink({ connected: false }));
  const offStatus = await offline.faceStatus();
  assert.equal(offStatus.supported, false);
  assert.match(offStatus.error, /USB/);
});

test('faceSwitch：先退电脑模式，再发 face N（手表模式下才看得到表盘）', async () => {
  const link = makeFakeLink({ pcMode: true, passthrough: true });
  link.handler = (line) => (line.startsWith('face ') ? `face -> ${line.split(' ')[1]} (0=default 3=image)` : null);
  const service = makeService(link);
  const result = await service.faceSwitch(3);
  assert.equal(result.ok, true);
  assert.equal(result.id, 3);
  assert.equal(service.faceId, 3);
  assert.equal(link.state.pcMode, true, '切完表盘应自动回到电脑模式');
  const calls = link.calls.map((c) => `${c.type}:${c.line}`);
  assert.deepEqual(calls.slice(0, 2), ['shell:pcmode 0', 'command:face 3']);
  assert.ok(calls.includes('shell:pcmode 1'), '应自动发 pcmode 1 回电脑模式');
  assert.ok(link.calls.some((c) => c.type === 'hello'), '应重新握手恢复透传');
});

test('faceSwitch：退出 PC 模式后第一条命令丢包 → 自动重试一次', async () => {
  const link = makeFakeLink({ pcMode: true, passthrough: true });
  let seen = 0;
  link.handler = (line) => {
    if (!line.startsWith('face ')) return null;
    seen += 1;
    return seen === 1 ? null : `face -> ${line.split(' ')[1]} (0=default 3=image)`;
  };
  const service = makeService(link);
  const result = await service.faceSwitch(3);
  assert.equal(result.ok, true, '重试后应成功');
  assert.equal(seen, 2);
  assert.equal(service.faceId, 3);
});

test('faceSwitch：face 1/2 已从固件删除 → 直接拒绝，不发任何命令', async () => {
  const link = makeFakeLink({ pcMode: true, passthrough: true });
  link.handler = (line) => (line.startsWith('face ') ? 'face -> 0 (0=default 3=image)' : null);
  const service = makeService(link);
  for (const id of [1, 2, 9, -1, 'x']) {
    const result = await service.faceSwitch(id);
    assert.equal(result.ok, false, `id=${id} 应被拒绝`);
    assert.match(result.error, /0（默认）|3（图片）/);
  }
  assert.deepEqual(link.calls, [], '被拒绝时不能发 face / pcmode 命令');
  assert.equal(service.faceId, 0);

  const ok = await service.faceSwitch(0);
  assert.equal(ok.ok, true);
  assert.equal(service.faceId, 0);
});

test('faceInstallStart：完整状态机（erase → begin → 4822×w → end → info → apply）', async () => {
  const packed = solidPfi();
  const crc = packed.crcHex;
  // 从电脑模式开始：安装前应该先发 pcmode 0 回到手表模式
  const link = makeFakeLink({ pcMode: true, passthrough: true });
  link.handler = (line) => {
    if (line === 'faceimg') return statusLine('', { valid: false });
    if (line === 'faceimg erase') return '[faceimg] erase ok';
    if (line.startsWith('faceimg begin')) return `[faceimg] begin len=${IMAGE_LEN} crc=${crc} ok`;
    if (line.startsWith('faceimg w ')) return 'W-OK';
    if (line.startsWith('faceimg end ')) return `[faceimg] install OK crc=${crc}`;
    if (line === 'faceimg info') return `${statusLine(crc)} crc-checked`;
    if (line === 'faceimg apply') return '[faceimg] apply queued (face 3)';
    return null;
  };
  const service = makeService(link);
  const started = await service.faceInstallStart(packed.pfi, { apply: true });
  assert.equal(started.ok, true);
  await service.faceJob.finished;
  const view = service.faceInstallView();
  assert.equal(view.ok, true, `安装应成功：${view.error}`);
  assert.equal(view.stage, 'done');
  assert.equal(view.percent, 100);
  assert.equal(view.crc, crc);
  assert.equal(service.faceId, 3, 'apply 后插件侧记住 face 3');
  await settle(60);
  const restoreSeq = link.calls.filter((c) => c.type === 'shell' || c.type === 'hello').map((c) => `${c.type}:${c.line ?? ''}`);
  assert.ok(restoreSeq.includes('shell:pcmode 1'), '装完应自动回电脑模式');
  assert.equal(link.state.pcMode, true);

  const wCalls = link.calls.filter((c) => c.type === 'command' && c.line.startsWith('faceimg w '));
  assert.equal(wCalls.length, Math.ceil(IMAGE_LEN / 48), '48 B/行，共 4822 行');
  assert.equal(wCalls[0].line, `faceimg w 0 ${packed.blob.subarray(0, 48).toString('hex').toUpperCase()}`);
  const seq = link.calls.filter((c) => c.type === 'command' || c.type === 'shell').map((c) => c.line);
  assert.ok(seq.includes('shell:pcmode 0') || seq.includes('pcmode 0'), '先退出电脑模式');
});

test('faceInstallStart：设备侧 CRC 不一致 → 任务失败、不报成功', async () => {
  const packed = solidPfi();
  const link = makeFakeLink();
  link.handler = (line) => {
    if (line === 'faceimg') return statusLine('', { valid: false });
    if (line === 'faceimg erase') return '[faceimg] erase ok';
    if (line.startsWith('faceimg begin')) return `[faceimg] begin len=${IMAGE_LEN} crc=${packed.crcHex} ok`;
    if (line.startsWith('faceimg w ')) return 'W-OK';
    if (line.startsWith('faceimg end ')) return `[faceimg] install OK crc=${packed.crcHex}`;
    if (line === 'faceimg info') return statusLine('DEADBEEF');
    return null;
  };
  const service = makeService(link);
  await service.faceInstallStart(packed.pfi);
  await service.faceJob.finished;
  const view = service.faceInstallView();
  assert.equal(view.ok, false);
  assert.equal(view.stage, 'error');
  assert.match(view.error, /CRC|不一致/);
});

test('faceInstallStart：W-ERR 立即失败（截图/擦除异常路径）', async () => {
  const packed = solidPfi();
  const link = makeFakeLink();
  link.handler = (line) => {
    if (line === 'faceimg') return statusLine('', { valid: false });
    if (line === 'faceimg erase') return '[faceimg] erase ok';
    if (line.startsWith('faceimg begin')) return `[faceimg] begin len=${IMAGE_LEN} crc=${packed.crcHex} ok`;
    if (line.startsWith('faceimg w ')) return '[faceimg] W-ERR off=48 rc=-1 (bad args/len/align)';
    return null;
  };
  const service = makeService(link);
  await service.faceInstallStart(packed.pfi);
  await service.faceJob.finished;
  const view = service.faceInstallView();
  assert.equal(view.ok, false);
  assert.match(view.error, /W-ERR/);
});

test('faceInstallStart：离线拒绝、忙时拒绝、坏 PFI 拒绝', async () => {
  const offline = makeService(makeFakeLink({ connected: false }));
  assert.equal((await offline.faceInstallStart(solidPfi().pfi)).ok, false);

  const link = makeFakeLink();
  link.handler = (line) => {
    if (line === 'faceimg') return statusLine('', { valid: false });
    if (line === 'faceimg erase') return '[faceimg] erase ok';
    if (line.startsWith('faceimg begin')) return `[faceimg] begin len=${IMAGE_LEN} crc=00000000 ok`;
    if (line.startsWith('faceimg w ')) return 'W-OK';
    return null;
  };
  const service = makeService(link);
  // faceInstallStart 的同步部分会先建好 job；不 await 才能抢在上传循环前取消
  const startedPromise = service.faceInstallStart(solidPfi().pfi);
  assert.equal((await service.faceInstallCancel()), true);
  await startedPromise;
  await service.faceJob.finished;
  assert.match(String(service.faceInstallView().error), /取消/);

  const bad = await service.faceInstallStart(Buffer.alloc(10));
  assert.equal(bad.ok, false);
  assert.match(bad.error, /PFI/);
});

test('enterPcMode：发 pcmode 1 + HELLO，恢复透传与整屏重发', async () => {
  const link = makeFakeLink({ pcMode: false });
  const service = makeService(link);
  const result = await service.enterPcMode();
  assert.equal(result.ok, true);
  assert.equal(link.state.pcMode, true);
  assert.equal(link.state.passthrough, true);
  assert.ok(link.calls.some((c) => c.type === 'shell' && c.line === 'pcmode 1'));
  assert.ok(link.calls.some((c) => c.type === 'hello'));
});

test('路由：/faces/thumb 命中缓存返回 dataURL、未缓存返回 no-cache；/faces 带 thumbCached', async () => {
  const { registerRoutes } = await import('../src/routes.mjs');
  const { cacheThumb } = await import('../src/face-cache.mjs');
  const fs = await import('node:fs');
  const os = await import('node:os');
  const path = await import('node:path');

  const cacheDir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-face-route-'));
  process.env.PROTORIG_FACE_CACHE = cacheDir;
  try {
    cacheThumb('DBF3F247', Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]));
    const entries = [];
    const ctx = {
      inject: (deps, fn) => fn({ webServer: { register: (entry) => { entries.push(entry); return () => {}; } } }),
    };
    const panel = {
      stateForApi: () => ({ connected: true }),
      faceStatus: async () => ({ supported: true, valid: true, crc: 'DBF3F247' }),
      faceInstallView: () => ({ running: false }),
      faceInstallStart: async () => ({ ok: true, jobId: 't' }),
      faceSwitch: async () => ({ ok: true }),
      faceInstallCancel: () => true,
      enterPcMode: async () => ({ ok: true }),
      exitPcMode: async () => ({ ok: true }),
    };
    registerRoutes(ctx, { panel, flash: { view: () => ({}) }, logger: silent, balance: { publicView: () => null } });

    const call = async (pathname, url) => {
      const entry = entries.find((e) => e.path === pathname);
      assert.ok(entry, `应注册 ${pathname}`);
      let body = null;
      const res = { writeHead: () => {}, end: (text) => { body = JSON.parse(text); } };
      entry.handler({ method: 'GET', url }, res);
      await settle(10);
      return body;
    };

    const hit = await call('/api/dsh-protorig-panel/faces/thumb', '/api/dsh-protorig-panel/faces/thumb?crc=DBF3F247');
    assert.equal(hit.ok, true);
    assert.match(hit.dataUrl, /^data:image\/png;base64,/);
    assert.equal(hit.crc, 'DBF3F247');

    const miss = await call('/api/dsh-protorig-panel/faces/thumb', '/api/dsh-protorig-panel/faces/thumb?crc=00000000');
    assert.equal(miss.ok, false);
    assert.equal(miss.error, 'no-cache');

    const faces = await call('/api/dsh-protorig-panel/faces', '/api/dsh-protorig-panel/faces');
    assert.equal(faces.thumbCached, true, '图片槽 CRC 有缓存 → thumbCached=true');
    assert.equal(faces.valid, true);

    /* 裁剪框 WYSIWYG 用的内置暗角 mask：同一份资产，base64 PNG */
    const mask = await call('/api/dsh-protorig-panel/faces/panel-mask', '/api/dsh-protorig-panel/faces/panel-mask');
    assert.equal(mask.ok, true);
    assert.match(mask.dataUrl, /^data:image\/png;base64,/);
    assert.ok(mask.dataUrl.length > 1000, 'mask PNG 不应为空');
  } finally {
    delete process.env.PROTORIG_FACE_CACHE;
    fs.rmSync(cacheDir, { recursive: true, force: true });
  }
});

test('下半区边缘羽化：直接用内置 watch photo 的暗角 mask（中心不变、上边也渐黑）', async () => {
  const mask = await loadPanelMask();
  assert.equal(mask.width, PANEL_W);
  assert.equal(mask.height, PANEL_H);
  const rgba = new Uint8Array(PANEL_W * PANEL_H * 4).fill(255);
  applyPanelVignette(rgba, PANEL_W, PANEL_H, { mask: mask.alpha });
  const px = (x, y) => rgba[(y * PANEL_W + x) * 4];
  assert.equal(px(240, 116), 255, '中心保持原图（底部文字压暗不算羽化）');
  assert.equal(px(240, 0), 0, '上边中点 = 内置图顶部渐隐起点，必须为黑（旧实现是 255）');
  assert.ok(px(240, 10) > 0 && px(240, 10) < 128, '上边过渡带是低值而不是全亮');
  assert.equal(px(240, 28), 255, '顶部渐隐结束于约 28 px');
  assert.equal(px(0, 116), 0, '左中在表盘圆外 → 全黑');
  assert.equal(px(479, 116), 0, '右中同理 → 全黑');
  assert.equal(px(240, 231), 0, '下边中点在圆外 → 全黑');
  assert.equal(px(0, 0), 0, '角落在圆外 → 全黑');
  assert.ok(px(390, 116) > 0 && px(390, 116) < 255, '圆边过渡带是中间值');
  assert.equal(px(420, 116), 0, '圆边外已全黑');
  for (let i = 3; i < rgba.length; i += 4) assert.equal(rgba[i], 255, 'mask 后 alpha 保持 255');
});

test('纯白面板套 mask 打包：解码 palette+索引后中心白、四角/上边/下边黑', async () => {
  const { packPanelRgba } = await import('../src/face-panel.mjs');
  const mask = await loadPanelMask();
  const rgba = new Uint8Array(PANEL_W * PANEL_H * 4).fill(255);
  applyPanelVignette(rgba, PANEL_W, PANEL_H, { mask: mask.alpha });
  const packed = packPanelRgba(rgba, { dither: false });
  assert.equal(packed.blob.length, PANEL_W * PANEL_H + 1024);
  const rgbAt = (x, y) => {
    const index = packed.blob[1024 + y * PANEL_W + x];
    return packed.palette[index];
  };
  assert.deepEqual(rgbAt(240, 116), [255, 255, 255], '中心白');
  for (const [x, y, label] of [
    [0, 0, '左上角'], [479, 0, '右上角'], [0, 231, '左下角'], [479, 231, '右下角'],
    [240, 0, '上边中点'], [0, 116, '左边中点'], [479, 116, '右边中点'], [240, 231, '下边中点'],
  ]) {
    assert.deepEqual(rgbAt(x, y), [0, 0, 0], `${label}为黑`);
  }
  const mid = rgbAt(390, 116);
  assert.ok(mid[0] > 0 && mid[0] < 255, '圆边过渡带是中间灰度');
});

test('panelClear：清完下半区自动回电脑模式（USB）', async () => {
  const link = makeFakeLink({ pcMode: true, passthrough: true });
  link.handler = (line) => {
    if (line === 'facepanel') return '[facepanel] valid=0 rc=-5 (bad header)';
    if (line === 'facepanel clear') return '[facepanel] clear ok';
    return null;
  };
  const service = makeService(link);
  const result = await service.panelClear();
  assert.equal(result.ok, true, result.error);
  await settle(60);
  assert.equal(link.state.pcMode, true, '清完应自动回电脑模式');
  const seq = link.calls.filter((c) => c.type === 'shell' || c.type === 'hello').map((c) => `${c.type}:${c.line ?? ''}`);
  assert.ok(seq.includes('shell:pcmode 1'), '应发 pcmode 1');
  assert.ok(seq.includes('hello:'), '应重新握手');
});

test('面板安装：end OK 但 info 超时 → 成功 + warning，不再误报失败', async () => {
  const rgba = new Uint8Array(PANEL_W * PANEL_H * 4).fill(255);
  const { blob } = packPanelRgba(rgba, { dither: false });
  assert.equal(blob.length, PANEL_IMAGE_LEN);
  const link = makeFakeLink({ pcMode: true, passthrough: true });
  link.handler = (line) => {
    if (line === 'facepanel') return '[facepanel] valid=0 rc=-5 (bad header)';
    if (line.startsWith('facepanel begin')) return `[facepanel] begin len=${PANEL_IMAGE_LEN} crc=00000000 ok`;
    if (line.startsWith('facepanel w ')) return 'W-OK';
    if (line === 'facepanel end') return '[facepanel] install OK crc=00000000';
    if (line === 'facepanel info') return null;
    return null;
  };
  const service = makeService(link);
  const started = await service.panelInstallStart(blob);
  assert.equal(started.ok, true, started.error);
  await service.panelJob.finished;
  await settle(80);
  const view = service.panelInstallView();
  assert.equal(view.ok, true, `不应误报失败：${view.error}`);
  assert.equal(view.stage, 'done');
  assert.match(String(view.warning || ''), /超时|复核/);
  assert.equal(link.state.pcMode, true, '仍应自动回电脑模式');
});
