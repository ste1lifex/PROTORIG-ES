/*
 * face-pack.test.mjs —— PNG → PFI 打包器（P9）的纯逻辑测试
 *
 * 不碰串口/手表：只验证格式契约（research/70 §2、svc_face_store.h）：
 *   header 256 B / blob 231,424 B / palette B,G,R,A / 索引 / zlib CRC32。
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { createCanvas } from '@napi-rs/canvas';
import { loadImage } from '@napi-rs/canvas';
import {
  FACE_IMAGE_H,
  FACE_IMAGE_W,
  HEADER_LEN,
  IMAGE_LEN,
  PALETTE_N,
  PALETTE_BYTES,
  PFI_LEN,
  buildHeader,
  crc32,
  makeDemoPng,
  normalizeLayout,
  packImageBuffer,
  packRgba,
  parsePfi,
  pfiToPreviewPng,
  quantizePalette,
} from '../src/face-pack.mjs';

const solidRgba = (r, g, b) => {
  const rgba = new Uint8Array(FACE_IMAGE_W * FACE_IMAGE_H * 4);
  for (let i = 0; i < rgba.length; i += 4) {
    rgba[i] = r;
    rgba[i + 1] = g;
    rgba[i + 2] = b;
    rgba[i + 3] = 255;
  }
  return rgba;
};

test('layout：默认值与固件/face_install.py 一致，越界参数报错', () => {
  const lay = normalizeLayout({});
  assert.deepEqual(lay, { timeX: 110, timeY: 170, fontId: 1, flags: 3, color: 0xfffa00 });
  assert.equal(normalizeLayout({ fontId: 0 }).fontId, 0);
  assert.throws(() => normalizeLayout({ fontId: 4 }), /font/);
  assert.throws(() => normalizeLayout({ flags: 4 }), /flags/);
  assert.throws(() => normalizeLayout({ timeX: 40000 }), /time_x/);
  assert.throws(() => normalizeLayout({ color: -1 }), /color/);
});

test('header：magic/尺寸/palette/layout/CRC 字段都在正确偏移', () => {
  const header = buildHeader({ layout: { timeX: 12, timeY: 34, fontId: 2, flags: 1, color: 0x112233 }, imageCrc32: 0xdeadbeef });
  assert.equal(header.length, HEADER_LEN);
  assert.equal(header.readUInt32LE(0), 0x4d494650, "'PFIM'");
  assert.equal(header[4], 1, 'version');
  assert.equal(header[5], 1, 'fmt I8');
  assert.equal(header.readUInt16LE(6), 480);
  assert.equal(header.readUInt16LE(8), 480);
  assert.equal(header.readUInt16LE(10), 256);
  assert.equal(header.readInt16LE(12), 12);
  assert.equal(header.readInt16LE(14), 34);
  assert.equal(header[16], 2);
  assert.equal(header[17], 1);
  assert.equal(header.readUInt32LE(18), 0x112233);
  assert.equal(header.readUInt32LE(22), IMAGE_LEN);
  assert.equal(header.readUInt32LE(26), 0xdeadbeef);
  assert.equal(header.readUInt32LE(32), crc32(header.subarray(0, 32)), 'header_crc32 覆盖前 32 B');
});

test('packRgba：纯色图 → 1 色调色板 + 全 0 索引，PFI 长度/CRC 正确', () => {
  const packed = packRgba(solidRgba(255, 128, 0), {}, { dither: false });
  assert.equal(packed.pfi.length, PFI_LEN);
  assert.equal(packed.blob.length, IMAGE_LEN);
  assert.equal(packed.size, PFI_LEN);
  // 纯色时中位切分无法继续分，只剩 1 个调色板项
  assert.equal(packed.palette.length, 1);
  // palette 内存序 B,G,R,A
  assert.deepEqual([...packed.blob.subarray(0, 4)], [0, 128, 255, 255]);
  assert.equal(packed.blob[PALETTE_BYTES], 0, '索引全 0');
  assert.equal(packed.blob[PALETTE_BYTES + FACE_IMAGE_W * FACE_IMAGE_H - 1], 0);
  const parsed = parsePfi(packed.pfi);
  assert.equal(parsed.ok, true);
  assert.equal(parsed.headerOk, true);
  assert.equal(parsed.blobOk, true);
  assert.equal(parsed.imageCrc32, packed.crc32);
});

test('packRgba：渐变图 → 256 色，索引都落在调色板内，CRC 自洽', () => {
  const rgba = new Uint8Array(FACE_IMAGE_W * FACE_IMAGE_H * 4);
  for (let y = 0; y < FACE_IMAGE_H; y += 1) {
    for (let x = 0; x < FACE_IMAGE_W; x += 1) {
      const i = (y * FACE_IMAGE_W + x) * 4;
      rgba[i] = Math.round((x / FACE_IMAGE_W) * 255);
      rgba[i + 1] = Math.round((y / FACE_IMAGE_H) * 255);
      rgba[i + 2] = 128;
      rgba[i + 3] = 255;
    }
  }
  const packed = packRgba(rgba, { fontId: 1 }, { dither: true });
  assert.equal(packed.palette.length, PALETTE_N);
  let maxIndex = 0;
  for (let i = PALETTE_BYTES; i < IMAGE_LEN; i += 1) maxIndex = Math.max(maxIndex, packed.blob[i]);
  assert.ok(maxIndex < PALETTE_N, `索引 ${maxIndex} 越界`);
  const parsed = parsePfi(packed.pfi);
  assert.equal(parsed.ok, true);
  assert.equal(parsed.layout.fontId, 1);
  assert.equal(parsed.imageLen, IMAGE_LEN);
});

test('quantizePalette：颜色数少于 256 时不会造出多余颜色', () => {
  const rgba = new Uint8Array(4 * 4 * 4); // 4x4 两色
  for (let p = 0; p < 16; p += 1) {
    const i = p * 4;
    const red = p % 2 === 0;
    rgba[i] = red ? 255 : 0;
    rgba[i + 1] = 0;
    rgba[i + 2] = red ? 0 : 255;
    rgba[i + 3] = 255;
  }
  const palette = quantizePalette(rgba, 256);
  assert.ok(palette.length >= 2 && palette.length <= 4, `实际 ${palette.length} 色`);
});

test('packImageBuffer：真实 PNG（demo 图）→ PFI 往返解析一致', async () => {
  const png = await makeDemoPng();
  const packed = await packImageBuffer(png, { timeX: 120, timeY: 160, fontId: 3, flags: 2, color: 0x00ff00 });
  assert.equal(packed.size, PFI_LEN);
  assert.equal(packed.layout.timeX, 120);
  assert.equal(packed.layout.fontId, 3);
  const parsed = parsePfi(packed.pfi);
  assert.equal(parsed.ok, true);
  assert.deepEqual(parsed.layout, { timeX: 120, timeY: 160, fontId: 3, flags: 2, color: 0x00ff00 });
  assert.match(packed.crcHex, /^[0-9A-F]{8}$/);
});

test('packImageBuffer：返回 480×480 预览 PNG（缩略图缓存用），PFI 也能还原预览', async () => {
  const png = await makeDemoPng();
  const packed = await packImageBuffer(png, {});
  assert.ok(Buffer.isBuffer(packed.previewPng) && packed.previewPng.length > 1000, '应有 previewPng');
  const img = await loadImage(packed.previewPng);
  assert.equal(img.width, 480);
  assert.equal(img.height, 480);
  const fromPfi = await loadImage(pfiToPreviewPng(packed.pfi));
  assert.equal(fromPfi.width, 480);
  assert.equal(fromPfi.height, 480);
});

test('packImageBuffer：任意尺寸 PNG 会 cover 裁成 480×480', async () => {
  const canvas = createCanvas(900, 300);
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#224466';
  ctx.fillRect(0, 0, 900, 300);
  const png = canvas.toBuffer('image/png');
  const packed = await packImageBuffer(png, {});
  assert.equal(packed.pfi.length, PFI_LEN);
  assert.equal(parsePfi(packed.pfi).ok, true);
});

test('parsePfi：长度/magic 不对时抛错', () => {
  assert.throws(() => parsePfi(Buffer.alloc(100)), /长度/);
  const bad = Buffer.alloc(PFI_LEN);
  assert.throws(() => parsePfi(bad), /magic/);
});
