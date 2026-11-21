/*
 * face-panel.mjs —— 默认表盘（face 0）下半区可替换面板：打包 + shell 命令序列
 *
 * 契约（以固件为准，research/76 §2.1/§2.2 + research/78）：
 *   · 面板图 = I8：1024 B palette（lv_color32_t 内存序 B,G,R,A）+ 480×232 索引
 *     = 112,384 B（0x1B700）；
 *   · 面板区域在 480×480 屏幕的 (0, 248)，高 232；上半区时间/表针/日期/电量由固件实时绘制；
 *   · PC 只上传 image blob，**header 由固件 `facepanel end` 校验 CRC 后自己写**
 *     （PC 侧不需要构造 256 B header，也不需要新开二进制协议）；
 *   · 上传走 shell，48 B/行（96 hex），与 faceimg 一致（shell 单行 120 字符上限）。
 *
 * 量化/编码复用 face-pack.mjs 的 packI8Blob —— 与图片表盘（480×480）共用一份实现。
 */
import { readFile } from 'node:fs/promises';
import { createCanvas, loadImage } from '@napi-rs/canvas';
import { PALETTE_BYTES, crc32, decodeToCanvasSized, packI8Blob } from './face-pack.mjs';

export const PANEL_W = 480;
export const PANEL_H = 232;
/** 面板在 480×480 屏幕里的位置：x=0, y=248（与内置 watch_photo 完全同位置）。 */
export const PANEL_X = 0;
export const PANEL_Y = 248;
/** 固件 header 的 magic 'PFPN'（只在固件写；这里仅用于文档/自检）。 */
export const PANEL_MAGIC = 0x4e504650;
export const PANEL_HEADER_LEN = 256;
/** palette 固定 1024 B（与图片表盘一致）。 */
export const PANEL_PALETTE_BYTES = PALETTE_BYTES;
/** image blob 长度：1024 + 480×232 = 112,384 B。 */
export const PANEL_IMAGE_LEN = PANEL_PALETTE_BYTES + PANEL_W * PANEL_H;
/** shell 单行上限（固件 CONFIG_SHELL_CMD_SIZE=120）：48 B = 96 hex，最坏行 `facepanel w 112320 `（19）+ 96 = 115。 */
export const PANEL_UPLOAD_CHUNK = 48;
/** 上传行数：112,384 = 2341×48 + 16 → 2342 行。 */
export const PANEL_UPLOAD_LINES = Math.ceil(PANEL_IMAGE_LEN / PANEL_UPLOAD_CHUNK);

/** shell 命令字面量（与固件 main.c 的 cmd_facepanel 一一对应，改一处要同步）。 */
export const PANEL_CMD = 'facepanel';
export const PANEL_CMD_STATUS = PANEL_CMD;
export const PANEL_CMD_INFO = 'facepanel info';
export const PANEL_CMD_BEGIN = 'facepanel begin';
export const PANEL_CMD_END = 'facepanel end';
export const PANEL_CMD_CLEAR = 'facepanel clear';

/** CRC32 → 8 位大写 hex（固件 printf 用 %08lX，插件发 begin 时也要同一形式）。 */
export function panelCrcHex(crc) {
  const value = Number(crc) >>> 0;
  return value.toString(16).toUpperCase().padStart(8, '0');
}

/** cover 裁切成 480×232（与表盘用同一套 cover 规则）。 */
export async function decodePanelToCanvas(imageBuffer) {
  return decodeToCanvasSized(imageBuffer, PANEL_W, PANEL_H);
}

/**
 * RGBA(480×232) → 面板 blob。
 * @returns {{blob:Buffer, palette:number[][], indices:Uint8Array, crc32:number, crcHex:string,
 *            size:number, width:number, height:number}}
 */
export function packPanelRgba(rgba, { dither = true, maxColors } = {}) {
  const built = packI8Blob(rgba, PANEL_W, PANEL_H, { dither, maxColors });
  if (built.blob.length !== PANEL_IMAGE_LEN) {
    throw new Error(`面板 blob 长度不对（${built.blob.length}，期望 ${PANEL_IMAGE_LEN}）`);
  }
  return { ...built, size: built.blob.length };
}

/** 内置 watch photo 的羽化 alpha 资产（480×232；由 tools/make-panel-mask.py 从内置图反解）。 */
const PANEL_MASK_FILE = new URL('../assets/faces/panel-mask.png', import.meta.url);

let panelMaskPngPromise = null;
/** 读 mask PNG（只读一次；`GET /faces/panel-mask` 与打包共用同一份）。 */
export function panelMaskPng() {
  if (!panelMaskPngPromise) {
    panelMaskPngPromise = readFile(PANEL_MASK_FILE).catch((error) => {
      panelMaskPngPromise = null;
      throw new Error(`面板 mask 读取失败：${error.message}`);
    });
  }
  return panelMaskPngPromise;
}

let panelMaskPromise = null;
/**
 * 解码 mask → 480×232 alpha（0..255）。
 *
 * PNG 是白底 + A 通道：这里从画布取 A，与客户端裁剪框 `destination-in` 用的是同一条通道。
 * 结果缓存（含原始 PNG），并发调用共享同一个 Promise。
 *
 * @returns {Promise<{width:number, height:number, alpha:Uint8Array}>}
 */
export function loadPanelMask() {
  if (!panelMaskPromise) {
    panelMaskPromise = (async () => {
      const png = await panelMaskPng();
      const image = await loadImage(png);
      if (image.width !== PANEL_W || image.height !== PANEL_H) {
        throw new Error(`面板 mask 尺寸不对（${image.width}×${image.height}，期望 ${PANEL_W}×${PANEL_H}）`);
      }
      const canvas = createCanvas(PANEL_W, PANEL_H);
      const ctx = canvas.getContext('2d');
      ctx.drawImage(image, 0, 0);
      const { data } = ctx.getImageData(0, 0, PANEL_W, PANEL_H);
      const alpha = new Uint8Array(PANEL_W * PANEL_H);
      for (let i = 0; i < alpha.length; i += 1) alpha[i] = data[i * 4 + 3];
      return { width: PANEL_W, height: PANEL_H, alpha };
    })().catch((error) => {
      panelMaskPromise = null;
      throw error;
    });
  }
  return panelMaskPromise;
}

/**
 * 下半区图片边缘羽化：直接乘内置 watch photo 的暗角 alpha mask。
 *
 * 2026-11-14 用户要求：不再用手调径向/四边常数，和内置佩丽卡图用同一套羽化形状，
 * 而且上边缘也要羽化（旧实现上边中点全亮）。mask 由 `tools/make-panel-mask.py`
 * 从 `firmware/lvgl_test/assets/watch_photo/preview.png` 反解生成：顶部渐隐 + 圆边
 * 渐隐；底部文字压暗不是羽化，所以 mask 中心保持 255（面板中心不受影响）。
 *
 * @param {Uint8Array} rgba 480×232 RGBA（原地修改，alpha 复位 255）
 * @param {number} width
 * @param {number} height
 * @param {{mask: Uint8Array}} opts mask = `loadPanelMask()` 的 alpha（480×232）
 */
export function applyPanelVignette(rgba, width = PANEL_W, height = PANEL_H, { mask } = {}) {
  if (!rgba || rgba.length !== width * height * 4) {
    throw new Error(`RGBA 长度不对（${rgba?.length ?? 0}，期望 ${width * height * 4}）`);
  }
  if (!mask || mask.length !== width * height) {
    throw new Error(`缺少面板 mask（期望 ${width}×${height} 的 alpha，实际 ${mask?.length ?? 0}）`);
  }
  for (let p = 0, i = 0; p < mask.length; p += 1, i += 4) {
    const a = mask[p];
    rgba[i + 3] = 255;
    if (a === 255) continue;
    if (a === 0) {
      rgba[i] = 0;
      rgba[i + 1] = 0;
      rgba[i + 2] = 0;
      continue;
    }
    rgba[i] = Math.round((rgba[i] * a) / 255);
    rgba[i + 1] = Math.round((rgba[i + 1] * a) / 255);
    rgba[i + 2] = Math.round((rgba[i + 2] * a) / 255);
  }
  return rgba;
}

/**
 * 图片文件 Buffer（PNG/JPEG…）→ 面板 blob + 480×232 预览 PNG。
 * 预览图与"装进去的图"同一来源（cover 后的 canvas），保证看到的就是装进去的。
 */
export async function packPanelImageBuffer(imageBuffer, opts = {}) {
  if (!Buffer.isBuffer(imageBuffer) || imageBuffer.length === 0) throw new Error('图片数据为空');
  const { canvas, rgba } = await decodePanelToCanvas(imageBuffer);
  /* 量化前套内置图的羽化 mask：装进去的图和预览图都用同一份 masked RGBA，
   * 避免"预览有黑边、表上却是硬边"的错觉。 */
  const mask = await loadPanelMask();
  applyPanelVignette(rgba, PANEL_W, PANEL_H, { mask: mask.alpha });
  const packed = packPanelRgba(rgba, opts);
  const ctx = canvas.getContext('2d');
  try {
    const imageData = ctx.createImageData(PANEL_W, PANEL_H);
    imageData.data.set(rgba);
    ctx.putImageData(imageData, 0, 0);
  } catch {
    /* 老版本 canvas 不支持 putImageData 时预览保持原图（装进表里的仍然是 masked 图） */
  }
  return { ...packed, previewPng: canvas.toBuffer('image/png') };
}

/**
 * 上传序列：`facepanel begin <len> <crc>` → 2342 行 `facepanel w <off> <hex>` → `facepanel end`。
 * 生成器（不预先造 2342 个字符串，省内存），单测可以直接取前几行/行数。
 *
 * @param {Buffer} blob 112,384 B
 * @param {{chunk?:number}} opts
 * @returns {Generator<{off:number, len:number, hex:string, line:string}>}
 */
export function* panelChunks(blob, { chunk = PANEL_UPLOAD_CHUNK } = {}) {
  if (!Buffer.isBuffer(blob)) throw new Error('面板 blob 不是 Buffer');
  if (blob.length !== PANEL_IMAGE_LEN) {
    throw new Error(`面板 blob 长度不对（${blob.length}，期望 ${PANEL_IMAGE_LEN}）`);
  }
  if (!Number.isInteger(chunk) || chunk <= 0 || chunk > 48) throw new Error('每行最多 48 B（shell 单行 120 字符）');
  for (let off = 0; off < blob.length; off += chunk) {
    const len = Math.min(chunk, blob.length - off);
    const hex = blob.subarray(off, off + len).toString('hex').toUpperCase();
    yield { off, len, hex, line: `${PANEL_CMD} w ${off} ${hex}` };
  }
}

/** `facepanel begin <len> <crc32>`（len 必须是 112,384）。 */
export function panelBeginLine({ crcHex, length = PANEL_IMAGE_LEN } = {}) {
  if (length !== PANEL_IMAGE_LEN) throw new Error(`面板 len 必须是 ${PANEL_IMAGE_LEN}`);
  return `${PANEL_CMD_BEGIN} ${length} ${String(crcHex ?? '').toUpperCase()}`;
}

/** 自己算一遍 blob 的 CRC（上传前自检；固件 begin/end 也会各校验一次）。 */
export function panelBlobCrc(blob) {
  if (!Buffer.isBuffer(blob) || blob.length !== PANEL_IMAGE_LEN) {
    throw new Error(`面板 blob 长度不对（${blob?.length ?? 0}，期望 ${PANEL_IMAGE_LEN}）`);
  }
  const value = crc32(blob);
  return { crc32: value, crcHex: panelCrcHex(value) };
}

/**
 * 解析 `facepanel` / `facepanel info` 的状态行（research/76 §2.2 / 固件 cmd_facepanel）。
 *
 *   有效：`[facepanel] valid=1 crc=04F565B1 len=112384 w=480 h=232 y=248[ crc-checked]`
 *   无效：`[facepanel] valid=0 rc=-5 (bad header format)`
 *
 * @returns {null|{valid:boolean, crc:string, len:number|null, width:number|null, height:number|null,
 *                 y:number|null, rc:number|null, reason:string, crcChecked:boolean, raw:string}}
 *   不是 facepanel 行 → null（调用方据此判断"手表没回应"）。
 */
export function parsePanelStatus(line) {
  const text = String(line ?? '').trim();
  if (!text.includes('[facepanel]')) return null;
  const validMatch = /valid=(\d+)/.exec(text);
  if (!validMatch) return null;
  const valid = validMatch[1] === '1';
  const rc = /rc=(-?\d+)(?: \(([^)]+)\))?/.exec(text);
  const crc = /crc=([0-9A-Fa-f]{8})/.exec(text);
  const len = /len=(\d+)/.exec(text);
  const width = /\bw=(\d+)/.exec(text);
  const height = /\bh=(\d+)/.exec(text);
  const y = /\by=(\d+)/.exec(text);
  return {
    valid,
    crc: crc ? crc[1].toUpperCase() : '',
    len: len ? Number(len[1]) : null,
    width: width ? Number(width[1]) : null,
    height: height ? Number(height[1]) : null,
    y: y ? Number(y[1]) : null,
    rc: rc ? Number(rc[1]) : null,
    reason: rc?.[2] ?? '',
    crcChecked: /crc-checked/.test(text),
    raw: text,
  };
}

/** 面板状态行的尺寸是否符合 PC 侧契约（防止固件改了 480×232 而插件不知道）。 */
export function panelStatusMatchesContract(status) {
  if (!status?.valid) return false;
  return status.len === PANEL_IMAGE_LEN
    && status.width === PANEL_W
    && status.height === PANEL_H
    && (status.y === null || status.y === PANEL_Y);
}
