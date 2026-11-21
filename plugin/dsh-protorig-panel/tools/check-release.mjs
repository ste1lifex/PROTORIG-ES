#!/usr/bin/env node
/*
 * check-release.mjs —— 发行版依赖自检（npm 之外不允许有运行时外部依赖）
 *
 * 背景（2026-11-09）：发行版要求"装完 npm/pnpm 就能跑"，不允许用户再装
 * Python、FFmpeg、外部脚本之类。这个脚本扫描源码里的外部引用并列出分类：
 *
 *   ERROR  运行时功能依赖了 npm 之外的路径/命令 → 必须修
 *   WARN   开发功能的可选回退（字体 OTF 目录、烧录工具链）→ 允许，但要有文档
 *
 * 用法：node tools/check-release.mjs   （package.json: npm run check:release）
 * 退出码：有 ERROR → 1；否则 0。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const errors = [];
const warnings = [];
const ok = [];

function walk(dir, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) walk(full, out);
    else if (/\.(mjs|cjs|js)$/.test(entry.name)) out.push(full);
  }
  return out;
}

const pkg = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));

/* 1) 运行时依赖必须全部来自 npm（package.json dependencies） */
const deps = pkg.dependencies ?? {};
for (const name of ['serialport', '@napi-rs/canvas', '@resvg/resvg-js', '@deepseek-ai/schemastery', 'systeminformation', '@stoprocent/noble']) {
  if (!deps[name]) errors.push(`缺少运行时依赖 ${name}（package.json dependencies）`);
}
ok.push(`npm 运行时依赖：${Object.keys(deps).join(', ')}`);

/* 2) 包内资源（字体） */
const fontDir = path.join(root, 'assets/fonts');
for (const file of ['ProtorigPanelSans-Regular.ttf', 'ProtorigPanelSans-Medium.ttf', 'ProtorigPanelSans-Bold.ttf', 'OFL.txt', 'SOURCES.json']) {
  if (!fs.existsSync(path.join(fontDir, file))) errors.push(`缺少包内资源 assets/fonts/${file}（运行 tools/build_panel_fonts.py）`);
}
if (!(pkg.files ?? []).includes('assets')) errors.push('package.json files 必须包含 assets（否则 npm 包不带字体）');
ok.push('包内字体 + OFL + 来源记录齐全，且会被打包');

/* 3) 扫描源码：禁止 npm 之外的运行时依赖 */
const FORBIDDEN = [
  { pattern: /bleak|ble_bridge\.py/, hint: 'Python BLE 桥已被 @stoprocent/noble 取代' },
  { pattern: /require\(['"].*desktop\/|from ['"].*desktop\//, hint: '不能 require 仓库 desktop/ 目录（外部路径）' },
  { pattern: /desktop\/src|desktop\/node_modules/, hint: '不能引用 desktop/ 外部路径' },
];
const ALLOWED_DEV_FALLBACK = [
  { pattern: /firmware_m2\/(fonts_src|flash\.ps1)|firmware\/lvgl_test\/(build|fonts_src)|firmware\/font\/ttf/, hint: '烧录/字体开发机回退（发行版不要求存在；V1.6 布局优先，旧 firmware_m2 仅兼容）' },
];
for (const file of walk(path.join(root, 'src'))) {
  const rel = path.relative(root, file);
  const lines = fs.readFileSync(file, 'utf8').split('\n');
  lines.forEach((line, index) => {
    const code = line.trim();
    if (!code || code.startsWith('//') || code.startsWith('*') || code.startsWith('/*')) return; // 注释不算依赖
    for (const rule of FORBIDDEN) {
      if (rule.pattern.test(code)) errors.push(`${rel}:${index + 1}: 命中禁止模式（${rule.hint}）→ ${code.slice(0, 100)}`);
    }
    for (const rule of ALLOWED_DEV_FALLBACK) {
      if (rule.pattern.test(code)) warnings.push(`${rel}:${index + 1}: 开发机可选回退（${rule.hint}）`);
    }
  });
}

/* 4) 发行功能边界：烧录是固件开发功能（需要 SDK/工具链，不是 npm 能带的） */
const flashExternals = [
  'src/flash.mjs 依赖 firmware/lvgl_test/flash.ps1（Windows 固件工程；旧 firmware_m2 仅兼容）',
  'src/flash-mac.mjs 依赖 Bouffalo SDK 的 BLFlashCommand-macos + Rosetta 2',
];
for (const line of flashExternals) warnings.push(`烧录（开发功能）: ${line}`);

/* 5) 报告 */
console.log('== 发行版依赖自检 ==\n');
for (const line of ok) console.log(`  OK   ${line}`);
for (const line of warnings) console.log(`  WARN ${line}`);
for (const line of errors) console.log(`  ERR  ${line}`);
console.log('');
console.log(`npm pack 体积参考：${'（运行 npm pack --dry-run 查看）'}`);
if (errors.length > 0) {
  console.log(`\nFAIL：${errors.length} 个外部依赖问题需要修`);
  process.exit(1);
}
console.log('PASS：运行时依赖只有 npm 包 + 包内资源；其余只有"开发功能"的可选前置。');
