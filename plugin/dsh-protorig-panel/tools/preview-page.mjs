import fs from 'node:fs';
import path from 'node:path';
import { buildPanelSvg, loadDesign } from '../src/design.mjs';
import { loadResvg } from '../src/deps.mjs';
import { PanelRenderer } from '../src/renderer.mjs';

export async function renderPreview({ dir, sourceFiles, renderer, revision, title, intro, accent, base, cases }) {
  const design = loadDesign(dir);
  const browserSource = sourceFiles.map((file) => fs.readFileSync(file, 'utf8')
    .replace(/^import .*;\r?\n/gm, '')
    .replace(/\nexport \{[^}]+\};\s*$/, '')).join('\n');
  const html = `<!doctype html>
  <!-- 由 render-preview.mjs 生成 -->
  <html lang="zh-CN">
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>PROTORIG ES · DSH 圆屏预览</title>
  <style>
  :root { color-scheme: dark; font: 15px/1.65 "Microsoft YaHei", system-ui, sans-serif; color: #f5f3e6; background: #101411; }
  * { box-sizing: border-box; }
  body { margin: 0 auto; padding: 36px 24px; max-width: 1140px; }
  header { border-bottom: 1px solid #3a483e; padding: 0 0 18px; display: flex; justify-content: space-between; gap: 16px; align-items: center; }
  .id, .stamp, .overline { font: 700 12px/1.5 Consolas, monospace; letter-spacing: .16em; }
  .id, .overline { color: ${accent}; }
  .stamp { color: #a6b6a9; }
  main { display: grid; grid-template-columns: minmax(0, 1.38fr) minmax(0, 1fr); align-items: center; gap: clamp(24px,5vw,74px); padding: 32px 0 0; }
  .stage { min-width: 0; display: grid; justify-items: center; gap: 16px; }
  .watch { width: min(100%, 480px); aspect-ratio: 1; position: relative; border-radius: 50%; overflow: hidden; box-shadow: 0 0 0 2px #48534a, 0 0 0 8px #1c231e, 0 18px 48px #080b09; }
  .watch > svg { width: 100%; height: 100%; display: block; font-family: "Microsoft YaHei", "Noto Sans CJK SC", sans-serif; }
  .watch .action { position: absolute; border: 0; margin: 0; background: transparent; cursor: pointer; }
  .watch .action:hover { background: ${revision === 'V4' ? 'radial-gradient(ellipse at center, #ffffff18, transparent 70%)' : '#fff1'}; }
  .watch .action:focus-visible { outline: 2px solid #f5f3e6; outline-offset: -4px; }
  aside { min-width: 0; }
  h1 { font-size: clamp(30px,4.8vw,52px); line-height: 1.12; letter-spacing: -.05em; margin: 10px 0 16px; }
  p { margin: 0 0 22px; color: #b9c8bc; }
  nav { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 6px; margin: 0 0 18px; }
  nav button, .controls button { font: inherit; padding: 8px 6px; color: #e1e9dc; background: #1a221c; border: 1px solid #3a483e; cursor: pointer; }
  nav button[aria-pressed="true"] { color: #111512; background: ${accent}; border-color: ${accent}; font-weight: 700; }
  .controls { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
  .controls button { padding: 4px 10px; }
  #message { min-height: 5.2em; margin: 18px 0 0; padding-top: 16px; border-top: 1px solid #3a483e; }
  small { color: #a6b6a9; }
  @media (max-width: 760px) {
    body { padding: 20px 16px; }
    main { grid-template-columns: 1fr; }
    .stage { order: 1; }
    #message { min-height: 0; }
  }
  </style>
  <header><span class="id">PROTORIG ES / V1.6</span><span class="stamp">480 × 480 · RGB565</span></header>
  <main>
    <section class="stage" aria-label="圆屏模拟器">
      <div id="watch" class="watch"></div>
      <small>480 像素圆屏预览 · 小窗口自动缩放</small>
    </section>
    <aside>
      <div class="overline">DSH / PC MODE / ${revision}</div>
      <h1>${title}</h1>
      <p>${intro}</p>
      <nav aria-label="预览状态"></nav>
      <div class="controls">
        <button id="reset">重置本态</button>
        <button id="timer" hidden>模拟 10 秒移交</button>
        <small>样例数据 · 本地交互</small>
      </div>
      <p id="message" aria-live="polite"></p>
    </aside>
  </main>
  <script>
  ${browserSource}
  const tokens = ${JSON.stringify(design.tokens)};
  const base = ${JSON.stringify(base)};
  const cases = ${JSON.stringify(cases)};
  const watch = document.getElementById('watch');
  const nav = document.querySelector('nav');
  const message = document.getElementById('message');
  const timer = document.getElementById('timer');
  let index = 0;
  let model;
  let interval;

  function render(focus) {
    watch.innerHTML = ${renderer}(model, tokens);
    const svg = watch.firstElementChild;
    svg.setAttribute('role', 'img');
    svg.setAttribute('aria-label', model.time + '，' + cases[index][0]);
    for (const target of svg.querySelectorAll('[data-action]')) {
      const name = target.dataset.action;
      const [x, y, w, h] = tokens.hotzones[name];
      const control = document.createElement('button');
      control.className = 'action';
      control.dataset.control = name;
      control.style.cssText = 'left:' + x / 4.8 + '%;top:' + y / 4.8 + '%;width:' + w / 4.8 + '%;height:' + h / 4.8 + '%';
      control.setAttribute('aria-label', target.textContent.trim().replace(/\s+/g, ' '));
      control.addEventListener('click', () => act(name));
      watch.append(control);
    }
    if (focus) (watch.querySelector('[data-control="' + focus + '"]') ?? nav.children[index]).focus({ preventScroll: true });
  }

  function pick(i) {
    clearInterval(interval);
    index = i;
    const [, description, scenario] = cases[i];
    model = { ...base, ...structuredClone(scenario) };
    for (const [j, button] of [...nav.children].entries()) button.setAttribute('aria-pressed', String(j === i));
    timer.hidden = !model.approval;
    timer.disabled = false;
    message.textContent = description;
    render();
  }

  function act(action) {
    const q = model.question;
    if (action === 'next') q.index = (q.index + 1) % q.options.length;
    if (action === 'option') {
      if (q.selected.includes(q.index)) q.selected = q.selected.filter((i) => i !== q.index);
      else q.selected = q.multiSelect ? [...q.selected, q.index] : [q.index];
      message.textContent = '已选 ' + q.selected.length + ' 项。';
    }
    if (action === 'submit' || action === 'allow' || action === 'deny') {
      clearInterval(interval);
      message.textContent = { submit: '已模拟提交 ' + (q?.selected.length ?? 0) + ' 项。', allow: '已模拟允许执行一次。', deny: '已模拟拒绝执行。' }[action];
      model.approval = model.question = null;
      timer.hidden = true;
    }
    render(action);
  }

  cases.forEach(([name], i) => {
    const button = document.createElement('button');
    button.textContent = name;
    button.addEventListener('click', () => pick(i));
    nav.append(button);
  });
  document.getElementById('reset').addEventListener('click', () => pick(index));
  timer.addEventListener('click', () => {
    timer.disabled = true;
    interval = setInterval(() => {
      model.approval.countdown--;
      if (model.approval.countdown === 0) {
        clearInterval(interval);
        message.textContent = '已移交电脑，表上的审批按钮停用。';
      }
      render(document.activeElement?.dataset.control);
    }, 1000);
  });
  pick(0);
  </script>
  </html>`;
  fs.writeFileSync(path.join(dir, 'index.html'), html);

  const Resvg = await loadResvg();
  const fonts = new PanelRenderer().fontFiles;
  const frames = [0, 1, 2].map((index) => {
    const svg = buildPanelSvg({ ...base, ...cases[index][2] }, design);
    const rendered = new Resvg(svg, { font: { fontFiles: fonts, loadSystemFonts: false } }).render();
    const pixels = rendered.pixels;
    for (let i = 0; i < pixels.length; i += 4) {
      pixels[i] = Math.round((pixels[i] >> 3) * 255 / 31);
      pixels[i + 1] = Math.round((pixels[i + 1] >> 2) * 255 / 63);
      pixels[i + 2] = Math.round((pixels[i + 2] >> 3) * 255 / 31);
    }
    // One horizontal run per color in each scanline, used to embed the quantized frame in the sheet.
    const paths = new Map();
    for (let y = 0; y < 480; y++) {
      for (let x = 0; x < 480;) {
        const pos = (y * 480 + x) * 4;
        const rgb = pixels.subarray(pos, pos + 3).toString('hex');
        let end = x + 1;
        while (end < 480 && pixels.subarray((y * 480 + end) * 4, (y * 480 + end) * 4 + 3).toString('hex') === rgb) end++;
        const run = `M${x} ${y}h${end - x}v1h-${end - x}z`;
        paths.set(rgb, (paths.get(rgb) ?? '') + run);
        x = end;
      }
    }
    const art = [...paths].map(([rgb, d]) => `<path fill="#${rgb}" d="${d}"/>`).join('');
    return `<g transform="translate(${24 + index * 504} 58)">
      <text x="240" y="-17" text-anchor="middle" font-size="22" fill="${accent}">${cases[index][0]}</text>
      ${art}
    </g>`;
  });
  const sheet = `<svg xmlns="http://www.w3.org/2000/svg" width="1536" height="562" font-family="Source Han Sans SC">
    <rect width="1536" height="562" fill="#090D0C"/>${frames.join('')}
  </svg>`;
  fs.writeFileSync(path.join(dir, 'preview.png'), new Resvg(sheet, { font: { fontFiles: fonts, loadSystemFonts: false } }).render().asPng());
}
