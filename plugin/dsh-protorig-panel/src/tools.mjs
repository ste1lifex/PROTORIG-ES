/*
 * tools.mjs —— 给 agent 的低 token 工具
 *
 *   protorig_state  只读：连接 / 电脑模式 / 透传 / 电池 / 亮度 / 页面
 *   protorig_flash  烧录：复用 FlashService（reboot uart → ROM 烧写 → 校验），
 *                   返回结构化结果，render 只给模型几行；完整日志留在 job 里。
 *   protorig_face   表盘（P8/P9）：status / switch / install（PNG→480×480/256 色→USB 上传）/ pcmode。
 *
 * 不依赖 @deepseek-ai/dsh-tools 的 defineTool（避免插件多一份宿主包依赖）：
 * 直接注册宿主接受的 ToolDefinition 结构（parameters 是 JSON Schema）。
 */
import fs from 'node:fs';
import { packImageBuffer, parsePfi, pfiToPreviewPng, PFI_LEN } from './face-pack.mjs';
import { FACE_IDS, faceLabel, normalizeFaceId } from './face-ids.mjs';
import { cacheThumb } from './face-cache.mjs';
import { packPanelImageBuffer } from './face-panel.mjs';

const asText = (text) => [{ type: 'text', text }];

function basename(file) {
  const parts = String(file ?? '').split(/[\\/]/);
  return parts[parts.length - 1] || String(file ?? '');
}

function flashTool({ flash }) {
  const output = {
    type: 'object',
    additionalProperties: false,
    properties: {
      ok: { type: 'boolean' },
      stage: { type: 'string' },
      seconds: { type: 'number' },
      bin: { type: 'string' },
      sha8: { type: 'string' },
      size: { type: 'number' },
      needsPowerCycle: { type: 'boolean' },
      romPort: { type: 'string' },
      error: { type: 'string' },
      logTail: { type: 'string' },
    },
    required: ['ok', 'stage', 'seconds', 'bin', 'sha8', 'size', 'needsPowerCycle', 'romPort', 'error', 'logTail'],
  };
  return {
    name: 'protorig_flash',
    description:
      '把固件烧进 PROTORIG ES 手表（BL616）。不传 romPort 时自动判断应用口/下载口；应用模式会自动 reboot uart。'
      + '烧写并校验。Windows 调 flash.ps1（脚本路径 PROTORIG_FLASH_SCRIPT 可覆盖）；'
      + 'macOS 调 SDK 自带的 BLFlashCommand-macos（需要本机已编译固件 + Rosetta 2，'
      + '见公开仓库的 firmware/README.md）。'
      + '烧完必须由人手动拨动开关重启（SW2 ON→OFF→ON；拔 USB 不会断开电池供电），软复位不会退出下载模式。'
      + '烧录期间会暂时关闭手表面板串口。Linux/其它平台没有后端，会直接返回说明并拒绝。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        bin: { type: 'string', description: 'bin 文件的绝对路径（.bin）' },
        confirm: {
          type: 'boolean',
          description: '必须显式传 true 才会真的烧录（防误触发）。烧录中途被打断会让板子停在 ROM 下载模式。',
        },
        romPort: {
          type: 'string',
          description: '可选：板子已经停在 ROM 下载模式时，直接指定 ROM 串口（如 COM5），不经过 reboot uart。',
        },
        baud: { type: 'number', enum: [1000000, 2000000, 3000000], description: 'ROM 烧录波特率，默认 2000000' },
      },
      required: ['bin'],
    },
    output: {
      schema: output,
      render(args, value) {
        const name = basename(value.bin) || basename(args.bin);
        if (value.ok) {
          return asText(
            `protorig_flash OK · ${name} · ${value.size} B · sha ${value.sha8} · ${value.seconds}s\r\n`
            + '需要手动拨动开关重启：SW2 ON→OFF→ON（拔 USB 不会断开电池供电）。重启后插件会自动重连。',
          );
        }
        const tail = String(value.logTail || '').split('\n').filter(Boolean).slice(-4).join('\n');
        return asText(
          `protorig_flash FAILED · stage=${value.stage} · ${name}\r\n`
          + `${value.error || 'unknown error'}${tail ? `\r\n--- log tail ---\r\n${tail}` : ''}`,
        );
      },
    },
    async execute(args, exec) {
      if (args?.confirm !== true) {
        return {
          ok: false,
          stage: 'refused',
          seconds: 0,
          bin: String(args?.bin ?? ''),
          sha8: '',
          size: 0,
          needsPowerCycle: false,
          romPort: '',
          error: '未确认：必须传 confirm:true（烧录中途被打断会让板子停在 ROM 模式）',
          logTail: '',
        };
      }
      const result = await flash.runAndWait(args?.bin, {
        baud: typeof args?.baud === 'number' ? args.baud : undefined,
        romPort: typeof args?.romPort === 'string' && args.romPort ? args.romPort : null,
        signal: exec?.signal,
      });
      return {
        ok: result.ok === true,
        stage: String(result.stage ?? 'error'),
        seconds: Number.isFinite(result.seconds) ? result.seconds : 0,
        bin: String(result.bin ?? ''),
        sha8: String(result.sha8 ?? ''),
        size: Number.isFinite(result.size) ? result.size : 0,
        needsPowerCycle: result.needsPowerCycle === true,
        romPort: String(result.romPort ?? ''),
        error: String(result.error ?? ''),
        logTail: String(result.logTail ?? ''),
      };
    },
  };
}

function faceTool({ panel }) {
  const output = {
    type: 'object',
    additionalProperties: false,
    properties: {
      ok: { type: 'boolean' },
      action: { type: 'string' },
      current: { type: 'number' },
      connected: { type: 'boolean' },
      valid: { type: 'boolean' },
      crc: { type: 'string' },
      stage: { type: 'string' },
      percent: { type: 'number' },
      seconds: { type: 'number' },
      pcMode: { type: 'boolean' },
      thumbCached: { type: 'boolean' },
      image: { type: 'string' },
      error: { type: 'string' },
    },
    required: ['ok', 'action', 'error'],
  };
  return {
    name: 'protorig_face',
    description:
      'PROTORIG ES 手表表盘（P8/P9）。action=status 查询当前表盘/图片槽/安装进度；'
      + 'switch 切表盘（**只支持 id 0=默认 / 3=图片**；face 1/2 已从固件删除；切表盘会退出电脑模式回手表模式）；'
      + 'install 把本机 PNG/JPG 转成 480×480、256 色图片表盘并经 USB 上传（约 45 s，'
      + '需要 confirm:true，且需要手表 USB 连接）；pcmode 在手表模式/电脑副屏之间切换。'
      + '切回默认表盘 = switch id=0。默认表盘下半区（480×232）请用 protorig_panel 工具。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        action: { type: 'string', enum: ['status', 'switch', 'install', 'pcmode'] },
        id: { type: 'number', enum: [...FACE_IDS], description: 'switch 的目标表盘 id：0=默认，3=图片' },
        withCrc: { type: 'boolean', description: 'status 时是否让固件读回 231 KB 重算 CRC（较慢）' },
        imagePath: { type: 'string', description: 'install：本机 PNG/JPG（或现成 .pfi）路径' },
        imageBase64: { type: 'string', description: 'install：图片 base64（与 imagePath 二选一）' },
        layout: {
          type: 'object',
          description: 'install：叠层布局 {timeX,timeY,fontId,flags,color(0xRRGGBB)}；默认 (110,170,1,0x03,0xFFFA00)',
        },
        apply: { type: 'boolean', description: 'install：装完是否切到图片表盘（默认 true）' },
        confirm: { type: 'boolean', description: 'install 必须显式 true（会写手表 flash）' },
        enter: { type: 'boolean', description: 'pcmode：true=回电脑副屏（默认），false=回手表模式' },
      },
      required: ['action'],
    },
    output: {
      schema: output,
      render(args, value) {
        if (value.action === 'status') {
          return asText(
            `protorig_face status · face=${value.current}(${faceLabel(value.current)})`
            + ` · image=${value.valid ? `installed crc=${value.crc}` : 'not-installed'}`
            + ` · connected=${value.connected}`,
          );
        }
        if (value.action === 'switch') {
          return asText(value.ok
            ? `protorig_face switch OK · 当前 face=${value.current}（手表模式；需要副屏用 pcmode enter）`
            : `protorig_face switch FAILED · ${value.error}`);
        }
        if (value.action === 'pcmode') {
          return asText(value.ok
            ? `protorig_face pcmode OK · pcMode=${value.pcMode}`
            : `protorig_face pcmode FAILED · ${value.error}`);
        }
        return asText(value.ok
          ? `protorig_face install OK · ${value.image || 'image'} · crc=${value.crc} · ${value.seconds}s · 当前 face=${value.current}`
          : `protorig_face install FAILED · stage=${value.stage} · ${value.error}`);
      },
    },
    async execute(args) {
      const action = String(args?.action ?? 'status');
      if (!panel || typeof panel.faceSwitch !== 'function') {
        return { ok: false, action, error: '表盘服务不可用（插件未正确加载）' };
      }
      if (action === 'status') {
        const status = await panel.faceStatus({ withCrc: args?.withCrc === true }).catch((error) => ({ supported: false, error: error.message }));
        const job = typeof panel.faceInstallView === 'function' ? panel.faceInstallView() : null;
        return {
          ok: status.supported !== false,
          action: 'status',
          current: Number.isFinite(panel.faceId) ? panel.faceId : 0,
          connected: status.supported !== false,
          valid: status.valid === true,
          crc: String(status.crc ?? ''),
          stage: job?.running ? String(job.stage ?? '') : '',
          percent: Number.isFinite(job?.percent) ? job.percent : 0,
          error: String(status.error ?? ''),
        };
      }
      if (action === 'switch') {
        const id = normalizeFaceId(args?.id);
        if (id === null) {
          return {
            ok: false, action: 'switch', error: 'id 只支持 0（默认）/ 3（图片）—— face 1/2 已从固件删除', current: panel.faceId ?? 0,
          };
        }
        const result = await panel.faceSwitch(id);
        return {
          ok: result.ok !== false,
          action: 'switch',
          current: Number.isFinite(panel.faceId) ? panel.faceId : id,
          error: String(result.error ?? ''),
        };
      }
      if (action === 'pcmode') {
        const result = args?.enter === false
          ? await panel.exitPcMode?.()
          : await panel.enterPcMode?.();
        return {
          ok: result?.ok !== false,
          action: 'pcmode',
          pcMode: result?.pcMode === true,
          error: String(result?.error ?? ''),
        };
      }
      /* install */
      if (args?.confirm !== true) {
        return { ok: false, action: 'install', error: '未确认：install 会写手表 flash，必须传 confirm:true' };
      }
      let buffer = null;
      let imageName = '';
      try {
        if (typeof args?.imagePath === 'string' && args.imagePath.trim()) {
          imageName = basename(args.imagePath);
          buffer = fs.readFileSync(args.imagePath);
        } else if (typeof args?.imageBase64 === 'string' && args.imageBase64.trim()) {
          imageName = 'inline-image';
          const raw = args.imageBase64.includes(',')
            ? args.imageBase64.slice(args.imageBase64.indexOf(',') + 1)
            : args.imageBase64;
          buffer = Buffer.from(raw, 'base64');
        }
      } catch (error) {
        return { ok: false, action: 'install', error: `读图片失败：${error.message}` };
      }
      if (!buffer || buffer.length === 0) {
        return { ok: false, action: 'install', error: '缺少 imagePath 或 imageBase64' };
      }
      let pfi;
      let crcHex = '';
      let previewPng = null;
      try {
        if (/\.pfi$/i.test(imageName) && buffer.length === PFI_LEN) {
          const parsed = parsePfi(buffer);
          if (!parsed.ok) throw new Error('PFI 校验失败（header/blob CRC 不一致）');
          pfi = buffer;
          crcHex = parsed.imageCrc32.toString(16).toUpperCase().padStart(8, '0');
          previewPng = pfiToPreviewPng(buffer);
          imageName = `${imageName} (crc ${crcHex})`;
        } else {
          const packed = await packImageBuffer(buffer, args?.layout ?? {});
          pfi = packed.pfi;
          crcHex = packed.crcHex;
          previewPng = packed.previewPng;
          imageName = `${imageName} → ${packed.crcHex}`;
        }
      } catch (error) {
        return { ok: false, action: 'install', error: `图片转换失败：${error.message}` };
      }
      /* 本地缩略图缓存（best effort；装过之后设置页能显示"当前图片表盘"缩略图） */
      let thumbCached = false;
      try {
        thumbCached = Boolean(cacheThumb(crcHex, previewPng));
      } catch {
        /* 缓存不是关键路径 */
      }
      const started = await panel.faceInstallAndWait(pfi, { apply: args?.apply !== false });
      if (!started.ok) {
        return { ok: false, action: 'install', error: String(started.error ?? '安装启动失败') };
      }
      const view = started.view ?? {};
      return {
        ok: view.ok === true,
        action: 'install',
        current: Number.isFinite(panel.faceId) ? panel.faceId : 0,
        crc: String(view.crc ?? crcHex ?? ''),
        stage: String(view.stage ?? ''),
        percent: Number.isFinite(view.percent) ? view.percent : 0,
        seconds: Number.isFinite(view.seconds) ? view.seconds : 0,
        image: imageName,
        thumbCached,
        error: String(view.error ?? ''),
      };
    },
  };
}

function stateTool({ panel }) {
  const output = {
    type: 'object',
    additionalProperties: false,
    properties: {
      ok: { type: 'boolean' },
      connected: { type: 'boolean' },
      port: { type: 'string' },
      pcMode: { type: 'boolean' },
      passthrough: { type: 'boolean' },
      battery: { type: 'number' },
      bright: { type: 'number' },
      page: { type: 'number' },
      fps: { type: 'number' },
      ble: { type: 'boolean' },
      flashHold: { type: 'boolean' },
      deviceId: { type: 'string' },
    },
    required: ['ok', 'connected', 'port', 'pcMode', 'passthrough', 'battery', 'bright', 'page', 'fps', 'ble', 'flashHold', 'deviceId'],
  };
  return {
    name: 'protorig_state',
    description: '只读查询 PROTORIG ES 手表面板状态（串口/电脑模式/透传/电池/亮度/页面）。',
    parameters: { type: 'object', additionalProperties: false, properties: {} },
    output: {
      schema: output,
      render(args, value) {
        const battery = Number.isFinite(value.battery) ? `${value.battery}%` : '—';
        const bright = Number.isFinite(value.bright) ? String(value.bright) : '—';
        return asText(
          `protorig_state: ${value.connected ? 'connected' : 'disconnected'}`
          + `${value.port ? ` ${value.port}` : ''} · pc=${value.pcMode ? 1 : 0} · passthrough=${value.passthrough ? 1 : 0}`
          + ` · battery=${battery} · bright=${bright} · fps=${value.fps}`
          + `${value.deviceId ? ` · device=${value.deviceId}` : ''}`
          + `${value.ble ? ' · BLE' : ''}${value.flashHold ? ' · flashHold' : ''}`,
        );
      },
    },
    async execute() {
      const state = panel.stateForApi();
      return {
        ok: true,
        connected: !!state.connected,
        port: String(state.port ?? ''),
        pcMode: !!state.pcMode,
        passthrough: !!state.passthrough,
        battery: Number.isFinite(state.battery) ? state.battery : -1,
        bright: Number.isFinite(state.bright) ? state.bright : -1,
        page: Number.isFinite(state.page) ? state.page : -1,
        fps: Number.isFinite(state.fps) ? state.fps : 0,
        ble: !!state.ble,
        flashHold: !!state.flashHold,
        deviceId: String(state.device?.id ?? ''),
      };
    },
  };
}

function notifyTool({ panel }) {
  const output = {
    type: 'object',
    additionalProperties: false,
    properties: {
      ok: { type: 'boolean' },
      shown: { type: 'boolean' },
      via: { type: 'string' },
      durationSec: { type: 'number' },
      vibrated: { type: 'boolean' },
      note: { type: 'string' },
      error: { type: 'string' },
    },
    required: ['ok', 'shown', 'via', 'durationSec', 'vibrated', 'error'],
  };
  return {
    name: 'protorig_notify',
    description: '在手表上显示一条限时通知（USB 像素副屏）；BLE 只震动。审批/问答卡在屏时返回忙。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        title: { type: 'string', description: '标题，≤24 字' },
        body: { type: 'string', description: '正文，≤100 字' },
        durationSec: { type: 'number', description: '显示秒数 1–30，默认 5' },
        vibrate: { type: 'boolean', description: '同时震一下，默认 true' },
      },
    },
    output: {
      schema: output,
      render(_args, value) {
        return asText(value.ok
          ? `protorig_notify: ${value.shown ? '已显示' : '未显示'} ${value.via}`
            + `${value.durationSec ? ` · ${value.durationSec}s` : ''}${value.vibrated ? ' · 震动' : ''}`
          : `protorig_notify FAILED · ${value.error}`);
      },
    },
    async execute(args = {}) {
      const value = await panel.showNotification(args);
      return { ...value, note: value.note ?? '', error: value.error ?? '' };
    },
  };
}

function vibrateTool({ panel }) {
  const output = {
    type: 'object',
    additionalProperties: false,
    properties: {
      ok: { type: 'boolean' },
      ms: { type: 'number' },
      times: { type: 'number' },
      via: { type: 'string' },
      error: { type: 'string' },
    },
    required: ['ok', 'ms', 'times', 'via', 'error'],
  };
  return {
    name: 'protorig_vibrate',
    description: '让手表震动（提醒/找表）。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        ms: { type: 'number', description: '单次毫秒 20–2000，默认 180' },
        times: { type: 'number', description: '次数 1–5，默认 1' },
      },
    },
    output: {
      schema: output,
      render(_args, value) {
        return asText(value.ok
          ? `protorig_vibrate: ${value.times}×${value.ms}ms · ${value.via}`
          : `protorig_vibrate FAILED · ${value.error}`);
      },
    },
    async execute(args = {}) {
      const value = await panel.vibrate(args);
      return { ...value, error: value.error ?? '' };
    },
  };
}

function screenshotTool({ panel }) {
  const output = {
    type: 'object',
    additionalProperties: false,
    properties: {
      ok: { type: 'boolean' },
      path: { type: 'string' },
      width: { type: 'number' },
      height: { type: 'number' },
      notice: { type: 'boolean' },
      pcMode: { type: 'boolean' },
      error: { type: 'string' },
    },
    required: ['ok', 'path', 'width', 'height', 'notice', 'pcMode', 'error'],
  };
  return {
    name: 'protorig_screenshot',
    description: '把手表当前画面按同一 renderer 渲成 PNG 并返回本机路径（配合 read_image 查看）。',
    parameters: { type: 'object', additionalProperties: false, properties: {} },
    output: {
      schema: output,
      render(_args, value) {
        return asText(value.ok
          ? `protorig_screenshot: ${value.path} · ${value.width}x${value.height}`
            + `${value.notice ? ' · notice' : ''}${value.pcMode ? '' : ' · pcMode=0'}`
          : `protorig_screenshot FAILED · ${value.error}`);
      },
    },
    async execute() {
      const value = await panel.screenshot();
      return { ...value, error: value.error ?? '' };
    },
  };
}

/**
 * 默认表盘下半区面板（480×232，media 分区 I8）。
 *
 * 以前这个功能只在设置页里，agent 侧够不到——想验证一次底图更换就必须人肉点页面。
 * 这里补成一个工具，动作和设置页的分组一一对应（status / install / clear）。
 */
export function panelTool({ panel }) {
  const output = {
    type: 'object',
    additionalProperties: false,
    properties: {
      ok: { type: 'boolean' },
      action: { type: 'string' },
      supported: { type: 'boolean' },
      valid: { type: 'boolean' },
      crc: { type: 'string' },
      width: { type: 'number' },
      height: { type: 'number' },
      y: { type: 'number' },
      size: { type: 'number' },
      stage: { type: 'string' },
      percent: { type: 'number' },
      seconds: { type: 'number' },
      error: { type: 'string' },
      warning: { type: 'string' },
    },
  };
  return {
    name: 'protorig_panel',
    description:
      'PROTORIG ES 默认表盘**下半区面板**（480×232，写 media 分区 I8，112,384 B）。'
      + 'action=status 查询当前面板的 CRC/尺寸（withCrc:true 会让固件读回 112 KB 复算，较慢）；'
      + 'install 把本机 PNG/JPG cover 成 480×232、256 色后经 USB 上传（会临时退出电脑模式，约 1–2 分钟，'
      + '需 confirm:true，需手表 USB 连接）；clear 恢复内置底图（固件只擦 header，立刻回内置图）。'
      + '注意：面板存在 media 分区，**不会被烧录擦除**——换完固件它还在，要回内置图必须 clear。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        action: { type: 'string', enum: ['status', 'install', 'clear'] },
        imagePath: { type: 'string', description: 'install：本机 PNG/JPG 路径（会 cover 裁切成 480×232）' },
        imageBase64: { type: 'string', description: 'install：图片 base64（与 imagePath 二选一）' },
        dither: { type: 'boolean', description: 'install：调色板抖动，默认 true' },
        withCrc: { type: 'boolean', description: 'status：让固件读回 112 KB 复算 CRC（较慢）' },
        confirm: { type: 'boolean', description: 'install 必须显式 true（会写手表 media 分区）' },
      },
      required: ['action'],
    },
    output: {
      schema: output,
      render(args, value) {
        if (value.action === 'clear') {
          return asText(value.ok
            ? 'protorig_panel clear OK · 已恢复内置底图'
            : `protorig_panel clear FAILED · ${value.error ?? '未知错误'}`);
        }
        if (value.action === 'install') {
          return asText(value.ok
            ? `protorig_panel install OK · crc=${value.crc} · ${value.seconds}s`
              + (value.warning ? ` · ${value.warning}` : '')
            : `protorig_panel install FAILED · stage=${value.stage ?? '?'} · ${value.error ?? '未知错误'}`);
        }
        /* supported=false 时不能只说"not-installed"——那看起来像"面板没装"，
         * 实际是当前链路根本查不了（BLE 不支持下半区面板）。 */
        if (!value.supported) {
          return asText(`protorig_panel status · 查不到（${value.error || '需要 USB 连接（BLE 不支持下半区面板）'}）`);
        }
        const panel = value.valid
          ? `installed crc=${value.crc} ${value.width}×${value.height} y=${value.y}`
          : 'not-installed（内置底图）';
        return asText(`protorig_panel status · ${panel}`);
      },
    },
    async execute(args) {
      const action = String(args?.action ?? 'status');
      if (!panel || typeof panel.panelStatus !== 'function') {
        return { ok: false, action, error: '面板服务不可用（插件未正确加载）' };
      }
      if (action === 'status') {
        const value = await panel.panelStatus({ withCrc: args?.withCrc === true })
          .catch((error) => ({ supported: false, error: error.message }));
        /* 面板状态里还有 len/rc/raw/withCrc 等调试字段，输出 schema 是封闭的，
         * 这里只投影声明的字段（并保证类型，null 会直接被宿主判为非法）。 */
        return {
          ok: !value.error,
          action,
          supported: Boolean(value.supported),
          valid: value.valid === true,
          crc: String(value.crc ?? ''),
          width: Number(value.width ?? 0),
          height: Number(value.height ?? 0),
          y: Number(value.y ?? 0),
          size: Number(value.len ?? 0),
          error: String(value.error ?? ''),
        };
      }
      if (action === 'clear') {
        const result = await panel.panelClear().catch((error) => ({ ok: false, error: error.message }));
        return {
          ok: result.ok === true,
          action,
          supported: true,
          valid: false,
          crc: '',
          width: 0,
          height: 0,
          y: 0,
          size: 0,
          error: String(result.error ?? ''),
        };
      }
      if (args?.confirm !== true) {
        return { ok: false, action, error: 'install 需要 confirm:true（会写手表 media 分区）' };
      }
      let buffer;
      try {
        if (typeof args?.imagePath === 'string' && args.imagePath.trim()) {
          buffer = fs.readFileSync(args.imagePath);
        } else if (typeof args?.imageBase64 === 'string' && args.imageBase64.trim()) {
          const raw = args.imageBase64.includes(',')
            ? args.imageBase64.slice(args.imageBase64.indexOf(',') + 1)
            : args.imageBase64;
          buffer = Buffer.from(raw, 'base64');
        }
      } catch (error) {
        return { ok: false, action, error: `读取图片失败：${error.message}` };
      }
      if (!buffer?.length) return { ok: false, action, error: '缺少 imagePath 或 imageBase64' };
      let packed;
      try {
        packed = await packPanelImageBuffer(buffer, { dither: args?.dither !== false });
      } catch (error) {
        return { ok: false, action, error: `面板图转换失败：${error.message}` };
      }
      const result = await panel.panelInstallAndWait(packed.blob).catch((error) => ({ ok: false, view: { error: error.message } }));
      const view = result.view ?? {};
      return {
        ok: result.ok === true && view.ok === true,
        action,
        supported: true,
        valid: result.ok === true && view.ok === true,
        crc: String(packed.crcHex ?? ''),
        size: Number(packed.size ?? 0),
        width: Number(packed.width ?? 0),
        height: Number(packed.height ?? 0),
        y: 0,
        stage: String(view.stage ?? ''),
        percent: Number(view.percent ?? 0),
        seconds: Number(view.seconds ?? 0),
        warning: String(view.warning ?? ''),
        error: String(view.error ?? result.error ?? ''),
      };
    },
  };
}

/**
 * 串口/BLE 的"让出来 / 收回来"。
 *
 * 插件在宿主进程内常驻，会一直占着 /dev/tty.usbmodem*（或 BLE 连接），而且每 1.5 s
 * 还会自己重连一次。用户/agent 想跑 CLI 脚本、`ble-e2e`、串口助手或自定义工具时，
 * 不先让开就必然撞"口被占用"，刚让开又会被插件抢回去。
 *
 * pause 带 TTL（默认 120 s）：脚本写崩了、agent 忘了 resume，也不会把插件永久挂起。
 */
export function connTool({ panel }) {
  const output = {
    type: 'object',
    additionalProperties: false,
    properties: {
      ok: { type: 'boolean' },
      action: { type: 'string' },
      paused: { type: 'boolean' },
      connected: { type: 'boolean' },
      ble: { type: 'boolean' },
      port: { type: 'string' },
      device: { type: 'string' },
      seconds: { type: 'number' },
      error: { type: 'string' },
    },
  };
  const snapshot = (action, extra = {}) => {
    const st = panel?.stateForApi?.() ?? {};
    return {
      ok: true,
      action,
      paused: Boolean(st.paused),
      connected: Boolean(st.connected),
      ble: Boolean(st.ble),
      port: String(st.port ?? ''),
      device: String(st.device?.id ?? ''),
      seconds: 0,
      error: '',
      ...extra,
    };
  };
  return {
    name: 'protorig_conn',
    description:
      '让出 / 收回手表的串口与 BLE 连接。action=pause 会关串口、停 BLE 并暂停自动重连'
      + '（**跑 CLI 脚本、ble-e2e、串口助手或自定义工具前必须先调**，否则串口被插件占用或刚让开又被抢回）；'
      + 'seconds 是暂停时长（默认 120 s，5–3600），到期自动恢复，脚本崩了也不会永久挂起。'
      + 'action=resume 立即恢复连接；action=status 查看当前是否暂停/连的哪个口。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        action: { type: 'string', enum: ['status', 'pause', 'resume'] },
        seconds: { type: 'number', description: 'pause 的暂停秒数（默认 120，范围 5–3600）' },
      },
      required: ['action'],
    },
    output: {
      schema: output,
      render(args, value) {
        if (value.action === 'pause') {
          return asText(`protorig_conn pause · 已让出串口/BLE · ${value.seconds}s 后自动恢复`);
        }
        if (value.action === 'resume') {
          return asText(value.ok
            ? `protorig_conn resume OK · connected=${value.connected}${value.port ? ` · ${value.port}` : ''}`
            : `protorig_conn resume FAILED · ${value.error }`);
        }
        return asText(
          `protorig_conn status · paused=${value.paused} connected=${value.connected}`
          + ` · ble=${value.ble}${value.port ? ` · ${value.port}` : ''}`,
        );
      },
    },
    async execute(args) {
      const action = String(args?.action ?? 'status');
      if (!panel || typeof panel.stateForApi !== 'function') {
        return { ok: false, action, error: '面板服务不可用（插件未正确加载）', paused: false, connected: false, ble: false, port: '', device: '', seconds: 0 };
      }
      if (action === 'pause') {
        if (typeof panel.pauseForScript !== 'function') {
          return { ...snapshot(action), ok: false, error: '当前插件版本不支持 pause' };
        }
        const result = await panel.pauseForScript(args?.seconds ?? 120).catch((error) => ({ error: error.message }));
        if (result?.error) return { ...snapshot(action), ok: false, error: String(result.error) };
        return snapshot(action, { paused: true, connected: false, ble: false, seconds: Number(result?.seconds ?? 0) });
      }
      if (action === 'resume') {
        const result = await panel.reconnectNow().catch((error) => ({ opened: false, error: error.message }));
        const view = snapshot(action);
        return {
          ...view,
          ok: result?.opened === true,
          paused: false,
          error: result?.opened === true ? '' : String(result?.error ?? '没找到手表：插好线 / 进电脑模式后重试'),
        };
      }
      return snapshot(action);
    },
  };
}

/** 注册到 DSH tools 服务；没有 tools 服务的 profile 里自动跳过。 */
export function registerTools(ctx, { panel, flash, logger }) {
  if (typeof ctx?.inject !== 'function') return;
  ctx.inject(['tools'], (sctx) => {
    const disposers = [];
    try {
      disposers.push(sctx.tools.register(flashTool({ flash })));
      disposers.push(sctx.tools.register(faceTool({ panel })));
      disposers.push(sctx.tools.register(panelTool({ panel })));
      disposers.push(sctx.tools.register(connTool({ panel })));
      disposers.push(sctx.tools.register(stateTool({ panel })));
      disposers.push(sctx.tools.register(notifyTool({ panel })));
      disposers.push(sctx.tools.register(vibrateTool({ panel })));
      disposers.push(sctx.tools.register(screenshotTool({ panel })));
      logger.info('agent 工具已注册：protorig_flash / protorig_face / protorig_panel / protorig_conn / protorig_state / protorig_notify / protorig_vibrate / protorig_screenshot');
    } catch (error) {
      logger.warn(`agent 工具注册失败：${error.message}`);
    }
    return () => {
      for (const dispose of disposers) {
        try {
          dispose();
        } catch {
          /* ignore */
        }
      }
    };
  });
}
