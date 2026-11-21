/*
 * flash-mac.test.mjs —— macOS 烧录后端（flash-mac.mjs）的单元测试
 *
 * 原则：**不碰真实串口、不真的烧录**。串口枚举/子进程/reboot uart/Rosetta 检查
 * 全部注入替身；只验证 macOS 后端自己的纯逻辑与状态机。
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { EventEmitter } from 'node:events';
import {
  MacFlashService,
  buildFlashArgs,
  checkRosetta,
  errorOfFlashLine,
  pickRomPort,
  resolveFlashPaths,
  stageBin,
  stageOfFlashLine,
  stripLogPrefix,
} from '../src/flash-mac.mjs';
import { flashBackend, flashSupported } from '../src/platform.mjs';

const silent = { info() {}, warn() {}, error() {}, debug() {} };

function tmpFixture() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-macflash-'));
  const firmwareDir = path.join(dir, 'firmware', 'lvgl_test');
  const buildOut = path.join(firmwareDir, 'build', 'build_out');
  fs.mkdirSync(buildOut, { recursive: true });
  const cfg = path.join(firmwareDir, 'build', 'flash_prog_cfg.ini');
  fs.writeFileSync(cfg, '[cfg]\nerase = 1\n');
  const tool = path.join(dir, 'BLFlashCommand-macos');
  fs.writeFileSync(tool, '#!/bin/sh\nexit 0\n');
  fs.chmodSync(tool, 0o755);
  const bin = path.join(dir, 'demo_bl616.bin');
  fs.writeFileSync(bin, Buffer.from('PROTORIG-FW'));
  return {
    dir,
    repoDir: dir,
    firmwareDir,
    buildOut,
    cfg,
    tool,
    bin,
    appBin: path.join(buildOut, 'protorig_m2_bl616.bin'),
    backupDir: path.join(buildOut, '_prev'),
  };
}

function makePaths(fixture) {
  return {
    repoDir: fixture.repoDir,
    firmwareDir: fixture.firmwareDir,
    sdkBase: path.join(fixture.dir, 'sdk'),
    tool: fixture.tool,
    cfg: fixture.cfg,
    buildOut: fixture.buildOut,
    appBin: fixture.appBin,
    backupDir: fixture.backupDir,
  };
}

/* ---------------------------------------------------------------- 平台能力 */

test('平台能力：macOS 现在有烧录后端（BLFlashCommand），Linux 没有', () => {
  assert.equal(flashSupported('win32'), true, 'Windows 继续有 PowerShell 后端');
  assert.equal(flashSupported('darwin'), true, 'macOS 有 BLFlashCommand 后端');
  assert.equal(flashSupported('linux'), false);
  assert.equal(flashBackend('win32'), 'powershell');
  assert.equal(flashBackend('darwin'), 'blflashcommand');
  assert.equal(flashBackend('linux'), null);
});

/* ---------------------------------------------------------------- 纯函数 */

test('路径解析：默认按仓库根找 firmware/lvgl_test + ~/Developer/.tools SDK，可被环境变量覆盖', () => {
  /* 2026-11-08 Windows 侧修正：断言用 path.join 生成，别写死 POSIX 分隔符
   * （macOS 上仍是 /repo/...，Windows 上是 \repo\...，两边都应绿）。 */
  const repo = path.join('/repo');
  const home = path.join('/home/u');
  const defaults = resolveFlashPaths({ repoDir: repo, env: {}, home });
  assert.equal(defaults.firmwareDir, path.join(repo, 'firmware', 'lvgl_test'));
  assert.equal(defaults.sdkBase, path.join(home, 'Developer', '.tools', 'bouffalo_sdk'));
  assert.match(defaults.tool, /BLFlashCommand-macos$/);
  assert.equal(defaults.cfg, path.join(repo, 'firmware', 'lvgl_test', 'build', 'flash_prog_cfg.ini'));

  const overridden = resolveFlashPaths({
    repoDir: repo,
    env: {
      PROTORIG_FIRMWARE_DIR: '/fw',
      BL_SDK_BASE: '/sdk',
      PROTORIG_FLASH_TOOL: '/tool',
    },
    home,
  });
  assert.equal(overridden.firmwareDir, '/fw');
  assert.equal(overridden.sdkBase, '/sdk');
  assert.equal(overridden.tool, '/tool');
});

test('BLFlashCommand 参数与 SDK 的 make flash 一致', () => {
  const args = buildFlashArgs({ port: '/dev/cu.usbmodem123', baud: 2000000, cfg: '/fw/build/flash_prog_cfg.ini' });
  assert.deepEqual(args, [
    '--interface=uart',
    '--baudrate=2000000',
    '--port=/dev/cu.usbmodem123',
    '--chipname=bl616',
    '--config=/fw/build/flash_prog_cfg.ini',
  ]);
});

test('日志前缀剥离 + 阶段/错误解析（用真机日志格式）', () => {
  const line = '2026-11-08 13:32:40,729 - INFO - [13:32:40.729] - Start programming';
  assert.equal(stripLogPrefix(line), 'Start programming');
  assert.equal(stageOfFlashLine(line, 'starting'), 'flash');
  assert.equal(stageOfFlashLine('2026-11-08 13:32:40,728 - INFO - [13:32:40.728] - Bootrom load', 'wait-rom'), 'wait-rom');
  assert.equal(stageOfFlashLine('FLASH OK', 'flash'), 'done');
  const err = '2026-11-08 13:32:40,729 - INFO - [13:32:40.729] - [Errno 2] could not open port /dev/cu.usbmodemFAKE: No such file or directory';
  assert.match(errorOfFlashLine(err), /could not open port/);
  assert.equal(errorOfFlashLine('2026-11-08 13:32:40,566 - INFO - [13:32:40.566] - Start programming'), null);
});

test('pickRomPort：优先新出现的口，其次"应用口消失后剩下的唯一手表口"', () => {
  const app = '/dev/cu.usbmodemAPP';
  assert.equal(pickRomPort({ before: [app], after: [app, '/dev/cu.usbmodemROM'] }), '/dev/cu.usbmodemROM');
  assert.equal(pickRomPort({ before: [app], after: ['/dev/cu.usbmodemROM'], appPort: app }), '/dev/cu.usbmodemROM');
  assert.equal(pickRomPort({ before: [app], after: [app] }), '', '还是原来的口，不能当 ROM 口');
  /* 新出现多个口时取第一个（与 Windows flash.ps1 的 $new[0] 行为一致）；
   * 真正的判定仍由 BLFlashCommand 的握手结果兜底 */
  assert.equal(pickRomPort({ before: [app], after: ['/a', '/b'], appPort: app }), '/a');
});

test('stageBin：暂存到 build_out + 旧文件备份到 _prev + sha 校验', () => {
  const fx = tmpFixture();
  try {
    fs.writeFileSync(fx.appBin, Buffer.from('OLD-FW'));
    const result = stageBin({ bin: fx.bin, appBin: fx.appBin, backupDir: fx.backupDir });
    assert.equal(result.staged, true);
    assert.equal(result.size, 11);
    assert.match(result.sha8, /^[0-9A-F]{8}$/);
    assert.equal(fs.readFileSync(fx.appBin).toString(), 'PROTORIG-FW');
    const kept = fs.readdirSync(fx.backupDir);
    assert.equal(kept.length, 1, '旧 bin 被备份');
    assert.match(kept[0], /^protorig_m2_bl616_.*\.bin$/);

    // 源就是目标时不做复制（避免自己拷自己）
    const same = stageBin({ bin: fx.appBin, appBin: fx.appBin, backupDir: fx.backupDir });
    assert.equal(same.staged, false);
    assert.equal(same.size, 11);
  } finally {
    fs.rmSync(fx.dir, { recursive: true, force: true });
  }
});

/* ------------------------------------------------------------ preflight */

async function serviceFor(fixture, overrides = {}) {
  const paths = makePaths(fixture);
  return {
    service: new MacFlashService({
      panel: { stateForApi: () => ({ connected: true }), suspendForFlash: async () => '', resumeAfterFlash() {} },
      logger: silent,
      platform: 'darwin',
      paths,
      rosettaCheckFn: async () => true,
      ...overrides,
    }),
    paths,
  };
}

test('preflight：通过（工具/cfg/bin 都在 + Rosetta 可用）', async () => {
  const fx = tmpFixture();
  try {
    const { service } = await serviceFor(fx);
    const pre = await service.preflight(fx.bin);
    assert.equal(pre.ok, true);
    assert.equal(pre.size, 11);
    assert.match(pre.sha8, /^[0-9A-F]{8}$/);
    assert.equal(pre.platform, 'macOS');
  } finally {
    fs.rmSync(fx.dir, { recursive: true, force: true });
  }
});

test('preflight：报错可读（空路径/不存在/扩展名/缺工具/缺 cfg/缺 Rosetta）', async () => {
  const fx = tmpFixture();
  try {
    const { service } = await serviceFor(fx);
    assert.match((await service.preflight('')).error, /路径为空/);
    assert.match((await service.preflight(path.join(fx.dir, 'nope.bin'))).error, /不存在/);
    const txt = path.join(fx.dir, 'x.txt');
    fs.writeFileSync(txt, 'x');
    assert.match((await service.preflight(txt)).error, /\.bin/);

    const noTool = await serviceFor(fx, { paths: { ...makePaths(fx), tool: path.join(fx.dir, 'missing-tool') } });
    assert.match((await noTool.service.preflight(fx.bin)).error, /烧录工具不存在/);

    const noCfg = await serviceFor(fx, { paths: { ...makePaths(fx), cfg: path.join(fx.dir, 'missing.ini') } });
    assert.match((await noCfg.service.preflight(fx.bin)).error, /固件还没编译/);

    const noRosetta = await serviceFor(fx, { rosettaCheckFn: async () => false });
    assert.match((await noRosetta.service.preflight(fx.bin)).error, /Rosetta/);
  } finally {
    fs.rmSync(fx.dir, { recursive: true, force: true });
  }
});

test('checkRosetta：注入 spawn 替身，exit 0 = 可用，非 0 = 不可用', async () => {
  const fake = (code) => () => {
    const child = new EventEmitter();
    setTimeout(() => child.emit('exit', code), 0);
    return child;
  };
  assert.equal(await checkRosetta({ spawnFn: fake(0) }), true);
  assert.equal(await checkRosetta({ spawnFn: fake(1) }), false);
  assert.equal(await checkRosetta({ spawnFn: () => { throw new Error('no arch'); } }), false);
});

/* ------------------------------------------------------- 端到端状态机（替身） */

function fakeWatchPorts(sequence) {
  let index = 0;
  return async () => {
    const ports = sequence[Math.min(index, sequence.length - 1)];
    index += 1;
    return ports;
  };
}

function fakeBlflash({ lines = [], code = 0 } = {}) {
  const calls = [];
  const spawnFn = (command, args, options) => {
    calls.push({ command, args, options });
    const child = new EventEmitter();
    child.stdout = new EventEmitter();
    child.stderr = new EventEmitter();
    child.kill = () => {};
    setTimeout(() => {
      for (const line of lines) child.stderr.emit('data', `${line}\n`);
      child.emit('exit', code);
    }, 10);
    return child;
  };
  return { spawnFn, calls };
}

test('烧录成功：reboot uart → 等 ROM 口 → BLFlashCommand → 保持 flashHold 等断电', async () => {
  const fx = tmpFixture();
  try {
    let resumed = 0;
    let rebooted = null;
    const listPortsFn = fakeWatchPorts([
      ['/dev/cu.usbmodemAPP'], // #resolveRomPort 的 before 快照
      ['/dev/cu.usbmodemROM'], // 第一次轮询：ROM 口已经出现
    ]);
    const { spawnFn, calls } = fakeBlflash({
      lines: [
        '2026-11-08 13:32:40,566 - INFO - [13:32:40.566] - Start programming',
        '2026-11-08 13:32:41,000 - INFO - [13:32:41.000] - Verify done',
        '2026-11-08 13:32:42,000 - INFO - [13:32:42.000] - FLASH OK',
      ],
      code: 0,
    });
    const panel = {
      stateForApi: () => ({ connected: true, port: '/dev/cu.usbmodemAPP' }),
      suspendForFlash: async () => '/dev/cu.usbmodemAPP',
      resumeAfterFlash() { resumed += 1; },
    };
    const service = new MacFlashService({
      panel, logger: silent, platform: 'darwin', paths: makePaths(fx),
      listPortsFn,
      sendRebootFn: async (portPath) => { rebooted = portPath; },
      rosettaCheckFn: async () => true,
      spawnFn,
    });
    const started = await service.start(fx.bin, {});
    assert.equal(started.ok, true);
    await service.job.finished;
    const view = service.view();
    assert.equal(view.ok, true);
    assert.equal(view.stage, 'done');
    assert.equal(view.needsPowerCycle, true);
    assert.equal(resumed, 0, '成功后保持 flashHold，等用户断电重启');
    assert.equal(rebooted, '/dev/cu.usbmodemAPP', '先给应用口发 reboot uart');
    assert.equal(calls.length, 1);
    assert.equal(calls[0].command, fx.tool);
    assert.ok(calls[0].args.includes('--port=/dev/cu.usbmodemROM'));
    assert.ok(calls[0].args.includes(`--config=${fx.cfg}`));
    assert.equal(fs.readFileSync(fx.appBin).toString(), 'PROTORIG-FW', '镜像已暂存到 cfg 指向的位置');
  } finally {
    fs.rmSync(fx.dir, { recursive: true, force: true });
  }
});

test('烧录失败：BLFlashCommand 打不开口 → 恢复面板链路、错误可读', async () => {
  const fx = tmpFixture();
  try {
    let resumed = 0;
    const listPortsFn = fakeWatchPorts([
      ['/dev/cu.usbmodemAPP'],
      ['/dev/cu.usbmodemROM'],
    ]);
    const { spawnFn } = fakeBlflash({
      lines: [
        '2026-11-08 13:32:40,729 - INFO - [13:32:40.729] - [Errno 2] could not open port /dev/cu.usbmodemROM: No such file or directory',
      ],
      code: 1,
    });
    const panel = {
      stateForApi: () => ({ connected: true }),
      suspendForFlash: async () => '/dev/cu.usbmodemAPP',
      resumeAfterFlash() { resumed += 1; },
    };
    const service = new MacFlashService({
      panel, logger: silent, platform: 'darwin', paths: makePaths(fx),
      listPortsFn,
      sendRebootFn: async () => {},
      rosettaCheckFn: async () => true,
      spawnFn,
    });
    await service.start(fx.bin, {});
    await service.job.finished;
    const view = service.view();
    assert.equal(view.ok, false);
    assert.equal(view.stage, 'error');
    assert.match(view.error, /could not open port/);
    assert.equal(resumed, 1, '失败后恢复面板链路');
  } finally {
    fs.rmSync(fx.dir, { recursive: true, force: true });
  }
});

test('非 macOS 平台：start 直接拒绝，不碰串口/工具', async () => {
  const service = new MacFlashService({
    panel: { stateForApi: () => ({ connected: true }) },
    logger: silent,
    platform: 'linux',
    paths: {},
  });
  assert.equal(service.supported, false);
  assert.equal(service.view().supported, false);
  const started = await service.start('/nope.bin');
  assert.equal(started.ok, false);
  assert.equal(started.unreachable, true);
  assert.match(started.error, /Windows \/ macOS/);
});
