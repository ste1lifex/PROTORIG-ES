/*
 * routes.mjs —— 设置页 / 外部脚本用的本机 HTTP 路由
 *
 *   GET  /api/dsh-protorig-panel/state          面板状态（连接/模式/电池/亮度/设备身份/平台能力）
 *   GET  /api/dsh-protorig-panel/devices        候选设备（?refresh=1 重新枚举；?probe=1 逐口问 adminid）
 *   POST /api/dsh-protorig-panel/reconnect      重连手表（清身份缓存后按绑定重新挑口）
 *   POST /api/dsh-protorig-panel/disconnect     断开车表并暂停自动重连
 *   POST /api/dsh-protorig-panel/identify       震动识别（确认在操作哪块表）
 *   POST /api/dsh-protorig-panel/cache/reset    重置下发缓存（调试用）
 *   POST /api/dsh-protorig-panel/preflight      {bin} → 文件大小 + sha256 前 8 位
 *   POST /api/dsh-protorig-panel/flash          {bin, baud?} → 启动烧录任务 {jobId}（仅 Windows）
 *   GET  /api/dsh-protorig-panel/flash/state    当前/最后任务 {job}
 *   POST /api/dsh-protorig-panel/flash/cancel   取消当前任务
 *   GET  /api/dsh-protorig-panel/faces          表盘状态（当前 face + 图片槽 + 安装任务 + 面板缓存）
 *   POST /api/dsh-protorig-panel/faces/status   重算设备侧图片 CRC（慢）
 *   POST /api/dsh-protorig-panel/faces/switch   {id 0|3} 切表盘
 *   GET  /api/dsh-protorig-panel/faces/thumb    ?crc=XXXXXXXX 本地缩略图（dataURL）
 *   POST /api/dsh-protorig-panel/faces/install  {image(dataURL/base64), layout?, apply?} 装图（顺带缓存缩略图）
 *   GET  /api/dsh-protorig-panel/faces/install/state 安装进度
 *   POST /api/dsh-protorig-panel/faces/install/cancel 取消安装
 *   GET  /api/dsh-protorig-panel/facepanel      默认表盘下半区状态（?crc=1 让固件重算 CRC）
 *   POST /api/dsh-protorig-panel/facepanel/install  {image(dataURL/base64)} 装 480×232 面板（顺带缓存预览）
 *   GET  /api/dsh-protorig-panel/facepanel/install/state 面板安装进度
 *   POST /api/dsh-protorig-panel/facepanel/install/cancel 取消面板安装
 *   POST /api/dsh-protorig-panel/facepanel/clear 恢复内置 watch photo（facepanel clear）
 *   GET  /api/dsh-protorig-panel/facepanel/thumb ?crc=XXXXXXXX 面板预览（dataURL）
 *   GET  /api/dsh-protorig-panel/faces/panel-mask 内置 watch photo 的羽化 alpha（裁剪框 WYSIWYG）
 *   POST /api/dsh-protorig-panel/pcmode         {enter:true|false} 进出电脑模式
 *   GET  /api/dsh-protorig-panel/balance        余额视图
 *   POST /api/dsh-protorig-panel/balance/refresh 强制刷新余额
 *
 * 只在有 webServer 的 profile（web / desktop）注册；headless 里自动跳过。
 */

import { readFile } from 'node:fs/promises';
import { packImageBuffer } from './face-pack.mjs';
import { packPanelImageBuffer, panelMaskPng } from './face-panel.mjs';
import {
  cachePanelThumb,
  cacheThumb,
  hasPanelThumb,
  hasThumb,
  readPanelThumb,
  readThumb,
} from './face-cache.mjs';

const MAX_BODY = 64 * 1024;
/** 图片上传（base64）单独放宽：13 MB 原图编成 base64 约 17 MB，留余量。 */
const MAX_IMAGE_BODY = 24 * 1024 * 1024;

function json(res, status, body) {
  const text = JSON.stringify(body);
  res.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'cache-control': 'no-store',
  });
  res.end(text);
}

function readJson(req, maxBody = MAX_BODY) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > maxBody) {
        reject(new Error('body too large'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      if (!size) {
        resolve({});
        return;
      }
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString('utf8')));
      } catch (error) {
        reject(new Error(`invalid json: ${error.message}`));
      }
    });
    req.on('error', reject);
  });
}

function route(method, pathname, run) {
  return {
    kind: 'exact',
    path: pathname,
    handler: (req, res) => {
      if (req.method !== method) {
        json(res, 405, { ok: false, error: 'method-not-allowed' });
        return;
      }
      Promise.resolve(run(req))
        .then((value) => json(res, 200, value))
        .catch((error) => json(res, 500, { ok: false, error: error instanceof Error ? error.message : String(error) }));
    },
  };
}

/** 读 ?key=value（设置页"刷新设备列表"用）。 */
function query(req) {
  try {
    return new URL(req?.url ?? '/', 'http://localhost').searchParams;
  } catch {
    return new URLSearchParams();
  }
}

/** 注册路由；返回 disposer 列表（供插件卸载）。 */
export function registerRoutes(ctx, { panel, flash, logger, balance }) {
  if (typeof ctx?.inject !== 'function') return;
  ctx.inject(['webServer'], (sctx) => {
    const routes = [
      route('GET', '/api/dsh-protorig-panel/state', () => ({
        ok: true,
        state: panel.stateForApi(),
        flash: flash.view(),
      })),
      route('POST', '/api/dsh-protorig-panel/devices/probe/usb', async () => {
        const result = typeof panel.probeUsbDevices === 'function' ? await panel.probeUsbDevices() : {};
        return { ok: true, ...result, state: panel.stateForApi() };
      }),
      route('POST', '/api/dsh-protorig-panel/devices/probe/ble', async () => {
        const result = typeof panel.probeBleDevices === 'function' ? await panel.probeBleDevices() : {};
        return { ok: true, ...result, state: panel.stateForApi() };
      }),
      route('GET', '/api/dsh-protorig-panel/devices', async (req) => {
        const params = query(req);
        const devices = typeof panel.listDevices === 'function'
          ? await panel.listDevices({
            refresh: params.get('refresh') === '1',
            probe: params.get('probe') === '1',
          })
          : {};
        return { ok: true, ...devices, state: panel.stateForApi() };
      }),
      route('POST', '/api/dsh-protorig-panel/reconnect', async () => {
        const result = typeof panel.reconnectNow === 'function' ? await panel.reconnectNow() : {};
        return { ok: true, ...result, state: panel.stateForApi() };
      }),
      route('POST', '/api/dsh-protorig-panel/disconnect', async () => {
        const result = typeof panel.manualDisconnect === 'function' ? await panel.manualDisconnect() : {};
        return { ok: true, ...result, state: panel.stateForApi() };
      }),
      route('POST', '/api/dsh-protorig-panel/identify', async () => {
        const result = typeof panel.identify === 'function'
          ? await panel.identify()
          : { ok: false, error: 'not-supported' };
        return { ...result, ok: result.ok !== false, state: panel.stateForApi() };
      }),
      route('POST', '/api/dsh-protorig-panel/cache/reset', () => {
        panel.resetCaches?.();
        return { ok: true, state: panel.stateForApi() };
      }),
      route('POST', '/api/dsh-protorig-panel/preflight', async (req) => {
        const body = await readJson(req);
        const pre = await flash.preflight(body?.bin);
        return { ok: pre.ok, ...pre };
      }),
      route('POST', '/api/dsh-protorig-panel/flash', async (req) => {
        const body = await readJson(req);
        const started = await flash.start(body?.bin, {
          baud: typeof body?.baud === 'number' ? body.baud : undefined,
        });
        return { ...started, job: flash.view() };
      }),
      /* ---- 表盘（P8/P9） ---- */
      route('GET', '/api/dsh-protorig-panel/faces', async () => {
        const status = await panel.faceStatus().catch((error) => ({
          supported: false,
          current: panel.faceId ?? 0,
          valid: false,
          error: error.message,
        }));
        const thumbCached = status.valid === true && hasThumb(status.crc);
        return {
          ok: true,
          ...status,
          thumbCached,
          job: panel.faceInstallView?.() ?? null,
          /* 面板状态只带缓存（这里不发 shell：/faces 已经每 2.5 s 问一次 faceimg）；要新鲜值用 /facepanel */
          panel: panel.panelView?.() ?? null,
          busy: panel.faceInstallBusy?.() ?? null,
          state: panel.stateForApi(),
        };
      }),
      /* 本地缩略图缓存（按 image_crc32 索引）：装过图才有；没有就 404，前端显示占位 */
      route('GET', '/api/dsh-protorig-panel/faces/preview', async () => {
        /* 默认表盘（固件内置）的静态预览：由 assets/faces/default.png 生成，
         * 免得设置页两张卡片都是黑框。图片表盘仍用装图时缓存的真实缩略图。 */
        const png = await readFile(new URL('../assets/faces/default.png', import.meta.url));
        return { ok: true, dataUrl: `data:image/png;base64,${png.toString('base64')}` };
      }),
      route('GET', '/api/dsh-protorig-panel/facepanel/builtin-preview', async () => {
        const png = await readFile(new URL('../assets/faces/builtin-panel.png', import.meta.url));
        return { ok: true, dataUrl: `data:image/png;base64,${png.toString('base64')}` };
      }),
      /* 内置 watch photo 的羽化 alpha：客户端裁剪框直接 destination-in 这张图，
       * 与服务端打包用的是同一份 mask（不再在浏览器里重算径向公式）。 */
      route('GET', '/api/dsh-protorig-panel/faces/panel-mask', async () => {
        const png = await panelMaskPng();
        return { ok: true, dataUrl: `data:image/png;base64,${png.toString('base64')}` };
      }),
      route('GET', '/api/dsh-protorig-panel/faces/thumb', async (req) => {
        const params = query(req);
        const crc = params.get('crc') || '';
        const png = readThumb(crc);
        if (!png) return { ok: false, error: 'no-cache', crc: String(crc).toUpperCase() };
        return {
          ok: true,
          crc: String(crc).toUpperCase(),
          mime: 'image/png',
          size: png.length,
          dataUrl: `data:image/png;base64,${png.toString('base64')}`,
        };
      }),
      route('POST', '/api/dsh-protorig-panel/faces/status', async () => {
        const status = await panel.faceStatus({ withCrc: true }).catch((error) => ({ supported: false, error: error.message }));
        return { ok: status.supported !== false, ...status };
      }),
      route('POST', '/api/dsh-protorig-panel/faces/switch', async (req) => {
        const body = await readJson(req);
        const result = await panel.faceSwitch(body?.id);
        return { ...result, state: panel.stateForApi() };
      }),
      route('POST', '/api/dsh-protorig-panel/faces/install', async (req) => {
        const body = await readJson(req, MAX_IMAGE_BODY);
        const image = String(body?.image ?? '');
        if (!image) return { ok: false, error: '缺少 image（dataURL 或 base64 的 PNG/JPG）' };
        const base64 = image.includes(',') ? image.slice(image.indexOf(',') + 1) : image;
        let buffer;
        try {
          buffer = Buffer.from(base64, 'base64');
        } catch {
          return { ok: false, error: '图片 base64 解码失败' };
        }
        if (!buffer.length) return { ok: false, error: '图片为空' };
        let packed;
        try {
          packed = await packImageBuffer(buffer, body?.layout ?? {}, { dither: body?.dither !== false });
        } catch (error) {
          return { ok: false, error: `图片转换失败：${error.message}` };
        }
        /* 本地缩略图缓存：按 CRC 存 480×480 预览（"看到的就是装进去的"） */
        let thumbCached = false;
        try {
          thumbCached = Boolean(cacheThumb(packed.crcHex, packed.previewPng));
        } catch {
          /* 缓存失败不影响安装 */
        }
        const started = await panel.faceInstallStart(packed.pfi, { apply: body?.apply !== false });
        return {
          ...started,
          thumbCached,
          packed: { crc: packed.crcHex, layout: packed.layout, size: packed.size },
        };
      }),
      route('GET', '/api/dsh-protorig-panel/faces/install/state', () => ({ ok: true, job: panel.faceInstallView() })),
      route('POST', '/api/dsh-protorig-panel/faces/install/cancel', () => {
        const cancelled = panel.faceInstallCancel?.() ?? false;
        return { ok: true, cancelled, job: panel.faceInstallView() };
      }),
      /* ---- 默认表盘下半区（research/76 §2.3） ---- */
      route('GET', '/api/dsh-protorig-panel/facepanel', async (req) => {
        const params = query(req);
        const status = await panel.panelStatus({ withCrc: params.get('crc') === '1' }).catch((error) => ({
          supported: false,
          valid: false,
          crc: '',
          error: error.message,
        }));
        const thumbCached = status.valid === true && hasPanelThumb(status.crc);
        return {
          ok: status.supported !== false,
          ...status,
          thumbCached,
          job: panel.panelInstallView?.() ?? null,
          busy: panel.faceInstallBusy?.() ?? null,
          state: panel.stateForApi(),
        };
      }),
      /* 本地预览缓存（按面板 image_crc32 索引，文件名 panel-<CRC>.png） */
      route('GET', '/api/dsh-protorig-panel/facepanel/thumb', async (req) => {
        const params = query(req);
        const crc = params.get('crc') || '';
        const png = readPanelThumb(crc);
        if (!png) return { ok: false, error: 'no-cache', crc: String(crc).toUpperCase() };
        return {
          ok: true,
          crc: String(crc).toUpperCase(),
          mime: 'image/png',
          size: png.length,
          dataUrl: `data:image/png;base64,${png.toString('base64')}`,
        };
      }),
      route('POST', '/api/dsh-protorig-panel/facepanel/install', async (req) => {
        const body = await readJson(req, MAX_IMAGE_BODY);
        const image = String(body?.image ?? '');
        if (!image) return { ok: false, error: '缺少 image（dataURL 或 base64 的 PNG/JPG）' };
        const base64 = image.includes(',') ? image.slice(image.indexOf(',') + 1) : image;
        let buffer;
        try {
          buffer = Buffer.from(base64, 'base64');
        } catch {
          return { ok: false, error: '图片 base64 解码失败' };
        }
        if (!buffer.length) return { ok: false, error: '图片为空' };
        let packed;
        try {
          /* cover 480×232 → 256 色 → I8（1024 B palette + 111,360 B 索引 = 112,384 B）→ CRC32 */
          packed = await packPanelImageBuffer(buffer, { dither: body?.dither !== false });
        } catch (error) {
          return { ok: false, error: `面板图转换失败：${error.message}` };
        }
        let thumbCached = false;
        try {
          thumbCached = Boolean(cachePanelThumb(packed.crcHex, packed.previewPng));
        } catch {
          /* 预览缓存失败不影响安装 */
        }
        const started = await panel.panelInstallStart(packed.blob);
        return {
          ...started,
          thumbCached,
          packed: {
            crc: packed.crcHex, size: packed.size, width: packed.width, height: packed.height,
          },
        };
      }),
      route('GET', '/api/dsh-protorig-panel/facepanel/install/state', () => ({ ok: true, job: panel.panelInstallView() })),
      route('POST', '/api/dsh-protorig-panel/facepanel/install/cancel', () => {
        const cancelled = panel.panelInstallCancel?.() ?? false;
        return { ok: true, cancelled, job: panel.panelInstallView() };
      }),
      route('POST', '/api/dsh-protorig-panel/facepanel/clear', async () => {
        const result = await panel.panelClear();
        return { ...result, state: panel.stateForApi() };
      }),
      /* 表盘操作会退出电脑模式；这里给设置页"回到电脑模式"用 */
      route('POST', '/api/dsh-protorig-panel/display/apply', async (req) => {
        const body = await readJson(req);
        const result = typeof panel.applyDisplaySettings === 'function'
          ? await panel.applyDisplaySettings(body ?? {})
          : { ok: false, error: 'unsupported' };
        return { ...result, state: panel.stateForApi() };
      }),
      route('POST', '/api/dsh-protorig-panel/pcmode', async (req) => {
        const body = await readJson(req);
        const result = body?.enter === false ? await panel.exitPcMode() : await panel.enterPcMode();
        return { ...result, state: panel.stateForApi() };
      }),
      route('GET', '/api/dsh-protorig-panel/balance', () => ({
        ok: true,
        balance: balance?.publicView?.() ?? null,
      })),
      route('POST', '/api/dsh-protorig-panel/balance/refresh', async () => {
        const view = await balance?.refresh?.({ force: true });
        return { ok: true, balance: balance?.publicView?.() ?? view ?? null };
      }),
      route('GET', '/api/dsh-protorig-panel/flash/state', () => ({ ok: true, job: flash.view() })),
      route('POST', '/api/dsh-protorig-panel/flash/cancel', () => {
        const cancelled = flash.abort('用户取消');
        return { ok: true, cancelled };
      }),
    ];
    const disposers = routes.map((entry) => sctx.webServer.register(entry));
    logger.info(`设置页 HTTP 路由已注册（${routes.length} 条）`);
    return () => {
      for (const dispose of disposers) {
        try {
          dispose();
        } catch {
          /* ignore */
        }
      }
    };
  });
}
