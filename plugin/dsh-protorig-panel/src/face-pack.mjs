/*
 * face-pack.mjs —— PNG/JPEG → 图片表盘包（.pfi：256 B header + 231,424 B I8 blob）
 *
 * 同时提供**通用的 I8 量化/编码原语**（decodeToCanvasSized / packI8Blob），
 * 给 face-panel.mjs 的「默认表盘下半区」（480×232）复用 —— 两处不要各写一套。
 *
 * 契约（以固件代码为准）：
 *   · research/70_表盘可替换_实现方案.md §2
 *   · research/71_表盘可替换_实现记录.md §2.5
 *   · firmware/lvgl_test/services/svc_face_store.h
 *
 * 格式：
 *   header 256 B（小端）：magic 'PFIM' / version 1 / fmt 1(I8) / 480x480 / palette 256 /
 *     time_x i16 / time_y i16 / font_id u8 / flags u8 / time_color u32 / image_len u32 /
 *     image_crc32 u32 / reserved 2 / header_crc32 u32（覆盖 header[0..31]）
 *   image blob 231,424 B = 1024 B palette（lv_color32_t 内存序 B,G,R,A）+ 480*480 索引
 *
 * CRC 一律用标准 zlib（与固件 16 项 nibble 表 / Python binascii.crc32 等价）。
 *
 * 图片管线（对齐 Python 工具 tools/face_install.py）：
 *   cover 裁切到 480×480（LANCZOS 级别滤波）→ 256 色量化 → 可选 Floyd–Steinberg 抖动。
 *   Pillow 走 MEDIANCUT，这里实现等价的「加权中位切分」+ 15-bit 最近色查找表。
 */
import zlib from 'node:zlib';
import { createCanvas, loadImage } from '@napi-rs/canvas';

export const FACE_IMAGE_W = 480;
export const FACE_IMAGE_H = 480;
export const PALETTE_N = 256;
export const PALETTE_BYTES = PALETTE_N * 4; // lv_color32_t: B,G,R,A
export const IMAGE_LEN = PALETTE_BYTES + FACE_IMAGE_W * FACE_IMAGE_H; // 231,424
export const HEADER_LEN = 256;
export const PFI_LEN = HEADER_LEN + IMAGE_LEN; // 231,680
export const MAGIC = 0x4d494650; // 'PFIM'
export const VERSION = 1;
export const FMT_I8 = 1;
/** shell 单行上限（SDK CONFIG_SHELL_CMD_SIZE=120）：48 B = 96 hex + 前缀 17 = 113。 */
export const UPLOAD_CHUNK = 48;

/** 打包默认叠层（对齐 face_install.py：偏上、超大数字、日期+电量、信号黄）。 */
export const DEFAULT_LAYOUT = Object.freeze({
  timeX: 110,
  timeY: 170,
  fontId: 1,
  flags: 0x03,
  color: 0xfffa00,
});

const FONT_NAMES = ['ui48', 'num104', 'ui28', 'ui20'];

export function fontName(fontId) {
  return FONT_NAMES[fontId] ?? String(fontId);
}

/** 校验并补全叠层参数（固件 end 命令的取值范围）。 */
export function normalizeLayout(layout = {}) {
  const merged = { ...DEFAULT_LAYOUT, ...layout };
  const timeX = Number(merged.timeX);
  const timeY = Number(merged.timeY);
  const fontId = Number(merged.fontId);
  const flags = Number(merged.flags);
  const color = Number(merged.color);
  if (!Number.isFinite(timeX) || timeX < -32768 || timeX > 32767) throw new Error('time_x 必须是 int16');
  if (!Number.isFinite(timeY) || timeY < -32768 || timeY > 32767) throw new Error('time_y 必须是 int16');
  if (!Number.isInteger(fontId) || fontId < 0 || fontId > 3) throw new Error('font 必须是 0..3');
  if (!Number.isInteger(flags) || flags < 0 || flags > 0x03) throw new Error('flags 只能是 bit0(日期)/bit1(电量)');
  if (!Number.isInteger(color) || color < 0 || color > 0xffffff) throw new Error('color 必须是 0xRRGGBB');
  return { timeX, timeY, fontId, flags, color };
}

export function crc32(buf) {
  // Node >= 22 自带 zlib.crc32（与固件/Python 完全一致）
  if (typeof zlib.crc32 === 'function') return zlib.crc32(buf) >>> 0;
  throw new Error('当前 Node 没有 zlib.crc32（需要 Node >= 22）');
}

/** 生成 header（256 B，0xFF 填充）。 */
export function buildHeader({ layout, imageCrc32, imageLen = IMAGE_LEN } = {}) {
  const lay = normalizeLayout(layout);
  const header = Buffer.alloc(HEADER_LEN, 0xff);
  header.writeUInt32LE(MAGIC, 0);
  header[4] = VERSION;
  header[5] = FMT_I8;
  header.writeUInt16LE(FACE_IMAGE_W, 6);
  header.writeUInt16LE(FACE_IMAGE_H, 8);
  header.writeUInt16LE(PALETTE_N, 10);
  header.writeInt16LE(lay.timeX, 12);
  header.writeInt16LE(lay.timeY, 14);
  header[16] = lay.fontId;
  header[17] = lay.flags;
  header.writeUInt32LE(lay.color >>> 0, 18);
  header.writeUInt32LE(imageLen >>> 0, 22);
  header.writeUInt32LE(imageCrc32 >>> 0, 26);
  header[30] = 0;
  header[31] = 0;
  header.writeUInt32LE(crc32(header.subarray(0, 32)), 32);
  return header;
}

/* ------------------------------------------------------------------ 图片解码 */

/**
 * 解码任意图片（PNG/JPEG…）→ width×height canvas（cover 裁切，与 Pillow ImageOps.fit 一致：
 * 等比放大到铺满、居中裁掉多余）。预览缩略图也从这里出，保证"看到的就是装进去的"。
 *
 * 通用版本给图片表盘（480×480）和默认表盘下半区面板（480×232）共用，
 * 避免两套解码/裁切代码各自漂移。
 */
export async function decodeToCanvasSized(imageBuffer, width, height) {
  const img = await loadImage(imageBuffer);
  const canvas = createCanvas(width, height);
  const ctx = canvas.getContext('2d');
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  const scale = Math.max(width / img.width, height / img.height);
  const sw = width / scale;
  const sh = height / scale;
  const sx = (img.width - sw) / 2;
  const sy = (img.height - sh) / 2;
  ctx.drawImage(img, sx, sy, sw, sh, 0, 0, width, height);
  const data = ctx.getImageData(0, 0, width, height).data;
  return { canvas, rgba: data, width, height };
}

/** 解码并 cover 裁成图片表盘的 480×480。 */
export async function decodeToCanvas(imageBuffer) {
  return decodeToCanvasSized(imageBuffer, FACE_IMAGE_W, FACE_IMAGE_H);
}

/** 兼容旧入口：只要 RGBA 时用它。 */
export async function decodeToRgba(imageBuffer) {
  const { rgba, width, height } = await decodeToCanvas(imageBuffer);
  return { rgba, width, height };
}

/* ---------------------------------------------------------------- 量化（中位切分） */

function buildHistogram(rgba) {
  // 5-bit/通道直方图：32768 个桶，桶内累计 RGB 与像素数
  const bins = new Map();
  for (let i = 0; i < rgba.length; i += 4) {
    const key = ((rgba[i] >> 3) << 10) | ((rgba[i + 1] >> 3) << 5) | (rgba[i + 2] >> 3);
    let bin = bins.get(key);
    if (!bin) {
      bin = { r: 0, g: 0, b: 0, n: 0 };
      bins.set(key, bin);
    }
    bin.r += rgba[i];
    bin.g += rgba[i + 1];
    bin.b += rgba[i + 2];
    bin.n += 1;
  }
  const colors = [];
  for (const bin of bins.values()) {
    colors.push({ r: bin.r / bin.n, g: bin.g / bin.n, b: bin.b / bin.n, n: bin.n });
  }
  return colors;
}

function boxOf(colors) {
  let rMin = 255;
  let rMax = 0;
  let gMin = 255;
  let gMax = 0;
  let bMin = 255;
  let bMax = 0;
  let count = 0;
  for (const c of colors) {
    if (c.r < rMin) rMin = c.r;
    if (c.r > rMax) rMax = c.r;
    if (c.g < gMin) gMin = c.g;
    if (c.g > gMax) gMax = c.g;
    if (c.b < bMin) bMin = c.b;
    if (c.b > bMax) bMax = c.b;
    count += c.n;
  }
  return {
    colors,
    count,
    range: Math.max(rMax - rMin, gMax - gMin, bMax - bMin),
    axis: rMax - rMin >= gMax - gMin && rMax - rMin >= bMax - bMin ? 'r' : gMax - gMin >= bMax - bMin ? 'g' : 'b',
  };
}

function splitBox(box) {
  const sorted = [...box.colors].sort((a, b) => a[box.axis] - b[box.axis]);
  const half = box.count / 2;
  let acc = 0;
  let cut = 1;
  for (let i = 0; i < sorted.length - 1; i += 1) {
    acc += sorted[i].n;
    cut = i + 1;
    if (acc >= half) break;
  }
  return [boxOf(sorted.slice(0, cut)), boxOf(sorted.slice(cut))];
}

/** 加权中位切分 → 最多 maxColors 个 RGB 调色板项（按 count 降序，索引 0 最常用）。 */
export function quantizePalette(rgba, maxColors = PALETTE_N) {
  const colors = buildHistogram(rgba);
  if (colors.length === 0) throw new Error('图片没有像素');
  let boxes = [boxOf(colors)];
  while (boxes.length < maxColors) {
    let best = -1;
    let bestScore = -1;
    for (let i = 0; i < boxes.length; i += 1) {
      const box = boxes[i];
      if (box.colors.length < 2) continue;
      // 优先切"跨度大且像素多"的盒子（避免把大片纯色反复切开）
      const score = box.range * Math.sqrt(box.count);
      if (score > bestScore) {
        bestScore = score;
        best = i;
      }
    }
    if (best < 0) break;
    const [a, b] = splitBox(boxes[best]);
    boxes.splice(best, 1, a, b);
  }
  return boxes
    .map((box) => {
      const sum = box.colors.reduce(
        (acc, c) => ({ r: acc.r + c.r * c.n, g: acc.g + c.g * c.n, b: acc.b + c.b * c.n }),
        { r: 0, g: 0, b: 0 },
      );
      return [Math.round(sum.r / box.count), Math.round(sum.g / box.count), Math.round(sum.b / box.count)];
    })
    .map(([r, g, b]) => [Math.max(0, Math.min(255, r)), Math.max(0, Math.min(255, g)), Math.max(0, Math.min(255, b))]);
}

/** 15-bit RGB → 最近调色板索引（32768 项查找表，量化阶段只算一次）。 */
export function buildLookup(palette) {
  const lut = new Uint8Array(32 * 32 * 32);
  for (let key = 0; key < lut.length; key += 1) {
    const r = (((key >> 10) & 31) << 3) | 4;
    const g = (((key >> 5) & 31) << 3) | 4;
    const b = ((key & 31) << 3) | 4;
    let best = 0;
    let bestDist = Infinity;
    for (let i = 0; i < palette.length; i += 1) {
      const dr = r - palette[i][0];
      const dg = g - palette[i][1];
      const db = b - palette[i][2];
      const dist = dr * dr + dg * dg + db * db;
      if (dist < bestDist) {
        bestDist = dist;
        best = i;
      }
    }
    lut[key] = best;
  }
  return lut;
}

const keyOf = (r, g, b) => (((r >> 3) & 31) << 10) | (((g >> 3) & 31) << 5) | ((b >> 3) & 31);
const clamp255 = (v) => (v < 0 ? 0 : v > 255 ? 255 : v);

/**
 * RGBA(width×height) → 索引（w*h B）。dither=true 时做 Floyd–Steinberg 抖动（与 Pillow 默认一致）。
 * 抖动误差扩散用两行 Float32 缓冲，不额外分配大数组。
 */
export function mapIndexedSized(rgba, width, height, palette, lut, { dither = true } = {}) {
  const w = width;
  const h = height;
  const out = new Uint8Array(w * h);
  if (!dither) {
    for (let i = 0, p = 0; p < out.length; i += 4, p += 1) {
      out[p] = lut[keyOf(rgba[i], rgba[i + 1], rgba[i + 2])];
    }
    return out;
  }
  let cur = new Float32Array((w + 2) * 3);
  let next = new Float32Array((w + 2) * 3);
  const addErr = (arr, x, ch, value) => {
    const idx = (x + 1) * 3 + ch;
    arr[idx] += value;
  };
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const i = (y * w + x) * 4;
      const base = (x + 1) * 3;
      const r = clamp255(rgba[i] + cur[base]);
      const g = clamp255(rgba[i + 1] + cur[base + 1]);
      const b = clamp255(rgba[i + 2] + cur[base + 2]);
      const idx = lut[keyOf(r, g, b)];
      out[y * w + x] = idx;
      const [pr, pg, pb] = palette[idx];
      const er = r - pr;
      const eg = g - pg;
      const eb = b - pb;
      if (x + 1 < w) {
        addErr(cur, x + 1, 0, (er * 7) / 16);
        addErr(cur, x + 1, 1, (eg * 7) / 16);
        addErr(cur, x + 1, 2, (eb * 7) / 16);
      }
      if (x - 1 >= 0) {
        addErr(next, x - 1, 0, (er * 3) / 16);
        addErr(next, x - 1, 1, (eg * 3) / 16);
        addErr(next, x - 1, 2, (eb * 3) / 16);
      }
      addErr(next, x, 0, (er * 5) / 16);
      addErr(next, x, 1, (eg * 5) / 16);
      addErr(next, x, 2, (eb * 5) / 16);
      if (x + 1 < w) {
        addErr(next, x + 1, 0, er / 16);
        addErr(next, x + 1, 1, eg / 16);
        addErr(next, x + 1, 2, eb / 16);
      }
    }
    const swap = cur;
    cur = next;
    next = swap;
    next.fill(0);
  }
  return out;
}

/** 图片表盘（480×480）的索引映射（旧入口，保持签名）。 */
export function mapIndexed(rgba, palette, lut, opts = {}) {
  return mapIndexedSized(rgba, FACE_IMAGE_W, FACE_IMAGE_H, palette, lut, opts);
}

/* ------------------------------------------------------------------ 打包 */

/**
 * RGBA(width×height) → I8 blob（1024 B palette + width*height 索引）。
 *
 * 图片表盘（480×480）和默认表盘下半区面板（480×232）**共用这一份**量化/编码：
 * 两边固件都按 lv_color32_t 内存序 B,G,R,A + 8 位索引解释，没有第二套实现。
 * palette 固定预留 256 项（1024 B），实际颜色少于 256 时剩余项为 0。
 */
export function packI8Blob(rgba, width, height, { dither = true, maxColors = PALETTE_N } = {}) {
  if (!Number.isInteger(width) || !Number.isInteger(height) || width <= 0 || height <= 0) {
    throw new Error(`尺寸不合法：${width}×${height}`);
  }
  if (!rgba || rgba.length !== width * height * 4) {
    throw new Error(`RGBA 长度不对（${rgba?.length ?? 0}，期望 ${width * height * 4}）`);
  }
  const palette = quantizePalette(rgba, maxColors);
  const lut = buildLookup(palette);
  const indices = mapIndexedSized(rgba, width, height, palette, lut, { dither });
  const blob = Buffer.alloc(PALETTE_BYTES + width * height);
  for (let i = 0; i < palette.length; i += 1) {
    // lv_color32_t 内存序：B, G, R, A
    blob[i * 4] = palette[i][2];
    blob[i * 4 + 1] = palette[i][1];
    blob[i * 4 + 2] = palette[i][0];
    blob[i * 4 + 3] = 0xff;
  }
  for (let i = 0; i < indices.length; i += 1) {
    blob[PALETTE_BYTES + i] = indices[i];
  }
  const imageCrc32 = crc32(blob);
  return {
    blob,
    palette,
    indices,
    width,
    height,
    /** palette 固定 1024 B（固件 I8 假设 256 项）。 */
    paletteBytes: PALETTE_BYTES,
    crc32: imageCrc32,
    crcHex: imageCrc32.toString(16).toUpperCase().padStart(8, '0'),
    size: blob.length,
  };
}

/** RGBA(480×480) → PFI（header + blob）。 */
export function packRgba(rgba, layout = {}, opts = {}) {
  const lay = normalizeLayout(layout);
  const built = packI8Blob(rgba, FACE_IMAGE_W, FACE_IMAGE_H, opts);
  if (built.blob.length !== IMAGE_LEN) throw new Error('索引长度不对');
  const header = buildHeader({ layout: lay, imageCrc32: built.crc32 });
  const pfi = Buffer.concat([header, built.blob]);
  return {
    pfi,
    header,
    blob: built.blob,
    layout: lay,
    palette: built.palette,
    crc32: built.crc32,
    crcHex: built.crcHex,
    size: pfi.length,
  };
}

/** 图片文件 Buffer（PNG/JPEG…）→ PFI + 480×480 预览 PNG（设置页缩略图缓存用）。 */
export async function packImageBuffer(imageBuffer, layout = {}, opts = {}) {
  if (!Buffer.isBuffer(imageBuffer) || imageBuffer.length === 0) throw new Error('图片数据为空');
  const { canvas, rgba } = await decodeToCanvas(imageBuffer);
  const packed = packRgba(rgba, layout, opts);
  return { ...packed, previewPng: canvas.toBuffer('image/png') };
}

/**
 * PFI → 预览 PNG（用于"从 .pfi 安装"时没有原图、但仍要缓存缩略图的场景）。
 * 按 header 里的 palette/索引重建 480×480 RGBA。
 */
export function pfiToPreviewPng(pfi) {
  const parsed = parsePfi(pfi);
  if (!parsed.ok) throw new Error('PFI 校验失败');
  const blob = pfi.subarray(HEADER_LEN);
  const canvas = createCanvas(FACE_IMAGE_W, FACE_IMAGE_H);
  const ctx = canvas.getContext('2d');
  const imageData = ctx.createImageData(FACE_IMAGE_W, FACE_IMAGE_H);
  const out = imageData.data;
  for (let i = 0; i < FACE_IMAGE_W * FACE_IMAGE_H; i += 1) {
    const idx = blob[PALETTE_BYTES + i];
    const base = idx * 4;
    // blob 里 palette 是 B,G,R,A；ImageData 要 R,G,B,A
    out[i * 4] = blob[base + 2];
    out[i * 4 + 1] = blob[base + 1];
    out[i * 4 + 2] = blob[base];
    out[i * 4 + 3] = 255;
  }
  ctx.putImageData(imageData, 0, 0);
  return canvas.toBuffer('image/png');
}

/** 读回/校验 PFI（PC 侧自检、路由预检用）。 */
export function parsePfi(pfi) {
  if (!Buffer.isBuffer(pfi) || pfi.length !== PFI_LEN) {
    throw new Error(`PFI 长度不对（${pfi?.length ?? 0}，期望 ${PFI_LEN}）`);
  }
  if (pfi.readUInt32LE(0) !== MAGIC) throw new Error('magic 不是 PFIM');
  if (pfi[4] !== VERSION) throw new Error(`version=${pfi[4]}（期望 ${VERSION}）`);
  if (pfi[5] !== FMT_I8) throw new Error(`fmt=${pfi[5]}（期望 I8）`);
  const width = pfi.readUInt16LE(6);
  const height = pfi.readUInt16LE(8);
  const paletteEntries = pfi.readUInt16LE(10);
  const imageLen = pfi.readUInt32LE(22);
  const imageCrc32 = pfi.readUInt32LE(26);
  const headerCrc32 = pfi.readUInt32LE(32);
  const headerOk = crc32(pfi.subarray(0, 32)) === headerCrc32;
  const blob = pfi.subarray(HEADER_LEN);
  const blobOk = blob.length === imageLen && crc32(blob) === imageCrc32;
  return {
    width,
    height,
    paletteEntries,
    imageLen,
    imageCrc32,
    headerCrc32,
    headerOk,
    blobOk,
    layout: {
      timeX: pfi.readInt16LE(12),
      timeY: pfi.readInt16LE(14),
      fontId: pfi[16],
      flags: pfi[17],
      color: pfi.readUInt32LE(18) & 0xffffff,
    },
    ok: headerOk && blobOk && width === FACE_IMAGE_W && height === FACE_IMAGE_H,
  };
}

/** 生成一张上板/演示用的测试图（渐变 + 圆环刻度 + 暗色时间区），返回 PNG Buffer。 */
export async function makeDemoPng() {
  const canvas = createCanvas(FACE_IMAGE_W, FACE_IMAGE_H);
  const ctx = canvas.getContext('2d');
  const grad = ctx.createLinearGradient(0, 0, FACE_IMAGE_W, FACE_IMAGE_H);
  grad.addColorStop(0, '#0b1d3a');
  grad.addColorStop(0.55, '#123b5e');
  grad.addColorStop(1, '#081018');
  ctx.fillStyle = grad;
  ctx.fillRect(0, 0, FACE_IMAGE_W, FACE_IMAGE_H);
  ctx.strokeStyle = 'rgba(255,250,0,0.85)';
  ctx.lineWidth = 3;
  for (let i = 0; i < 60; i += 1) {
    const angle = (i / 60) * Math.PI * 2 - Math.PI / 2;
    const inner = i % 5 === 0 ? 186 : 198;
    ctx.beginPath();
    ctx.moveTo(240 + Math.cos(angle) * inner, 240 + Math.sin(angle) * inner);
    ctx.lineTo(240 + Math.cos(angle) * 210, 240 + Math.sin(angle) * 210);
    ctx.stroke();
  }
  // 暗色时间区（保证叠字可读）
  ctx.fillStyle = 'rgba(0,0,0,0.55)';
  ctx.beginPath();
  ctx.ellipse(240, 236, 150, 92, 0, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillStyle = 'rgba(255,250,0,0.25)';
  ctx.font = '20px sans-serif';
  ctx.textAlign = 'center';
  ctx.fillText('PROTORIG ES', 240, 330);
  return canvas.toBuffer('image/png');
}
