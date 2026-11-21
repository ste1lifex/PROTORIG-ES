/*
 * framebuffer.mjs —— 480×480 RGB565 影子帧缓冲 + 16 行带脏块刷新
 *
 * 手表没有整帧缓存，也不会自己做脏区检测（research/56 §3）：电脑必须自己维护
 * 完整画面，只把变化的 16 行带重新发一遍。这个模块就是那层。
 *
 * 像素格式：RGB565 小端（fmt=2）。Uint16Array 存数值，发的时候拆成 LE 字节。
 */
import { tileBegin, tileData, tileEnd } from './protocol.mjs';

export const W = 480;
export const H = 480;
export const BAND_H = 16;
export const BAND_COUNT = H / BAND_H; // 30

export class FrameBuffer {
  constructor(w = W, h = H, bandH = BAND_H) {
    this.w = w;
    this.h = h;
    this.bandH = bandH;
    this.bandCount = Math.ceil(h / bandH);
    this.px = new Uint16Array(w * h); // 期望画面
    this.sent = new Uint16Array(w * h); // 上次成功发出的画面
    this.sentBands = new Uint8Array(this.bandCount); // 每条带是否发过一次
    this.bytesPerBand = w * bandH * 2;
  }

  /** 整屏填充一个 565 颜色。 */
  fill(color) {
    this.px.fill(color);
  }

  /** 填一块矩形（不做圆角/裁剪，调用方保证在界内）。 */
  fillRect(x, y, w, h, color) {
    const x0 = Math.max(0, x | 0);
    const y0 = Math.max(0, y | 0);
    const x1 = Math.min(this.w, x0 + (w | 0));
    const y1 = Math.min(this.h, y0 + (h | 0));
    for (let yy = y0; yy < y1; yy++) {
      const row = yy * this.w;
      for (let xx = x0; xx < x1; xx++) this.px[row + xx] = color;
    }
  }

  setPixel(x, y, color) {
    if (x < 0 || y < 0 || x >= this.w || y >= this.h) return;
    this.px[y * this.w + x] = color;
  }

  /**
   * 用 resvg 渲染出来的 RGBA 像素覆盖整屏。
   * alpha < 255 时按黑底混合（设计是不透明的，这里只是兜底）。
   */
  setFromRGBA(rgba) {
    const need = this.w * this.h * 4;
    if (rgba.length < need) throw new Error(`RGBA 长度不足：${rgba.length} < ${need}`);
    for (let i = 0, p = 0; i < this.px.length; i++, p += 4) {
      const a = rgba[p + 3];
      let r = rgba[p];
      let g = rgba[p + 1];
      let b = rgba[p + 2];
      if (a !== 255) {
        r = (r * a) / 255;
        g = (g * a) / 255;
        b = (b * a) / 255;
      }
      this.px[i] = (((r >> 3) & 0x1f) << 11) | (((g >> 2) & 0x3f) << 5) | ((b >> 3) & 0x1f);
    }
  }

  /** 把某条带的像素导出成 RGB565 小端字节。 */
  bandBytes(band) {
    const y0 = band * this.bandH;
    const rows = Math.min(this.bandH, this.h - y0);
    const out = Buffer.allocUnsafe(this.w * rows * 2);
    let o = 0;
    for (let y = y0; y < y0 + rows; y++) {
      const row = y * this.w;
      for (let x = 0; x < this.w; x++) {
        const v = this.px[row + x];
        out[o++] = v & 0xff;
        out[o++] = (v >> 8) & 0xff;
      }
    }
    return out;
  }

  /** 每像素列的"这一带内是否有变化"位图缓存（避免每帧重复扫描） */
  _colDirty(band, rows) {
    const y0 = band * this.bandH;
    const w = this.w;
    const cols = this._cols ?? (this._cols = new Uint8Array(w));
    cols.fill(0);
    for (let y = y0; y < y0 + rows; y++) {
      const row = y * w;
      for (let x = 0; x < w; x++) {
        if (this.px[row + x] !== this.sent[row + x]) cols[x] = 1;
      }
    }
    return cols;
  }

  /**
   * 脏区：按 16 行带逐带求"变化的水平**连续段**"。
   *
   * 为什么不是每条带一个外框：v4 的三环是细弧但横跨左右两侧，外框会把
   * 20..460 整段吞掉（一条带 ~14 KB）。按连续段切分后，弧尾变化只剩几个
   * 几十像素宽的窄条，三环同跳从 308 KB 降到几 KB 量级。
   *
   * @returns {{band:number,x:number,y:number,w:number,h:number,full:boolean}[]}
   */
  dirtyRects() {
    const out = [];
    for (let b = 0; b < this.bandCount; b++) {
      const y0 = b * this.bandH;
      const rows = Math.min(this.bandH, this.h - y0);
      if (!this.sentBands[b]) {
        out.push({ band: b, x: 0, y: y0, w: this.w, h: rows, full: true });
        continue;
      }
      const cols = this._colDirty(b, rows);
      let x = 0;
      while (x < this.w) {
        while (x < this.w && !cols[x]) x++;
        if (x >= this.w) break;
        const startX = x;
        while (x < this.w && cols[x]) x++;

        /* ★ 窗口必须"起点偶、宽度偶"：RGB565 下一个 DBI 字装 2 像素，
         *   奇数宽窗口的最后一列只占半个字，硬件取低半字 → 最右一列必错。
         *   这是 docs/history/bug_thin_stroke_smearing.md §11 定位过的根因，
         *   LVGL 路径用 flushmode=1（x1&=~1,x2|=1）修的；电脑模式绕过 LVGL，
         *   必须在这里做同样的对齐，否则每次局部刷新最右列都会花。 */
        let x0 = startX & ~1;
        let x1 = Math.min(this.w - 1, (x - 1) | 1);
        if (x1 < x0) x1 = x0;
        out.push({ band: b, x: x0, y: y0, w: x1 - x0 + 1, h: rows, full: false });
      }
    }
    return out;
  }

  /** 把矩形内的像素导出成 RGB565 小端字节（行优先）。 */
  rectBytes(x, y, w, h) {
    const out = Buffer.allocUnsafe(w * h * 2);
    let o = 0;
    for (let yy = y; yy < y + h; yy++) {
      const row = yy * this.w;
      for (let xx = x; xx < x + w; xx++) {
        const v = this.px[row + xx];
        out[o++] = v & 0xff;
        out[o++] = (v >> 8) & 0xff;
      }
    }
    return out;
  }

  /** 把矩形记成"已发送"。 */
  markRectSent({ band, x, y, w, h }) {
    for (let yy = y; yy < y + h; yy++) {
      const row = yy * this.w;
      this.sent.set(this.px.subarray(row + x, row + x + w), row + x);
    }
    if (x === 0 && w === this.w) this.sentBands[band] = 1;
  }

  markAllSent() {
    this.sent.set(this.px);
    this.sentBands.fill(1);
  }

  /** 强制下一次全屏重发（重连后必须整屏重发，手表没有缓存）。 */
  invalidate() {
    // 计数：重连/重进电脑模式都要整屏重发，回归脚本要断言它真的涨了
    this.invalidations = (this.invalidations ?? 0) + 1;
    this.sentBands.fill(0);
  }
}

/**
 * 把帧缓冲的脏矩形刷到手表。
 * 逐个矩形串行发送，统计字节数与耗时，方便在 CLI 里看真实带宽。
 */
export class BandFlusher {
  constructor(link, fb, { onBand } = {}) {
    this.link = link;
    this.fb = fb;
    this.onBand = onBand;
    this.stats = { frames: 0, bytes: 0, ms: 0, bands: 0 };
  }

  /** @returns {Promise<{bands:number, frames:number, bytes:number, ms:number}>} */
  async flush({ force = false } = {}) {
    if (force) this.fb.invalidate();
    const rects = this.fb.dirtyRects();
    const t0 = performance.now();
    let frames = 0;
    let bytes = 0;

    for (const rect of rects) {
      const data = this.fb.rectBytes(rect.x, rect.y, rect.w, rect.h);
      await this.link.write(tileBegin(rect.x, rect.y, rect.w, rect.h));
      frames++;
      bytes += 12;
      for (const f of tileData(data)) {
        await this.link.write(f);
        frames++;
        bytes += f.length;
      }
      await this.link.write(tileEnd());
      frames++;
      bytes += 4;
      this.fb.markRectSent(rect);
      if (this.onBand) this.onBand(rect, rects.length);
    }

    const ms = performance.now() - t0;
    this.stats.frames += frames;
    this.stats.bytes += bytes;
    this.stats.ms += ms;
    this.stats.bands += rects.length;
    return { bands: rects.length, frames, bytes, ms };
  }
}
