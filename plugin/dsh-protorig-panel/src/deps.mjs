/*
 * deps.mjs —— 依赖解析
 *
 * 发行版原则（2026-11-09）：**只依赖 npm 能下载的包**。插件自己的
 * dependencies 里带 serialport / @napi-rs/canvas / @resvg/resvg-js / systeminformation，
 * 安装后由 Node 正常解析即可；不再回退到桌面端仓库的 node_modules（那是外部路径）。
 * 需要指向别的 node_modules（离线调试）时可用 PROTORIG_DESKTOP_MODULES 显式指定。
 */
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));

const fallbackDirs = [
  process.env.PROTORIG_DESKTOP_MODULES,
].filter(Boolean);

/**
 * 依赖模块缓存跨插件实例复用。
 *
 * DSH 在同一进程里热重载插件时会重新 import；像 serialport 这种 CJS + 原生绑定的模块
 * 二次初始化可能"import 成功但 SerialPort 是 undefined"，之后整个进程都发现不了串口。
 * 第一次加载成功的命名空间挂到 globalThis 上，后续实例直接复用；只有进程重启才会重载。
 */
const dependencyCache = globalThis.__protorigDependencyCache
  ?? (globalThis.__protorigDependencyCache = new Map());

/** 先按 ESM 正常解析，失败再在 desktop/node_modules 里找。 */
export async function importAny(name) {
  if (dependencyCache.has(name)) return dependencyCache.get(name);
  let mod = null;
  try {
    mod = await import(name);
  } catch {
    /* fallthrough */
  }
  for (const dir of fallbackDirs) {
    if (mod) break;
    try {
      const req = createRequire(path.join(dir, '_resolve.cjs'));
      mod = req(name);
    } catch {
      /* try next */
    }
  }
  if (!mod) throw new Error(`cannot resolve dependency "${name}"（试过 ${fallbackDirs.join(', ')}）`);
  dependencyCache.set(name, mod);
  return mod;
}

/** 常用封装：拿到 serialport 的 SerialPort 构造器。 */
export async function loadSerialPort() {
  let mod = await importAny('serialport');
  let serial = mod?.SerialPort ?? mod?.default?.SerialPort;
  if (serial) return serial;
  /* 缓存到一个半初始化模块（热重载时见过）：清 CJS 缓存后按插件依赖树重新 require。 */
  dependencyCache.delete('serialport');
  try {
    const req = createRequire(new URL('../package.json', import.meta.url));
    for (const key of Object.keys(req.cache)) {
      if (key.includes('serialport') || key.includes('node-gyp-build')) delete req.cache[key];
    }
    mod = req('serialport');
    serial = mod?.SerialPort ?? mod?.default?.SerialPort;
    if (serial) {
      dependencyCache.set('serialport', mod);
      return serial;
    }
  } catch {
    /* fallthrough */
  }
  /* 最后回退到宿主 profile 根的另一份 serialport（不同文件 URL，绕开坏掉的模块缓存）。 */
  try {
    const profile = createRequire(path.join(here, '..', '..', '..', 'package.json'));
    mod = profile('serialport');
    serial = mod?.SerialPort ?? mod?.default?.SerialPort;
    if (serial) return serial;
  } catch {
    /* fallthrough */
  }
  return undefined;
}

/** 常用封装：拿到 @napi-rs/canvas（Skia）的 createCanvas / GlobalFonts / Path2D。 */
export async function loadCanvas() {
  const mod = await importAny('@napi-rs/canvas');
  return { createCanvas: mod.createCanvas, GlobalFonts: mod.GlobalFonts, Path2D: mod.Path2D };
}

/** 常用封装：拿到 resvg 的 Resvg 构造器。 */
export async function loadResvg() {
  const mod = await importAny('@resvg/resvg-js');
  return mod.Resvg ?? mod.default?.Resvg;
}
