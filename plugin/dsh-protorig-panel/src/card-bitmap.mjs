/*
 * card-bitmap.mjs —— BLE 卡片层的 1-bit 文字位图渲染
 *
 * 为什么不用"固件字体直接画中文"：固件 lv_font 只有 ~600 个常用汉字
 * （fonts_gen 里 --symbols 的集合），真实会话标题（"PROTORIG ES BLE 对齐 USB
 * 开发任务"里的 对齐/开发）会缺字；2 MB app 分区也塞不下 3500 汉字。
 * 所以标题/命令/题目这类"任意文本"由插件用 Skia 渲成 1-bit 位图下发，
 * 固件扩 A8 后用 lv_image 画 —— 和 USB 路径渲染标题位图的做法一致。
 *
 * 输出格式（与 svc_pc.c 的 0x57 一致）：
 *   h = 20，w 是 8 的倍数且 ≤ 296（固件 w 上限 300，字节对齐取 296）
 *   行优先、MSB-first、1 = 亮
 * 单张 ≤ 296×30/8 = 1110 B，BLE 2.9 KB/s 下约 0.4 s，只在文本变化时传。
 */
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { loadCanvas } from './deps.mjs';
import { panelFont, resolvePanelFontFamily } from './fonts.mjs';
import { dshFishPath } from './assets/dsh-fish.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
/* 面板位图用的思源 OTF 不在 V1.6 仓库里（见 firmware/lvgl_test/fonts_src/README.md），
 * 启动时按"V1.6 fonts_src → 旧 firmware_m2 fonts_src"找一份；都没有时才在渲染时报错。 */
const FONT_DIR = process.env.PROTORIG_FONT_DIR
  ?? [path.resolve(here, '../../../firmware/lvgl_test/fonts_src'),
      path.resolve(here, '../../../../firmware_m2/fonts_src')]
    .find((dir) => fs.existsSync(path.join(dir, 'SourceHanSansSC-Medium.otf')))
  ?? path.resolve(here, '../../../firmware/lvgl_test/fonts_src');
const FONT_FILES = ['SourceHanSansSC-Medium.otf', 'SourceHanSansSC-Bold.otf'];

const HEIGHT = 30;
const MAX_WIDTH = 296;
const FONT_SIZE = 25;
const BASELINE = 24;
/* 2-bit（4 级灰）：0/85/170/255。1-bit 的硬边是"不像 v4"的最大来源，
 * 灰度让 Skia 的抗锯齿被保存下来（research/66 §2 0x57）。 */
const BPP = 2;
const CACHE_LIMIT = 24;

export class CardBitmapRenderer {
  constructor({ fontFiles } = {}) {
    this.fontFiles = fontFiles ?? FONT_FILES.map((f) => path.join(FONT_DIR, f));
    this.createCanvas = null;
    this.Path2D = null;
    this.measureCtx = null;
    this.ready = false;
    this.cache = new Map();
    this.failed = null;
  }

  async init() {
    if (this.ready) return this;
    const { createCanvas, GlobalFonts, Path2D } = await loadCanvas();
    for (const file of this.fontFiles) {
      if (fs.existsSync(file)) GlobalFonts.registerFromPath(file, 'SHSans');
    }
    /* 字体回退：SHSans 在没装 OTF 的机器上不存在（macOS 会渲染成方块） */
    this.fontFamily = resolvePanelFontFamily();
    this.createCanvas = createCanvas;
    this.Path2D = Path2D;
    const probe = createCanvas(8, 8);
    this.measureCtx = probe.getContext('2d');
    this.measureCtx.font = panelFont(700, FONT_SIZE);
    this.ready = true;
    return this;
  }

  /**
   * 文本 → 1-bit 位图。
   * @param {string} text
   * @param {{maxWidth?:number}} [options]
   * @returns {{w:number,h:number,bits:Buffer}|null}
   */
  render(text, { maxWidth = MAX_WIDTH } = {}) {
    const clean = String(text ?? '').replace(/[\r\n\t]+/g, ' ').replace(/\s+/g, ' ').trim();
    if (!this.ready || !clean) return null;
    const limit = Math.max(8, Math.min(MAX_WIDTH, Math.floor(maxWidth / 8) * 8));
    const key = `${limit}\u0000${clean}`;
    const cached = this.cache.get(key);
    if (cached) {
      this.cache.delete(key);
      this.cache.set(key, cached);
      return cached;
    }
    try {
      /* v4 的标题是"1 行 + 省略号"（lines(title,316,25,1)）：超宽就裁到放得下 `…` */
      let draw = clean;
      if (this.measureCtx.measureText(draw).width > limit - 4) {
        while (draw.length > 1
          && this.measureCtx.measureText(`${draw}…`).width > limit - 4) {
          draw = draw.slice(0, -1);
        }
        draw = `${draw.trimEnd()}…`;
      }
      const width = Math.ceil((this.measureCtx.measureText(draw).width + 4) / 8) * 8;
      const w = Math.max(8, Math.min(limit, width));
      const canvas = this.createCanvas(w, HEIGHT);
      const ctx = canvas.getContext('2d');
      ctx.fillStyle = '#000000';
      ctx.fillRect(0, 0, w, HEIGHT);
      ctx.fillStyle = '#FFFFFF';
      ctx.font = panelFont(700, FONT_SIZE);
      // 居中的卡片（标题/选项）由固件决定位置，这里永远左对齐到 (2, baseline)
      ctx.textAlign = 'left';
      ctx.textBaseline = 'alphabetic';
      ctx.fillText(draw, 2, BASELINE);
      const bits = packGray(ctx, w);
      const bitmap = { w, h: HEIGHT, bpp: BPP, bits };
      this.cache.set(key, bitmap);
      while (this.cache.size > CACHE_LIMIT) this.cache.delete(this.cache.keys().next().value);
      return bitmap;
    } catch (error) {
      if (!this.failed) {
        this.failed = error.message;
      }
      return null;
    }
  }
}

/**
 * 文本 → **多行**位图（按宽度折行、每行居中）。
 *
 * BLE 卡片层的位图槽原先只有一行高度，长题干/长选项只能截断。固件把槽高抬到
 * 44 px 之后，这里按 2 行渲染就能完整放下；行数仍受 `maxLines` 限制，超了才截断。
 *
 * @param {string} text
 * @param {{maxWidth?:number, maxLines?:number, fontSize?:number, lineHeight?:number}} [opts]
 * @returns {{w:number,h:number,bpp:number,bits:Buffer}|null}
 */
CardBitmapRenderer.prototype.renderBlock = function renderBlock(text, opts = {}) {
  const { maxWidth = MAX_WIDTH, maxLines = 2, fontSize = 16, lineHeight = 17 } = opts;
  const clean = String(text ?? '').replace(/[\r\n\t]+/g, ' ').replace(/\s+/g, ' ').trim();
  if (!this.ready || !clean) return null;
  const limit = Math.max(8, Math.min(MAX_WIDTH, Math.floor(maxWidth / 8) * 8));
  const key = `B${limit}|${maxLines}|${fontSize}|${clean}`;
  const cached = this.cache.get(key);
  if (cached) {
    this.cache.delete(key);
    this.cache.set(key, cached);
    return cached;
  }
  try {
    this.measureCtx.font = panelFont(700, fontSize);
    const fits = (t) => this.measureCtx.measureText(t).width <= limit - 6;
    const rows = [];
    let rest = clean;
    while (rest && rows.length < maxLines) {
      let take = rest.length;
      while (take > 1 && !fits(rest.slice(0, take))) take -= 1;
      if (take < rest.length) {
        const space = rest.lastIndexOf(' ', take);
        if (space > 0 && space > take * 0.6) take = space;
      }
      let row = rest.slice(0, take).trim();
      rest = rest.slice(take).trim();
      if (!row) break;
      const lastLine = rows.length === maxLines - 1;
      if (lastLine && rest) {
        while (row.length > 1 && !fits(`${row}…`)) row = row.slice(0, -1);
        row = `${row.trimEnd()}…`;
        rest = '';
      }
      rows.push(row);
    }
    if (!rows.length) return null;
    const h = rows.length * lineHeight;
    const canvas = this.createCanvas(limit, h);
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, limit, h);
    ctx.fillStyle = '#FFFFFF';
    ctx.font = panelFont(700, fontSize);
    ctx.textAlign = 'center';
    ctx.textBaseline = 'alphabetic';
    rows.forEach((row, i) => {
      ctx.fillText(row, limit / 2, i * lineHeight + Math.round(fontSize * 0.82));
    });
    const bitmap = { w: limit, h, bpp: BPP, bits: packGray(ctx, limit, h) };
    this.cache.set(key, bitmap);
    while (this.cache.size > CACHE_LIMIT) this.cache.delete(this.cache.keys().next().value);
    return bitmap;
  } catch (error) {
    if (!this.failed) this.failed = error.message;
    return null;
  }
};

/**
 * 画布 → 2-bit 行优先位图（每像素 2 bit，MSB 在前；值 0..3）。
 * 用红色通道当覆盖率：白字 = 3，黑底 = 0。
 */
function packGray(ctx, w, h = HEIGHT) {
  const rgba = ctx.getImageData(0, 0, w, h).data;
  const stride = w >> 2;
  const bits = Buffer.alloc(stride * h);
  for (let y = 0; y < h; y += 1) {
    const row = y * w;
    for (let x = 0; x < w; x += 1) {
      const level = Math.min(3, Math.round(rgba[(row + x) * 4] / 85));
      if (!level) continue;
      bits[y * stride + (x >> 2)] |= level << (6 - ((x & 3) << 1));
    }
  }
  return bits;
}

/**
 * 空态整行：DSH 鲸鱼 + `探索未至之境`（v4 用 Path2D 画鱼，这里渲成同一张 2-bit 位图）。
 * @returns {{w:number,h:number,bpp:number,bits:Buffer}|null}
 */
CardBitmapRenderer.prototype.renderEmpty = function renderEmpty(text = '探索未至之境') {
  if (!this.ready) return null;
  const key = `empty\u0000${text}`;
  const cached = this.cache.get(key);
  if (cached) return cached;
  try {
    const label = String(text);
    this.measureCtx.font = panelFont(500, 26);
    const textWidth = Math.ceil(this.measureCtx.measureText(label).width);
    const fishWidth = 34;
    const total = Math.min(MAX_WIDTH, 4 + fishWidth + 10 + textWidth + 4);
    const w = Math.max(8, Math.ceil(total / 8) * 8);
    const canvas = this.createCanvas(w, HEIGHT);
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, w, HEIGHT);
    if (typeof this.Path2D === 'function') {
      ctx.save();
      ctx.translate(2, 1);
      ctx.scale(fishWidth / 23.16, 28 / 17.04);
      ctx.fillStyle = '#FFFFFF';
      ctx.fill(new this.Path2D(dshFishPath));
      ctx.restore();
    }
    ctx.fillStyle = '#FFFFFF';
    ctx.font = panelFont(500, 26);
    ctx.textAlign = 'left';
    ctx.textBaseline = 'alphabetic';
    ctx.fillText(label, 4 + fishWidth + 10, BASELINE);
    const bits = packGray(ctx, w);
    const bitmap = { w, h: HEIGHT, bpp: BPP, bits };
    this.cache.set(key, bitmap);
    while (this.cache.size > CACHE_LIMIT) this.cache.delete(this.cache.keys().next().value);
    return bitmap;
  } catch {
    return null;
  }
};

export { HEIGHT as CARD_BITMAP_HEIGHT, MAX_WIDTH as CARD_BITMAP_MAX_WIDTH };
