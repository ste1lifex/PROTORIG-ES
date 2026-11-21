/*
 * protocol.mjs —— PROTORIG ES 电脑模式线协议
 *
 * 上行（电脑 → 手表，USB 原始二进制）：
 *     A5 5A len payload(len 字节)      len ∈ 1..240，payload[0] = op
 *
 * 下行（手表 → 电脑，仍是文本行，见 research/56 §9.4）：
 *     @PCM1 PC 0|1                     进出电脑模式
 *     @PCM1 MODE 0|1                   透传开关（MODE 1 = 可以发像素）
 *     @PCM1 FRAME a55a50<hex>          hex = [cmd][payload...]
 *          cmd 0x50 → payload[0] 是 op（0x04 = 触摸）
 *          cmd 0x81 → HELLO 回复：ver, maxW lo/hi, maxH lo/hi, fmt
 *          cmd 0x91 → 状态：link, batt, bright, page
 *     admin=2BEA4E chip=...            shell `adminid` 输出（设备身份，research/74 §2.3）
 *     [pc] ...                         普通日志（忽略或转 log 事件）
 */
import { parseAdminLine } from './device-id.mjs';

export const SYNC0 = 0xa5;
export const SYNC1 = 0x5a;

export const OP = {
  HELLO: 0x01,
  BRIGHT: 0x02,
  VIBRATE: 0x03,
  TOUCH: 0x04, // 手表 → 电脑
  TILE_BEGIN: 0x10,
  TILE_DATA: 0x11,
  TILE_END: 0x12,
  CLEAR: 0x13,
  STATUS_REQ: 0x14,
  ALWAYS_ON: 0x15,
  /* BLE 卡片层（research/65）：无像素链路时把下半屏内容发给固件本地渲染 */
  SESSION: 0x53, // state, steps, ctx, tool, title
  APPROVAL: 0x54, // canAllow, computer, countdown, tool, command
  QUESTION: 0x55, // index, count, multi, selMask, prompt, option
  CARD_CLEAR: 0x56,
  CARD_BMP: 0x57, // slot, w, h, index, total, 1-bit data
};

export const CMD = {
  PC_FRAME: 0x50,
  PC_MODE: 0x51,
  HELLO_RSP: 0x81,
  STATUS_RSP: 0x91,
  /* 手表 → 电脑：卡片热区自报 / 卡片帧回执 / 整屏重发请求 */
  ZONES: 0x58,
  CARD_ACK: 0x59,
  REDRAW: 0x92,
};

/** 卡片帧回执的 what（与固件 svc_pc.c 的 PC_ACK_* 一致）。 */
export const ACK = { CARD: 1, BMP_OK: 2, BMP_BAD: 3, CARD_BAD: 4 };

/** 固件自报热区的 id → 插件内部热区名（与 svc_pc.c 的 PC_ZONE_* 一致）。 */
export const ZONE_NAMES = { 1: 'deny', 2: 'allow', 3: 'option', 4: 'next', 5: 'submit' };

/** 固件卡片层的字段上限（svc_pc.c / pc_monitor.h 同一套，超了固件会丢帧）。 */
export const CARD_LIMIT = {
  TOOL: 11,
  TITLE: 72,
  CMD: 96,
  CHOICE: 96,
  PROMPT: 72,
  BMP_W: 300,
  BMP_H: 44,
  FOOTER: 48,
  BMP_CHUNK: 231,
};

/** 单片像素上限（payload 含 op，所以是 239 字节像素）。 */
export const CHUNK_PIXELS = 239;
/** 瓦片最大高度（HELLO 会回报，别写死；这里只是默认值）。 */
export const DEFAULT_TILE_H = 16;

const MAX_PAYLOAD = 240;

/** 组一个上行帧：A5 5A len payload。 */
export function frame(payload) {
  const p = Uint8Array.from(payload);
  if (p.length < 1 || p.length > MAX_PAYLOAD) {
    throw new Error(`payload 长度必须在 1..${MAX_PAYLOAD}，收到 ${p.length}`);
  }
  const out = Buffer.allocUnsafe(3 + p.length);
  out[0] = SYNC0;
  out[1] = SYNC1;
  out[2] = p.length;
  out.set(p, 3);
  return out;
}

export const u16le = (v) => [v & 0xff, (v >> 8) & 0xff];

export const hello = () => frame([OP.HELLO]);
export const bright = (level) => frame([OP.BRIGHT, Math.max(32, Math.min(255, level | 0))]);
export const vibrate = (ms) => frame([OP.VIBRATE, ...u16le(Math.max(0, Math.min(2000, ms | 0)))]);
export const clear = (r, g, b) => frame([OP.CLEAR, r & 0xff, g & 0xff, b & 0xff]);
export const statusRequest = () => frame([OP.STATUS_REQ]);
export const tileBegin = (x, y, w, h) =>
  frame([OP.TILE_BEGIN, ...u16le(x), ...u16le(y), ...u16le(w), ...u16le(h)]);
export const tileEnd = () => frame([OP.TILE_END]);

/** 一片像素数据（自动切片，每片 ≤239 字节）。 */
export function* tileData(pixels) {
  for (let off = 0; off < pixels.length; off += CHUNK_PIXELS) {
    const n = Math.min(CHUNK_PIXELS, pixels.length - off);
    const payload = new Uint8Array(n + 1);
    payload[0] = OP.TILE_DATA;
    payload.set(pixels.subarray(off, off + n), 1);
    yield frame(payload);
  }
}

/* ------------------------------------------------- 卡片层载荷（BLE 数字通道） */

const utf8 = new TextEncoder();

/** 按字节上限裁 UTF-8，且不切断半个字符（固件那边还会再兜一次）。 */
export function utf8Clamp(text, maxBytes) {
  const source = typeof text === 'string' ? text : '';
  let out = '';
  let used = 0;
  if (!(maxBytes > 0)) return '';
  for (const ch of source) {
    const n = utf8.encode(ch).length;
    if (used + n > maxBytes) break;
    used += n;
    out += ch;
  }
  return out;
}

/** u8 长度前缀的 UTF-8 字符串。 */
function str8(text, maxBytes) {
  const bytes = utf8.encode(utf8Clamp(text, maxBytes));
  const out = new Uint8Array(1 + bytes.length);
  out[0] = bytes.length;
  out.set(bytes, 1);
  return out;
}

function concat(parts) {
  const total = parts.reduce((sum, part) => sum + part.length, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}

const clampByte = (value, fallback = 0) => {
  const n = Number(value);
  if (!Number.isFinite(n)) return fallback;
  return Math.max(0, Math.min(255, Math.round(n)));
};

/**
 * 会话卡载荷：state, steps, ctx(i8, -1=无), tool, title（固件渲染，标题走位图）。
 * @returns {Uint8Array}
 */
export function cardSession({ state = 0, steps = 0, ctx = -1, tool = '', title = '', footer = '' } = {}) {
  const ctxByte = Number.isFinite(ctx) && ctx >= 0 ? clampByte(Math.min(100, ctx)) : 0xff;
  return concat([
    Uint8Array.from([OP.SESSION, clampByte(state), clampByte(Math.min(255, steps)), ctxByte]),
    str8(tool, CARD_LIMIT.TOOL),
    str8(title, CARD_LIMIT.TITLE),
    str8(footer, CARD_LIMIT.FOOTER),
  ]);
}

/** 审批卡载荷：canAllow, computer, countdown(s), tool, command。 */
export function cardApproval({ canAllow = 1, computer = 0, countdown = 0, tool = '', command = '' } = {}) {
  return concat([
    Uint8Array.from([
      OP.APPROVAL,
      canAllow ? 1 : 0,
      computer ? 1 : 0,
      clampByte(Math.min(99, countdown)),
    ]),
    str8(tool, CARD_LIMIT.TOOL),
    str8(command, CARD_LIMIT.CMD),
  ]);
}

/**
 * 问答卡载荷：questionIndex, questionCount, multi, selMask(u16), page, prompt, option。
 * page 是当前显示的选项序号（固件用它高亮"这一项已选"）。
 */
export function cardQuestion({
  index = 0, count = 1, multi = 0, selected = [], page = 0, optionCount = 1,
  prompt = '', option = '', computer = 0,
} = {}) {
  let mask = 0;
  for (const value of selected) {
    const n = Number(value);
    if (Number.isInteger(n) && n >= 0 && n < 16) mask |= 1 << n;
  }
  return concat([
    Uint8Array.from([
      OP.QUESTION,
      clampByte(index),
      Math.max(1, clampByte(count, 1)),
      multi ? 1 : 0,
      mask & 0xff,
      (mask >> 8) & 0xff,
      clampByte(page),
      Math.max(1, clampByte(optionCount, 1)),
    ]),
    str8(prompt, CARD_LIMIT.PROMPT),
    str8(option, CARD_LIMIT.CHOICE),
    /* 0.4.19：问答超时移交电脑。旧固件不读这一字节；新固件用它改标题/禁用按钮。 */
    Uint8Array.from([computer ? 1 : 0]),
  ]);
}

/** 撤卡（回会话卡）。 */
export const cardClear = () => Uint8Array.from([OP.CARD_CLEAR]);

/** 清掉一个位图槽（标题/题目变空时用，w=h=total=bpp=0，无数据）。 */
export const cardBitmapClear = (slot) => Uint8Array.from([
  OP.CARD_BMP, slot & 1, 0, 0, 0, 0, 0, 0, 0,
]);

/**
 * 卡片位图分片：slot 0/1，w 必须是 8 的倍数，h ≤ 30，bpp = 1|2（2 = 4 级灰）。
 * 每片载荷 ≤ 240 B（9 B 头 + 231 B 数据）。
 * @param {{w:number,h:number,bpp?:number,bits:Buffer|Uint8Array}} bitmap
 * @returns {Generator<Uint8Array>}
 */
export function* cardBitmapChunks(slot, { w, h, bpp = 1, bits }) {
  const width = Math.max(8, Math.min(CARD_LIMIT.BMP_W, (w | 0) & ~7));
  const height = Math.max(1, Math.min(CARD_LIMIT.BMP_H, h | 0));
  const depth = bpp === 2 ? 2 : 1;
  const stride = (width * depth) >> 3;
  const data = Buffer.from(bits ?? []);
  const need = stride * height;
  const total = Math.max(1, Math.ceil(need / CARD_LIMIT.BMP_CHUNK));
  for (let index = 0; index < total; index += 1) {
    const offset = index * CARD_LIMIT.BMP_CHUNK;
    const chunk = data.subarray(offset, Math.min(offset + CARD_LIMIT.BMP_CHUNK, need));
    const payload = new Uint8Array(9 + chunk.length);
    payload[0] = OP.CARD_BMP;
    payload[1] = slot & 1;
    payload[2] = width & 0xff;
    payload[3] = (width >> 8) & 0xff;
    payload[4] = height & 0xff;
    payload[5] = (height >> 8) & 0xff;
    payload[6] = index;
    payload[7] = total;
    payload[8] = depth;
    payload.set(chunk, 9);
    yield payload;
  }
}

/* --------------------------------------------------------------- 下行解析 */

/**
 * 解析固件 BLE notify 的一帧：A5 5A <cmd> <len> <payload…>。
 * 返回 null 表示帧头不合法或长度不足（多半是 MTU 截断，直接丢）。
 */
export function parseBleFrame(bytes) {
  const buffer = Buffer.from(bytes ?? []);
  if (buffer.length < 4 || buffer[0] !== SYNC0 || buffer[1] !== SYNC1) return null;
  const cmd = buffer[2];
  const len = buffer[3];
  if (buffer.length < 4 + len) return null;
  return decodeFrame(cmd, buffer.subarray(4, 4 + len));
}

function decodeZones(payload) {
  if (payload.length < 2) return { type: 'malformed', raw: 'zones' };
  const kind = payload[0];
  const count = payload[1];
  const list = [];
  for (let i = 0; i < count; i += 1) {
    const at = 2 + i * 9;
    if (at + 9 > payload.length) break;
    const read16 = (offset) => payload[at + offset] | (payload[at + offset + 1] << 8);
    const id = payload[at];
    list.push({
      id,
      name: ZONE_NAMES[id] ?? `zone${id}`,
      x: read16(1),
      y: read16(3),
      w: read16(5),
      h: read16(7),
    });
  }
  return { type: 'zones', kind, list };
}

/**
 * cmd + payload → 事件对象。USB 文本行（@PCM1 FRAME）和 BLE notify 共用这一套，
 * 保证两条链路的事件语义完全一致（判定逻辑只有一份）。
 */
export function decodeFrame(cmd, rawPayload) {
  const payload = rawPayload instanceof Uint8Array ? rawPayload : Uint8Array.from(rawPayload ?? []);
  if (cmd === CMD.HELLO_RSP) {
    if (payload.length < 6) return { type: 'malformed', raw: 'hello' };
    return {
      type: 'hello',
      ver: payload[0],
      maxW: payload[1] | (payload[2] << 8),
      maxH: payload[3] | (payload[4] << 8),
      fmt: payload[5],
    };
  }
  if (cmd === CMD.STATUS_RSP) {
    if (payload.length < 4) return { type: 'malformed', raw: 'status' };
    return {
      type: 'status',
      link: payload[0],
      battery: payload[1],
      bright: payload[2],
      page: payload[3],
    };
  }
  if (cmd === CMD.PC_FRAME) {
    const op = payload[0];
    if (op === OP.TOUCH && payload.length >= 6) {
      return {
        type: 'touch',
        pressed: payload[1] !== 0,
        x: payload[2] | (payload[3] << 8),
        y: payload[4] | (payload[5] << 8),
      };
    }
    return { type: 'frame', cmd, op, payload };
  }
  if (cmd === CMD.ZONES) return decodeZones(payload);
  if (cmd === CMD.CARD_ACK) {
    if (payload.length < 2) return { type: 'malformed', raw: 'ack' };
    return { type: 'cardack', what: payload[0], code: payload[1] };
  }
  if (cmd === CMD.REDRAW) {
    /* 0x92：固件本地弹窗（闹钟/计时器）结束后请主机整屏重发。
     * payload[0] = reason（1 = 弹窗结束）。旧插件不认识时应直接忽略。 */
    return { type: 'redraw', reason: payload.length >= 1 ? payload[0] : 0 };
  }
  return { type: 'frame', cmd, payload };
}

/**
 * 增量解析手表发来的文本流。喂任意字节块，回调里拿结构化的行事件。
 * 只认行；手表不会发二进制下来。
 */
export class LineParser {
  constructor(onLine) {
    this.onLine = onLine;
    this.buf = '';
  }

  push(chunk) {
    this.buf += chunk.toString('utf8');
    // 一行最长也就几百字节；防止异常数据把内存撑爆
    if (this.buf.length > 65536) this.buf = this.buf.slice(-4096);
    let idx;
    while ((idx = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, idx).replace(/\r$/, '');
      this.buf = this.buf.slice(idx + 1);
      const clean = stripAnsi(line).replace(/\x00/g, '').trim();
      if (clean) this.onLine(clean);
    }
  }
}

function stripAnsi(s) {
  return s.replace(/\x1b\[[0-9;]*[A-Za-z]/g, '');
}

/**
 * 去掉 shell 提示符。串口是文本和协议行混在一起的，手表的 shell 提示符
 * （`bouffalolab />`）经常和我们的事件行粘在同一行：
 *     bouffalolab />@PCM1 MODE 1
 * 不剥掉就会整行匹配失败（这个坑踩过一次：MODE 1 明明来了，解析器却没看见）。
 */
function stripPrompt(s) {
  return s.replace(/bouffalolab\s*\/>/g, '').trim();
}

/**
 * 解析一行 → 事件对象。
 * @returns {{type:string, [k:string]:any}|null}
 */
export function parseLine(rawLine) {
  // 提示符可能和事件行粘在一起；先剥掉，再从 '@PCM1' 开始解析
  const stripped = stripPrompt(rawLine);
  const at = stripped.indexOf('@PCM1');
  const line = at >= 0 ? stripped.slice(at) : stripped;
  let m;
  if ((m = /^@PCM1 PC ([01])$/.exec(line))) return { type: 'pc', value: Number(m[1]) };
  if ((m = /^@PCM1 MODE ([01])$/.exec(line))) return { type: 'mode', value: Number(m[1]) };
  if ((m = /^@PCM1 FRAME a55a50([0-9A-Fa-f]+)$/.exec(line))) {
    /* 线上的 hex = [cmd][payload…]；和 BLE notify 的 cmd+payload 走同一个解码器，
     * 所以 USB / BLE 两条链路的事件语义永远一致。 */
    const bytes = Buffer.from(m[1], 'hex');
    if (bytes.length < 1) return null;
    return decodeFrame(bytes[0], bytes.subarray(1));
  }
  /* shell `adminid` 的输出：`admin=2BEA4E chip=0123…`（可能带提示符/夹在日志里）。
   * 放在普通日志前解析，link 就能把身份做成事件（research/74 §2.3）。 */
  const admin = parseAdminLine(stripped);
  if (admin) return { type: 'admin', ...admin };
  if (line.startsWith('[pc]')) {
    const m2 = /^\[pc\] active=(\d+) transport=(\d+) passthrough=(\d+) usbraw=(\d+)(.*)$/.exec(line);
    if (m2) {
      return {
        type: 'pcstate',
        active: Number(m2[1]) === 1,
        transport: Number(m2[2]),
        passthrough: Number(m2[3]) === 1,
        usbraw: Number(m2[4]) === 1,
        raw: line,
      };
    }
    return { type: 'log', text: line };
  }
  return { type: 'other', text: line };
}

/* --------------------------------------------------------------- 颜色工具 */

/** RGB565（小端按字节存的顺序在 framebuffer 里处理）。 */
export function rgb565(r, g, b) {
  return (((r >> 3) & 0x1f) << 11) | (((g >> 2) & 0x3f) << 5) | ((b >> 3) & 0x1f);
}

export function hexToRgb565(hex) {
  const h = hex.replace('#', '');
  const n = parseInt(h.length === 3 ? h.split('').map((c) => c + c).join('') : h, 16);
  return rgb565((n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff);
}
