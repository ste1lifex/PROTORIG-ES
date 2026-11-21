/*
 * device-id.mjs —— 设备身份（admin_id）的解析与选择规则（纯函数，不碰硬件）
 *
 * 身份来源（research/73 §1、74）：
 *   · USB：shell `adminid` → `admin=2BEA4E chip=0123456789ABCDEF`（进 PC 模式之前可用）
 *   · BLE：广播名 `PROTORIG ES 2BEA4E` 的后缀（CoreBluetooth 的 UUID 只当本机句柄，
 *     不写进配置 —— macOS 上那个 UUID 每台机器都不同）
 *   · 两边的字符串必须是同一个 6 位大写十六进制，这样同一块表在 Win/mac 配置里 ID 相同。
 *
 * 这里只做"字符串 → 身份 / 候选 → 选哪个"的纯逻辑，单测覆盖；
 * 真正开串口/读广播的部分在 link.mjs 与 ble-bridge.mjs。
 */
import { portLabel } from './platform.mjs';

/** 广播名前缀（固件 ble_link.c 的 BLE_DEVICE_NAME_PREFIX，不能单独硬编码成整名）。 */
export const ADV_PREFIX = 'PROTORIG ES';

/** 人类可读的默认设备名。 */
export const DEVICE_NAME = ADV_PREFIX;

const ADMIN_RE = /^(?:[0-9A-F]{6})$/;

/**
 * 规范化 admin_id：6 位十六进制、大写；其它（空/位数不对/非 hex）一律返回 ''。
 * `''` 的含义是"没有身份"（未绑定 / 没问出来），不是合法的表。
 */
export function normalizeAdminId(value) {
  if (typeof value !== 'string') {
    if (typeof value === 'number' && Number.isInteger(value)) {
      return normalizeAdminId((value & 0xffffff).toString(16).padStart(6, '0'));
    }
    return '';
  }
  const text = value.trim().replace(/^0x/i, '').toUpperCase();
  return ADMIN_RE.test(text) ? text : '';
}

/**
 * 解析固件 shell `adminid` 的输出行。
 *
 * 真机输出（research/74 §2.3）：`admin=2BEA4E chip=0123456789ABCDEF`
 * 实际串口流里常带 shell 提示符：`bouffalolab />admin=2BEA4E chip=...`，
 * 所以按"行内查找"解析，不要求整行匹配。chip 是 8 B eFuse ID 的 hex，
 * efuse 读失败时是全 0 —— 这种值仍然解析出来，但标记 `chipOk=false`（只用于诊断）。
 *
 * @returns {{admin:string, chip:string, chipOk:boolean, raw:string}|null}
 */
export function parseAdminLine(line) {
  const raw = String(line ?? '');
  if (!raw || !raw.includes('admin=')) return null;
  const match = /admin=([0-9A-Fa-f]{6})(?!\S)/.exec(raw);
  if (!match) return null;
  const admin = normalizeAdminId(match[1]);
  if (!admin) return null;
  const chipMatch = /\bchip=([0-9A-Fa-f]{0,16})\b/.exec(raw);
  const chip = (chipMatch?.[1] ?? '').toUpperCase();
  /* 2026-11-14：adminid 输出附带固件版本（fw=1.6.0 build=Nov 14 2026），
   * 概览里显示"固件版本"比协议版本更有参考意义。 */
  const fwMatch = /\bfw=([^\s]+)/.exec(raw);
  const fw = fwMatch?.[1] ?? '';
  const buildMatch = /\bbuild=([^\r\n]+)/.exec(raw);
  const build = buildMatch?.[1]?.trim() ?? '';
  // 8 B 全 0（或压根没 chip）说明 efuse 读失败，编号会回落 0x7C3E82
  const chipOk = chip.length === 16 && !/^0+$/.test(chip);
  return { admin, chip, chipOk, fw, build, raw };
}

/**
 * 从 BLE 广播名解析 admin_id。
 *
 * `PROTORIG ES 2BEA4E` → '2BEA4E'；旧固件的 `PROTORIG ES` → ''（无后缀，兼容）；
 * 别的设备名 → ''。前缀大小写不敏感（Android/旧插件都用 contains 匹配）。
 */
export function parseAdvName(name) {
  const text = String(name ?? '').trim();
  if (!text) return '';
  const upper = text.toUpperCase();
  if (!upper.startsWith(ADV_PREFIX)) return '';
  const rest = text.slice(ADV_PREFIX.length);
  if (!rest) return ''; // 旧固件：只有前缀，没有身份（前缀匹配仍然可用）
  if (!/^\s+[0-9A-Fa-f]{6}$/.test(rest)) return '';
  return normalizeAdminId(rest.trim());
}

/** 绑定后 BLE 桥应该匹配的完整名字（桥内部是 startswith 前缀匹配）。 */
export function advNameFor(boundId, fallback = ADV_PREFIX) {
  const admin = normalizeAdminId(boundId);
  return admin ? `${ADV_PREFIX} ${admin}` : (String(fallback ?? '').trim() || ADV_PREFIX);
}

/**
 * 设备标签：`PROTORIG ES · 2BEA4E · USB COM8` / `PROTORIG ES · 2BEA4E · BLE`。
 * 无身份时退回 `PROTORIG ES · 未识别`（绝不编一个编号出来）。
 */
export function deviceLabel({ admin = '', transport = '', port = '', platform = process.platform } = {}) {
  const id = normalizeAdminId(admin);
  const where = transport === 'ble'
    ? 'BLE'
    : port
      ? `USB ${portLabel(port, platform)}`
      : '';
  const parts = [DEVICE_NAME];
  parts.push(id || '未识别');
  if (where) parts.push(where);
  return parts.join(' · ');
}

/**
 * 从已经带身份标注的候选里选目标口。
 *
 * candidates: [{ path, admin: '2BEA4E'|''|null, weak?, ... }]（已按平台排好序）
 *   · 未绑定（boundId=''）：选第一个 —— 与升级前行为一致（research/73 P11）；
 *   · 已绑定：优先选身份对得上的口；其次选"还没问出身份"的口（开进去再验，
 *     见 panel-service 的握手门禁）；全部身份都对不上 → 不连，避免连错表。
 *
 * @returns {{candidate: object|null, reason: string}}
 */
export function selectCandidate(candidates, boundId = '') {
  const list = Array.isArray(candidates) ? candidates.filter((item) => item && item.path) : [];
  const bound = normalizeAdminId(boundId);
  if (!list.length) return { candidate: null, reason: 'no-device' };
  if (!bound) {
    return { candidate: list[0], reason: list.length > 1 ? 'unbound-multiple' : 'unbound-single' };
  }
  const known = list.find((item) => normalizeAdminId(item.admin) === bound);
  if (known) return { candidate: known, reason: 'bound-known' };
  const unknown = list.find((item) => !normalizeAdminId(item.admin));
  if (unknown) return { candidate: unknown, reason: 'bound-unknown' };
  return { candidate: null, reason: 'bound-mismatch' };
}

/** 候选选择结果 → 给用户看的一句话（设置页/日志共用）。 */
export function describeSelection(reason, { bound = '' } = {}) {
  const id = normalizeAdminId(bound);
  switch (reason) {
    case 'unbound-single': return '未绑定：自动连接唯一候选口';
    case 'unbound-multiple': return '检测到多块手表但未绑定：先连第一个，建议在设置页绑定编号';
    case 'bound-known': return `已绑定 ${id}：连接对应的手表`;
    case 'bound-unknown': return `已绑定 ${id}：候选口还没问出编号，先连上再校验`;
    case 'bound-mismatch': return `未找到绑定的手表 ${id}（候选口都属于其它表）`;
    case 'no-device': return id ? `未找到绑定的手表 ${id}（没有候选串口）` : '没找到手表';
    default: return '未知设备状态';
  }
}
