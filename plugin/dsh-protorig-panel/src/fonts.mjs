/*
 * fonts.mjs —— 面板/卡片渲染的运行时字体解析
 *
 * 为什么需要：
 *   Skia（@napi-rs/canvas）里写死 `SHSans` 只是 **Windows 开发机注册的别名**
 *   （早期开发机把 firmware_m2/fonts_src 的思源黑体 OTF 注册成这个别名；
 *   该字体现在在 firmware/lvgl_test/fonts_src）。换到没注册字体文件的
 *   机器（比如 macOS），`SHSans` 找不到 → 回退到无中文字形的字体 → 中文全变方块。
 *
 * 策略（按优先级，全部运行时探测）：
 *   1. 环境变量 PROTORIG_PANEL_FONT_FAMILY：直接指定一个系统字体族；
 *   2. 环境变量 PROTORIG_PANEL_FONT_FILE：注册一个自定义 TTF/OTF 为 "SHSans"；
 *   3. 仓库里能找到的思源黑体 OTF（firmware/lvgl_test/fonts_src；旧开发机副本
 *      firmware_m2/fonts_src）→ 注册为 "SHSans"；
 *   4. 系统已装的 CJK 族：SHSans / Source Han Sans SC / Source Han Sans /
 *      HarmonyOS Sans SC / Heiti SC / Hiragino Sans GB / PingFang SC /
 *      Microsoft YaHei / Noto Sans CJK SC …；
 *   5. 都没有才退到 sans-serif（会缺字，至少不崩）。
 *
 * macOS 实测（2026-11-09）：系统有 `Source Han Sans SC`，所以无需装字体就能正常渲染中文。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { GlobalFonts } from '@napi-rs/canvas';

const here = path.dirname(fileURLToPath(import.meta.url));

/** 字体来源目录（按优先级）：
 *   1. 包内自带（npm 包 assets/fonts，macOS/Windows 完全一致）——发行版主路径；
 *   2. 开发机/仓库里的思源黑体 OTF（firmware/lvgl_test/fonts_src；旧开发机副本
 *      firmware_m2/fonts_src）。 */
const BUNDLED_FONT_DIR = path.resolve(here, '../assets/fonts');
const FONT_DIRS = [
  process.env.PROTORIG_FONT_DIR,
  BUNDLED_FONT_DIR,
  path.resolve(here, '../../../../firmware_m2/fonts_src'),
  path.resolve(here, '../../../firmware/lvgl_test/fonts_src'),
  path.resolve(here, '../../../firmware/font/ttf'),
].filter(Boolean);

/** 包内字体（tools/build_panel_fonts.py 生成；OFL，见 assets/fonts/OFL.txt）。 */
const BUNDLED_FONTS = [
  { family: 'PROTORIG Panel Sans', weight: 400, file: 'ProtorigPanelSans-Regular.ttf' },
  { family: 'PROTORIG Panel Sans Medium', weight: 500, file: 'ProtorigPanelSans-Medium.ttf' },
  { family: 'PROTORIG Panel Sans Bold', weight: 700, file: 'ProtorigPanelSans-Bold.ttf' },
];

const LEGACY_FONT_FILES = [
  'SourceHanSansSC-Regular.otf',
  'SourceHanSansSC-Medium.otf',
  'SourceHanSansSC-Bold.otf',
];

/** 系统字体族优先级（与固件字体同源的思源黑体排最前，视觉最接近设计稿）。 */
const FAMILY_CANDIDATES = [
  'SHSans',
  'Source Han Sans SC',
  'Source Han Sans',
  'HarmonyOS Sans SC',
  'HarmonyOS Sans',
  'Heiti SC',
  'Hiragino Sans GB',
  'PingFang SC',
  'Microsoft YaHei',
  'Noto Sans CJK SC',
  'Noto Sans SC',
  'Arial Unicode MS',
];

let registered = false;
let resolvedFamily = null;
/** 权重 → 实际注册的 Skia 家族名（包内字体为「一个权重一个别名」，避免 Skia 挑错字重）。 */
let weightFamilies = null;

function families() {
  return new Set(GlobalFonts.families.map((entry) => entry.family));
}

function tryRegister(file, alias) {
  try {
    if (!file || !fs.existsSync(file)) return false;
    // @napi-rs/canvas 成功时返回 FontKey 对象（不是 true），失败返回 null/抛错
    const result = GlobalFonts.registerFromPath(file, alias);
    return Boolean(result);
  } catch {
    return false;
  }
}

/** 注册字体：包内字体（全平台一致）→ 环境变量指定文件 → 开发机思源黑体。 */
function registerBundledFonts() {
  if (registered) return;
  registered = true;

  // 1) npm 包自带的 Noto Sans SC 子集（重新改名为 PROTORIG Panel Sans）
  const mapping = {};
  let bundledAll = true;
  for (const entry of BUNDLED_FONTS) {
    const file = path.join(BUNDLED_FONT_DIR, entry.file);
    if (!tryRegister(file, entry.family)) bundledAll = false;
    else mapping[entry.weight] = entry.family;
  }
  if (bundledAll) {
    weightFamilies = mapping;
    return;
  }
  weightFamilies = null;

  // 2) 环境变量指定一个字体文件（开发/自定义字体）
  const envFile = process.env.PROTORIG_PANEL_FONT_FILE;
  if (envFile && tryRegister(envFile, 'SHSans')) {
    weightFamilies = { 400: 'SHSans', 500: 'SHSans', 700: 'SHSans' };
    return;
  }

  // 3) 开发机上的思源黑体 OTF（firmware/lvgl_test/fonts_src 优先，旧副本 firmware_m2/fonts_src）
  for (const dir of FONT_DIRS) {
    const names = LEGACY_FONT_FILES.filter((name) => fs.existsSync(path.join(dir, name)));
    if (names.length === 0) continue;
    const byWeight = { 400: 'SHSans', 500: 'SHSans', 700: 'SHSans' };
    names.forEach((name, index) => {
      const alias = index === 0 ? 'SHSans' : `SHSans ${index}`;
      if (tryRegister(path.join(dir, name), alias)) byWeight[[400, 500, 700][index] ?? 400] = alias;
    });
    weightFamilies = byWeight;
    return;
  }
}

/**
 * 解析当前机器上可用的面板字体族（结果缓存；测试可用 resetPanelFontCache 清掉）。
 * @returns {string} 字体族名（保证非空）
 */
export function resolvePanelFontFamily() {
  if (resolvedFamily) return resolvedFamily;
  registerBundledFonts();
  const envFamily = String(process.env.PROTORIG_PANEL_FONT_FAMILY ?? '').trim();
  const available = families();
  if (envFamily && (available.has(envFamily) || envFamily === 'sans-serif')) {
    resolvedFamily = envFamily;
    return resolvedFamily;
  }
  if (weightFamilies) {
    resolvedFamily = weightFamilies[400];
    return resolvedFamily;
  }
  resolvedFamily = FAMILY_CANDIDATES.find((family) => available.has(family)) || 'sans-serif';
  return resolvedFamily;
}

/** 某字重实际使用的 Skia 家族名（包内字体按权重注册成不同别名）。 */
export function resolvePanelFontFamilyForWeight(weight = 400) {
  resolvePanelFontFamily();
  const name = Number(weight) >= 700
    ? (weightFamilies?.[700] ?? resolvedFamily)
    : Number(weight) >= 500
      ? (weightFamilies?.[500] ?? resolvedFamily)
      : (weightFamilies?.[400] ?? resolvedFamily);
  return name || 'sans-serif';
}

/** 测试用：清掉缓存（改环境变量后重新探测）。 */
export function resetPanelFontCache() {
  resolvedFamily = null;
  registered = false;
  weightFamilies = null;
}

/**
 * canvas ctx.font 的字体串（自动带引号，族名含空格也安全）。
 * @param {number} weight 100..900
 * @param {number} sizePx 像素
 */
export function panelFont(weight, sizePx) {
  return `${weight} ${sizePx}px "${resolvePanelFontFamilyForWeight(weight)}"`;
}

/** 诊断信息（/state、日志、设置页"高级/诊断"用）。 */
export function panelFontInfo() {
  const family = resolvePanelFontFamily();
  const available = families();
  return {
    family,
    bundled: Boolean(weightFamilies),
    weights: weightFamilies ? { ...weightFamilies } : null,
    fallback: family === 'sans-serif',
    bundledDir: BUNDLED_FONT_DIR,
    totalFamilies: available.size,
    candidates: FAMILY_CANDIDATES.filter((name) => available.has(name)),
    platform: process.platform,
  };
}
