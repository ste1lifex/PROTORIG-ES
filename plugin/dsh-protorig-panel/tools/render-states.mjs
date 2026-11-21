#!/usr/bin/env node
/*
 * render-states.mjs —— 渲染 v4 面板的参考图（PNG）
 *
 * 用途：和手表（BLE 本地页）对照。BLE 下的卡片版式是照这份 v4 渲染器的坐标做的
 * （research/66 §4.3），所以改版式前先看这里的参考图，不要凭印象。
 *
 * 用法：
 *   node tools/render-states.mjs                 # 输出到 ../../outputs/v4_ref/
 *   node tools/render-states.mjs --out D:/tmp    # 指定目录
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadDesign } from '../src/design.mjs';
import { loadCanvas } from '../src/deps.mjs';
import { MonitorCanvasRenderer } from '../src/render-monitor-canvas.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const outIndex = process.argv.indexOf('--out');
const outDir = outIndex > 0 ? process.argv[outIndex + 1] : path.resolve(here, '../../../outputs/v4_ref');
/* 按钮位置来自 tokens.json（#button 只有在 hotzones 里存在时才画） */
const tokens = loadDesign(process.env.PROTORIG_DESIGN_DIR ?? path.resolve(here, '../design/dsh_panel_v4')).tokens;

const metrics = {
  cpuPercent: 12.3, gpuPercent: 45.6, memoryPercent: 78.9, gpuMemoryPercent: 52.8,
  gpuVendor: 'NVIDIA', gpuName: 'RTX', networkRxBps: 43520, networkTxBps: 4915,
};
const base = {
  time: '18:42', date: '2026-11-07', battery: 97, balance: '¥91.38',
  footer: '', flashPct: -1, metrics, connection: 'usb',
  session: { state: 'RUNNING', title: 'BLE 对齐 USB', tool: 'bash', steps: 3, ctx: 42 },
  approval: null, question: null,
};

const states = [
  ['session', base],
  ['approval', { ...base, approval: { toolName: 'bash', cmd: 'rm -rf build', reason: '', countdown: 9, canAnswer: true } }],
  ['question', {
    ...base,
    question: {
      multiSelect: false, prompt: '先做哪一项？', index: 0, selected: [0], canAnswer: true,
      options: [{ label: '固件卡片层' }, { label: '插件触摸路由' }, { label: 'BLE 位图' }],
    },
  }],
  ['question-multi', {
    ...base,
    question: {
      multiSelect: true, prompt: '这两项都要做', index: 1, selected: [0, 1], canAnswer: true,
      options: [{ label: '固件卡片层' }, { label: '插件触摸路由' }, { label: 'BLE 位图' }],
    },
  }],
  ['empty', { ...base, session: { state: 'IDLE', title: '', tool: '', steps: 0, ctx: -1 } }],
];

async function main() {
  const { createCanvas } = await loadCanvas();
  const renderer = await new MonitorCanvasRenderer().init();
  fs.mkdirSync(outDir, { recursive: true });
  for (const [name, state] of states) {
    const image = renderer.draw(state, tokens);
    const canvas = createCanvas(480, 480);
    const ctx = canvas.getContext('2d');
    const data = ctx.createImageData(480, 480);
    data.data.set(image.rgba);
    ctx.putImageData(data, 0, 0);
    const file = path.join(outDir, `v4_${name}.png`);
    fs.writeFileSync(file, canvas.toBuffer('image/png'));
    console.log(`${name.padEnd(15)} -> ${file}`);
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
