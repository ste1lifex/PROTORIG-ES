// Browser half of dsh-protorig-panel: one PROTORIG section in DSH Settings.
//
// Loaded by DSH's client module loader; `require` supplies React and the other
// client dependencies. The Host half (src/index.mjs) owns the hardware and the
// HTTP routes under /api/dsh-protorig-panel/*.
//
// 信息架构（2026-11 设置页整理，用户要求"拨动开关 / 名称简洁 / 字号统一 / 按常用度排"）：
//   概览 → 显示 → 提醒与审批 → 表盘 → 连接 → 设备 → 高级
// 原则：
//   · 布尔项一律用手机式拨动开关（role="switch"），不再用 checkbox；
//   · 字号只有四档：17（页面标题）/ 14（分组标题）/ 13（正文与控件）/ 12（说明与状态）；
//   · 常用的显示、提醒放前面，链路/烧录等放进默认折叠的「高级」；
//   · USB / 蓝牙两套显示参数用同一个分组里的「USB | 蓝牙」切换，不再并排两个重复分组。
// 设置字段（config 路径）没有变化：仍然写 usb.* / ble.* / approval.* 等原字段。
window.__ModuleLoader__.load({
  id: 'dsh-protorig-panel',
  factory: (require) => {
    const React = require('react');
    const h = React.createElement;
    const NS = 'protorigPanel.settings';

    const zh = {
      nav: 'PROTORIG_ES',
      title: 'PROTORIG_ES 面板',
      intro: '把手表当 DSH 副屏；常用设置都在这里。',
      loadingSettings: '正在读取设置…',
      notLoaded: '设置暂不可用',
      reloadSettings: '刷新页面',
      persistFailed: '保存失败',
      saved: '已保存',
      saving: '保存中…',
      failed: '操作失败',
      save: '保存',
      reload: '还原',
      unsaved: '有未保存的修改',
      autoApplyNote: '显示参数即时生效，其余设置保存后生效。',
      seconds: ' 秒',

      /* 概览 */
      overview: '概览',
      statusUsb: 'USB 已连接',
      statusBle: '蓝牙连接',
      statusOffline: '未连接',
      needWatch: '等待手表连接…',
      deviceId: '设备',
      deviceIdUnknown: '未识别',
      batteryLabel: '电量',
      protocolLabel: '固件',
      pcModeLabel: '电脑模式',
      pcModeOn: '已进入',
      pcModeOff: '未进入',
      actionReconnect: '重连',
      actionDisconnect: '断开',
      actionIdentify: '震动识别',
      disconnected: '已断开（暂停重连）',
      identified: '手表已震动',
      boundTo: '绑定',
      bindMismatch: '当前连接的不是绑定的那块表',

      /* 设备 */
      devices: '设备',
      devicesHint: '绑定后 USB/蓝牙只连这块表；换表会自动重发亮度并整屏重绘。',
      unbound: '未绑定（自动选第一块）',
      unbind: '解绑',
      bind: '绑定',
      bound: '已绑定',
      connectedNow: '已连接',
      scanning: '扫描中…',
      refreshDevices: '刷新列表',
      deviceNone: '没发现设备：检查线缆/电量后再刷新。',
      deviceScannedAt: '上次扫描',
      deviceAddHint: '同一台电脑插多块表时，点「刷新列表」可看到各自编号。',
      knownDevices: '已登记设备',
      knownDevicesHint: '插过一次或广播过就会自动登记。',
      candidatesTitle: '当前串口（未登记）',
      noKnownDevices: '还没有登记过设备：插上 USB 或让手表广播一次。',
      bindDone: '已绑定',
      unbindDone: '已解绑',

      /* 表盘 */
      faces: '表盘',
      facesCurrent: '当前表盘',
      faceDefault: '默认',
      faceImage: '图片',
      facesImageStatus: '图片表盘',
      facesInstalled: '已安装',
      facesNotInstalled: '未安装',
      facesNeedUsb: '表盘写入需要 USB 连接（蓝牙不支持）。',
      facesWatchMode: '当前是手表模式（电脑副屏已暂停）。',
      facesBackToPc: '回到电脑模式',
      facesDoubleClickHint: '单击切换，双击看大图',
      facesPreview: '预览',
      facesClose: '关闭',
      facesNoThumb: '暂无缩略图（在本机装过图后自动缓存）',
      facesNoThumbShort: '无缩略图',
      facesBuiltinPreview: '内置表盘由手表渲染，电脑侧没有图可预览。',
      facesImageTitle: '安装图片表盘',
      facesImageHint: '图片会裁成 480×480、量化 256 色后写入（USB，约 15–40 秒），装完自动切换。',
      facesPickImage: '选择图片',
      facesLayout: '时钟布局',
      facesTime: '时间位置',
      facesFont: '字体',
      facesFlags: '叠层',
      facesShowDate: '日期',
      facesShowBatt: '电量',
      facesColor: '文字颜色',
      facesInstall: '安装并切换',
      facesInstalling: '安装中',
      facesDone: '安装完成，已切到图片表盘。',
      facesFailed: '图片表盘安装失败',
      facesPanelTitle: '默认表盘下半区',

      /* 默认表盘下半区 */
      facePanelStatusInstalled: '已安装',
      facePanelStatusBuiltin: '未安装（内置图）',
      facePanelHint: '默认表盘的下半区可换成自己的图：裁成 480×232、量化 256 色，约 110 KB 经 USB 写入；上半区时间/表针照常实时。',
      facePanelPick: '选择图片',
      facePanelZoom: '缩放',
      facePanelReset: '重置',
      facePanelCropHint: '拖动调整位置，滑杆缩放；边缘自动羽化后与表盘融合。',
      facePanelInstall: '安装',
      facePanelInstalling: '写入中…',
      facePanelDone: '下半区图已安装。',
      facePanelFailed: '下半区安装失败',
      installWarning: '注意',
      facePanelClear: '恢复内置图',
      facePanelCleared: '已恢复内置图',
      facePanelNeedUsb: '下半区写入需要 USB 连接。',
      facePanelNoThumb: '暂无预览（在本机装过图后自动缓存）',
      facePanelCrc: 'CRC',
      facePanelSize: '尺寸',
      facePanelPreviewAlt: '下半区预览',
      facePanelBusy: '另一项表盘写入进行中，稍后再试。',

      /* 显示 */
      display: '显示',
      displayHint: 'USB 与蓝牙各自一套参数，进入电脑模式后生效。',
      channelUsb: 'USB',
      channelBle: '蓝牙',
      brightness: '亮度',
      alwaysOn: '常亮',
      screenOff: '息屏',
      screenOffHint: '常亮关闭时生效；“跟随手表”不覆盖手表自身的息屏设置',
      screenOffDefault: '跟随手表（默认 15 s）',
      footerInfo: '底部信息',
      balanceHint: '读取 DSH 登录账号的余额；未登录时回退 DEEPSEEK_API_KEY。',
      balanceEnabled: '显示余额',
      balanceInterval: '刷新间隔',

      /* 连接 */
      connection: '连接',
      connectionHint: '关闭后不再主动连接；探测只登记编号，不改变当前连接。',
      usbSwitch: 'USB 连接',
      bleSwitch: '蓝牙连接',
      probeTitle: '设备探测',
      probeUsb: '探测 USB',
      probeBle: '探测蓝牙',
      probeUsbDone: 'USB 探测完成，已登记',
      probeBleDone: '蓝牙探测完成，已登记',

      /* 提醒与审批 */
      interaction: '提醒与审批',
      interactionHint: '完成/需确认/失败三类事件；审批与问答超时后交回电脑。',
      reminder: '事件提醒',
      reminderHow: '提醒方式',
      eventDone: '完成',
      eventNeeds: '需要确认',
      eventFailed: '失败',
      vibrate: '震动',
      bright: '亮度脉冲',
      vibratePreset: '振动模式',
      presetShort: '短',
      presetMedium: '中',
      presetLong: '长',
      presetCustom: '自定义',
      vibrateMs: '自定义时长',
      approval: '审批与问答',
      approvalEnabled: '手表优先审批',
      approvalTimeout: '移交超时',
      questionsEnabled: '手表问答',
      questionsTimeout: '独占超时',
      queueWait: '排队上限',
      queueWaitHint: '审批/问答同时到达时，后一张最多等这么久（10–60 秒，超时交回电脑）',

      /* 高级 */
      advanced: '高级',
      advancedHint: '链路参数、调试、烧录与版本。',
      link: '链路',
      linkHint: '串口留空 = 自动发现（VID 349B / MI_00，macOS /dev/cu.usbmodem*）。',
      fps: '刷新率',
      port: '串口',
      portPlaceholder: '留空自动发现（COM8 / /dev/cu.usbmodem14201）',
      autoEnter: '插线自动进入电脑模式',
      maintenance: '维护',
      resetCache: '重置下发缓存',
      resetCacheHint: '清掉已下发缓存，下个 tick 重发亮度/常亮/整屏。',
      resetCacheDone: '缓存已重置',
      flash: '烧录',
      flashHint: '调 flash.ps1：reboot 进下载模式 → 写入 → 校验；完成后拨 SW2 ON→OFF→ON 重启。',
      flashHintMac: '本机调 BLFlashCommand-macos：reboot → 等下载口 → 写入校验；完成后拨 SW2 ON→OFF→ON 重启。',
      flashUnsupported: '烧录仅支持 Windows / macOS。',
      flashBin: '固件 bin',
      flashBinPlaceholder: 'firmware/lvgl_test/build/build_out/protorig_m2_bl616.bin',
      flashBaud: '波特率',
      flashPreflight: '预检',
      flashStart: '开始烧录',
      flashCancel: '取消',
      flashExample: '填示例',
      flashPreflightOk: '预检通过',
      flashRunning: '烧录中…',
      flashDone: '烧录成功。请手动拨动开关重启（SW2 ON→OFF→ON）。',
      flashFailed: '烧录失败',
      flashStage: '阶段',
      flashStageIdle: '空闲',
      flashStageStarting: '准备',
      flashStageReboot: 'reboot uart',
      flashStageWaitRom: '等待下载口',
      flashStageFlash: '写入',
      flashStageVerify: '校验',
      flashStageDone: '完成',
      flashStageError: '失败',
      flashLog: '最近输出',
      size: '大小',
      sha: 'SHA256',
      stage: '阶段',
      about: '关于',
      aboutText: '固件需提供 admin_id（广播名后缀 / adminid / GET_INFO 末尾 4 B）。',
    };
    const en = {
      nav: 'PROTORIG_ES',
      title: 'PROTORIG_ES panel',
      intro: 'Use the watch as a DSH side screen. The common settings live here.',
      loadingSettings: 'Loading settings…',
      notLoaded: 'Settings unavailable',
      reloadSettings: 'Reload page',
      persistFailed: 'Save failed',
      saved: 'Saved',
      saving: 'Saving…',
      failed: 'Failed',
      save: 'Save',
      reload: 'Revert',
      unsaved: 'Unsaved changes',
      autoApplyNote: 'Display values apply instantly; other settings apply on save.',
      seconds: ' s',

      overview: 'Overview',
      statusUsb: 'USB connected',
      statusBle: 'BLE connected',
      statusOffline: 'Not connected',
      needWatch: 'Waiting for the watch…',
      deviceId: 'Device',
      deviceIdUnknown: 'unknown',
      batteryLabel: 'Battery',
      protocolLabel: 'Firmware',
      pcModeLabel: 'PC mode',
      pcModeOn: 'on',
      pcModeOff: 'off',
      actionReconnect: 'Reconnect',
      actionDisconnect: 'Disconnect',
      actionIdentify: 'Vibrate to identify',
      disconnected: 'Disconnected (auto-reconnect paused)',
      identified: 'Watch vibrated',
      boundTo: 'Bound',
      bindMismatch: 'The connected watch is not the bound one',

      devices: 'Devices',
      devicesHint: 'When bound, USB and BLE both use only that watch. Swapping re-sends brightness and repaints the whole screen.',
      unbound: 'Not bound (first device wins)',
      unbind: 'Unbind',
      bind: 'Bind',
      bound: 'Bound',
      connectedNow: 'connected',
      scanning: 'Scanning…',
      refreshDevices: 'Refresh',
      deviceNone: 'No device found: check the cable/power, then refresh.',
      deviceScannedAt: 'Last scan',
      deviceAddHint: 'With several watches on one computer, "Refresh" shows each ID.',
      knownDevices: 'Known devices',
      knownDevicesHint: 'Registered automatically after a USB plug-in or one BLE advertisement.',
      candidatesTitle: 'Ports seen now (not registered)',
      noKnownDevices: 'No device registered yet: plug in USB or let the watch advertise once.',
      bindDone: 'Bound',
      unbindDone: 'Unbound',

      faces: 'Watch faces',
      facesCurrent: 'Current face',
      faceDefault: 'Default',
      faceImage: 'Image',
      facesImageStatus: 'Image face',
      facesInstalled: 'installed',
      facesNotInstalled: 'not installed',
      facesNeedUsb: 'Face writes need a USB connection (BLE is not supported).',
      facesWatchMode: 'Watch mode (the PC second screen is paused).',
      facesBackToPc: 'Back to PC mode',
      facesDoubleClickHint: 'Click to switch, double-click to preview',
      facesPreview: 'Preview',
      facesClose: 'Close',
      facesNoThumb: 'No thumbnail yet (cached after installing here)',
      facesNoThumbShort: 'No thumbnail',
      facesBuiltinPreview: 'The built-in face is rendered on the watch; there is no local image.',
      facesImageTitle: 'Install image face',
      facesImageHint: 'The image is cropped to 480×480, quantized to 256 colors and written over USB (~15–40 s). The watch switches when done.',
      facesPickImage: 'Choose image',
      facesLayout: 'Clock layout',
      facesTime: 'Time position',
      facesFont: 'Font',
      facesFlags: 'Overlay',
      facesShowDate: 'date',
      facesShowBatt: 'battery',
      facesColor: 'Text color',
      facesInstall: 'Install and switch',
      facesInstalling: 'Installing',
      facesDone: 'Installed and switched to the image face.',
      facesFailed: 'Image face install failed',
      facesPanelTitle: 'Lower half of the default face',

      facePanelStatusInstalled: 'installed',
      facePanelStatusBuiltin: 'not installed (built-in image)',
      facePanelHint: 'The lower half of the default face can show your own image: cropped to 480×232, quantized to 256 colors, ~110 KB written over USB. The upper half (time/hands) stays live.',
      facePanelPick: 'Choose image',
      facePanelZoom: 'Zoom',
      facePanelReset: 'Reset',
      facePanelCropHint: 'Drag to position, slider to zoom; edges are feathered to blend with the face.',
      facePanelInstall: 'Install',
      facePanelInstalling: 'Writing…',
      facePanelDone: 'Lower-half image installed.',
      facePanelFailed: 'Lower-half install failed',
      installWarning: 'Note',
      facePanelClear: 'Restore built-in',
      facePanelCleared: 'Built-in image restored',
      facePanelNeedUsb: 'Writing the lower half needs a USB connection.',
      facePanelNoThumb: 'No preview yet (cached after installing here)',
      facePanelCrc: 'CRC',
      facePanelSize: 'Size',
      facePanelPreviewAlt: 'Lower-half preview',
      facePanelBusy: 'Another face write is in progress, try again shortly.',

      display: 'Display',
      displayHint: 'USB and BLE have separate values; applied in PC mode.',
      channelUsb: 'USB',
      channelBle: 'BLE',
      brightness: 'Brightness',
      alwaysOn: 'Always on',
      screenOff: 'Screen off',
      screenOffHint: 'Applies when always-on is off; "Watch setting" keeps the watch\'s own timeout',
      screenOffDefault: 'Watch setting (15 s default)',
      footerInfo: 'Footer',
      balanceHint: 'Reads the signed-in DSH account balance; falls back to DEEPSEEK_API_KEY.',
      balanceEnabled: 'Show balance',
      balanceInterval: 'Refresh interval',

      connection: 'Connection',
      connectionHint: 'When off, the plugin stops connecting. Probing only registers IDs.',
      usbSwitch: 'USB connection',
      bleSwitch: 'BLE connection',
      probeTitle: 'Device probe',
      probeUsb: 'Probe USB',
      probeBle: 'Probe BLE',
      probeUsbDone: 'USB probe done, registered',
      probeBleDone: 'BLE probe done, registered',

      interaction: 'Reminders & approvals',
      interactionHint: 'Done / needs-you / failed events; approvals and questions hand off to the computer on timeout.',
      reminder: 'Event reminders',
      reminderHow: 'How to remind',
      eventDone: 'Done',
      eventNeeds: 'Needs you',
      eventFailed: 'Failed',
      vibrate: 'Vibration',
      bright: 'Brightness pulse',
      vibratePreset: 'Vibration pattern',
      presetShort: 'Short',
      presetMedium: 'Medium',
      presetLong: 'Long',
      presetCustom: 'Custom',
      vibrateMs: 'Custom duration',
      approval: 'Approvals & questions',
      approvalEnabled: 'Watch-first approvals',
      approvalTimeout: 'Hand-off timeout',
      questionsEnabled: 'Watch questions',
      questionsTimeout: 'Exclusive timeout',
      queueWait: 'Queue limit',
      queueWaitHint: 'When approvals/questions arrive together, how long the next card may wait (10–60 s, then hand to the computer)',

      advanced: 'Advanced',
      advancedHint: 'Link options, diagnostics, flashing and version.',
      link: 'Link',
      linkHint: 'Leave the port empty to auto-discover (Windows VID 349B / MI_00; macOS /dev/cu.usbmodem*).',
      fps: 'Refresh rate',
      port: 'Serial port',
      portPlaceholder: 'auto (e.g. COM8 / /dev/cu.usbmodem14201)',
      autoEnter: 'Enter PC mode when plugged in',
      maintenance: 'Maintenance',
      resetCache: 'Reset delivery cache',
      resetCacheHint: 'Clears the applied-value cache so brightness/always-on/full frame are re-sent next tick.',
      resetCacheDone: 'Cache reset',
      flash: 'Flashing',
      flashHint: 'Runs flash.ps1: reboot into download mode → write → verify. Power-cycle afterwards.',
      flashHintMac: 'Runs BLFlashCommand-macos locally: reboot → wait for the ROM port → write and verify. Power-cycle afterwards.',
      flashUnsupported: 'Flashing is available on Windows / macOS only.',
      flashBin: 'Firmware bin',
      flashBinPlaceholder: 'firmware/lvgl_test/build/build_out/protorig_m2_bl616.bin',
      flashBaud: 'Baud rate',
      flashPreflight: 'Preflight',
      flashStart: 'Flash',
      flashCancel: 'Cancel',
      flashExample: 'Fill example',
      flashPreflightOk: 'Preflight OK',
      flashRunning: 'Flashing…',
      flashDone: 'Flash OK. Toggle SW2 ON→OFF→ON to reboot (unplugging USB won\'t power-cycle a battery board).',
      flashFailed: 'Flash failed',
      flashStage: 'Stage',
      flashStageIdle: 'idle',
      flashStageStarting: 'starting',
      flashStageReboot: 'reboot uart',
      flashStageWaitRom: 'waiting for ROM port',
      flashStageFlash: 'writing',
      flashStageVerify: 'verify',
      flashStageDone: 'done',
      flashStageError: 'error',
      flashLog: 'Recent output',
      size: 'Size',
      sha: 'SHA256',
      stage: 'Stage',
      about: 'About',
      aboutText: 'The firmware must provide admin_id (adv-name suffix / adminid / GET_INFO tail 4 B).',
    };
    const dictionaries = { zh, en };

    const VERSION = '0.4.17'; // 与 package.json 保持一致（客户端读不到包元数据）
    /* 示例 bin：相对仓库根，不暴露本机路径。macOS 的产物路径与 Windows 不同。 */
    const EXAMPLE_BIN_WIN = 'firmware/lvgl_test/build/build_out/protorig_m2_bl616.bin';
    const EXAMPLE_BIN_MAC = 'firmware/lvgl_test/build/build_out/protorig_m2_bl616.bin';
    const DEFAULTS = {
      port: '', deviceId: '', fps: 2, autoEnter: true, queueWaitSec: 60,
      reminder: { done: true, needs: true, failed: true, vibrate: true, bright: true, vibratePreset: 'medium', vibrateMs: 180 },
      display: { brightness: 201, alwaysOn: true },
      usb: { enabled: true, brightness: 201, alwaysOn: true, screenOffSec: 0 },
      approval: { enabled: true, watchTimeoutMs: 10000 },
      questions: { enabled: true, watchTimeoutMs: 30000 },
      ble: { enabled: true, brightness: 201, alwaysOn: true, screenOffSec: 0 },
      balance: { enabled: true, intervalSec: 60 },
      flash: { bin: '', baud: 2000000 },
    };
    const clone = (value) => JSON.parse(JSON.stringify(value));
    const equal = (a, b) => JSON.stringify(a) === JSON.stringify(b);
    const readPath = (value, path) => path.reduce((current, key) => (current == null ? undefined : current[key]), value);
    const writePath = (target, path, value) => {
      let current = target;
      for (let i = 0; i < path.length - 1; i += 1) {
        if (current[path[i]] == null || typeof current[path[i]] !== 'object') current[path[i]] = {};
        current = current[path[i]];
      }
      current[path[path.length - 1]] = value;
    };
    /** Merge accepted config over defaults (nested groups shallow-merged). */
    const resolve = (value) => {
      const out = clone(DEFAULTS);
      if (!value || typeof value !== 'object') return out;
      for (const [key, fallback] of Object.entries(DEFAULTS)) {
        if (value[key] === undefined) continue;
        out[key] = fallback && typeof fallback === 'object' && !Array.isArray(fallback) && value[key] && typeof value[key] === 'object'
          ? { ...fallback, ...value[key] }
          : value[key];
      }
      // legacy showBalance → balance.enabled
      if (value.balance?.enabled === undefined && value.showBalance === true) out.balance.enabled = true;
      return out;
    };

    /** 与 Host 的 device-id.mjs 同规则：6 位大写十六进制，否则视为未绑定。 */
    const normalizeId = (value) => {
      const text = String(value ?? '').trim().replace(/^0x/i, '').toUpperCase();
      return /^[0-9A-F]{6}$/.test(text) ? text : '';
    };
    /** 设备标签（Host 端 deviceLabel 的浏览器版，只用于展示）。 */
    const deviceText = (id) => (id ? `PROTORIG ES · ${id}` : 'PROTORIG ES');
    const timeText = (at) => {
      if (!at) return '—';
      try {
        return new Date(at).toLocaleTimeString();
      } catch {
        return '—';
      }
    };

    /* -------------------------------------------------------- 设计变量 */
    /*
     * 只保留一个字号表（17/14/13/12）和一套颜色变量；同级元素共用常量，
     * 不再出现 12.5 / 13.5 / 15 混排。
     */
    const C = {
      primary: 'var(--dsw-alias-label-primary, #1f2328)',
      secondary: 'var(--dsw-alias-label-secondary, #61666b)',
      border: 'var(--dsw-alias-border-l2, rgba(128,128,128,0.28))',
      borderStrong: 'var(--dsw-alias-border-l3, rgba(128,128,128,0.35))',
      layer1: 'var(--dsw-alias-bg-layer-1, Canvas)',
      layer2: 'var(--dsw-alias-bg-layer-2, transparent)',
      module: 'var(--dsw-alias-bg-module-platform, rgba(128,128,128,0.08))',
      error: 'var(--dsw-alias-state-error-primary, #d14343)',
      success: 'var(--dsw-alias-state-success-primary, #2f9e44)',
      onBrand: 'var(--dsw-alias-label-primary-foreground, #fff)',
    };
    const FONT = '-apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC", "Microsoft YaHei", system-ui, sans-serif';
    /** 选中/滑杆强调色：不跟随 --dsw-alias-brand-primary（深色主题里它是白色，会白底白字）。 */
    const ACCENT = '#3b82f6';

    const styles = {
      root: {
        width: '100%', maxWidth: 820, boxSizing: 'border-box',
        fontFamily: FONT, fontSize: 13, lineHeight: 1.5, color: C.primary,
        paddingBottom: 4,
      },
      header: { margin: '0 0 14px' },
      pageTitle: { fontSize: 17, fontWeight: 600, lineHeight: 1.4, margin: 0 },
      intro: { fontSize: 12, color: C.secondary, margin: '4px 0 0' },

      /* 分组 */
      section: {
        border: `1px solid ${C.border}`, borderRadius: 12, marginBottom: 10,
        background: C.layer2, overflow: 'hidden',
      },
      groupHeader: {
        display: 'flex', alignItems: 'center', gap: 8, padding: '10px 14px',
        cursor: 'pointer', userSelect: 'none',
      },
      caret: {
        display: 'inline-block', fontSize: 10, lineHeight: 1, width: 10,
        color: C.secondary, transition: 'transform .15s ease', flex: '0 0 auto',
      },
      sectionTitle: { fontSize: 14, fontWeight: 600, margin: 0 },
      groupRight: {
        marginLeft: 'auto', fontSize: 12, color: C.secondary,
        whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', minWidth: 0,
      },
      groupBody: { padding: '0 14px 12px' },
      hint: { fontSize: 12, color: C.secondary, margin: '0 0 8px' },
      note: { fontSize: 12, color: C.secondary, margin: '4px 0 0' },
      warn: { fontSize: 12, color: C.error, margin: '4px 0 0' },

      /* 子分区（图片表盘 / 下半区） */
      sub: { marginTop: 12, borderTop: `1px solid ${C.border}`, paddingTop: 4 },
      subHeader: {
        display: 'flex', alignItems: 'center', gap: 6, padding: '4px 0',
        cursor: 'pointer', userSelect: 'none',
      },
      subCaret: {
        display: 'inline-block', fontSize: 9, lineHeight: 1, width: 9,
        color: C.secondary, transition: 'transform .15s ease', flex: '0 0 auto',
      },
      subTitle: { fontSize: 13, fontWeight: 600 },
      subBody: { padding: '2px 0' },
      subHeadRow: { display: 'flex', alignItems: 'baseline', gap: 8, margin: '12px 0 2px', flexWrap: 'wrap' },
      subNote: { fontSize: 12, color: C.secondary },
      divider: { height: 1, background: C.border, margin: '10px 0 2px' },

      /* 行 */
      row: {
        display: 'flex', alignItems: 'center', justifyContent: 'space-between',
        gap: 12, padding: '6px 0', minHeight: 32, flexWrap: 'wrap',
      },
      rowLabel: { display: 'flex', flexDirection: 'column', gap: 1, minWidth: 0, flex: '1 1 auto' },
      label: { fontSize: 13 },
      inlineHint: { fontSize: 12, color: C.secondary, lineHeight: 1.45 },
      control: {
        display: 'flex', alignItems: 'center', justifyContent: 'flex-end',
        gap: 8, flexWrap: 'wrap', flex: '0 0 auto', marginLeft: 'auto',
      },
      inlineControl: { display: 'inline-flex', alignItems: 'center', gap: 6, fontSize: 13 },

      /* 控件 */
      input: {
        padding: '4px 8px', borderRadius: 6, border: `1px solid ${C.borderStrong}`,
        background: C.layer1, color: 'inherit', fontFamily: 'inherit', fontSize: 13,
        lineHeight: '20px', boxSizing: 'border-box',
      },
      range: { width: 220, accentColor: ACCENT, margin: 0 },
      button: {
        padding: '5px 12px', borderRadius: 7, border: `1px solid ${C.borderStrong}`,
        background: C.layer1, color: 'inherit', fontFamily: 'inherit', fontSize: 13,
        lineHeight: '20px', cursor: 'pointer', boxSizing: 'border-box',
      },
      primary: {
        padding: '5px 14px', borderRadius: 7, border: 'none',
        background: 'var(--dsw-alias-button-primary-fill, #0f1115)', color: C.onBrand,
        fontFamily: 'inherit', fontSize: 13, lineHeight: '20px', cursor: 'pointer', boxSizing: 'border-box',
      },
      toggle: {
        position: 'relative', width: 44, height: 26, padding: 0, borderRadius: 999, border: 'none',
        display: 'inline-flex', alignItems: 'center', justifyContent: 'flex-start',
        transition: 'background-color .18s ease, box-shadow .18s ease', boxSizing: 'border-box',
      },
      knob: {
        display: 'block', width: 22, height: 22, marginLeft: 2, borderRadius: '50%', background: '#fff',
        boxShadow: '0 1px 3px rgba(0,0,0,0.32)',
        transition: 'transform .18s cubic-bezier(.2,.8,.2,1)',
      },
      segmented: {
        display: 'inline-flex', gap: 2, padding: 2, borderRadius: 8, boxSizing: 'border-box',
        background: C.module, border: `1px solid ${C.border}`,
      },
      segmentedButton: {
        border: 'none', borderRadius: 6, padding: '3px 14px',
        background: 'transparent', color: C.secondary, cursor: 'pointer',
        fontFamily: 'inherit', fontSize: 13, lineHeight: '18px',
      },
      segmentedActive: { background: '#2563eb', color: '#fff', fontWeight: 600 },

      /* 状态 */
      statusLine: { display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap', margin: '0 0 2px' },
      statusText: { fontSize: 13, fontWeight: 600 },
      statusDot: (on, warn) => ({
        width: 8, height: 8, borderRadius: 999, display: 'inline-block', flex: '0 0 auto',
        background: on ? C.success : warn ? C.error : 'var(--dsw-alias-border-l3, rgba(128,128,128,0.5))',
      }),
      metaLine: {
        fontSize: 12, color: C.secondary, margin: '2px 0 0',
        display: 'flex', flexWrap: 'wrap', gap: 6, alignItems: 'baseline',
      },
      actions: { display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap', marginTop: 8 },
      message: { fontSize: 12 },

      /* 设备 / 表盘 */
      deviceRow: {
        display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 10,
        padding: '6px 10px', borderRadius: 8, margin: '4px 0', background: C.module,
      },
      deviceName: { fontFamily: 'ui-monospace, Consolas, monospace', fontSize: 12 },
      badge: {
        display: 'inline-block', padding: '0 8px', borderRadius: 999, fontSize: 12,
        border: `1px solid ${C.border}`, color: C.secondary,
      },
      faceGrid: {
        display: 'grid', gridTemplateColumns: 'repeat(2, minmax(0, 150px))',
        gap: 10, margin: '10px 0 8px',
      },
      faceTile: (active) => ({
        display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6, padding: 6,
        borderRadius: 12, cursor: 'pointer', boxSizing: 'border-box',
        background: active ? 'rgba(59, 130, 246, 0.12)' : 'transparent',
        border: `1px solid ${active ? ACCENT : C.border}`,
      }),
      faceThumb: {
        width: '100%', aspectRatio: '1 / 1', borderRadius: 10, overflow: 'hidden',
        background: '#0f141b', display: 'flex', alignItems: 'center', justifyContent: 'center',
      },
      faceLabel: { fontSize: 13 },
      previewBox: { border: `1px solid ${C.border}`, borderRadius: 12, padding: 10, margin: '0 0 12px' },

      /* 进度 */
      progress: {
        height: 6, marginTop: 8, borderRadius: 3, overflow: 'hidden',
        background: 'rgba(128,128,128,0.25)',
      },
      progressFill: { height: 6, background: C.success, borderRadius: 3, transition: 'width .2s ease' },

      /* 预览 / 日志 */
      panelPreview: { margin: '8px 0 4px' },
      panelPlaceholder: {
        width: 280, aspectRatio: '480 / 232', display: 'flex', alignItems: 'center',
        justifyContent: 'center', borderRadius: 10, background: '#0f141b', color: '#9aa4b2',
        fontSize: 12, textAlign: 'center', padding: 10, boxSizing: 'border-box',
      },
      cropCanvas: {
        width: 360, height: 174, borderRadius: 10, background: '#000',
        cursor: 'grab', touchAction: 'none', display: 'block',
      },
      log: {
        margin: '8px 0 0', padding: '8px 10px', borderRadius: 8, maxHeight: 150, overflow: 'auto',
        background: C.module, fontFamily: 'ui-monospace, Consolas, monospace',
        fontSize: 12, lineHeight: 1.5, whiteSpace: 'pre-wrap', wordBreak: 'break-all',
      },

      /* 保存条 */
      saveBar: {
        position: 'sticky', bottom: 0, zIndex: 5,
        display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap',
        marginTop: 10, padding: '10px 0 8px',
        borderTop: `1px solid ${C.border}`, background: C.layer1,
      },
      saveState: { display: 'flex', flexDirection: 'column', gap: 2, minWidth: 0 },
      saveHint: { fontSize: 12, color: C.secondary },
      saveActions: { marginLeft: 'auto', display: 'flex', gap: 8 },
    };

    /* -------------------------------------------------------- 基础组件 */

    const Row = ({ label, hint, children }) => h('div', { style: styles.row },
      h('div', { style: styles.rowLabel },
        h('span', { style: styles.label }, label),
        hint ? h('span', { style: styles.inlineHint }, hint) : null),
      h('div', { style: styles.control }, children));

    /** 可折叠分组：默认展开/折叠由调用方给，用户点了才变。 */
    const Group = ({ title, hint, right, open, onToggle, children }) => h('section', { style: styles.section },
      h('div', {
        style: styles.groupHeader,
        role: 'button',
        tabIndex: 0,
        'aria-expanded': !!open,
        onClick: onToggle,
        onKeyDown: (event) => {
          if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            onToggle?.();
          }
        },
      },
      h('span', { style: { ...styles.caret, transform: open ? 'rotate(90deg)' : 'none' } }, '▶'),
      h('h3', { style: styles.sectionTitle }, title),
      right ? h('span', { style: styles.groupRight }, right) : null),
      open ? h('div', { style: styles.groupBody },
        hint ? h('p', { style: styles.hint }, hint) : null,
        children) : null);

    /** 分组内子分区（表盘里的「图片表盘」「下半区」）：不另起卡片，一条分隔线。 */
    const Sub = ({ title, hint, open, onToggle, children }) => h('div', { style: styles.sub },
      h('div', {
        style: styles.subHeader,
        role: 'button',
        tabIndex: 0,
        'aria-expanded': !!open,
        onClick: onToggle,
        onKeyDown: (event) => {
          if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            onToggle?.();
          }
        },
      },
      h('span', { style: { ...styles.subCaret, transform: open ? 'rotate(90deg)' : 'none' } }, '▶'),
      h('span', { style: styles.subTitle }, title)),
      open ? h('div', { style: styles.subBody },
        hint ? h('p', { style: styles.hint }, hint) : null,
        children) : null);

    const SubHead = ({ title, note }) => h('div', { style: styles.subHeadRow },
      h('span', { style: styles.subTitle }, title),
      note ? h('span', { style: styles.subNote }, note) : null);

    const Divider = () => h('div', { style: styles.divider });

    /** 手机式拨动开关：role=switch，键盘/读屏可用；尺寸 44×26，行程 18 px。 */
    const Toggle = ({ checked, onChange, disabled, label }) => {
      const [focus, setFocus] = React.useState(false);
      const on = !!checked;
      return h('button', {
        type: 'button',
        role: 'switch',
        'aria-checked': on,
        'aria-label': label || undefined,
        disabled: !!disabled,
        onClick: () => { if (!disabled) onChange(!on); },
        onFocus: () => setFocus(true),
        onBlur: () => setFocus(false),
        style: {
          ...styles.toggle,
          background: on ? 'var(--dsw-alias-state-success-primary, #34c759)' : 'var(--dsw-alias-border-l3, rgba(128,128,128,0.45))',
          opacity: disabled ? 0.45 : 1,
          cursor: disabled ? 'not-allowed' : 'pointer',
          boxShadow: focus ? '0 0 0 3px rgba(52, 199, 89, 0.38)' : 'none',
        },
      }, h('span', { style: { ...styles.knob, transform: on ? 'translateX(18px)' : 'translateX(0)' } }));
    };

    const Select = ({ value, options, onChange, disabled }) => h('select', {
      style: { ...styles.input, cursor: disabled ? 'not-allowed' : 'pointer', opacity: disabled ? 0.5 : 1 },
      value: String(value), disabled: !!disabled,
      onChange: (event) => onChange(event.target.value),
    }, options.map(([optionValue, label]) => h('option', { key: String(optionValue), value: String(optionValue) }, label)));

    const Button = ({ children, onClick, style, disabled, title }) => h('button', {
      type: 'button', style: { ...(style === 'primary' ? styles.primary : styles.button), opacity: disabled ? 0.55 : 1 },
      onClick, disabled, title,
    }, children);

    /** USB/蓝牙两个通道的切换（同一组控件，避免重复两遍显示设置）。 */
    const Segmented = ({ value, options, onChange }) => h('div', { style: styles.segmented, role: 'group' },
      options.map(([id, text]) => h('button', {
        key: id,
        type: 'button',
        'aria-pressed': value === id,
        onClick: () => onChange(id),
        style: { ...styles.segmentedButton, ...(value === id ? styles.segmentedActive : null) },
      }, text)));

    const Progress = ({ percent }) => h('div', { style: styles.progress },
      h('div', {
        style: {
          ...styles.progressFill,
          width: `${Math.max(2, Math.min(100, Number(percent) || 0))}%`,
        },
      }));

    const STAGES = {
      idle: 'flashStageIdle', starting: 'flashStageStarting', reboot: 'flashStageReboot',
      'wait-rom': 'flashStageWaitRom', flash: 'flashStageFlash', verify: 'flashStageVerify',
      done: 'flashStageDone', error: 'flashStageError',
    };

    /* --------------------------------------------------------- Host 状态 */

    /**
     * 设置页只读 PanelService 的状态快照（research/73 §8.1：不各自维护副本）。
     * 1.5 s 轮询 /state —— 插线/拔线 1 s 内能反映到概览；Host 不在时静默失败。
     */
    function useHostState(intervalMs = 1500) {
      const [state, setState] = React.useState(null);
      const alive = React.useRef(true);
      const refresh = React.useCallback(async () => {
        try {
          const response = await fetch('/api/dsh-protorig-panel/state', { headers: { accept: 'application/json' } });
          const body = await response.json();
          if (alive.current && body?.ok) setState(body);
          return body;
        } catch {
          return null;
        }
      }, []);
      React.useEffect(() => {
        alive.current = true;
        void refresh();
        const timer = setInterval(() => { void refresh(); }, intervalMs);
        return () => {
          alive.current = false;
          clearInterval(timer);
        };
      }, [refresh, intervalMs]);
      return [state, refresh];
    }

    async function postJson(pathname, body) {
      const response = await fetch(pathname, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(body ?? {}),
      });
      return response.json();
    }

    /* ------------------------------------------------------------ 烧录 */

    function FlashPanel({ t, draft, change, flash, setFlash, platformKind = 'windows' }) {
      const [preflight, setPreflight] = React.useState(null);
      const [busy, setBusy] = React.useState(false);
      const [message, setMessage] = React.useState('');
      const job = flash.job;
      const running = !!job?.running;
      const exampleBin = platformKind === 'macos' ? EXAMPLE_BIN_MAC : EXAMPLE_BIN_WIN;

      React.useEffect(() => {
        let timer;
        const poll = async () => {
          try {
            const response = await fetch('/api/dsh-protorig-panel/flash/state', { headers: { accept: 'application/json' } });
            const body = await response.json();
            if (body?.ok) setFlash({ job: body.job });
          } catch { /* Host still starting */ }
        };
        if (running) {
          timer = setInterval(poll, 800);
          void poll();
        }
        return () => { if (timer) clearInterval(timer); };
      }, [running]);

      const runPreflight = async () => {
        setBusy(true); setMessage('');
        try {
          const body = await postJson('/api/dsh-protorig-panel/preflight', { bin: draft.flash.bin });
          setPreflight(body);
          setMessage(body.ok ? t('flashPreflightOk') : (body.error || t('failed')));
        } catch (error) { setMessage(String(error.message || error)); }
        finally { setBusy(false); }
      };
      const startFlash = async () => {
        setBusy(true); setMessage('');
        try {
          const body = await postJson('/api/dsh-protorig-panel/flash', {
            bin: draft.flash.bin, baud: Number(draft.flash.baud),
          });
          if (!body.ok) { setMessage(body.error || t('flashFailed')); setBusy(false); return; }
          setFlash({ job: { ...(flash.job ?? {}), id: body.jobId, running: true, stage: 'starting' } });
        } catch (error) { setMessage(String(error.message || error)); }
        finally { setBusy(false); }
      };
      const cancelFlash = async () => {
        try {
          await postJson('/api/dsh-protorig-panel/flash/cancel', {});
        } catch { /* ignore */ }
      };
      const stageText = job ? t(STAGES[job.stage] || 'flashStageIdle') : t('flashStageIdle');
      return h('div', null,
        h(Row, { label: t('flashBin') },
          h('input', {
            style: { ...styles.input, width: 340 }, type: 'text', value: draft.flash.bin,
            placeholder: t('flashBinPlaceholder'), onChange: (event) => change(['flash', 'bin'], event.target.value),
          })),
        h('div', { style: styles.actions },
          h(Button, { onClick: () => { change(['flash', 'bin'], exampleBin); setPreflight(null); } }, t('flashExample')),
          h(Button, { onClick: runPreflight, disabled: busy || running }, t('flashPreflight')),
          h('span', { style: styles.subNote }, t('flashBaud')),
          h(Select, {
            value: draft.flash.baud, disabled: busy || running,
            options: [[1000000, '1,000,000'], [2000000, '2,000,000'], [3000000, '3,000,000']],
            onChange: (value) => change(['flash', 'baud'], Number(value)),
          })),
        preflight ? h('p', { style: styles.note },
          `${preflight.display ? `${preflight.display} · ` : ''}${t('size')}: ${preflight.size} B · ${t('sha')}: ${preflight.sha8 || '—'}${preflight.error ? ` · ${preflight.error}` : ''}`) : null,
        h('div', { style: styles.actions },
          h(Button, { style: 'primary', onClick: startFlash, disabled: busy || running }, running ? t('flashRunning') : t('flashStart')),
          running ? h(Button, { onClick: cancelFlash }, t('flashCancel')) : null,
          h('span', { style: styles.message }, message || (job && !running && job.ok ? t('flashDone') : '')),
          job ? h('span', { style: styles.badge }, `${t('stage')}: ${stageText}${job.seconds ? ` · ${job.seconds}${t('seconds')}` : ''}`) : null),
        job?.error ? h('p', { style: styles.warn }, `${t('flashFailed')}: ${job.error}`) : null,
        job?.logTail
          ? h('div', null,
            h('p', { style: styles.note }, t('flashLog')),
            h('pre', { style: styles.log }, job.logTail))
          : null,
      );
    }

    /* ------------------------------------------------ 表盘（P8/P9 + 下半区面板） */

    /**
     * 「默认表盘下半区」（research/76 §2.3）：选图 → cover 480×232 → 256 色 → USB
     * `facepanel begin/w/end`（112,384 B I8）；`facepanel clear` 恢复内置 watch photo。
     *
     * 状态读 `/facepanel`：只在打开分组 / 安装结束 / 动作后拉取 —— 这条路由会发 shell 命令，
     * 不能像 /state 那样每 1.5 s 轮询。安装中只轮询 `/facepanel/install/state`（纯内存）。
     * 预览复用图片表盘那套本地缓存思路（`<DATA_DIR>/faces/panel-<CRC>.png`）。
     */
    function PanelSection({ t, connected }) {
      const [info, setInfo] = React.useState(null);
      const [job, setJob] = React.useState(null);
      const [busy, setBusy] = React.useState(false);
      const [message, setMessage] = React.useState('');
      const [thumb, setThumb] = React.useState(null);       // 已安装面板的本地预览（dataURL）
      const [builtinPanel, setBuiltinPanel] = React.useState(null); // 内置 watch photo 预览
      /* 裁剪器：上传头像式交互 —— 480×232 框内拖动/缩放，确定后再上传。 */
      const [crop, setCrop] = React.useState(null);
      const cropCanvasRef = React.useRef(null);
      const cropDragRef = React.useRef(null);
      const cropMaskRef = React.useRef(null);          // 内置 watch photo 的羽化 mask（Image，只加载一次）
      const cropMaskFetchRef = React.useRef(false);
      const [cropMaskReady, setCropMaskReady] = React.useState(false);
      const [thumbCrc, setThumbCrc] = React.useState('');   // 预览对应的 CRC，避免重复拉取

      const load = React.useCallback(async () => {
        try {
          const response = await fetch('/api/dsh-protorig-panel/facepanel', { headers: { accept: 'application/json' } });
          const body = await response.json();
          if (body) {
            setInfo(body);
            if (body.job) setJob(body.job);
          }
        } catch { /* Host still starting */ }
      }, []);

      React.useEffect(() => { void load(); }, [load, connected]);
      /* 内置 watch photo 预览（未安装下半区图时显示真实内置图，而不是黑框）。 */
      React.useEffect(() => {
        let alive = true;
        (async () => {
          try {
            const response = await fetch('/api/dsh-protorig-panel/facepanel/builtin-preview', { headers: { accept: 'application/json' } });
            const body = await response.json();
            if (alive && body?.ok && body.dataUrl) setBuiltinPanel(body.dataUrl);
          } catch { /* Host 还没起好 */ }
        })();
        return () => { alive = false; };
      }, []);

      const running = !!job?.running;
      /* 安装中只轮询进度（不发 shell）；结束时补拉一次状态（设备侧 CRC 复算已完成）。 */
      React.useEffect(() => {
        if (!running) return undefined;
        const timer = setInterval(async () => {
          try {
            const response = await fetch('/api/dsh-protorig-panel/facepanel/install/state', { headers: { accept: 'application/json' } });
            const body = await response.json();
            if (body?.job) {
              setJob(body.job);
              if (body.job.running === false) void load();
            }
          } catch { /* ignore */ }
        }, 800);
        return () => clearInterval(timer);
      }, [running, load]);

      /* 预览缓存：已安装才有 CRC；CRC 变了才重新拉 */
      React.useEffect(() => {
        let alive = true;
        if (!info?.valid || !info?.crc) {
          setThumb(null);
          setThumbCrc('');
          return () => { alive = false; };
        }
        if (info.crc === thumbCrc) return () => { alive = false; };
        (async () => {
          try {
            const response = await fetch(
              `/api/dsh-protorig-panel/facepanel/thumb?crc=${encodeURIComponent(info.crc)}`,
              { headers: { accept: 'application/json' } },
            );
            const body = await response.json();
            if (!alive) return;
            setThumb(body?.ok && body.dataUrl ? body.dataUrl : null);
            setThumbCrc(info.crc);
          } catch {
            if (alive) setThumb(null);
          }
        })();
        return () => { alive = false; };
      }, [info?.valid, info?.crc, thumbCrc]);

      const post = async (pathname, body) => {
        const response = await fetch(pathname, {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify(body ?? {}),
        });
        return response.json();
      };
      const pickImage = (event) => {
        const file = event.target.files?.[0];
        if (!file) return;
        const url = URL.createObjectURL(file);
        const img = new Image();
        img.onload = () => {
          URL.revokeObjectURL(url);
          setCrop({ img, zoom: 1, x: 0, y: 0, name: file.name });
        };
        img.onerror = () => {
          URL.revokeObjectURL(url);
          setMessage(`${t('failed')}: 图片解码失败`);
        };
        img.src = url;
      };

      /* 480×232 裁剪画布：cover 基础缩放 × 用户缩放 + 拖动偏移。 */
      const clampPan = (next) => {
        const canvas = cropCanvasRef.current;
        if (!canvas || !next?.img) return next;
        const W = canvas.width;
        const H = canvas.height;
        const base = Math.max(W / next.img.naturalWidth, H / next.img.naturalHeight);
        const scale = base * (next.zoom || 1);
        const dw = next.img.naturalWidth * scale;
        const dh = next.img.naturalHeight * scale;
        const maxX = Math.max(0, (dw - W) / 2);
        const maxY = Math.max(0, (dh - H) / 2);
        return {
          ...next,
          x: Math.max(-maxX, Math.min(maxX, next.x || 0)),
          y: Math.max(-maxY, Math.min(maxY, next.y || 0)),
        };
      };
      /* 裁剪框 WYSIWYG：直接画服务端打包用的同一份 mask（内置 watch photo 的羽化 alpha），
       * 不在浏览器里手算径向公式。选图后只拉一次，Image 缓存在 ref 里。 */
      React.useEffect(() => {
        if (!crop?.img || cropMaskFetchRef.current) return undefined;
        cropMaskFetchRef.current = true;
        let alive = true;
        (async () => {
          try {
            const response = await fetch('/api/dsh-protorig-panel/faces/panel-mask', { headers: { accept: 'application/json' } });
            const body = await response.json();
            if (!alive || !body?.ok || !body.dataUrl) return;
            const img = new Image();
            img.onload = () => {
              if (!alive) return;
              cropMaskRef.current = img;
              setCropMaskReady(true);
            };
            img.onerror = () => { /* 没加载到时先按未羽化显示，安装仍由服务端套 mask */ };
            img.src = body.dataUrl;
          } catch { /* Host 还没起好 */ }
        })();
        return () => { alive = false; };
      }, [crop?.img]);
      React.useEffect(() => {
        const canvas = cropCanvasRef.current;
        if (!canvas || !crop?.img) return;
        const ctx = canvas.getContext('2d');
        const W = canvas.width;
        const H = canvas.height;
        ctx.globalCompositeOperation = 'source-over';
        ctx.fillStyle = '#000';
        ctx.fillRect(0, 0, W, H);
        const base = Math.max(W / crop.img.naturalWidth, H / crop.img.naturalHeight);
        const scale = base * (crop.zoom || 1);
        const dw = crop.img.naturalWidth * scale;
        const dh = crop.img.naturalHeight * scale;
        ctx.drawImage(crop.img, (W - dw) / 2 + (crop.x || 0), (H - dh) / 2 + (crop.y || 0), dw, dh);
        const mask = cropMaskRef.current;
        if (mask) {
          ctx.save();
          ctx.globalCompositeOperation = 'destination-in';
          ctx.drawImage(mask, 0, 0, W, H);
          ctx.restore();
        }
        ctx.globalCompositeOperation = 'source-over';
      }, [crop, cropMaskReady]);
      const onCropDown = (event) => {
        if (!crop) return;
        event.currentTarget.setPointerCapture?.(event.pointerId);
        cropDragRef.current = {
          id: event.pointerId,
          x: event.clientX,
          y: event.clientY,
          ox: crop.x || 0,
          oy: crop.y || 0,
          w: event.currentTarget.getBoundingClientRect().width || 360,
        };
      };
      const onCropMove = (event) => {
        const drag = cropDragRef.current;
        if (!drag || !crop || drag.id !== event.pointerId) return;
        const k = 480 / Math.max(1, drag.w);
        setCrop(clampPan({
          ...crop,
          x: drag.ox + (event.clientX - drag.x) * k,
          y: drag.oy + (event.clientY - drag.y) * k,
        }));
      };
      const onCropUp = (event) => {
        if (cropDragRef.current?.id === event.pointerId) cropDragRef.current = null;
      };
      const setZoom = (zoom) => setCrop((current) => (current ? clampPan({ ...current, zoom }) : current));
      const resetCrop = () => setCrop((current) => (current ? { ...current, zoom: 1, x: 0, y: 0 } : current));
      const install = async () => {
        if (!crop) return;
        setBusy(true); setMessage('');
        try {
          const canvas = cropCanvasRef.current;
          const dataUrl = canvas?.toDataURL ? canvas.toDataURL('image/png') : '';
          if (!dataUrl) throw new Error('裁剪预览不可用');
          const body = await post('/api/dsh-protorig-panel/facepanel/install', { image: dataUrl });
          if (!body.ok) setMessage(body.error || t('facePanelFailed'));
          else setJob(body.job ?? { running: true, stage: 'starting', percent: 0 });
          await load();
        } catch (error) { setMessage(String(error.message || error)); } finally { setBusy(false); }
      };
      const clearPanel = async () => {
        setBusy(true); setMessage('');
        try {
          const body = await post('/api/dsh-protorig-panel/facepanel/clear', {});
          setMessage(body.ok ? t('facePanelCleared') : (body.error || t('facePanelFailed')));
          await load();
        } catch (error) { setMessage(String(error.message || error)); } finally { setBusy(false); }
      };
      const cancel = async () => {
        try { await post('/api/dsh-protorig-panel/facepanel/install/cancel', {}); } catch { /* ignore */ }
      };

      const installed = info?.valid === true;
      const faceBusy = info?.busy?.face === true; // 图片表盘正在写：同一把串口锁
      const statusText = installed
        ? `${t('facePanelStatusInstalled')} · ${t('facePanelSize')} ${info.width ?? 480}×${info.height ?? 232} · ${t('facePanelCrc')} ${info.crc || '—'}`
        : (info?.error || (info?.supported === false ? t('facePanelNeedUsb') : t('facePanelStatusBuiltin')));
      const stageText = job?.stage ? `${t('facePanelInstalling')} ${job.stage} · ${Number(job.percent) || 0}%` : '';
      const preview = installed ? thumb : builtinPanel;

      return h('div', null,
        !connected ? h('p', { style: styles.warn }, t('facePanelNeedUsb')) : null,
        h('p', { style: styles.note }, statusText),
        h('div', { style: styles.panelPreview },
          preview
            ? h('img', {
              src: preview,
              alt: t('facePanelPreviewAlt'),
              style: { width: 280, aspectRatio: '480 / 232', objectFit: 'cover', borderRadius: 10, display: 'block' },
            })
            : h('div', { style: styles.panelPlaceholder }, installed ? t('facePanelNoThumb') : t('facePanelStatusBuiltin'))),
        h(Row, { label: t('facePanelPick') },
          h('input', {
            style: styles.input, type: 'file', accept: 'image/png,image/jpeg,image/webp',
            disabled: busy || running || faceBusy, onChange: pickImage,
          })),
        crop
          ? h('div', { style: { marginTop: 8 } },
            h('canvas', {
              ref: cropCanvasRef,
              width: 480,
              height: 232,
              style: styles.cropCanvas,
              onPointerDown: onCropDown,
              onPointerMove: onCropMove,
              onPointerUp: onCropUp,
              onPointerCancel: onCropUp,
            }),
            h(Row, { label: t('facePanelZoom') },
              h('input', {
                style: { width: 200, accentColor: ACCENT }, type: 'range', min: 100, max: 400,
                value: Math.round((crop.zoom || 1) * 100),
                onChange: (event) => setZoom(Number(event.target.value) / 100),
              }),
              h(Button, { onClick: resetCrop }, t('facePanelReset'))),
            h('p', { style: styles.note }, t('facePanelCropHint')))
          : null,
        h('div', { style: styles.actions },
          h(Button, {
            style: 'primary',
            disabled: busy || !connected || !crop || running || faceBusy,
            onClick: () => { void install(); },
          }, running ? t('facePanelInstalling') : t('facePanelInstall')),
          h(Button, {
            disabled: busy || !connected || running || faceBusy || !installed,
            onClick: () => { void clearPanel(); },
          }, t('facePanelClear')),
          running ? h(Button, { onClick: () => { void cancel(); } }, t('flashCancel')) : null,
          h('span', { style: styles.message }, message || (job && !running && job.ok ? t('facePanelDone') : ''))),
        faceBusy ? h('p', { style: styles.note }, t('facePanelBusy')) : null,
        running || stageText
          ? h('div', null, h(Progress, { percent: job?.percent }), h('p', { style: styles.note }, stageText))
          : null,
        job?.error ? h('p', { style: styles.warn }, `${t('facePanelFailed')}: ${job.error}`) : null,
        job?.warning ? h('p', { style: styles.note }, `${t('installWarning')}: ${job.warning}`) : null,
      );
    }

    /**
     * 「表盘」分组：只保留 0 默认 / 3 图片（research/76 §1）。
     * 第一层只有两张卡片（单击切换 / 双击预览）；安装图片表盘、默认表盘下半区
     * 分别收进两个子分区（默认折叠），避免一打开就是一屏表单。
     */
    function FacesGroup({ t, host, connected }) {
      const runtime = host?.state ?? null;
      const [info, setInfo] = React.useState(null);
      const [job, setJob] = React.useState(null);
      const [busy, setBusy] = React.useState(false);
      const [message, setMessage] = React.useState('');
      const [image, setImage] = React.useState(null);
      const [thumb, setThumb] = React.useState(null);       // 当前图片表盘的本地缩略图（dataURL）
      const [preview, setPreview] = React.useState(null);   // 默认表盘的静态预览（dataURL）
      const [thumbCrc, setThumbCrc] = React.useState('');   // 缩略图对应的 CRC，避免重复拉取
      const [previewId, setPreviewId] = React.useState(null); // 双击预览的表盘 id
      const [openImage, setOpenImage] = React.useState(false);
      const [openPanel, setOpenPanel] = React.useState(false);
      const clickTimer = React.useRef(null);
      const [layout, setLayout] = React.useState({
        timeX: 110, timeY: 170, fontId: 1, flags: 3, color: '#FFFA00',
      });

      const load = React.useCallback(async () => {
        try {
          const response = await fetch('/api/dsh-protorig-panel/faces', { headers: { accept: 'application/json' } });
          const body = await response.json();
          if (body?.ok) {
            setInfo(body);
            setJob(body.job ?? null);
          }
        } catch { /* Host still starting */ }
      }, []);

      React.useEffect(() => { void load(); }, [load, connected]);
      /* 默认表盘预览只拉一次（assets 里的静态图，不随设备变化）。 */
      React.useEffect(() => {
        let alive = true;
        (async () => {
          try {
            const response = await fetch('/api/dsh-protorig-panel/faces/preview', { headers: { accept: 'application/json' } });
            const body = await response.json();
            if (alive && body?.ok && body.dataUrl) setPreview(body.dataUrl);
          } catch { /* Host 还没起好 */ }
        })();
        return () => { alive = false; };
      }, []);
      const running = !!job?.running;
      React.useEffect(() => {
        if (!connected && !running) return undefined;
        const timer = setInterval(() => { void load(); }, running ? 800 : 2500);
        return () => clearInterval(timer);
      }, [connected, running, load]);

      /* 本地缩略图缓存：装过图才有；按 CRC 拉一次，换图（CRC 变）再拉 */
      React.useEffect(() => {
        let alive = true;
        if (!info?.valid || !info?.crc) {
          setThumb(null);
          setThumbCrc('');
          return () => { alive = false; };
        }
        if (info.crc === thumbCrc) return () => { alive = false; };
        (async () => {
          try {
            const response = await fetch(
              `/api/dsh-protorig-panel/faces/thumb?crc=${encodeURIComponent(info.crc)}`,
              { headers: { accept: 'application/json' } },
            );
            const body = await response.json();
            if (!alive) return;
            if (body?.ok && body.dataUrl) {
              setThumb(body.dataUrl);
              setThumbCrc(info.crc);
            } else {
              setThumb(null);
              setThumbCrc(info.crc);
            }
          } catch {
            if (alive) setThumb(null);
          }
        })();
        return () => { alive = false; };
      }, [info?.valid, info?.crc, thumbCrc]);

      const post = async (path, body) => {
        const response = await fetch(path, {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify(body ?? {}),
        });
        return response.json();
      };
      const switchFace = async (id) => {
        setBusy(true); setMessage('');
        try {
          const body = await post('/api/dsh-protorig-panel/faces/switch', { id });
          setMessage(body.ok ? `${t('faces')}: ${id}` : (body.error || t('failed')));
          await load();
        } catch (error) { setMessage(String(error.message || error)); } finally { setBusy(false); }
      };
      const pickImage = (event) => {
        const file = event.target.files?.[0];
        if (!file) return;
        const reader = new FileReader();
        reader.onload = () => setImage({ name: file.name, dataUrl: String(reader.result || '') });
        reader.readAsDataURL(file);
      };
      const install = async () => {
        if (!image) return;
        setBusy(true); setMessage('');
        try {
          const body = await post('/api/dsh-protorig-panel/faces/install', {
            image: image.dataUrl,
            layout: {
              timeX: Number(layout.timeX),
              timeY: Number(layout.timeY),
              fontId: Number(layout.fontId),
              flags: Number(layout.flags),
              color: parseInt(String(layout.color).replace('#', ''), 16) || 0,
            },
            apply: true,
          });
          if (!body.ok) setMessage(body.error || t('facesFailed'));
          else setJob(body.job ?? { running: true, stage: 'starting', percent: 0 });
          await load();
        } catch (error) { setMessage(String(error.message || error)); } finally { setBusy(false); }
      };
      const cancel = async () => {
        try { await post('/api/dsh-protorig-panel/faces/install/cancel', {}); } catch { /* ignore */ }
      };
      const backToPc = async () => {
        setBusy(true);
        try {
          const body = await post('/api/dsh-protorig-panel/pcmode', { enter: true });
          setMessage(body.ok ? t('facesBackToPc') : (body.error || t('failed')));
          await load();
        } catch (error) { setMessage(String(error.message || error)); } finally { setBusy(false); }
      };
      const toggleFlag = (bit) => setLayout((current) => ({ ...current, flags: current.flags ^ bit }));

      /* 单击 = 切表盘（延迟 260 ms，给双击让路）；双击 = 预览大图 */
      const handleFaceClick = (id) => {
        if (clickTimer.current) clearTimeout(clickTimer.current);
        clickTimer.current = setTimeout(() => {
          clickTimer.current = null;
          void switchFace(id);
        }, 260);
      };
      const handleFaceDoubleClick = (id) => {
        if (clickTimer.current) {
          clearTimeout(clickTimer.current);
          clickTimer.current = null;
        }
        setPreviewId(id);
      };

      const current = Number.isFinite(info?.current) ? info.current : (runtime?.faces?.current ?? 0);
      /* research/76 §1：固件只保留 0 默认 / 3 图片，界面同步只留这两张卡片。 */
      const faceOptions = [[0, t('faceDefault')], [3, t('faceImage')]];
      const currentName = faceOptions.find(([id]) => id === current)?.[1] ?? String(current);
      const panelBusy = info?.busy?.panel === true; // 下半区正在写：同一把串口锁
      const installed = info?.valid === true;
      const imageInfo = installed
        ? `${t('facesInstalled')} · CRC ${info.crc || '—'}`
        : (info?.supported === false ? (info?.error || t('facesNeedUsb')) : t('facesNotInstalled'));
      const stageText = job?.stage ? `${t('facesInstalling')} ${job.stage} · ${Number(job.percent) || 0}%` : '';

      return h('div', null,
        !connected ? h('p', { style: styles.warn }, t('facesNeedUsb')) : null,
        h('p', { style: { ...styles.note, marginTop: 0 } },
          `${t('facesCurrent')}：${currentName} · ${t('facesDoubleClickHint')}`),
        h('div', { style: styles.faceGrid },
          faceOptions.map(([id, label]) => h('button', {
            key: `face-${id}`,
            type: 'button',
            title: t('facesDoubleClickHint'),
            'aria-pressed': id === current,
            disabled: busy || !connected || running,
            onClick: () => handleFaceClick(id),
            onDoubleClick: () => handleFaceDoubleClick(id),
            style: {
              ...styles.faceTile(id === current),
              opacity: busy || !connected || running ? 0.55 : 1,
            },
          },
          h('div', { style: styles.faceThumb },
            (id === 3 && thumb) || (id === 0 && preview)
              ? h('img', {
                src: id === 3 ? thumb : preview,
                alt: label,
                style: { width: '100%', height: '100%', objectFit: 'cover' },
              })
              : h('span', {
                style: {
                  ...styles.note, margin: 0, padding: 6, textAlign: 'center', lineHeight: 1.35,
                },
              }, id === 3 ? t('facesNoThumbShort') : label)),
          h('span', { style: styles.faceLabel }, label)))),
        h('p', { style: styles.note }, `${t('facesImageStatus')}：${imageInfo}`),
        runtime && runtime.connected && !runtime.pcMode
          ? h('div', { style: styles.actions },
            h(Button, { onClick: () => { void backToPc(); }, disabled: busy }, t('facesBackToPc')),
            h('span', { style: styles.note }, t('facesWatchMode')))
          : null,

        previewId !== null
          ? h('div', { style: styles.previewBox },
            h('div', { style: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 8 } },
              h('span', { style: styles.subTitle }, `${t('facesPreview')}：${faceOptions.find(([id]) => id === previewId)?.[1] ?? previewId}`),
              h(Button, { onClick: () => setPreviewId(null) }, t('facesClose'))),
            previewId === 3
              ? (thumb
                ? h('img', {
                  src: thumb,
                  alt: t('facesPreview'),
                  style: { width: 288, height: 288, objectFit: 'cover', borderRadius: 12, marginTop: 8 },
                })
                : h('p', { style: styles.note }, t('facesNoThumb')))
              : (preview
                ? h('img', {
                  src: preview,
                  alt: t('facesPreview'),
                  style: { width: 288, height: 288, objectFit: 'cover', borderRadius: 12, marginTop: 8 },
                })
                : h('p', { style: styles.note }, t('facesBuiltinPreview'))),
            previewId === 3 && info?.layout
              ? h('p', { style: styles.note },
                `time=(${info.layout.timeX},${info.layout.timeY}) font=${info.layout.fontId} flags=${info.layout.flags} color=#${
                  Number(info.layout.color || 0).toString(16).toUpperCase().padStart(6, '0')}`)
              : null)
          : null,

        /* 图片表盘安装：默认折叠，展开才看到布局表单与进度 */
        h(Sub, {
          title: t('facesImageTitle'),
          hint: t('facesImageHint'),
          open: openImage,
          onToggle: () => setOpenImage((value) => !value),
        },
        panelBusy ? h('p', { style: styles.note }, t('facePanelBusy')) : null,
        h(Row, { label: t('facesPickImage') },
          h('input', {
            style: styles.input, type: 'file', accept: 'image/png,image/jpeg,image/webp',
            disabled: busy || running, onChange: pickImage,
          }),
          image ? h('img', {
            src: image.dataUrl,
            alt: '',
            style: { width: 56, height: 56, objectFit: 'cover', borderRadius: 8 },
          }) : null),
        h(SubHead, { title: t('facesLayout') }),
        h(Row, { label: t('facesTime') },
          h('span', { style: styles.subNote }, 'X'),
          h('input', {
            style: { ...styles.input, width: 76 }, type: 'number', value: layout.timeX,
            disabled: busy || running,
            onChange: (event) => setLayout({ ...layout, timeX: Number(event.target.value) }),
          }),
          h('span', { style: styles.subNote }, 'Y'),
          h('input', {
            style: { ...styles.input, width: 76 }, type: 'number', value: layout.timeY,
            disabled: busy || running,
            onChange: (event) => setLayout({ ...layout, timeY: Number(event.target.value) }),
          })),
        h(Row, { label: t('facesFont') },
          h(Select, {
            value: layout.fontId, disabled: busy || running,
            options: [[0, 'ui48'], [1, 'num104'], [2, 'ui28'], [3, 'ui20']],
            onChange: (value) => setLayout({ ...layout, fontId: Number(value) }),
          })),
        h(Row, { label: t('facesFlags') },
          h('span', { style: styles.inlineControl },
            h('span', { style: styles.subNote }, t('facesShowDate')),
            h(Toggle, {
              checked: (layout.flags & 1) === 1, disabled: busy || running,
              label: t('facesShowDate'), onChange: () => toggleFlag(1),
            })),
          h('span', { style: styles.inlineControl },
            h('span', { style: styles.subNote }, t('facesShowBatt')),
            h(Toggle, {
              checked: (layout.flags & 2) === 2, disabled: busy || running,
              label: t('facesShowBatt'), onChange: () => toggleFlag(2),
            }))),
        h(Row, { label: t('facesColor') },
          h('input', {
            type: 'color', value: layout.color, disabled: busy || running,
            onChange: (event) => setLayout({ ...layout, color: event.target.value }),
            style: { width: 44, height: 26, padding: 0, border: `1px solid ${C.borderStrong}`, borderRadius: 6, background: C.layer1, cursor: 'pointer' },
          })),
        h('div', { style: styles.actions },
          h(Button, {
            style: 'primary', disabled: busy || !connected || !image || running || panelBusy,
            onClick: () => { void install(); },
          }, running ? t('facesInstalling') : t('facesInstall')),
          running ? h(Button, { onClick: () => { void cancel(); } }, t('flashCancel')) : null,
          h('span', { style: styles.message }, message || (job && !running && job.ok ? t('facesDone') : ''))),
        running || stageText
          ? h('div', null, h(Progress, { percent: job?.percent }), h('p', { style: styles.note }, stageText))
          : null,
        job?.error ? h('p', { style: styles.warn }, `${t('facesFailed')}: ${job.error}`) : null,
        job?.warning ? h('p', { style: styles.note }, `${t('installWarning')}: ${job.warning}`) : null),

        /* 默认表盘（face 0）下半区：独立子分区，安装/恢复内置（research/76 §2.3） */
        h(Sub, {
          title: t('facesPanelTitle'),
          hint: t('facePanelHint'),
          open: openPanel,
          onToggle: () => setOpenPanel((value) => !value),
        },
        h(PanelSection, { t, connected })),
      );
    }

    /** 概览：状态 + 身份 + 电量 + 快捷操作（73 §8 / §8.2）。 */
    function OverviewGroup({ t, host, onAction, busy, message, open, onToggle }) {
      const runtime = host?.state ?? null;
      const connected = !!runtime?.connected;
      const ble = !!runtime?.ble;
      const device = runtime?.device ?? {};
      const status = connected ? t('statusUsb') : ble ? t('statusBle') : t('statusOffline');
      const where = connected && runtime?.port ? ` · ${runtime.port}` : '';
      const fw = runtime?.firmwareVersion
        || (Number.isFinite(runtime?.protocolVersion) ? `v${runtime.protocolVersion}` : '—');
      const firmware = runtime?.firmwareBuild ? `${fw} · ${runtime.firmwareBuild}` : fw;
      const battery = Number.isFinite(runtime?.battery) ? `${runtime.battery}%` : '—';
      const meta = [
        `${t('deviceId')} ${device.id || t('deviceIdUnknown')}`,
        `${t('batteryLabel')} ${battery}`,
        `${t('protocolLabel')} ${firmware}`,
        `${t('pcModeLabel')} ${runtime?.pcMode ? t('pcModeOn') : t('pcModeOff')}`,
        device.bound && device.bound !== device.id ? `${t('boundTo')} ${device.bound}` : '',
      ].filter(Boolean).join('  ·  ');
      return h(Group, {
        title: t('overview'),
        right: device.id ? deviceText(device.id) : '',
        open,
        onToggle,
      },
      h('div', { style: styles.statusLine },
        h('span', { style: styles.statusDot(connected || ble, false) }),
        h('span', { style: styles.statusText }, `${status}${where}`),
        message ? h('span', { style: { ...styles.message, color: C.secondary } }, message) : null),
      h('p', { style: styles.metaLine }, meta),
      device.mismatch ? h('p', { style: styles.warn }, t('bindMismatch')) : null,
      h('div', { style: styles.actions },
        h(Button, { style: 'primary', onClick: () => onAction('reconnect'), disabled: busy }, t('actionReconnect')),
        h(Button, { onClick: () => onAction('disconnect'), disabled: busy || !connected }, t('actionDisconnect')),
        h(Button, { onClick: () => onAction('identify'), disabled: busy || !(connected || ble) }, t('actionIdentify'))),
      !connected && !ble ? h('p', { style: styles.note }, t('needWatch')) : null);
    }

    /** 设备：候选列表 + 绑定/解绑 + 当前连接（P10）。 */
    function DeviceGroup({ t, host, open, onToggle, onBind, bindBusy, deviceMessage, onRefresh, scanning }) {
      const runtime = host?.state ?? null;
      const device = runtime?.device ?? {};
      const candidates = Array.isArray(host?.state?.candidates) ? host.state.candidates : [];
      const known = Array.isArray(host?.state?.knownDevices) ? host.state.knownDevices : [];
      const bound = device.bound || '';
      /* 已登记列表和"当前枚举到的串口"会包含同一块表：同一编号只显示一行，
       * 有串口信息时用带端口的 label（PROTORIG ES · 2BEA4E · USB COM8）。 */
      const knownIds = new Set(known.map((item) => normalizeId(item.id)).filter(Boolean));
      const candidateFor = (id) => candidates.find((c) => normalizeId(c.admin) === id) ?? null;
      const extraCandidates = candidates.filter((c) => !normalizeId(c.admin) || !knownIds.has(normalizeId(c.admin)));
      return h(Group, {
        title: t('devices'),
        right: bound ? `${t('boundTo')} ${bound}` : t('unbound'),
        hint: t('devicesHint'),
        open,
        onToggle,
      },
      bound
        ? h('div', { style: styles.row },
          h('span', { style: styles.label }, t('boundTo')),
          h('div', { style: styles.control },
            h('span', { style: styles.deviceName }, deviceText(bound)),
            h(Button, { onClick: () => onBind(''), disabled: bindBusy }, t('unbind'))))
        : h('div', { style: styles.row },
          h('span', { style: styles.label }, t('boundTo')),
          h('span', { style: styles.note }, t('unbound'))),
      h(SubHead, { title: t('knownDevices'), note: t('knownDevicesHint') }),
      known.length
        ? known.map((d) => h('div', { key: `known-${d.id}`, style: styles.deviceRow },
          h('span', { style: styles.deviceName }, candidateFor(normalizeId(d.id))?.label || deviceText(d.id)),
          h('div', { style: styles.control },
            d.usb ? h('span', { style: styles.badge }, 'USB') : null,
            d.ble ? h('span', { style: styles.badge }, 'BLE') : null,
            d.connected ? h('span', { style: styles.badge }, t('connectedNow')) : null,
            d.selected
              ? h('span', { style: styles.badge }, t('bound'))
              : h(Button, { onClick: () => onBind(d.id), disabled: bindBusy }, t('bind')))))
        : h('p', { style: styles.note }, t('noKnownDevices')),
      extraCandidates.length
        ? h(SubHead, { title: t('candidatesTitle') }) : null,
      extraCandidates.length
        ? extraCandidates.map((candidate) => h('div', { key: candidate.path, style: styles.deviceRow },
          h('span', { style: styles.deviceName }, candidate.label || deviceText(candidate.admin)),
          h('div', { style: styles.control },
            candidate.connected ? h('span', { style: styles.badge }, t('connectedNow')) : null,
            bound && candidate.admin === bound ? h('span', { style: styles.badge }, t('bound')) : null,
            candidate.admin && bound !== candidate.admin ? h(Button, {
              onClick: () => onBind(candidate.admin),
              disabled: bindBusy,
            }, t('bind')) : null)))
        : (known.length ? null : h('p', { style: styles.note }, t('deviceNone'))),
      h('div', { style: styles.actions },
        h(Button, { onClick: onRefresh, disabled: scanning }, scanning ? t('scanning') : t('refreshDevices')),
        h('span', { style: styles.note },
          deviceMessage || `${t('deviceScannedAt')}: ${timeText(host?.state?.candidatesAt)}`)),
      h('p', { style: styles.note }, t('deviceAddHint')));
    }

    function Card({ scope, t, saveOps, as = 'li', idPrefix = 'protorig' }) {
      const snapshot = React.useSyncExternalStore(
        (listener) => scope.subscribe(listener),
        () => scope.getSnapshot(),
      );
      const loading = snapshot.status !== undefined && snapshot.status !== 'ready';
      if (loading || !snapshot.value) {
        return h(as, { 'data-protorig-unavailable': true },
          h('p', { role: 'status' }, t('loadingSettings')),
          h(Button, { onClick: () => window.location.reload() }, t('reloadSettings')));
      }
      return h(Editor, { scope, snapshot, t, saveOps, as, idPrefix });
    }

    function Editor({ snapshot, t, saveOps, as, idPrefix }) {
      const saved = React.useMemo(() => resolve(snapshot.value), [JSON.stringify(snapshot.value)]);
      const [draft, setDraft] = React.useState(() => clone(saved));
      const [dirty, setDirty] = React.useState(false);
      const [saving, setSaving] = React.useState(false);
      const [message, setMessage] = React.useState('');
      const [tone, setTone] = React.useState('');
      const [flash, setFlash] = React.useState({ job: null });
      const [host, refreshHost] = useHostState();
      const [actionBusy, setActionBusy] = React.useState(false);
      const [actionMessage, setActionMessage] = React.useState('');
      const [bindBusy, setBindBusy] = React.useState(false);
      const [probeBusy, setProbeBusy] = React.useState('');
      const [probeMessage, setProbeMessage] = React.useState('');
      const [deviceMessage, setDeviceMessage] = React.useState('');
      const [scanning, setScanning] = React.useState(false);
      const [channel, setChannel] = React.useState('usb');
      const writable = snapshot.writable !== false;

      const runtime = host?.state ?? null;
      const flashAvailable = host?.state?.platform?.flash === true;
      const platformKind = host?.state?.platform?.kind ?? 'windows';
      const connected = !!runtime?.connected;
      const deviceBound = normalizeId(runtime?.device?.bound ?? draft.deviceId);

      /* 分组展开状态：概览/显示/表盘常开；设备按"多台/未绑定"默认展开；高级默认折叠。 */
      const [openGroups, setOpenGroups] = React.useState(() => ({
        overview: true,
        display: true,
        faces: true,
        interaction: false,
        connection: false,
        devices: false,
        advanced: false,
      }));
      const toggle = (key) => setOpenGroups((current) => ({ ...current, [key]: !current[key] }));

      // 设备分组：单表且已绑定 → 默认折叠（73 §8 展示规则）
      const candidates = Array.isArray(runtime?.candidates) ? runtime.candidates : [];
      const devicesShouldOpen = candidates.length > 1 || (runtime?.knownDevices?.length ?? 0) > 1 || !deviceBound;
      const devicesOpen = openGroups.devicesTouched ? openGroups.devices : devicesShouldOpen;

      React.useEffect(() => { if (!dirty) setDraft(clone(saved)); }, [JSON.stringify(saved), dirty]);

      /* 显示参数即时生效：滑杆/常亮/息屏时间改动 250 ms 后直接下发给手表，
       * 不再要求先点保存（用户实测"设置 USB 亮度不管用"就是卡在这里）。 */
      React.useEffect(() => {
        if (!dirty) return undefined;
        const timer = setTimeout(() => {
          postJson('/api/dsh-protorig-panel/display/apply', {
            usb: draft.usb,
            ble: draft.ble,
          }).catch(() => {});
        }, 250);
        return () => clearTimeout(timer);
      }, [draft.usb, draft.ble, dirty]);

      const change = (path, value) => {
        setDraft((current) => {
          const next = clone(current);
          writePath(next, path, value);
          setDirty(!equal(next, saved));
          return next;
        });
        setMessage('');
      };

      const diffOps = () => {
        const ops = [];
        const walk = (defaults, savedValue, draftValue, path) => {
          for (const [key, fallback] of Object.entries(defaults)) {
            const current = path.concat(key);
            const savedField = savedValue?.[key];
            const draftField = draftValue?.[key];
            if (fallback && typeof fallback === 'object' && !Array.isArray(fallback)
              && savedField && typeof savedField === 'object' && draftField && typeof draftField === 'object') {
              walk(fallback, savedField, draftField, current);
            } else if (!equal(savedField, draftField)) {
              ops.push({ op: 'set', path: current, value: draftField });
            }
          }
        };
        walk(DEFAULTS, saved, draft, []);
        return ops;
      };

      const save = async () => {
        const ops = diffOps();
        if (!ops.length) { setDirty(false); setMessage(t('saved')); setTone('success'); return; }
        setSaving(true); setMessage('');
        try {
          await saveOps(ops);
          setDirty(false);
          setMessage(t('saved'));
          setTone('success');
        } catch (error) {
          setMessage(`${t('persistFailed')}: ${String(error.message || error)}`);
          setTone('error');
        } finally { setSaving(false); }
      };

      const restore = () => {
        setDraft(clone(saved));
        setDirty(false);
        setMessage('');
        setTone('');
      };

      /** 绑定/解绑：直接写配置（不经过"保存"按钮），下一次 /state 就能看到。 */
      const bindDevice = async (rawId) => {
        const id = normalizeId(rawId);
        setBindBusy(true);
        try {
          await saveOps([{ op: 'set', path: ['deviceId'], value: id }]);
          setDraft((current) => {
            const next = clone(current);
            next.deviceId = id;
            return next;
          });
          setDirty(false);
          setDeviceMessage(id ? `${t('bindDone')} ${id}` : t('unbindDone'));
          await refreshHost();
        } catch (error) {
          setDeviceMessage(`${t('failed')}: ${String(error.message || error)}`);
        } finally { setBindBusy(false); }
      };

      const probeDevices = async (kind) => {
        setProbeBusy(kind);
        setProbeMessage('');
        try {
          const body = await postJson(kind === 'usb'
            ? '/api/dsh-protorig-panel/devices/probe/usb'
            : '/api/dsh-protorig-panel/devices/probe/ble', {});
          if (!body?.ok) throw new Error(body?.error || t('failed'));
          setProbeMessage(kind === 'usb' ? t('probeUsbDone') : t('probeBleDone'));
          await refreshHost();
        } catch (error) {
          setProbeMessage(`${t('failed')}: ${String(error.message || error)}`);
        } finally {
          setProbeBusy('');
        }
      };

      const scanDevices = async () => {
        setScanning(true);
        try {
          const response = await fetch('/api/dsh-protorig-panel/devices?refresh=1&probe=1', {
            headers: { accept: 'application/json' },
          });
          const body = await response.json();
          if (!body?.ok) throw new Error(body?.error || t('failed'));
          setOpenGroups((current) => ({ ...current, devices: true, devicesTouched: true }));
          await refreshHost();
        } catch (error) {
          setDeviceMessage(`${t('failed')}: ${String(error.message || error)}`);
        } finally { setScanning(false); }
      };

      const runAction = async (kind) => {
        setActionBusy(true);
        setActionMessage('');
        try {
          const paths = {
            reconnect: '/api/dsh-protorig-panel/reconnect',
            disconnect: '/api/dsh-protorig-panel/disconnect',
            identify: '/api/dsh-protorig-panel/identify',
          };
          const body = await postJson(paths[kind], {});
          if (!body?.ok) throw new Error(body?.error || t('failed'));
          setActionMessage(
            kind === 'reconnect' ? (body.opened ? t('saved') : t('needWatch'))
              : kind === 'disconnect' ? t('disconnected')
                : t('identified'),
          );
          await refreshHost();
        } catch (error) {
          setActionMessage(`${t('failed')}: ${String(error.message || error)}`);
        } finally { setActionBusy(false); }
      };

      const resetCache = async () => {
        setActionMessage('');
        try {
          const body = await postJson('/api/dsh-protorig-panel/cache/reset', {});
          if (!body?.ok) throw new Error(body?.error || t('failed'));
          setActionMessage(t('resetCacheDone'));
        } catch (error) {
          setActionMessage(`${t('failed')}: ${String(error.message || error)}`);
        }
      };

      const presetOptions = [
        ['short', t('presetShort')], ['medium', t('presetMedium')],
        ['long', t('presetLong')], ['custom', t('presetCustom')],
      ];
      const timeoutOptions = [[5000, '5'], [10000, '10'], [20000, '20']];
      const questionTimeouts = [[15000, '15'], [30000, '30'], [60000, '60']];
      const screenOffOptions = [[0, t('screenOffDefault')], [15, '15 s'], [30, '30 s'], [60, '60 s'], [120, '2 min'], [300, '5 min']];
      const channelCfg = channel === 'usb' ? draft.usb : draft.ble;
      const channelPath = channel === 'usb' ? 'usb' : 'ble';

      return h(as, { 'data-protorig-settings': true, style: styles.root },
        h('div', { style: styles.header },
          h('h2', { style: styles.pageTitle }, t('title')),
          h('p', { style: styles.intro }, t('intro'))),

        /* 1 概览 */
        h(OverviewGroup, {
          t, host, busy: actionBusy, message: actionMessage,
          open: openGroups.overview, onToggle: () => toggle('overview'),
          onAction: runAction,
        }),

        /* 2 显示（USB/蓝牙同一组控件，用分段切换） */
        h(Group, {
          title: t('display'), hint: t('displayHint'),
          right: `${channel === 'usb' ? t('channelUsb') : t('channelBle')} · ${t('brightness')} ${channelCfg.brightness}`,
          open: openGroups.display, onToggle: () => toggle('display'),
        },
        h('div', { style: { margin: '2px 0 4px' } },
          h(Segmented, {
            value: channel,
            options: [['usb', t('channelUsb')], ['ble', t('channelBle')]],
            onChange: setChannel,
          })),
        h(Row, { label: t('brightness') },
          h('input', {
            style: styles.range, type: 'range', min: 32, max: 255,
            value: channelCfg.brightness,
            onChange: (event) => change([channelPath, 'brightness'], Number(event.target.value)),
          }),
          h('span', { style: { ...styles.subNote, minWidth: 28, textAlign: 'right' } }, String(channelCfg.brightness))),
        h(Row, { label: t('alwaysOn') },
          h(Toggle, {
            checked: channelCfg.alwaysOn, label: t('alwaysOn'),
            onChange: (value) => change([channelPath, 'alwaysOn'], value),
          })),
        h(Row, { label: t('screenOff'), hint: t('screenOffHint') },
          h(Select, {
            value: channelCfg.screenOffSec, options: screenOffOptions,
            onChange: (value) => change([channelPath, 'screenOffSec'], Number(value)),
          })),
        h(Divider),
        h(SubHead, { title: t('footerInfo'), note: t('balanceHint') }),
        h(Row, { label: t('balanceEnabled') },
          h(Toggle, {
            checked: draft.balance.enabled, label: t('balanceEnabled'),
            onChange: (value) => change(['balance', 'enabled'], value),
          })),
        h(Row, { label: t('balanceInterval') },
          h(Select, {
            value: draft.balance.intervalSec, options: [[30, '30 s'], [60, '60 s'], [120, '120 s'], [300, '300 s']],
            onChange: (value) => change(['balance', 'intervalSec'], Number(value)),
          }))),

        /* 3 提醒与审批 */
        h(Group, {
          title: t('interaction'), hint: t('interactionHint'),
          open: openGroups.interaction, onToggle: () => toggle('interaction'),
        },
        h(SubHead, { title: t('reminder') }),
        h(Row, { label: t('eventDone') },
          h(Toggle, { checked: draft.reminder.done, label: t('eventDone'), onChange: (value) => change(['reminder', 'done'], value) })),
        h(Row, { label: t('eventNeeds') },
          h(Toggle, { checked: draft.reminder.needs, label: t('eventNeeds'), onChange: (value) => change(['reminder', 'needs'], value) })),
        h(Row, { label: t('eventFailed') },
          h(Toggle, { checked: draft.reminder.failed, label: t('eventFailed'), onChange: (value) => change(['reminder', 'failed'], value) })),
        h(SubHead, { title: t('reminderHow') }),
        h(Row, { label: t('vibrate') },
          h(Toggle, { checked: draft.reminder.vibrate, label: t('vibrate'), onChange: (value) => change(['reminder', 'vibrate'], value) })),
        h(Row, { label: t('bright') },
          h(Toggle, { checked: draft.reminder.bright, label: t('bright'), onChange: (value) => change(['reminder', 'bright'], value) })),
        h(Row, { label: t('vibratePreset') },
          h(Select, {
            value: draft.reminder.vibratePreset, options: presetOptions,
            onChange: (value) => change(['reminder', 'vibratePreset'], value),
          })),
        h(Row, { label: t('vibrateMs') },
          h('input', {
            type: 'number', min: 20, max: 2000,
            value: draft.reminder.vibrateMs, disabled: draft.reminder.vibratePreset !== 'custom',
            style: { ...styles.input, width: 96, opacity: draft.reminder.vibratePreset === 'custom' ? 1 : 0.5 },
            onChange: (event) => change(['reminder', 'vibrateMs'], Number(event.target.value)),
          }),
          h('span', { style: styles.subNote }, 'ms')),
        h(Divider),
        h(SubHead, { title: t('approval') }),
        h(Row, { label: t('approvalEnabled') },
          h(Toggle, {
            checked: draft.approval.enabled, label: t('approvalEnabled'),
            onChange: (value) => change(['approval', 'enabled'], value),
          })),
        h(Row, { label: t('approvalTimeout') },
          h(Select, {
            value: draft.approval.watchTimeoutMs,
            options: timeoutOptions.map(([value, label]) => [value, `${label}${t('seconds')}`]),
            onChange: (value) => change(['approval', 'watchTimeoutMs'], Number(value)),
          })),
        h(Row, { label: t('questionsEnabled') },
          h(Toggle, {
            checked: draft.questions.enabled, label: t('questionsEnabled'),
            onChange: (value) => change(['questions', 'enabled'], value),
          })),
        h(Row, { label: t('questionsTimeout') },
          h(Select, {
            value: draft.questions.watchTimeoutMs,
            options: questionTimeouts.map(([value, label]) => [value, `${label}${t('seconds')}`]),
            onChange: (value) => change(['questions', 'watchTimeoutMs'], Number(value)),
          })),
        h(Row, { label: t('queueWait'), hint: t('queueWaitHint') },
          h('input', {
            style: { ...styles.input, width: 80 }, type: 'number', min: 10, max: 60, step: 5,
            value: draft.queueWaitSec,
            onChange: (event) => change(['queueWaitSec'], Math.max(10, Math.min(60, Number(event.target.value) || 60))),
          }),
          h('span', { style: styles.subNote }, t('seconds')))),

        /* 4 表盘（切换 + 图片安装 + 下半区） */
        h(Group, {
          title: t('faces'),
          open: openGroups.faces, onToggle: () => toggle('faces'),
        },
        h(FacesGroup, { t, host, connected })),

        /* 5 连接开关 + 探测注册 */
        h(Group, {
          title: t('connection'), hint: t('connectionHint'),
          open: openGroups.connection, onToggle: () => toggle('connection'),
        },
        h(Row, { label: t('usbSwitch') },
          h(Toggle, {
            checked: draft.usb.enabled, label: t('usbSwitch'),
            onChange: (value) => change(['usb', 'enabled'], value),
          })),
        h(Row, { label: t('bleSwitch') },
          h(Toggle, {
            checked: draft.ble.enabled, label: t('bleSwitch'),
            onChange: (value) => change(['ble', 'enabled'], value),
          })),
        h(Divider),
        h(Row, { label: t('probeTitle') },
          h(Button, { onClick: () => probeDevices('usb'), disabled: probeBusy === 'usb' }, probeBusy === 'usb' ? t('scanning') : t('probeUsb')),
          h(Button, { onClick: () => probeDevices('ble'), disabled: probeBusy === 'ble' }, probeBusy === 'ble' ? t('scanning') : t('probeBle'))),
        probeMessage ? h('p', { style: styles.note }, probeMessage) : null),

        /* 6 设备 */
        h(DeviceGroup, {
          t, host, open: devicesOpen,
          onToggle: () => setOpenGroups((current) => ({ ...current, devicesTouched: true, devices: !devicesOpen })),
          onBind: bindDevice, bindBusy, deviceMessage, onRefresh: scanDevices, scanning,
        }),

        /* 7 高级（默认折叠；烧录仅 Windows/macOS 渲染） */
        h(Group, {
          title: t('advanced'), hint: t('advancedHint'),
          open: openGroups.advanced, onToggle: () => toggle('advanced'),
        },
        h(SubHead, { title: t('link'), note: t('linkHint') }),
        h(Row, { label: t('fps') },
          h(Select, {
            value: draft.fps, options: [[1, '1 fps'], [2, '2 fps'], [5, '5 fps']],
            onChange: (value) => change(['fps'], Number(value)),
          })),
        h(Row, { label: t('port') },
          h('input', {
            style: { ...styles.input, width: 280 }, type: 'text', value: draft.port,
            placeholder: t('portPlaceholder'), onChange: (event) => change(['port'], event.target.value),
          })),
        h(Row, { label: t('autoEnter') },
          h(Toggle, {
            checked: draft.autoEnter, label: t('autoEnter'),
            onChange: (value) => change(['autoEnter'], value),
          })),
        h(Divider),
        h(SubHead, { title: t('maintenance') }),
        h(Row, { label: t('resetCache'), hint: t('resetCacheHint') },
          h(Button, { onClick: resetCache }, t('resetCache'))),
        flashAvailable
          ? h('div', null,
            h(Divider),
            h(SubHead, { title: t('flash') }),
            h('p', { style: styles.note }, t(platformKind === 'macos' ? 'flashHintMac' : 'flashHint')),
            h(FlashPanel, { t, draft, change, flash, setFlash, platformKind }))
          : (host ? h('p', { style: styles.note }, t('flashUnsupported')) : null),
        h(Divider),
        h(SubHead, { title: t('about') }),
        h('p', { style: styles.note }, `dsh-protorig-panel v${VERSION} · MIT`),
        h('p', { style: styles.note }, t('aboutText'))),

        h('div', { style: styles.saveBar },
          h('div', { style: styles.saveState },
            h('span', { style: styles.saveHint }, dirty ? t('unsaved') : t('autoApplyNote')),
            message
              ? h('span', { style: { ...styles.message, color: tone === 'error' ? C.error : C.success } }, message)
              : null),
          h('div', { style: styles.saveActions },
            h(Button, { onClick: restore, disabled: saving }, t('reload')),
            h(Button, { style: 'primary', onClick: save, disabled: saving || !writable }, saving ? t('saving') : t('save')))),
      );
    }

    function apply(ctx) {
      ctx.effect(() => ctx.locale.register(NS, dictionaries));
      const t = ctx.locale.bind ? ctx.locale.bind(NS) : (key) => key;
      let modernRemote;
      ctx.inject(['remote', 'remote.settings'], (owner) => {
        modernRemote = owner.remote.settings;
        return () => { modernRemote = undefined; };
      });
      let mounted = false;
      const mount = (scope, owner, namespace) => {
        if (mounted || !scope) return;
        mounted = true;
        const saveOps = async (ops) => {
          const snapshot = scope.getSnapshot();
          if (snapshot.status !== undefined && snapshot.status !== 'ready') throw new Error(t('notLoaded'));
          if (modernRemote && owner?.configForms) {
            const response = await modernRemote.mutate(namespace, ops, snapshot.revision);
            if (response.ok) {
              try { owner.configForms.describe().acceptView(response.value); } catch { /* mirror already fresh */ }
            } else {
              try { await owner.configForms.describe().load(); } catch { /* ignore */ }
              throw new Error(response.error?.message || response.error?.code || t('persistFailed'));
            }
          } else if (typeof scope.mutate === 'function') {
            const accepted = await scope.mutate(ops, snapshot.revision);
            if (!accepted) throw new Error(t('persistFailed'));
          } else if (typeof scope.set === 'function') {
            for (const op of ops) {
              if (op.op === 'unset') await scope.unset?.(op.path[0]);
              else if (op.path.length === 1) await scope.set(op.path[0], op.value);
              else throw new Error('nested writes unsupported');
            }
          } else {
            throw new Error(t('persistFailed'));
          }
          await new Promise((resolve) => setTimeout(resolve, 30));
          const value = scope.getSnapshot().value;
          for (const op of ops) {
            if (op.op === 'unset') continue;
            if (!equal(readPath(value, op.path), op.value)) throw new Error(t('persistFailed'));
          }
        };
        ctx.slots.inject('settings.section', () => ctx.slots.register({
          name: 'settings.section', id: 'protorig-panel', order: 20,
          label: () => t('nav'), locale: NS,
          inject: () => ({ scope, saveOps, as: 'section', idPrefix: 'protorig-page' }),
        }, Card));
      };
      ctx.inject(['configForms'], (owner) => mount(owner.configForms.get('dsh-protorig-panel'), owner, 'dsh-protorig-panel'));
      ctx.inject(['settingsScope', 'connection', 'remote'], (owner) =>
        mount(owner.settingsScope.bind({ namespace: 'dsh-protorig-panel' }), owner, 'dsh-protorig-panel'));
    }

    return { apply, inject: ['slots', 'locale'] };
  },
});
