/*
 * flash-mac.mjs —— macOS 烧录后端（BL616 / Bouffalo BLFlashCommand-macos）
 *
 * 和 Windows 的 FlashService（flash.mjs → powershell + flash.ps1）对齐的同一套
 * 对外接口（supported / preflight / start / recover / runAndWait / abort / view /
 * job.finished），所以 routes.mjs、tools.mjs、设置页都不需要改协议。
 *
 * macOS 流程（与 Windows flash.ps1 等价）：
 *   1. 应用在跑：用 serialport 以 115200 给应用口发 `reboot uart`，让 BL616 进
 *      ROM 下载模式（USB 重新枚举，端口可能换成新的 /dev/cu.usbmodem*）；
 *   2. 轮询 SerialPort.list()，等到 ROM 下载口（VID 349B / cu.usbmodem*）；
 *   3. 把要烧的 .bin 暂存到 build/build_out/protorig_m2_bl616.bin
 *      （flash_prog_cfg.ini 固定指向这个文件；旧文件备份到 _prev/），
 *   4. 调 SDK 自带的 `BLFlashCommand-macos --interface=uart ... --config=<cfg>`，
 *      它按 cfg 烧 boot2 + partition + 应用；cfg 由本机编译生成；
 *   5. 成功 → 保持 flashHold，等用户手动拨动开关重启（SW2 ON→OFF→ON）；失败 → 恢复面板链路。
 *
 * 前置条件（preflight 会逐项检查）：
 *   · 本机已经编译过固件：firmware/lvgl_test/build/{flash_prog_cfg.ini,build_out/}
 *     （见仓库的 firmware/README.md）
 *   · Bouffalo SDK 在 ~/Developer/.tools/bouffalo_sdk（或 $BL_SDK_BASE）
 *   · Rosetta 2（SDK 的 BLFlashCommand-macos 是 x86_64）
 *
 * 烧录后**必须手动拨动开关重启**（SW2 ON→OFF→ON；拔 USB 不会断开电池供电）——
 * ROM 下载模式状态在 AON 域，软复位清不掉。与 Windows 完全一致。
 */
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { SerialPort } from 'serialport';
import { flashUnsupportedReason, isWatchPort, osKind } from './platform.mjs';
import {
  DATA_DIR, acquireLock, displayBinPath, releaseLock, resolveBinPath, sha256File,
} from './flash.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));

/** 真正的仓库根（plugin/src → plugin → <repo>）。Windows 侧 flash.mjs 的
 *  REPO_ROOT 已统一为 V1.6 仓库根（plugin/src -> plugin -> V1.6）；
 *  macOS 上用这个更直观的仓库根来解析相对 bin 路径。 */
export const REPO_DIR = process.env.PROTORIG_REPO_DIR || path.resolve(here, '../../..');

/** 所有路径都可注入/覆盖（测试与换机器时用）。 */
export function resolveFlashPaths({ repoDir = REPO_DIR, env = process.env, home = os.homedir() } = {}) {
  const firmwareDir = env.PROTORIG_FIRMWARE_DIR || path.join(repoDir, 'firmware', 'lvgl_test');
  const sdkBase = env.BL_SDK_BASE || path.join(home, 'Developer', '.tools', 'bouffalo_sdk');
  const tool = env.PROTORIG_FLASH_TOOL
    || path.join(sdkBase, 'tools', 'bflb_tools', 'bouffalo_flash_cube', 'BLFlashCommand-macos');
  const cfg = path.join(firmwareDir, 'build', 'flash_prog_cfg.ini');
  const buildOut = path.join(firmwareDir, 'build', 'build_out');
  const appBin = path.join(buildOut, 'protorig_m2_bl616.bin');
  const backupDir = path.join(buildOut, '_prev');
  return { repoDir, firmwareDir, sdkBase, tool, cfg, buildOut, appBin, backupDir };
}

export const PATHS = resolveFlashPaths();

const MAX_JOB_MS = 180000;
const ROM_WAIT_MS = 25000;
const LOG_TAIL_LINES = 16;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/* ---------------------------------------------------------------- 纯函数部分 */

/** BLFlashCommand 的参数（与 SDK 的 `make flash` 目标一致）。 */
export function buildFlashArgs({ port, baud = 2000000, cfg }) {
  return [
    '--interface=uart',
    `--baudrate=${baud}`,
    `--port=${port}`,
    '--chipname=bl616',
    `--config=${cfg}`,
  ];
}

/**
 * 从"重启前后两次端口快照"里挑 ROM 下载口（纯逻辑，单测覆盖）。
 *  - 优先：重启后新出现的口；
 *  - 兜底：应用口已经消失、只剩唯一一个手表口（macOS 重新枚举后路径可能不变）。
 */
export function pickRomPort({ before = [], after = [], appPort = null } = {}) {
  const beforeSet = new Set(before);
  const fresh = after.filter((portName) => !beforeSet.has(portName) && portName !== appPort);
  if (fresh.length > 0) return fresh[0];
  if (appPort && !after.includes(appPort)) {
    const remaining = after.filter((portName) => portName !== appPort);
    if (remaining.length === 1) return remaining[0];
  }
  return '';
}

/** BLFlashCommand 的日志行 → 烧录阶段（客户端的 stage 显示用）。 */
export function stageOfFlashLine(line, current) {
  const text = stripLogPrefix(String(line));
  if (/FLASH OK|POWER-CYCLE THE BOARD NOW/i.test(text)) return 'done';
  if (/verify|校验/i.test(text)) return 'verify';
  if (/Start programming|write flash|Flashing to/i.test(text)) return 'flash';
  if (/get boot info|Bootrom load|waiting for/i.test(text)) return 'wait-rom';
  return current;
}

/** BLFlashCommand 的日志行 → 失败原因（没有则 null）。 */
export function errorOfFlashLine(line) {
  const text = stripLogPrefix(String(line));
  if (/could not open port|No such file or directory|Errno \d+/i.test(text)) return text;
  if (/Failed to (flash|write|connect|verify|open)|Flash failed|擦除失败/i.test(text)) return text;
  return null;
}

/** 去掉 PyInstaller/python logging 前缀：`2026-11-08 13:32:40,560 - INFO - [ts] - msg`。 */
export function stripLogPrefix(line) {
  return String(line)
    .replace(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:,\d+)? - [A-Z]+ - /, '')
    .replace(/^\[\d{2}:\d{2}:\d{2}(?:\.\d+)?\] - /, '')
    .trim();
}

/**
 * 把要烧的 .bin 暂存到 build_out/protorig_m2_bl616.bin（cfg 固定指向它），
 * 旧文件备份到 _prev/，并校验 sha256 一致。
 */
export function stageBin({
  bin, appBin, backupDir, fsImpl = fs, stamp = new Date(),
} = {}) {
  const src = path.resolve(String(bin));
  const dst = path.resolve(String(appBin));
  const result = { staged: false, kept: '', size: 0, sha256: '', sha8: '' };
  if (src === dst) {
    result.size = fsImpl.statSync(src).size;
    return result;
  }
  const srcSize = fsImpl.statSync(src).size;
  const srcBuf = fsImpl.readFileSync(src);
  if (fsImpl.existsSync(dst)) {
    fsImpl.mkdirSync(backupDir, { recursive: true });
    const tag = stamp.toISOString().replace(/[-:T]/g, '').slice(0, 14);
    const keep = path.join(backupDir, `protorig_m2_bl616_${tag}.bin`);
    fsImpl.copyFileSync(dst, keep);
    result.kept = keep;
  }
  fsImpl.copyFileSync(src, dst);
  const dstBuf = fsImpl.readFileSync(dst);
  const a = sha256Buf(srcBuf);
  const b = sha256Buf(dstBuf);
  if (a !== b || srcBuf.length !== dstBuf.length) {
    throw new Error(`暂存校验失败：${path.basename(src)} 复制后内容不一致`);
  }
  result.staged = true;
  result.size = srcSize;
  result.sha256 = a;
  result.sha8 = a.slice(0, 8).toUpperCase();
  return result;
}

function sha256Buf(buf) {
  return crypto.createHash('sha256').update(buf).digest('hex');
}

/** 默认端口枚举：只留手表的候选口（VID 349B / cu.usbmodem*），返回 path 数组。
 *  macOS 上 SerialPort.list() 有时只给 /dev/tty.*；烧录要用 /dev/cu.*（不等 DCD），
 *  所以这里统一映射到 cu 路径并去重。 */
export async function defaultListPorts(platform = process.platform) {
  const list = await SerialPort.list();
  const paths = new Set();
  for (const port of list) {
    if (!port || !port.path || !isWatchPort(port, platform)) continue;
    let portPath = String(port.path);
    if (platform === 'darwin' && portPath.startsWith('/dev/tty.')) {
      const cuPath = portPath.replace('/dev/tty.', '/dev/cu.');
      if (fs.existsSync(cuPath)) portPath = cuPath;
    }
    paths.add(portPath);
  }
  return [...paths];
}

/** 给应用口发 `reboot uart`（默认行为；测试可注入替身）。 */
export function sendRebootUart(portPath, { baud = 115200, delayMs = 700 } = {}) {
  return new Promise((resolve, reject) => {
    const port = new SerialPort({ path: portPath, baudRate: baud, autoOpen: false });
    let settled = false;
    const finish = (error) => {
      if (settled) return;
      settled = true;
      const close = () => (error ? reject(error) : resolve(true));
      try {
        port.close(() => close());
      } catch {
        close();
      }
    };
    port.open((openError) => {
      if (openError) return finish(openError);
      port.write('reboot uart\r\n', (writeError) => {
        if (writeError) return finish(writeError);
        return port.drain(() => setTimeout(() => finish(null), delayMs));
      });
    });
  });
}

/** Rosetta 2 是否可用（SDK 的 BLFlashCommand-macos 是 x86_64）。 */
export function checkRosetta({ spawnFn = spawn } = {}) {
  return new Promise((resolve) => {
    try {
      const child = spawnFn('arch', ['-x86_64', '/usr/bin/true'], { stdio: 'ignore' });
      child.on('error', () => resolve(false));
      child.on('exit', (code) => resolve(code === 0));
    } catch {
      resolve(false);
    }
  });
}

/* ------------------------------------------------------------- 服务实现 */

/**
 * macOS 烧录服务。接口与 flash.mjs 的 FlashService 对齐，index.mjs 按平台二选一。
 */
export class MacFlashService {
  constructor({
    panel,
    logger,
    platform = process.platform,
    spawnFn = spawn,
    listPortsFn,
    sendRebootFn = sendRebootUart,
    rosettaCheckFn = checkRosetta,
    paths = PATHS,
  } = {}) {
    this.panel = panel;
    this.logger = logger;
    this.platform = platform;
    this.spawnFn = spawnFn;
    this.listPortsFn = listPortsFn || (() => defaultListPorts(platform));
    this.sendRebootFn = sendRebootFn;
    this.rosettaCheckFn = rosettaCheckFn;
    this.paths = paths;
    this.job = null;
    this.seq = 0;
    this.appPort = null;
  }

  /** 这个后端只负责 macOS；Windows 走 flash.mjs 的 FlashService。 */
  get supported() {
    return osKind(this.platform) === 'macos';
  }

  view() {
    const job = this.job;
    if (!job) {
      return { ok: false, stage: 'idle', running: false, supported: this.supported, backend: 'blflashcommand' };
    }
    return {
      id: job.id,
      ok: job.ok,
      stage: job.stage,
      running: job.running,
      supported: this.supported,
      backend: 'blflashcommand',
      bin: job.bin,
      size: job.size,
      sha8: job.sha8,
      seconds: job.seconds,
      needsPowerCycle: job.needsPowerCycle,
      error: job.error,
      logTail: job.logTail.slice(-8).join('\n'),
    };
  }

  async preflight(bin) {
    const result = {
      ok: false,
      bin: '',
      display: '',
      size: 0,
      sha256: '',
      sha8: '',
      script: this.paths.tool,
      cfg: this.paths.cfg,
      platform: 'macOS',
      error: '',
    };
    if (!this.supported) {
      result.error = flashUnsupportedReason(this.platform);
      return result;
    }
    if (typeof bin !== 'string' || !bin.trim()) {
      result.error = 'bin 路径为空';
      return result;
    }
    const resolved = resolveBinPath(bin, this.paths.repoDir);
    result.bin = resolved;
    result.display = displayBinPath(resolved, this.paths.repoDir);
    let stat;
    try {
      stat = fs.statSync(resolved);
    } catch {
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

    /* 烧录工具：SDK 的 BLFlashCommand-macos */
    if (!fs.existsSync(this.paths.tool)) {
      result.error = `烧录工具不存在：${this.paths.tool}（需要 Bouffalo SDK；用 firmware/tools/setup-mac-build.sh 装）`;
      return result;
    }
    /* 烧录配置：本机编译产物，cfg 里带着 boot2/partition/应用地址 */
    if (!fs.existsSync(this.paths.cfg)) {
      result.error = `固件还没编译：缺少 ${this.paths.cfg}（先 cd firmware/lvgl_test && ./build-mac.sh）`;
      return result;
    }
    /* x86_64 烧录工具需要 Rosetta 2 */
    const rosetta = await this.rosettaCheckFn();
    if (!rosetta) {
      result.error = '缺少 Rosetta 2（SDK 烧录工具是 x86_64）：softwareupdate --install-rosetta --agree-to-license';
      return result;
    }
    result.ok = true;
    return result;
  }

  async start(bin, { baud, romPort = null, retry = false } = {}) {
    if (!this.supported) {
      return { ok: false, error: flashUnsupportedReason(this.platform), unreachable: true, job: this.view() };
    }
    if (this.job?.running) return { ok: false, error: 'flash-busy', job: this.view() };
    const pre = await this.preflight(bin);
    if (!pre.ok) return { ok: false, error: pre.error, preflight: pre };

    if (!romPort) {
      const state = this.panel?.stateForApi?.() ?? {};
      if (!state.connected) {
        return {
          ok: false,
          error: '手表未连接：烧录前必须先连上应用串口（用于 reboot uart）；板子已在下载模式时用 romPort 直烧',
          preflight: pre,
        };
      }
    }
    if (!acquireLock({ pid: process.pid, at: Date.now(), bin: pre.bin, sha8: pre.sha8, backend: 'macos' })) {
      return { ok: false, error: '另一个烧录正在进行（锁被占用）', preflight: pre };
    }

    const id = `flash-mac-${++this.seq}-${Date.now().toString(36)}`;
    const job = {
      id,
      bin: pre.bin,
      size: pre.size,
      sha8: pre.sha8,
      romPort,
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

  /** 板子已经停在 ROM 下载模式时直烧（不需要 reboot uart）。 */
  async recover(bin, { port = null, baud } = {}) {
    return this.start(bin, { baud, romPort: port, retry: false });
  }

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
    try {
      job.child?.kill();
    } catch {
      /* 已经退出 */
    }
    return true;
  }

  #log(job, line) {
    const text = stripLogPrefix(line);
    if (!text) return;
    job.logTail.push(text);
    if (job.logTail.length > LOG_TAIL_LINES) job.logTail.shift();
    const error = errorOfFlashLine(text);
    if (error) job.error = error;
    job.stage = stageOfFlashLine(text, job.stage);
  }

  async #watchPorts() {
    try {
      const ports = await this.listPortsFn();
      return Array.isArray(ports)
        ? ports.map((port) => (typeof port === 'string' ? port : port?.path)).filter(Boolean)
        : [];
    } catch {
      return [];
    }
  }

  /** 应用口还在跑时：发 reboot uart → 等 ROM 口出现。 */
  async #resolveRomPort(job) {
    if (job.romPort) {
      job.stage = 'wait-rom';
      return job.romPort;
    }
    const appPort = await this.panel.suspendForFlash();
    this.appPort = appPort;
    if (!appPort) throw new Error('找不到应用串口：请插好手表，或用 romPort 指定 ROM 下载口');

    const before = await this.#watchPorts();
    this.logger?.info?.(`烧录：reboot uart → ${appPort}`);
    job.stage = 'reboot';
    let sendError = null;
    try {
      await this.sendRebootFn(appPort);
    } catch (error) {
      sendError = error instanceof Error ? error.message : String(error);
      this.logger?.warn?.(`烧录：reboot uart 发送失败（${sendError}），按"板子已在下载模式"继续尝试`);
    }

    job.stage = 'wait-rom';
    const deadline = Date.now() + ROM_WAIT_MS;
    let seenGone = false;
    while (Date.now() < deadline) {
      await sleep(400);
      const after = await this.#watchPorts();
      if (!after.includes(appPort)) seenGone = true;
      const fresh = after.filter((portName) => !before.includes(portName) && portName !== appPort);
      if (fresh.length > 0) {
        this.#log(job, `romport: ${fresh[0]}`);
        return fresh[0];
      }
      /* macOS 重新枚举后路径可能和原来一样：确认它先消失过、又回来，就是 ROM 口 */
      if (seenGone && after.includes(appPort)) {
        this.#log(job, `romport: ${appPort}（重新枚举后路径不变）`);
        return appPort;
      }
      /* reboot uart 都没发出去，而当前只剩一个手表口：多半本来就在下载模式 */
      if (sendError && after.length === 1 && after[0] === appPort) {
        this.#log(job, `romport: ${appPort}（按已在下载模式处理）`);
        return appPort;
      }
    }
    return '';
  }

  /** 真正烧一次；返回是否成功（结果写进 job）。 */
  async #attempt(job, attempt) {
    /* 1) 暂存镜像（cfg 固定指向 build_out/protorig_m2_bl616.bin） */
    job.stage = 'starting';
    const staged = stageBin({
      bin: job.bin,
      appBin: this.paths.appBin,
      backupDir: this.paths.backupDir,
    });
    if (staged.kept) this.#log(job, `keep  : ${path.basename(staged.kept)}`);
    this.logger?.info?.(
      `烧录开始（第 ${attempt} 次）：${path.basename(job.bin)} sha=${job.sha8} `
      + `boot2+partition+app（BLFlashCommand-macos）`,
    );

    /* 2) 找 ROM 下载口 */
    const port = await this.#resolveRomPort(job);
    if (!port) {
      job.error = `No new port appeared in ${ROM_WAIT_MS / 1000} s.`;
      this.#log(job, 'No new port appeared; 手工方法：按住 SW3(BOOT) → 点一下 SW1(RST) → 松开 SW3，然后用 romPort 直烧');
      return false;
    }
    this.#log(job, `[2/3] Flashing to ${port} ...`);
    job.stage = 'flash';

    /* 3) BLFlashCommand */
    const args = buildFlashArgs({ port, baud: job.baud, cfg: this.paths.cfg });
    let child;
    try {
      child = this.spawnFn(this.paths.tool, args, {
        cwd: path.dirname(this.paths.firmwareDir),
        env: process.env,
        stdio: ['ignore', 'pipe', 'pipe'],
      });
    } catch (error) {
      job.error = `无法启动烧录工具：${error instanceof Error ? error.message : String(error)}`;
      return false;
    }
    job.child = child;

    let spawnError = null;
    child.on?.('error', (error) => {
      spawnError = error;
    });
    const onChunk = (chunk) => {
      for (const line of String(chunk).split(/\r?\n/)) this.#log(job, line);
    };
    child.stdout?.on('data', onChunk);
    child.stderr?.on('data', onChunk);

    const rc = await new Promise((resolve) => {
      const timer = setTimeout(() => {
        job.error = job.error ?? `烧录超时（>${MAX_JOB_MS / 1000}s）`;
        try { child.kill(); } catch { /* ignore */ }
        resolve(-1);
      }, MAX_JOB_MS);
      child.on('exit', (code) => {
        clearTimeout(timer);
        resolve(Number.isInteger(code) ? code : 1);
      });
      child.on('error', () => {
        clearTimeout(timer);
        resolve(-1);
      });
    });

    if (spawnError) {
      job.error = `无法启动烧录工具：${spawnError.message}`;
      return false;
    }
    if (rc === 0) {
      this.#log(job, 'FLASH OK');
      this.#log(job, ' POWER-CYCLE THE BOARD NOW!');
      this.#log(job, ' Toggle SW2 ON -> OFF -> ON (USB unplug does NOT power-cycle a battery board).');
      return true;
    }
    job.error = job.error || `BLFlashCommand 退出码 ${rc}（板子未进下载模式 / 端口不对 / 波特率不匹配）`;
    return false;
  }

  async #run(job) {
    try {
      let ok = await this.#attempt(job, ++job.attempts);
      if (!ok && job.retry && !/取消/.test(job.error ?? '')) {
        this.logger?.warn?.(`烧录失败（${job.error}），2 s 后自动重试一次`);
        job.error = null;
        job.stage = 'wait-rom';
        await sleep(2000);
        ok = await this.#attempt(job, ++job.attempts);
      }
      if (!ok) {
        job.ok = false;
        job.stage = 'error';
        job.error = job.error || '烧录失败';
        this.logger?.warn?.(`烧录失败：${job.error}`);
        this.panel?.resumeAfterFlash?.();
      } else {
        job.ok = true;
        job.stage = 'done';
        job.needsPowerCycle = true;
        this.logger?.info?.(`烧录成功：${path.basename(job.bin)} sha=${job.sha8}（等待手动拨动开关重启 SW2 ON→OFF→ON）`);
      }
    } catch (error) {
      job.ok = false;
      job.stage = 'error';
      job.error = error instanceof Error ? error.message : String(error);
      this.logger?.warn?.(`烧录异常：${job.error}`);
      this.panel?.resumeAfterFlash?.();
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

/** 供设置页/工具显示的数据目录（与 Windows 烧录锁共用同一个）。 */
export const MAC_DATA_DIR = DATA_DIR;
