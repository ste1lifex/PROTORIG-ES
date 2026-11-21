#!/usr/bin/env node
/*
 * render-design.mjs —— 把设计目录里的 mock_*.svg 与模板实渲染成 PNG，方便逐张评审
 *
 *   node tools/render-design.mjs                      # 默认 local/design/dsh_panel
 *   node tools/render-design.mjs ../../local/design/dsh_panel_v2
 *
 * 只用仓库里已有的 resvg + 思源黑体（OFL），不联网。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadResvg } from '../src/deps.mjs';
import { loadDesign, buildPanelSvg } from '../src/design.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const dir = process.argv[2] ?? path.resolve(here, '../../../local/design/dsh_panel');
const fontDir = [path.resolve(here, '../../../firmware/lvgl_test/fonts_src'),
                 path.resolve(here, '../../../../firmware_m2/fonts_src')]
  .find((dir) => fs.existsSync(dir)) ?? path.resolve(here, '../../../firmware/lvgl_test/fonts_src');
const fonts = ['Regular', 'Medium', 'Bold'].map((w) => path.join(fontDir, `SourceHanSansSC-${w}.otf`));

const Resvg = await loadResvg();
const opts = (svg) => ({
  fitTo: { mode: 'original' },
  font: { fontFiles: fonts, loadSystemFonts: false, defaultFontFamily: 'Source Han Sans SC' },
});

const files = fs.readdirSync(dir).filter((f) => f.startsWith('mock_') && f.endsWith('.svg'));
for (const f of files) {
  const svg = fs.readFileSync(path.join(dir, f), 'utf8');
  const img = new Resvg(svg, opts()).render();
  const out = path.join(dir, `preview_${f.replace('.svg', '.png')}`);
  fs.writeFileSync(out, img.asPng());
  console.log(`rendered ${f} -> ${path.basename(out)} (${img.width}x${img.height})`);
}

// 模板用样例数据渲染一遍，确认占位符齐全、版面与 mock 一致
if (fs.existsSync(path.join(dir, 'panel.tpl.svg'))) {
  const design = loadDesign(dir);
  const sample = {
    time: '23:47',
    battery: 97,
    balance: '¥12.34',
    footer: 'STREAM OK',
    flashPct: -1,
    session: {
      title: '实现手表端副屏的圆形布置',
      state: 'RUNNING',
      tool: 'bash',
      steps: 7,
      ctx: 33,
      tokens: 47200,
      ageSec: 2,
      elapsed: '01:23',
    },
  };
  for (const [name, st] of [
    ['tpl_running', sample],
    [
      'tpl_approval',
      {
        ...sample,
        footer: 'AWAITING · 10s',
        approval: { toolName: 'bash', cmd: 'git push --force origin main' },
        session: { ...sample.session, title: 'bash 请求执行命令', state: 'NEEDS' },
      },
    ],
  ]) {
    const svg = buildPanelSvg(st, design);
    const unresolved = (svg.match(/\{\{\w+\}\}/g) ?? []).length;
    const img = new Resvg(svg, opts()).render();
    fs.writeFileSync(path.join(dir, `preview_${name}.png`), img.asPng());
    console.log(`rendered ${name} (unresolved placeholders: ${unresolved})`);
  }
}
