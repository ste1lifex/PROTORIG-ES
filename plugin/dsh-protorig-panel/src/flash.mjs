/*
 * flash.mjs —— 烧录（复用 flash.ps1，不在插件里重写 ISP）
 *
 * 脚本路径：`PROTORIG_FLASH_SCRIPT` 覆盖；否则按下面顺序找现成的：
 *   1. V1.6 布局 `<repo>/firmware/lvgl_test/flash.ps1`（当前主线，参数 -Bin/-Baud/-AppPort/-Port 兼容）
 *   2. 旧布局 `PROTORIG_ES/firmware_m2/flash.ps1`（V1.6 之前的开发目录，留着兼容）
 * 两个都不存在时用 1 的路径报错。npm 装到仓库外时请用 PROTORIG_FLASH_SCRIPT 指定。
 *
 * 链路（lead 已验证）：
 *   1. 应用运行时发 `reboot uart` → BL616 进 ROM 下载模式（flash.ps1 -AppPort 自己会做）；
 *   2. 等 ROM 口出现（VID 349B，COM 号会变）；
 *   3. flash.ps1 烧写 + 校验，打印 FLASH OK；
 *   4. 必须人工手动拨动开关重启（SW2 ON→OFF→ON；拔 USB 不会断开电池供电；ROM 标志在 AON 域，软复位无效）。
 *
 * 插件职责：烧录前关掉自己的串口（同一个 COM 口会冲突）→ 调脚本 → 解析阶段/输出
 * → 成功后保持 flashHold，等手动拨动开关重启后应用口出现再自动重连。
 */
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import { osKind, osLabel } from './platform.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
/* 固件工程脚本候选（见文件头）：V1.6 主线 → 旧 firmware_m2 布局。 */
const FLASH_SCRIPT_CANDIDATES = [
  path.resolve(here, '../../../firmware/lvgl_test/flash.ps1'),
  path.resolve(here, '../../../../firmware_m2/flash.ps1'),
];
export const DEFAULT_FLASH_SCRIPT = process.env.PROTORIG_FLASH_SCRIPT
  || FLASH_SCRIPT_CANDIDATES.find((candidate) => fs.existsSync(candidate))
  || FLASH_SCRIPT_CANDIDATES[0];
const MAX_JOB_MS = 120000;
const LOG_TAIL_LINES = 16;

/* 仓库根（plugin/src → plugin → V1.6，即本仓库根目录）。UI 里一律显示相对这里的路径，
 * 不把用户本机的绝对目录暴露到界面上（用户明确要求）。
 * 2026-11-15 从旧布局 PROTORIG_ES 统一到 V1.6：示例 bin / 固件脚本都按
 * firmware/lvgl_test/... 解析；需要老布局时用 PROTORIG_REPO_ROOT 覆盖。 */
export const REPO_ROOT = process.env.PROTORIG_REPO_ROOT
  || path.resolve(here, '../../..');

/** Windows 盘符/UNC 绝对路径（C:/...、C:\...、\\server\share）—— 跨平台识别用。
 *  注意不能用 path.win32.isAbsolute()：它会把 POSIX 的 `/Users/...` 也判成绝对路径。 */
function isWindowsAbsPath(value) {
  return /^[A-Za-z]:[\\/]/.test(value) || value.startsWith('\\\\');
}

/** Windows 后端被摆到非 Windows 平台时的说明（macOS 有独立后端）。 */
function windowsOnlyReason(platform) {
  return `Windows 烧录服务（flash.ps1）只在 Windows 上可用（当前 ${osLabel(platform)}）；`
    + 'macOS 使用内置的 BLFlashCommand 后端（flash-mac.mjs）。';
}

/** 输入可以是绝对路径，也可以是相对仓库根的路径。
 *  Windows 绝对路径（C:/... 或 C:\...）在 macOS/Linux 上也要原样保留 ——
 *  否则会被当成相对路径拼到仓库根上（配置里可能是从 Windows 机器抄过来的路径）。 */
export function resolveBinPath(input, repoRoot = REPO_ROOT) {
  const raw = String(input ?? '').trim();
  if (!raw) return '';
  if (isWindowsAbsPath(raw)) return raw;
  return path.isAbsolute(raw) ? raw : path.resolve(repoRoot, raw);
}

/** 界面展示用：仓库内显示相对路径，仓库外只显示文件名。 */
export function displayBinPath(absPath, repoRoot = REPO_ROOT) {
  const raw = String(absPath ?? '').trim();
  if (!raw) return '';
  /* Windows 绝对路径在非 Windows 宿主机上不能走 path.resolve（会被当成相对路径）；
   * 这种路径不可能是当前仓库内的路径，直接给文件名（用户要求界面不暴露本机绝对目录）。 */
  if (isWindowsAbsPath(raw)) {
    const winAbs = path.win32.normalize(raw);
    if (process.platform === 'win32') {
      const winRel = path.relative(repoRoot, winAbs);
      if (winRel && !winRel.startsWith('..') && !path.isAbsolute(winRel)) {
        return winRel.split(path.sep).join('/');
      }
    }
    return path.win32.basename(winAbs);
  }
  const abs = path.resolve(raw);
  const rel = path.relative(repoRoot, abs);
  if (rel && !rel.startsWith('..') && !path.isAbsolute(rel)) return rel.split(path.sep).join('/');
  return path.basename(abs);
}

const stripAnsi = (text) => String(text).replace(/\x1b\[[0-9;]*[A-Za-z]/g, '');


const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/* ---------------------------------------------------------------- 单实例锁 */
/*
 * 事故（research/63）：烧录进行中被外部强制停止，宿主一死、子进程跟着死，
 * 写 flash 到一半 → 板子停在 ROM 模式。三处加固：
 *   1) 烧录子进程 detached（独立进程组，不掉随宿主）；
 *   2) 输出落**文件**而不是管道（宿主死了也没有 EPIPE，子进程照跑）；
 *   3) 机器级锁文件，同一时刻只允许一个烧录，进程真死了才允许清锁。
 */
/* 注意：DSH 进程的 os.tmpdir() 指向每个会话独有的临时目录，不能当机器级锁用。
 * 这里固定到 %LOCALAPPDATA%\protorig-panel（没有就退回用户目录）。 */
export const DATA_DIR = process.env.PROTORIG_DATA_DIR
  || path.join(process.env.LOCALAPPDATA || process.env.USERPROFILE || os.homedir(), 'protorig-panel');
const LOCK_FILE = path.join(DATA_DIR, 'flash.lock');

function ensureDataDir() {
  try {
    fs.mkdirSync(DATA_DIR, { recursive: true });
  } catch {
    /* 已有或不可写 */
  }
}

function pidAlive(pid) {
  if (!pid) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return error.code === 'EPERM';
  }
}

export function lockInfo() {
  try {
    return JSON.parse(fs.readFileSync(LOCK_FILE, 'utf8'));
  } catch {
    return null;
  }
}

/** 拿锁；被活进程占着返回 false。 */
export function acquireLock(info) {
  ensureDataDir();
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      const fd = fs.openSync(LOCK_FILE, 'wx');
      fs.writeSync(fd, JSON.stringify(info));
      fs.closeSync(fd);
      return true;
    } catch (error) {
      if (error.code !== 'EEXIST') return false;
      const held = lockInfo();
      if (held && pidAlive(held.pid)) return false; // 真占着
      try {
        fs.unlinkSync(LOCK_FILE); // 陈锁（进程已死）
      } catch {
        return false;
      }
    }
  }
  return false;
}

export function releaseLock() {
  try {
    fs.unlinkSync(LOCK_FILE);
  } catch {
    /* 已经被清掉 */
  }
}

/**
 * 读日志文件（导出给单测）。
 *
 * PowerShell 5.1 把重定向到文件的输出写成 **UTF-16LE（带 BOM）**，Node 按 utf8
 * 读会得到夹着 NUL 的乱码，于是 "FLASH OK" 永远匹配不到 —— 事故后第一次真机
 * 测试就踩到了（烧录其实成功了，却判成失败并触发了一次多余的 retry）。
 */
export function readLogText(file) {
  const buf = fs.readFileSync(file);
  if (buf.length >= 2 && buf[0] === 0xFF && buf[1] === 0xFE) return buf.slice(2).toString('utf16le');
  if (buf.length >= 2 && buf[0] === 0xFE && buf[1] === 0xFF) {
    const swapped = Buffer.from(buf.slice(2));
    swapped.swap16();
    return swapped.toString('utf16le');
  }
  return buf.toString('utf8');
}

/** 读日志文件的增量行（导出给单测）。 */
export function drainLines(text, state) {
  const lines = [];
  let rest = text.slice(state.offset);
  const lastBreak = rest.lastIndexOf('\n');
  if (lastBreak < 0) return lines;
  rest = rest.slice(0, lastBreak);
  state.offset += lastBreak + 1;
  for (const raw of rest.split(/\r?\n/)) {
    const line = stripAnsi(raw).trim();
    if (line) lines.push(line);
  }
  return lines;
}

/** 输出行 → 烧录阶段（导出给单测）。 */
export function stageOfLine(line, current) {
  const text = stripAnsi(line);
  if (/FLASH OK/i.test(text)) return 'verify';
  if (/POWER-CYCLE THE BOARD NOW/i.test(text)) return 'done';
  if (/\[2\/3\].*Flashing/i.test(text)) return 'flash';
  if (/waiting for the download-mode port|romport:/i.test(text)) return 'wait-rom';
  if (/\[1\/3\].*reboot uart/i.test(text)) return 'reboot';
  return current;
}

/** 输出行是否是失败原因（导出给单测）。 */
export function errorOfLine(line) {
  const text = stripAnsi(line);
  if (/No new port appeared|No ROM download port|Missing:|FLASH FAILED|BLFlashCommand.*(error|fail)/i.test(text)) {
    return text.trim();
  }
  return null;
}

export function sha256File(file) {
  return new Promise((resolve, reject) => {
    const hash = crypto.createHash('sha256');
    const stream = fs.createReadStream(file);
    stream.on('error', reject);
    stream.on('data', (chunk) => hash.update(chunk));
    stream.on('end', () => resolve(hash.digest('hex')));
  });
}

/** 预检：文件存在、非空、sha256（前 8 位大写给 UI/工具用）。 */
/** 把待烧 bin 复制到数据目录再交给 flash.ps1：避免 -Bin 指向 build_out 时脚本 Copy-Item 自拷贝失败。 */
function stageBin(bin, sha8) {
  ensureDataDir();
  const dir = path.join(DATA_DIR, 'flash-staging');
  fs.mkdirSync(dir, { recursive: true });
  const base = path.basename(bin).replace(/\.bin$/i, '');
  const dest = path.join(dir, `${base}-${sha8}.bin`);
  if (path.resolve(dest) !== path.resolve(bin)) {
    fs.copyFileSync(bin, dest);
  }
  return dest;
}

/** 解析 PowerShell 绝对路径：DSH 宿主的 PATH 可能不含 System32，spawn('powershell.exe') 会 ENOENT。 */
export function powershellPath(platform = process.platform, env = process.env) {
  if (platform !== 'win32') return 'powershell.exe';
  if (typeof env.PROTORIG_POWERSHELL === 'string' && env.PROTORIG_POWERSHELL) return env.PROTORIG_POWERSHELL;
  const root = env.SystemRoot || env.windir || 'C:\\Windows';
  const candidate = path.join(root, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
  return fs.existsSync(candidate) ? candidate : 'powershell.exe';
}

/** 从串口列表挑唯一的 ROM 下载口（应用口在 Windows 上带 MI_00，ROM 口不带）。 */
export function pickRomPort(ports) {
  const list = Array.isArray(ports) ? ports : [];
  const roms = list.filter((p) => p && typeof p.path === 'string' && !/MI_00/i.test(String(p.pnpId ?? '')));
  return roms.length === 1 ? roms[0].path : null;
}

export async function preflightBin(bin, { scriptPath = DEFAULT_FLASH_SCRIPT } = {}) {
  const result = { ok: false, bin: '', display: '', size: 0, sha256: '', sha8: '', script: scriptPath, error: '' };
  if (typeof bin !== 'string' || !bin.trim()) {
    result.error = 'bin 路径为空';
    return result;
  }
  const resolved = resolveBinPath(bin);
  result.bin = resolved;
  result.display = displayBinPath(resolved);
  let stat;
  try {
    stat = fs.statSync(resolved);
  } catch (error) {
    result.error = `bin 不存在：${result.display || resolved}`;
    return result;
  }
  if (!stat.isFile()) {
    result.error = `不是文件：${result.display || resolved}`;
    return result;
  }
  if (stat.size <= 0) {
    result.error = 'bin 是空文件';
    return result;
  }
  if (!/\.bin$/i.test(resolved)) {
    result.error = '只接受 .bin 固件文件';
    return result;
  }
  result.size = stat.size;
  result.sha256 = await sha256File(resolved);
  result.sha8 = result.sha256.slice(0, 8).toUpperCase();
  if (!fs.existsSync(scriptPath)) {
    result.error = `烧录脚本不存在：${scriptPath}`;
    return result;
  }
  result.ok = true;
  return result;
}

/**
 * 烧录服务：同一时间只允许一个任务；失败/取消自动恢复面板链路，
 * 成功则保持 flashHold 等手动拨动开关重启。
 *
 * **本类只实现 Windows 后端**（research/73 §3.1 P3）：链路是 `flash.ps1` + `BLFlashCommand.exe`，
 * 非 Windows 平台 start()/preflight() 直接返回说明。macOS 走 flash-mac.mjs 的
 * MacFlashService；设置页两个平台都渲染烧录分组（client.js 按 platform.kind 切文案）。
 */
export class FlashService {
  constructor({ panel, logger, scriptPath = DEFAULT_FLASH_SCRIPT, spawnFn = spawn, platform = process.platform } = {}) {
    this.panel = panel;
    this.logger = logger;
    this.scriptPath = scriptPath;
    this.spawnFn = spawnFn;
    this.platform = platform;
    this.job = null;
    this.seq = 0;
  }

  /** 烧录能力（设置页/HTTP 路由都读它）。 */
  get supported() {
    // 本类只实现 Windows 后端；macOS 由 MacFlashService 负责
    return osKind(this.platform) === 'windows';
  }

  /**
   * 插件从 npm 装到 profile 后，默认的 `../../../firmware/lvgl_test/flash.ps1` 不在仓库里。
   * 从待烧 bin 的路径往上找 `firmware/lvgl_test/flash.ps1`，这样直接传仓库里的 bin 就能烧。
   */
  #scriptForBin(bin) {
    if (this.scriptPath && fs.existsSync(this.scriptPath)) return this.scriptPath;
    if (typeof bin === 'string' && bin.trim()) {
      let dir = path.dirname(path.resolve(bin));
      for (let i = 0; i < 8; i += 1) {
        for (const candidate of [
          path.join(dir, 'firmware', 'lvgl_test', 'flash.ps1'),
          path.join(dir, 'flash.ps1'),
        ]) {
          if (fs.existsSync(candidate)) return candidate;
        }
        const parent = path.dirname(dir);
        if (parent === dir) break;
        dir = parent;
      }
    }
    return this.scriptPath;
  }

  preflight(bin) {
    if (!this.supported) {
      return Promise.resolve({
        ok: false,
        bin: '',
        display: '',
        size: 0,
        sha256: '',
        sha8: '',
        script: this.scriptPath,
        platform: osLabel(this.platform),
        error: windowsOnlyReason(this.platform),
      });
    }
    return preflightBin(bin, { scriptPath: this.#scriptForBin(bin) });
  }

  /** 当前/最后一次任务视图（HTTP/UI/工具共用）。 */
  view() {
    const job = this.job;
    const supported = this.supported;
    if (!job) return { ok: false, stage: 'idle', running: false, supported };
    return {
      id: job.id,
      ok: job.ok,
      stage: job.stage,
      running: job.running,
      supported,
      bin: job.bin,
      size: job.size,
      sha8: job.sha8,
      seconds: job.seconds,
      needsPowerCycle: job.needsPowerCycle,
      romPort: job.romPort ?? '',
      error: job.error,
      logTail: job.logTail.slice(-8).join('\n'),
    };
  }

  /**
   * 启动烧录（不阻塞到结束）。
   * @param {string} bin
   * @param {{baud?:number, romPort?:string, retry?:boolean}} opts
   *        romPort 给"从 ROM 口恢复"用：板子已经停在下载模式时 DirectFlash。
   */
  async start(bin, { baud, romPort = null, retry = false } = {}) {
    if (!this.supported) {
      return { ok: false, error: windowsOnlyReason(this.platform), unreachable: true, job: this.view() };
    }
    if (this.job?.running) return { ok: false, error: 'flash-busy', job: this.view() };
    const pre = await this.preflight(bin);
    if (!pre.ok) return { ok: false, error: pre.error, preflight: pre };
    let resolvedRomPort = romPort;
    if (!resolvedRomPort) {
      /* 不传 romPort：让面板判断当前是应用模式还是 ROM 下载模式。
       * 应用口 → -AppPort（脚本自己 reboot uart）；ROM 口 → -Port 直烧。
       * 全新区板子停在下载模式时不需要用户先知道 COM 号。 */
      const target = typeof this.panel?.resolveFlashTarget === 'function'
        ? await this.panel.resolveFlashTarget()
        : (() => {
          const state = this.panel?.stateForApi?.() ?? {};
          return state.connected
            ? { mode: 'app', port: state.port ?? '', error: '' }
            : { mode: 'none', port: '', error: '手表未连接：请把板子进下载模式，或先连上应用口' };
        })();
      if (target?.mode === 'app' && target.port) {
        this.appPort = target.port;
      } else if (target?.mode === 'rom' && target.port) {
        resolvedRomPort = target.port;
      } else {
        return { ok: false, error: target?.error || '没有找到可烧录的板子：请确认板子在下载模式，或先连上应用口', preflight: pre };
      }
    }
    if (!acquireLock({ pid: process.pid, at: Date.now(), bin: pre.bin, sha8: pre.sha8 })) {
      return { ok: false, error: '另一个烧录正在进行（锁被占用）', preflight: pre };
    }
    const id = `flash-${++this.seq}-${Date.now().toString(36)}`;
    const job = {
      id,
      bin: pre.bin,
      script: pre.script,
      stagedBin: stageBin(pre.bin, pre.sha8),
      size: pre.size,
      sha8: pre.sha8,
      romPort: resolvedRomPort,
      retry,
      stage: romPort ? 'wait-rom' : 'starting',
      running: true,
      ok: false,
      seconds: 0,
      startedAt: Date.now(),
      needsPowerCycle: false,
      error: null,
      logTail: [],
      attempts: 0,
      child: null,
      timer: null,
      settle: null,
      baud: baud ?? 2000000,
    };
    job.finished = new Promise((resolve) => {
      job.settle = resolve;
    });
    this.job = job;
    void this.#run(job);
    return { ok: true, jobId: id, preflight: pre };
  }

  /** 从 ROM 下载模式直接烧（板子已经停在下载模式，应用不在线）。 */
  async recover(bin, { port = null, baud } = {}) {
    // 恢复模式没有“等应用出现”的必要，失败就立刻报错（重试只会多等一轮）
    return this.start(bin, { baud, romPort: port, retry: false });
  }

  /** 启动并等待结束（给 agent 工具用）。 */
  async runAndWait(bin, { baud, signal, romPort } = {}) {
    const started = await this.start(bin, { baud, romPort });
    if (!started.ok) return { ok: false, stage: 'error', error: started.error };
    const job = this.job;
    const onAbort = () => this.abort('调用方取消');
    signal?.addEventListener('abort', onAbort, { once: true });
    try {
      await job.finished;
    } finally {
      signal?.removeEventListener('abort', onAbort);
    }
    return this.view();
  }

  abort(reason = 'cancelled') {
    const job = this.job;
    if (!job?.running) return false;
    job.error = job.error ?? reason;
    this.#kill(job);
    return true;
  }

  #kill(job) {
    if (job.timer) {
      clearTimeout(job.timer);
      job.timer = null;
    }
    const child = job.child;
    if (!child) return;
    // detached 的子进程要按进程组杀，否则 powershell 死了 BLFlashCommand 还在写
    try {
      child.kill();
    } catch {
      /* 已经退出 */
    }
    try {
      spawn('taskkill', ['/PID', String(child.pid), '/T', '/F'], { windowsHide: true, stdio: 'ignore' });
    } catch {
      /* 尽力而为 */
    }
  }

  /** 烧一次（不重试）；返回是否成功，成败都写进 job。 */
  async #attempt(job, attempt) {
    ensureDataDir();
    const logFile = path.join(DATA_DIR, `flash-${job.id}-${attempt}.log`);
    fs.writeFileSync(logFile, '');
    job.logFile = logFile;

    const args = [
      '-NoProfile',
      '-ExecutionPolicy',
      'Bypass',
      '-File',
      job.script || this.scriptPath,
      '-Bin',
      job.stagedBin || job.bin,
      '-Baud',
      String(job.baud),
    ];
    if (job.romPort) {
      /* 板子停在 ROM 口时插件也可能已经把它当手表连上了；不先关链路，BLFlashCommand 会 Access denied。 */
      await this.panel?.suspendForFlash?.();
      args.push('-Port', job.romPort);
    } else {
      /* 必须每次都 suspend：start() 可能已经把 appPort 缓存成 COM8，
       * 若因为 this.appPort 存在就跳过，插件会一直占着串口，flash.ps1 打开即 Access denied。 */
      const suspended = await this.panel.suspendForFlash();
      const appPort = suspended || this.appPort;
      this.appPort = appPort;
      if (appPort) args.push('-AppPort', appPort);
      await sleep(150);
    }
    const powershell = powershellPath(this.platform, process.env);
    const commandLine = [powershell, ...args]
      .map((part) => (/\s/.test(part) ? `"${part}"` : part))
      .join(' ');
    this.logger.info(
      `烧录开始（第 ${attempt} 次）：${path.basename(job.bin)} sha=${job.sha8} ` +
        `${job.romPort ? `romPort=${job.romPort}` : `appPort=${this.appPort ?? '?'}`}`,
    );

    /* 为什么包一层 .cmd：Windows 上 detached 子进程拿不到父进程的 stdio，
     * 直接 spawn powershell + 文件句柄会一个字都写不出来（已实测）。
     * 让 .cmd 自己重定向到文件，才能同时做到「脱离宿主」和「留下日志」。 */
    /* Windows 上 detached + 重定向实测有两个坑（都踩过）：
     *   · detached 子进程拿不到父进程 stdio → 输出全丢；
     *   · .cmd 包装 + detached 在这个环境里连脚本都不执行。
     * 所以这里用**已验证可用**的方式：spawn powershell，stdout/stderr 直接给文件句柄。
     * 代价是宿主被杀时烧录会被打断 —— 那就靠 recover(--rom COMx) 恢复（见 research/63）。
     */
    const fd = fs.openSync(logFile, 'a');
    let child;
    let spawnError = null;
    try {
      child = this.spawnFn(powershell, args, {
        cwd: path.dirname(job.script || this.scriptPath),
        windowsHide: true,
        stdio: ['ignore', fd, fd],
        logFile, // 测试/诊断用：假子进程按这个契约写文件
      });
    } catch (error) {
      fs.closeSync(fd);
      job.error = `无法启动烧录进程：${error.message}`;
      return false;
    } finally {
      fs.closeSync(fd);
    }
    /* spawn 失败（最常见 ENOENT：宿主 PATH 没带 System32）是异步 'error' 事件；
     * 不接这个监听会变成进程级 uncaught exception，直接把 DSH 宿主带崩。 */
    child.on?.('error', (error) => {
      spawnError = error;
      job.error = `无法启动烧录进程：${error.message}`;
    });
    job.child = child;
    child.unref?.();

    const state = { offset: 0 };
    const deadline = Date.now() + MAX_JOB_MS;
    let gone = 0;
    while (true) {
      await sleep(300);
      if (spawnError) {
        this.#cleanupJobFiles(job);
        return false;
      }
      let text = '';
      try {
        text = readLogText(logFile);
      } catch {
        /* 还没落盘 */
      }
      for (const line of drainLines(text, state)) {
        job.logTail.push(line);
        if (job.logTail.length > LOG_TAIL_LINES) job.logTail.shift();
        const error = errorOfLine(line);
        if (error) job.error = error;
        job.stage = stageOfLine(line, job.stage);
        if (/FLASH OK/i.test(line)) {
          job.ok = true;
          job.stage = 'done';
          this.#cleanupJobFiles(job);
          return true;
        }
      }
      if (job.error) {
        this.#cleanupJobFiles(job);
        return false;
      }
      if (Date.now() > deadline) {
        job.error = `烧录超时（>${MAX_JOB_MS / 1000}s）`;
        this.#kill(job);
        return false;
      }
      // 子进程退出后日志可能还在落盘：等 EXITCODE 标记出现再判
      if (/EXITCODE=/i.test(text)) {
        this.#cleanupJobFiles(job);
        if (!job.error) job.error = '烧录进程退出但未见 FLASH OK';
        return false;
      }
      if (!pidAlive(child.pid)) {
        if (++gone > 6) {
          job.error = job.error ?? '烧录进程消失（可能被打断）';
          return false;
        }
      }
    }
  }

  /** 删掉本次任务的 .cmd 包装脚本（保留 .log 供追溯）。 */
  #cleanupJobFiles(job) {
    for (const suffix of ['.cmd']) {
      try {
        fs.unlinkSync(path.join(DATA_DIR, `flash-${job.id}-${job.attempts}${suffix}`));
      } catch {
        /* 已经没有 */
      }
    }
  }

  async #run(job) {
    try {
      job.attempts = 0;
      let ok = await this.#attempt(job, ++job.attempts);
      if (!ok && job.retry && !/取消/.test(job.error ?? '')) {
        // 失败一次自动重试：此时板子多半已在 ROM 模式，重试用纯 -Bin 让脚本自己找 ROM 口
        this.logger.warn(`烧录失败（${job.error}），2 s 后自动重试一次`);
        job.error = null;
        job.stage = 'wait-rom';
        // 恢复模式（romPort）保持原端口；应用模式重试则让脚本自己找 ROM 口
        if (!job.romPort) this.appPort = null;
        await sleep(2000);
        ok = await this.#attempt(job, ++job.attempts);
      }
      if (!ok) {
        job.ok = false;
        job.stage = 'error';
        job.error = job.error || '烧录失败';
        this.logger.warn(`烧录失败：${job.error}`);
        this.panel.resumeAfterFlash();
      } else {
        job.ok = true;
        job.stage = 'done';
        job.needsPowerCycle = true;
        this.logger.info(`烧录成功：${path.basename(job.bin)} sha=${job.sha8}（等待手动拨动开关重启 SW2 ON→OFF→ON）`);
      }
    } catch (error) {
      job.ok = false;
      job.stage = 'error';
      job.error = error instanceof Error ? error.message : String(error);
      this.logger.warn(`烧录异常：${job.error}`);
      this.panel.resumeAfterFlash();
    } finally {
      job.running = false;
      job.seconds = Math.round((Date.now() - job.startedAt) / 100) / 10;
      job.child = null;
      this.appPort = null;
      releaseLock();
      job.settle?.();
    }
  }
}
