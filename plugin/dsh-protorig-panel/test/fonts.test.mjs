/*
 * fonts.test.mjs —— 面板字体：发行版必须两端一致（包内字体，不依赖系统字体）
 *
 * 契约（2026-11-09）：
 *   · assets/fonts/ 里随 npm 包分发 Noto Sans SC 子集（OFL，改名为 PROTORIG Panel Sans）；
 *   · Skia 按权重注册三个别名（400/500/700），panelFont() 必须选对；
 *   · 不依赖 macOS 系统字体（旧实现写死 SHSans 导致方块）。
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createCanvas } from '@napi-rs/canvas';
import {
  panelFont,
  panelFontInfo,
  resetPanelFontCache,
  resolvePanelFontFamily,
  resolvePanelFontFamilyForWeight,
} from '../src/fonts.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');

test('包内字体齐全（OFL + 三个字重 + SOURCES）', () => {
  const dir = path.join(root, 'assets/fonts');
  for (const file of ['ProtorigPanelSans-Regular.ttf', 'ProtorigPanelSans-Medium.ttf', 'ProtorigPanelSans-Bold.ttf']) {
    const full = path.join(dir, file);
    assert.ok(fs.existsSync(full), `缺少 ${file}（运行 tools/build_panel_fonts.py）`);
    assert.ok(fs.statSync(full).size > 500 * 1024, `${file} 太小，可能不是完整子集`);
  }
  assert.ok(fs.existsSync(path.join(dir, 'OFL.txt')), '需随包分发 OFL 许可');
  assert.ok(fs.existsSync(path.join(dir, 'SOURCES.json')), '需记录字体来源');
  const pkg = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
  assert.ok(pkg.files.includes('assets'), 'package.json files 必须包含 assets（否则 npm 包不带字体）');
});

test('字体解析：使用包内家族（与平台无关），按权重映射三个别名', () => {
  resetPanelFontCache();
  const info = panelFontInfo();
  assert.equal(info.bundled, true, '发行版应使用包内字体');
  assert.equal(info.family, 'PROTORIG Panel Sans');
  assert.equal(resolvePanelFontFamily(), 'PROTORIG Panel Sans');
  assert.equal(resolvePanelFontFamilyForWeight(400), 'PROTORIG Panel Sans');
  assert.equal(resolvePanelFontFamilyForWeight(500), 'PROTORIG Panel Sans Medium');
  assert.equal(resolvePanelFontFamilyForWeight(700), 'PROTORIG Panel Sans Bold');
  assert.equal(panelFont(400, 24), '400 24px "PROTORIG Panel Sans"');
  assert.equal(panelFont(700, 48), '700 48px "PROTORIG Panel Sans Bold"');
});

test('渲染冒烟：中文字形真的画出来（不是方块）', () => {
  resetPanelFontCache();
  const canvas = createCanvas(200, 80);
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, 200, 80);
  ctx.fillStyle = '#fff';
  ctx.font = panelFont(400, 24);
  const width = ctx.measureText('中文测试').width;
  // 4 个汉字 @24px：正常中文字体约 96px；回退到无 CJK 的字体时明显偏小
  assert.ok(width > 80 && width < 120, `中文宽度异常：${width}`);
  ctx.fillText('等待 USB 连接', 4, 40);
  const png = canvas.toBuffer('image/png');
  assert.ok(png.length > 1000, '应输出有效 PNG');
});

test('权重不同 → 字体串不同（Regular vs Bold 不共用同一别名）', () => {
  assert.notEqual(panelFont(400, 24), panelFont(700, 24));
});
