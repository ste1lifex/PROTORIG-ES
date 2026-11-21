/*
 * renderer.mjs —— SVG → RGBA（resvg）
 *
 * 手表端不渲染文字（research/56 §3）：整个界面在电脑侧画成 SVG，
 * 光栅化成 480×480 RGBA，再转成 RGB565 帧缓冲。
 *
 * 字体优先用包内自带的 ProtorigPanelSans（`assets/fonts/`，Noto Sans SC 子集，OFL）；
 * 开发机回退到仓库里的思源黑体 OTF：firmware/lvgl_test/fonts_src
 * （旧开发机副本 firmware_m2/fonts_src/SourceHanSansSC-{Regular,Medium,Bold}.otf）。
 */
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { loadResvg } from './deps.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));

const DEFAULT_FONT_DIR = process.env.PROTORIG_FONT_DIR
  ?? path.resolve(here, '../../../../firmware_m2/fonts_src');
const FALLBACK_FONT_DIR = path.resolve(here, '../../../firmware/lvgl_test/fonts_src');
const FALLBACK_FONT_DIRS = [
  FALLBACK_FONT_DIR,
  path.resolve(here, '../../../firmware/font/ttf'),
];
/** 包内自带字体（npm 包 assets/fonts；macOS/Windows 完全一致）。 */
const BUNDLED_FONT_DIR = path.resolve(here, '../assets/fonts');
const BUNDLED_FONT_FILES = [
  'ProtorigPanelSans-Regular.ttf',
  'ProtorigPanelSans-Medium.ttf',
  'ProtorigPanelSans-Bold.ttf',
];

export class PanelRenderer {
  /**
   * @param {{fontFiles?:string[], loadSystemFonts?:boolean, fontDir?:string}} opts
   */
  constructor({ fontFiles, loadSystemFonts, fontDir } = {}) {
    this.fontDir = fontDir ?? DEFAULT_FONT_DIR;
    const windowsUiFont = path.join(process.env.WINDIR ?? 'C:/Windows', 'Fonts', 'msyh.ttc');
    this.fontFiles = fontFiles ?? [
      ...BUNDLED_FONT_FILES.map((f) => path.join(BUNDLED_FONT_DIR, f)),
      ...['SourceHanSansSC-Regular.otf', 'SourceHanSansSC-Medium.otf', 'SourceHanSansSC-Bold.otf'].map((f) =>
        path.join(this.fontDir, f),
      ),
      ...['SourceHanSansSC-Regular.otf', 'SourceHanSansSC-Medium.otf', 'SourceHanSansSC-Bold.otf'].map((f) =>
        path.join(FALLBACK_FONT_DIR, f),
      ),
      ...(fs.existsSync(windowsUiFont) ? [windowsUiFont] : []),
    ].filter((file) => fs.existsSync(file));
    /* 发行版不一定带 OTF：非 Windows 机器（macOS/Linux）直接让 resvg 加载系统字体，
     * 否则 SVG 里的中文会渲染成方块。 */
    this.loadSystemFonts = loadSystemFonts ?? process.platform !== 'win32';
    this.Resvg = null;
  }

  async init() {
    if (this.Resvg) return this;
    this.Resvg = await loadResvg();
    return this;
  }

  /**
   * @param {string} svg 完整 SVG 文档
   * @returns {{rgba: Buffer|Uint8Array, width:number, height:number}}
   */
  render(svg) {
    if (!this.Resvg) throw new Error('PanelRenderer.init() 还没调用');
    const img = new this.Resvg(svg, {
      fitTo: { mode: 'original' },
      font: {
        fontFiles: this.fontFiles,
        loadSystemFonts: this.loadSystemFonts,
        defaultFontFamily: process.env.PROTORIG_PANEL_FONT_FAMILY || 'PROTORIG Panel Sans',
      },
    }).render();
    return { rgba: img.pixels, width: img.width, height: img.height };
  }
}
