/*
 * render-monitor-canvas.mjs —— v4 三环面板的 Skia 渲染
 *
 * 为什么不继续用 resvg：实测同一张 480×480，
 *   resvg + 20MB 思源 OTF  = 750-766 ms/帧（每帧重建字体库）
 *   resvg + 1.5MB 子集     = 110 ms/帧
 *   Skia（@napi-rs/canvas）= 1.27 ms/帧（含 getImageData）
 * 面板要 1 Hz 以上刷新，resvg 会把每秒预算吃光。
 *
 * 版面与 design-monitor.mjs 的 SVG 版逐条对应（同一套坐标，方便对照）：
 * 改了那边记得同步这里；两边都只做"状态 → 图元"，颜色/热区仍来自 tokens。
 */
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { loadCanvas } from './deps.mjs';
import { panelFont, resolvePanelFontFamily } from './fonts.mjs';
import { lines, roundedPercent, approvalView, questionView } from './design-orbit.mjs';
import { dshFishPath } from './assets/dsh-fish.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
/* 面板字体回退：优先 V1.6 仓库里的 fonts_src，其次旧 firmware_m2 开发副本。 */
const FONT_DIR = process.env.PROTORIG_FONT_DIR
  ?? [path.resolve(here, '../../../firmware/lvgl_test/fonts_src'),
      path.resolve(here, '../../../../firmware_m2/fonts_src')]
    .find((dir) => fs.existsSync(path.join(dir, 'SourceHanSansSC-Medium.otf')))
  ?? path.resolve(here, '../../../firmware/lvgl_test/fonts_src');
const FONT_FILES = ['SourceHanSansSC-Regular.otf', 'SourceHanSansSC-Bold.otf'];

const WHITE = '#F0F0F0';
const GRAY = '#A8A8A8';
const YELLOW = '#FFFA00';
const BLUE = '#8FB8FF';
const RED = '#FF6B6B';

const clampPercent = (v) => (Number.isFinite(v) && v >= 0 ? Math.min(100, v) : null);
const reading = (p) => (p == null ? '--' : `${p.toFixed(1)}%`);

function channels(metrics) {
  const vendor = `${metrics.gpuVendor ?? ''} ${metrics.gpuName ?? ''}`.toLowerCase();
  const gpuColor = /nvidia|geforce|quadro/.test(vendor) ? '#76B900'
    : /\bamd\b|radeon|advanced micro devices/.test(vendor) ? '#ED1C24'
      : /intel/.test(vendor) ? '#0091FF' : BLUE;
  return [
    ['CPU', 228, YELLOW, metrics.cpuPercent],
    ['GPU', 218, gpuColor, metrics.gpuPercent],
    ['RAM', 208, WHITE, metrics.memoryPercent],
  ].map(([name, radius, color, value]) => ({ name, radius, color, percent: clampPercent(value) }));
}

/** SVG 的"从正北顺时针"角度 → Canvas 弧度（0 在 +x 轴）。 */
const canvasAngle = (deg) => ((deg - 90) * Math.PI) / 180;

function rate(value) {
  if (!Number.isFinite(value) || value < 0) return '--';
  const units = ['B/s', 'KB/s', 'MB/s', 'GB/s'];
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit++; }
  return `${value >= 100 ? Math.round(value) : value.toFixed(1)} ${units[unit]}`;
}

export class MonitorCanvasRenderer {
  constructor({ fontFiles } = {}) {
    this.fontFiles = fontFiles ?? FONT_FILES.map((f) => path.join(FONT_DIR, f));
    this.canvas = null;
    this.ctx = null;
    this.ready = false;
  }

  async init() {
    if (this.ready) return this;
    const { createCanvas, GlobalFonts, Path2D } = await loadCanvas();
    for (const f of this.fontFiles) {
      if (fs.existsSync(f)) GlobalFonts.registerFromPath(f, 'SHSans');
    }
    /* 字体回退：SHSans 只是 Windows 开发机注册的别名；macOS 等机器没有这些 OTF，
     * 这里自动挑系统里的思源黑体/HarmonyOS/Heiti…（见 src/fonts.mjs）。 */
    this.fontFamily = resolvePanelFontFamily();
    this.canvas = createCanvas(480, 480);
    this.ctx = this.canvas.getContext('2d');
    // @napi-rs/canvas 的 Path2D 不是全局构造器：IDLE（画 DSH 鲸鱼）时才用到，
    // CLI 面板一直跑 RUNNING 所以没暴露过。存到实例上。
    this.Path2D = Path2D;
    this.ready = true;
    return this;
  }

  /* ---------------------------------------------------------- 基础图元 */

  #text(str, x, y, size, color = WHITE, weight = 400, align = 'middle') {
    const ctx = this.ctx;
    ctx.fillStyle = color;
    ctx.font = panelFont(weight, size);
    // canvas 的 textAlign 只有 start/end/left/right/center；
    // 直接透传 SVG 的 'middle' 会被忽略、退回上一次的值（默认 start），
    // 表现就是所有居中文字整体右移、标题从中心往右跑出屏幕。
    ctx.textAlign = align === 'middle' ? 'center' : align;
    ctx.textBaseline = 'alphabetic';
    ctx.fillText(String(str), x, y);
  }

  /** 三段弧：底轨（虚线）+ 进度（实线）。角度 210°→510°，与 SVG 版同一条 A 命令。 */
  #rings(list) {
    const ctx = this.ctx;
    for (const { radius, color, percent } of list) {
      const a0 = canvasAngle(210);
      const a1 = canvasAngle(510);
      // 底轨：SVG stroke-dasharray 按弧长换算，这里用同样的比例
      const circumference = radius * Math.PI * (300 / 180);
      ctx.save();
      ctx.lineWidth = 3;
      ctx.strokeStyle = '#4C4C4C';
      ctx.globalAlpha = percent == null ? 0.6 : 1;
      const dash = (percent == null ? 0.6 : 0.18) * circumference / 100;
      const gap = (percent == null ? 2 : 1.4866667) * circumference / 100;
      ctx.setLineDash([dash, gap]);
      ctx.beginPath();
      ctx.arc(240, 240, radius, a0, a1, false);
      ctx.stroke();
      ctx.restore();

      if (percent && percent > 0) {
        ctx.save();
        ctx.lineWidth = 6;
        ctx.strokeStyle = color;
        ctx.beginPath();
        const end = a0 + (a1 - a0) * (percent / 100);
        ctx.arc(240, 240, radius, a0, end, false);
        ctx.stroke();
        ctx.restore();
      }
    }
  }

  /** 图标线条（拒绝/允许/下一项/提交/选项）——与 SVG 版同一套路径点。 */
  #icon(name, middle, iconY, ink, enabled, accent) {
    const ctx = this.ctx;
    ctx.save();
    ctx.strokeStyle = name === 'option' && enabled ? YELLOW : ink;
    ctx.lineWidth = 2;
    ctx.beginPath();
    if (name === 'deny') {
      ctx.moveTo(middle - 5, iconY - 5); ctx.lineTo(middle + 5, iconY + 5);
      ctx.moveTo(middle + 5, iconY - 5); ctx.lineTo(middle - 5, iconY + 5);
    } else if (name === 'allow') {
      ctx.moveTo(middle - 8, iconY); ctx.lineTo(middle + 8, iconY);
      ctx.moveTo(middle + 2, iconY - 6); ctx.lineTo(middle + 8, iconY); ctx.lineTo(middle + 2, iconY + 6);
    } else if (name === 'next') {
      ctx.moveTo(middle - 7, iconY - 5); ctx.lineTo(middle - 2, iconY); ctx.lineTo(middle - 7, iconY + 5);
      ctx.moveTo(middle + 1, iconY - 5); ctx.lineTo(middle + 6, iconY); ctx.lineTo(middle + 1, iconY + 5);
    } else if (name === 'submit') {
      ctx.moveTo(middle - 6, iconY); ctx.lineTo(middle - 2, iconY + 5); ctx.lineTo(middle + 7, iconY - 5);
    } else if (name === 'option') {
      if (accent) { ctx.moveTo(middle - 6, iconY); ctx.lineTo(middle - 2, iconY + 5); ctx.lineTo(middle + 7, iconY - 5); }
      else {
        ctx.moveTo(middle, iconY - 6); ctx.lineTo(middle + 6, iconY); ctx.lineTo(middle, iconY + 6); ctx.lineTo(middle - 6, iconY);
        ctx.closePath();
      }
    }
    ctx.stroke();
    ctx.restore();
  }

  #button(name, label, [x, y, w, h], { enabled = true, accent = false, hint = '', rows = null, multi = false } = {}) {
    const ctx = this.ctx;
    const active = enabled && accent;
    const ink = active ? YELLOW : enabled ? WHITE : '#707070';
    const middle = x + w / 2;
    /* 多行选项：图标上移，按 17 px 逐行铺开（最多 3 行，由 questionView 切页）。
     * 单行仍然走原来的坐标，保证既有版式像素级不变。 */
    /* multi 由调用方决定：翻页时最后一页可能只剩 1 行，但版式必须和前面几页一致，
     * 否则字号会突然跳大一档。 */
    const useRows = multi && Array.isArray(rows) && rows.length > 0;
    const iconY = y + (useRows ? 10 : name === 'option' ? 14 : 20);
    this.#icon(name, middle, iconY, ink, enabled, accent);
    if (useRows) {
      rows.forEach((row, i) => this.#text(row, middle, y + 34 + i * 19, 17, ink, 700));
    } else {
      this.#text(label, middle, y + (hint ? 50 : 58), 24, ink, 700);
      if (hint) this.#text(hint, middle, y + 74, 16, enabled ? GRAY : '#707070');
    }
    ctx.save();
    ctx.strokeStyle = active ? YELLOW : enabled ? '#757575' : '#333';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(middle - 24, y + h - 9);
    ctx.lineTo(middle + 24, y + h - 9);
    ctx.stroke();
    ctx.restore();
  }

  #calendar(value) {
    if (!/^\d{4}-\d{2}-\d{2}$/.test(value ?? '')) return;
    const day = new Date(`${value}T00:00:00Z`);
    if (!Number.isFinite(day.getTime()) || day.toISOString().slice(0, 10) !== value) return;
    this.#text(value.slice(5).replace('-', '/'), 310, 84, 18);
    this.#text(`星期${'日一二三四五六'[day.getUTCDay()]}`, 310, 111, 17, GRAY);
  }

  #readings(list, metrics, compact) {
    if (compact) {
      list.forEach(({ name, percent, color }, i) => this.#text(`${name} ${reading(percent)}`, 125 + i * 115, 149, 17, color, 700));
      return;
    }
    const [cpu, gpu, ram] = list;
    const vram = clampPercent(metrics.gpuMemoryPercent);
    const big = (percent, x, y, size, color) => {
      if (percent == null) { this.#text('--', x, y, size, GRAY, 700); return; }
      // SVG 用 tspan 让 % 小一号：这里先画数值（右对齐偏移），再画 %
      const ctx = this.ctx;
      ctx.fillStyle = color;
      ctx.font = panelFont(700, size);
      ctx.textAlign = 'center';
      const body = percent.toFixed(1);
      const small = size > 30 ? 20 : 16;
      ctx.font = panelFont(700, size);
      const wBody = ctx.measureText(body).width;
      ctx.font = panelFont(700, small);
      const wPct = ctx.measureText('%').width;
      const start = x - (wBody + wPct + 5) / 2;
      ctx.font = panelFont(700, size);
      ctx.textAlign = 'left';
      ctx.fillText(body, start, y);
      ctx.font = panelFont(700, small);
      ctx.fillText('%', start + wBody + 5, y);
    };
    this.#text('CPU', 157, 156, 16, cpu.color);
    big(cpu.percent, 157, 206, 48, cpu.color);
    this.#text('GPU', 322, 144, 14, gpu.color);
    big(gpu.percent, 322, 169, 28, gpu.color);
    this.#text(`VRAM ${reading(vram)}`, 322, 186, 14, gpu.color);
    this.#text('RAM', 322, 201, 14, ram.color);
    big(ram.percent, 322, 230, 28, ram.color);
    this.#text(`RX  ${rate(metrics.networkRxBps)}`, 157, 248, 16, GRAY);
    this.#text(`TX  ${rate(metrics.networkTxBps)}`, 322, 248, 16, GRAY);
  }

  #footer(s) {
    const battery = roundedPercent(s.battery);
    const balance = lines(s.balance, 90, 17, 1).rows[0];
    const power = battery == null ? '' : `电量 ${battery}%`;
    this.#text([power, balance].filter(Boolean).join('  ·  '), 240, 416, 17, GRAY);
  }

  #focus(s) {
    const session = s.session ?? {};
    const states = {
      RUNNING: ['DSH 运行中', WHITE],
      NEEDS: ['DSH 等待操作', YELLOW],
      DONE: ['DSH 本轮完成', BLUE],
      FAILED: ['DSH 运行失败', RED],
    };
    const state = Object.hasOwn(states, session.state) ? states[session.state] : null;
    if (!state) {
      this.#fish(117, 281, 42, 31);
      this.#text('探索未至之境', 171, 308, 32, WHITE, 500, 'start');
      this.#text('DSH 等待任务', 240, 363, 23, GRAY, 700);
      this.#footer(s);
      return;
    }
    const ctxValue = Number.isFinite(session.ctx) && session.ctx >= 0 ? roundedPercent(session.ctx) : null;
    const barColor = ctxValue >= 80 ? YELLOW : BLUE;
    const steps = Number.isFinite(session.steps) ? Math.min(99999, Math.max(0, Math.round(session.steps))) : '—';
    const tool = lines(session.tool, 95, 18, 1).rows[0];
    const title = lines(session.title || '未命名会话', 316, 25, 1).rows[0];
    this.#text(state[0], 240, 276, 20, state[1], 700);
    this.#text(title, 240, 310, 25, WHITE, 700);
    this.#text([tool, `${steps} 步`].filter(Boolean).join(' · '), 94, 338, 18, GRAY, 400, 'start');
    this.#text(`CTX ${ctxValue == null ? '--' : `${ctxValue}%`}`, 94, 364, 17, GRAY, 400, 'start');

    const ctx = this.ctx;
    ctx.fillStyle = '#333';
    ctx.fillRect(180, 356, 206, 6);
    if (ctxValue > 0) {
      ctx.fillStyle = barColor;
      ctx.fillRect(180, 356, Math.round((206 * ctxValue) / 100), 6);
    }
    this.#footer(s);
  }

  /** DSH 鲸鱼：SVG path 用浏览器 Path2D 直接吃，缩放靠 transform。 */
  #fish(x, y, w, h) {
    if (typeof this.Path2D !== 'function') return; // 兜底：没有 Path2D 就只少一只鲸鱼
    const ctx = this.ctx;
    ctx.save();
    ctx.translate(x, y);
    ctx.scale(w / 23.16, h / 17.04);
    ctx.fillStyle = WHITE;
    ctx.fill(new this.Path2D(dshFishPath));
    ctx.restore();
  }

  /* ------------------------------------------------------------ 整帧 */

  /**
   * @param {object} s 面板状态
   * @param {object} tokens design/dsh_panel_v4/tokens.json
   * @returns {{rgba: Uint8ClampedArray, width:number, height:number}}
   */
  /**
   * 通知卡：按圆形表盘的可见区域排版（480×480 画布，安全半径 210）。
   * 标题/正文/页脚全部居中，宽度收在圆心弦长内，避免被表盘圆边裁掉。
   * @param {{title?: string, body?: string}} card
   * @returns {{rgba: Uint8ClampedArray, width:number, height:number}}
   */
  drawNotification({ title = '', body = '' } = {}) {
    if (!this.ready) throw new Error('MonitorCanvasRenderer.init() 还没调用');
    const ctx = this.ctx;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, 480, 480);

    /* 圆形安全区：半径 210（留 30 px 黑边）；顶部小黄条和 header 都居中。 */
    ctx.fillStyle = YELLOW;
    ctx.fillRect(222, 62, 36, 5);
    this.#text('DSH · NOTIFY', 240, 94, 15, GRAY, 700);

    const titleRows = lines(title, 330, 28, 2).rows;
    let y = titleRows.length > 1 ? 158 : 172;
    for (const row of titleRows) {
      this.#text(row, 240, y, 28, WHITE, 700);
      y += 36;
    }
    y += 14;
    const bodyRows = lines(body, 330, 20, titleRows.length > 1 ? 5 : 6).rows;
    for (const row of bodyRows) {
      this.#text(row, 240, y, 20, GRAY, 400);
      y += 28;
    }
    this.#text('PROTORIG ES', 240, 404, 14, '#666', 400);
    return { rgba: ctx.getImageData(0, 0, 480, 480).data, width: 480, height: 480 };
  }

  draw(s, tokens) {
    if (!this.ready) throw new Error('MonitorCanvasRenderer.init() 还没调用');
    const ctx = this.ctx;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, 480, 480);

    const time = /^\d{2}:\d{2}$/.test(s.time ?? '') ? s.time : '--:--';
    const online = s.connection !== 'lost' && s.connection !== 'waiting';
    const flashing = online && Number.isFinite(s.flashPct) && s.flashPct >= 0;
    const metrics = online && !flashing ? s.metrics ?? {} : {};
    const list = channels(metrics);
    const approval = online && !flashing ? s.approval : null;
    const question = online && !flashing && !approval ? s.question : null;
    const hot = tokens.hotzones ?? {};

    this.#rings(list);

    // 时间：5 个数字槽位（与 SVG 版同一组 x）
    [136, 166, 192, 218, 248].forEach((x, i) => this.#text([...time][i] ?? '', x, 112, 48, WHITE, 700));
    this.#calendar(s.date);

    if (!online) {
      const lost = s.connection === 'lost';
      this.#text(lost ? '连接已中断' : '等待 USB 连接', 240, 257, 29, lost ? RED : WHITE, 700);
      this.#text(lost ? '检查 USB 后重新进入' : '连接后从表盘右滑进入', 240, 299, 20, GRAY);
    } else if (flashing) {
      const progress = roundedPercent(s.flashPct);
      this.#text('固件写入中', 240, 213, 25, YELLOW, 700);
      this.#text(`${progress}%`, 240, 288, 54, WHITE, 700);
      ctx.fillStyle = '#333'; ctx.fillRect(112, 313, 256, 6);
      ctx.fillStyle = YELLOW; ctx.fillRect(112, 313, Math.round((256 * progress) / 100), 6);
      this.#text('请保持 USB 连接', 240, 354, 22, GRAY);
    } else {
      this.#readings(list, metrics, !!(approval || question));
      if (approval) {
        const info = approvalView(approval);
        this.#text(info.computer ? '电脑待确认' : 'DSH 审批待确认', 240, 192, 25, YELLOW, 700);
        this.#text(`${info.tool} 请求执行`, 240, 229, 21, WHITE, 700);
        this.#text(info.command, 240, 263, 22, GRAY);
        if (hot.deny) this.#button('deny', '拒绝', hot.deny, { enabled: !info.computer });
        if (hot.allow) this.#button('allow', '允许一次', hot.allow, { enabled: info.canAllow, accent: true });
        this.#text(info.note, 240, 419, 17, GRAY);
      } else if (question) {
        const info = questionView(question);
        // 页码放进标题行：用户反馈"不知道为什么只看到一个选项"（research/62 §12）
        // 页码在选项提示里（设计稿口径），标题行不再带页码
        /* 标题上移给题干腾地方：题干 2 行 ×19 px，正好落在选项区（y=218）之前。
         * 翻页页码放在标题行末尾，不占底部那行（底部留给选项翻页/移交提示）。 */
        this.#text(
          (info.computer ? 'DSH 问答 · 已移交电脑' : `DSH 问答 · ${question.multiSelect ? '多选' : '单选'}`)
            + (info.promptPageText ? ` · ${info.promptPageText}` : ''),
          240, 166, 16, info.computer ? GRAY : YELLOW, 700,
        );
        const promptRows = Array.isArray(info.promptRows) && info.promptRows.length
          ? info.promptRows
          : [info.prompt ?? ''];
        if (promptRows.length > 1) {
          promptRows.forEach((row, i) => this.#text(row, 240, 186 + i * 18, 18, WHITE, 700));
        } else {
          this.#text(promptRows[0], 240, 194, 20, WHITE, 700);
        }
        const multiRow = info.choiceRows.length > 1 || info.scrolling;
        if (hot.option) this.#button('option', info.choice, hot.option, {
          enabled: info.canChoose,
          accent: info.chosen,
          hint: multiRow ? '' : info.hint,
          rows: info.choiceRows,
          multi: multiRow,
        });
        if (hot.next) this.#button('next', '下一项', hot.next, { enabled: info.more });
        if (hot.submit) this.#button('submit', '提交', hot.submit, { enabled: info.canSubmit, accent: true, hint: info.submitHint });
        /* 移交电脑后留一行明确指示：卡片还在，但已经不该在表上点了。 */
        if (info.computer) this.#text('已移交电脑 · 请在电脑上回答', 240, 419, 17, GRAY);
        /* 多行选项时按钮里挤不下提示，挪到卡片底部（含翻页进度）。 */
        else if (multiRow) this.#text(info.hint, 240, 419, 16, GRAY);
      } else {
        this.#focus(s);
      }
    }

    const data = ctx.getImageData(0, 0, 480, 480);
    return { rgba: data.data, width: 480, height: 480 };
  }
}
