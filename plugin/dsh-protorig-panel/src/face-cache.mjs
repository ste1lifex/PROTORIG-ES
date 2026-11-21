/*
 * face-cache.mjs —— 图片表盘 / 默认表盘下半区面板的本地缩略图缓存
 *
 * 为什么需要：图片装在手表 flash 里，固件只回 CRC，没有"把图读回来"的命令；
 * 设置页要显示缩略图，只能在**本机安装时**把预览 PNG 缓存下来，
 * 以 image_crc32（8 位大写 hex）为键。换电脑/清缓存后旧图就没有缩略图（显示占位）。
 *
 * 存放：<DATA_DIR>/faces/<CRC>.png           图片表盘（480×480）
 *       <DATA_DIR>/faces/panel-<CRC>.png     默认表盘下半区（480×232）
 *       （与烧录锁同一个数据目录；DATA_DIR = $PROTORIG_DATA_DIR 或 ~/protorig-panel，见 flash.mjs）
 * 策略：只缓存、不自动清理；读失败/坏 CRC 一律当作"没有"，不影响表盘操作。
 */
import fs from 'node:fs';
import path from 'node:path';
import { DATA_DIR } from './flash.mjs';

/** 缩略图目录（每次调用现算：测试/换机器可用 PROTORIG_FACE_CACHE 覆盖）。 */
export function faceCacheDir() {
  return process.env.PROTORIG_FACE_CACHE || path.join(DATA_DIR, 'faces');
}

/** 合法 CRC 规则：8 位十六进制（大小写均可）。返回大写 8 位，非法返回 ''。 */
export function normalizeCrc(crc) {
  const text = String(crc ?? '').trim();
  if (!/^[0-9a-fA-F]{8}$/.test(text)) return '';
  return text.toUpperCase();
}

export function thumbPath(crc) {
  const key = normalizeCrc(crc);
  if (!key) return '';
  return path.join(faceCacheDir(), `${key}.png`);
}

/** 面板缩略图路径：`panel-<CRC>.png`（与图片表盘的 `<CRC>.png` 分开，避免同名混淆）。 */
export function panelThumbPath(crc) {
  const key = normalizeCrc(crc);
  if (!key) return '';
  return path.join(faceCacheDir(), `panel-${key}.png`);
}

function readFileOrNull(file) {
  if (!file) return null;
  try {
    return fs.readFileSync(file);
  } catch {
    return null;
  }
}

/** 原子写（先写 .tmp 再 rename，避免并发读到半个文件）。失败/非法 CRC/空数据 → ''。 */
function writePng(file, pngBuffer) {
  if (!file) return '';
  if (!Buffer.isBuffer(pngBuffer) || pngBuffer.length === 0) return '';
  const dir = path.dirname(file);
  const tmp = `${file}.${process.pid}.tmp`;
  try {
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(tmp, pngBuffer);
    fs.renameSync(tmp, file);
    return file;
  } catch {
    try {
      fs.rmSync(tmp, { force: true });
    } catch {
      /* ignore */
    }
    return '';
  }
}

/** 读缓存；没有/非法/读失败 → null。 */
export function readThumb(crc) {
  return readFileOrNull(thumbPath(crc));
}

export function hasThumb(crc) {
  return readThumb(crc) !== null;
}

/**
 * 写缓存（原子写：先写 .tmp 再 rename，避免并发读到半个文件）。
 * @returns {string} 落盘路径；失败/非法 CRC/空数据 → ''（不抛错，缓存不是关键路径）
 */
export function cacheThumb(crc, pngBuffer) {
  return writePng(thumbPath(crc), pngBuffer);
}

/** 删缓存（设置页"清除缩略图缓存"/调试用）。返回是否真的删了。 */
export function removeThumb(crc) {
  const file = thumbPath(crc);
  if (!file) return false;
  try {
    fs.rmSync(file, { force: true });
    return true;
  } catch {
    return false;
  }
}

/* -------------------------------------------------- 默认表盘下半区面板（480×232） */

export function readPanelThumb(crc) {
  return readFileOrNull(panelThumbPath(crc));
}

export function hasPanelThumb(crc) {
  return readPanelThumb(crc) !== null;
}

/** 缓存面板预览（480×232 PNG），语义同 cacheThumb。 */
export function cachePanelThumb(crc, pngBuffer) {
  return writePng(panelThumbPath(crc), pngBuffer);
}

export function removePanelThumb(crc) {
  const file = panelThumbPath(crc);
  if (!file) return false;
  try {
    fs.rmSync(file, { force: true });
    return true;
  } catch {
    return false;
  }
}

/** 现有缓存列表（调试/诊断用）：[{crc, size, mtime}]。 */
export function listThumbs() {
  const dir = faceCacheDir();
  let names = [];
  try {
    names = fs.readdirSync(dir);
  } catch {
    return [];
  }
  return names
    .filter((name) => /^[0-9A-Fa-f]{8}\.png$/.test(name))
    .map((name) => {
      const file = path.join(dir, name);
      try {
        const stat = fs.statSync(file);
        return { crc: name.slice(0, 8).toUpperCase(), size: stat.size, mtime: stat.mtimeMs, file };
      } catch {
        return null;
      }
    })
    .filter(Boolean);
}
