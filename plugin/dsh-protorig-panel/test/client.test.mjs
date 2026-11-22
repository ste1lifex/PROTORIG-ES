/*
 * client.test.mjs —— 设置页（src/client.js）渲染冒烟测试
 *
 * 这个文件是浏览器半边的插件代码，平时只有 DSH Web GUI 能跑到它；
 * 这里用一套极小的 React 替身把它在 Node 里渲染出来，验证：
 *   · 设置页信息架构（2026-11 整理）：概览 / 显示 / 提醒与审批 / 表盘 / 连接 / 设备 / 高级 都在；
 *   · 概览能显示连接状态、`PROTORIG ES · 2BEA4E`、端口；
 *   · 布尔项统一为拨动开关（role=switch），不再有 checkbox；
 *   · 设备分组的折叠规则（单表已绑定 → 默认折叠；多表/未绑定 → 展开）和候选列表；
 *   · 烧录分组两个平台都渲染：Windows 走 flash.ps1 / macOS 走 BLFlashCommand-macos（P3 之后）；
 *   · 高级分组默认折叠，展开后才有链路/烧录内容（两个平台一致，只是烧录说明文案不同）。
 *
 * 不测样式，只测"结构 + 文案有没有渲染出来、不抛异常"。
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';

const CLIENT_SOURCE = fs.readFileSync(new URL('../src/client.js', import.meta.url), 'utf8');

const settle = (ms = 20) => new Promise((resolve) => setTimeout(resolve, ms));

/** 极小的 React 替身：支持一次渲染内的常用 hooks，多次 render 共享 hook store。 */
function makeReact() {
  const store = [];
  let index = 0;
  const reset = () => { index = 0; };
  const useSlot = () => index++;
  const React = {
    reset,
    createElement(type, props, ...kids) {
      const children = kids.length ? (kids.length === 1 ? kids[0] : kids) : (props?.children ?? null);
      return { type, props: { ...(props ?? {}), children } };
    },
    useState(init) {
      const i = useSlot();
      if (!(i in store)) store[i] = typeof init === 'function' ? init() : init;
      return [store[i], (value) => { store[i] = typeof value === 'function' ? value(store[i]) : value; }];
    },
    useRef(init) {
      const i = useSlot();
      if (!(i in store)) store[i] = { current: init };
      return store[i];
    },
    useMemo(fn) {
      useSlot();
      return fn();
    },
    useCallback(fn) {
      useSlot();
      return fn;
    },
    useEffect(fn) {
      useSlot();
      const cleanup = fn();
      return typeof cleanup === 'function' ? cleanup : () => {};
    },
    useSyncExternalStore(_subscribe, getSnapshot) {
      useSlot();
      return getSnapshot();
    },
  };
  return React;
}

/**
 * 把元素树渲染成字符串，同时把所有元素节点收集进 out（用于找分组、点开关）。
 * 函数组件直接调用（和原文件里 `FlashPanel({...})` 的用法一致）。
 */
function renderTree(node, out = []) {
  if (node == null || node === false || node === true) return '';
  if (Array.isArray(node)) return node.map((item) => renderTree(item, out)).join('\n');
  if (typeof node === 'string' || typeof node === 'number') return String(node);
  if (typeof node.type === 'function') {
    out.push(node);
    return renderTree(node.type(node.props), out);
  }
  out.push(node);
  return renderTree(node.props.children, out);
}

function hostState(overrides = {}) {
  return {
    ok: true,
    state: {
      connected: true,
      port: 'COM8',
      pcMode: true,
      passthrough: true,
      battery: 87,
      bright: 128,
      page: 0,
      fps: 2,
      ble: false,
      flashHold: false,
      paused: false,
      protocolVersion: 6,
      balance: null,
      device: {
        id: '2BEA4E',
        bound: '2BEA4E',
        source: 'usb',
        label: 'PROTORIG ES · 2BEA4E · USB COM8',
        mismatch: false,
      },
      platform: { os: 'win32', kind: 'windows', name: 'Windows', flash: true },
      candidates: [
        {
          path: 'COM8', port: 'COM8', admin: '2BEA4E', weak: false, connected: true,
          label: 'PROTORIG ES · 2BEA4E · USB COM8',
        },
      ],
      candidatesAt: Date.now(),
      ...overrides,
    },
    flash: { ok: false, stage: 'idle', running: false, supported: true },
  };
}

/**
 * 装载 src/client.js 并渲染设置页。
 * @returns {{render: Function, elements: Function, Comp: Function, module: object}}
 */
async function mountClient({ state = hostState(), platform = 'win32' } = {}) {
  const React = makeReact();
  const realSetInterval = globalThis.setInterval;
  const realFetch = globalThis.fetch;
  // 定时器：只要"能挂上"即可，别让测试进程被 interval 拖住
  globalThis.setInterval = () => 0;
  globalThis.fetch = async (url) => {
    const text = String(url);
    if (text.includes('/state')) return { json: async () => state };
    return { json: async () => ({ ok: true, state: state.state }) };
  };

  let module = null;
  const fakeWindow = {
    __ModuleLoader__: {
      load({ factory }) {
        module = factory((name) => {
          if (name === 'react') return React;
          throw new Error(`unexpected require(${name})`);
        });
      },
    },
  };
  // 每次挂载都重新求值一遍 client.js：ESM import 有缓存，第二个用例就再也拿不到 factory 了
  // eslint-disable-next-line no-new-func
  new Function('window', CLIENT_SOURCE)(fakeWindow);

  let registered = null;
  const scope = {
    getSnapshot: () => ({
      status: 'ready',
      writable: true,
      revision: 1,
      value: {
        port: '', deviceId: '2BEA4E', fps: 2, autoEnter: true,
        reminder: {}, display: {}, approval: {}, questions: {}, ble: {}, balance: {}, flash: {},
      },
    }),
    subscribe: () => () => {},
    mutate: async () => true,
  };
  const owner = {
    configForms: {
      get: () => scope,
      describe: () => ({ acceptView() {}, load: async () => ({}) }),
    },
    settingsScope: { bind: () => scope },
    remote: {},
  };
  const ctx = {
    locale: { register: () => {}, bind: () => (key) => key },
    effect: (fn) => { fn(); return () => {}; },
    inject: (deps, fn) => fn(owner),
    slots: {
      inject: (name, fn) => fn(),
      register: (def, Comp) => { registered = { def, Comp }; return () => {}; },
    },
  };

  module.apply(ctx);
  assert.ok(registered, '应注册 settings.section');
  assert.equal(registered.def.id, 'protorig-panel');

  const render = () => {
    React.reset();
    const elements = [];
    const html = renderTree(registered.Comp({
      scope, saveOps: async () => {}, as: 'section', idPrefix: 'protorig-page',
      t: (key) => key,
    }), elements);
    return { html, elements };
  };

  const cleanup = () => {
    globalThis.setInterval = realSetInterval;
    globalThis.fetch = realFetch;
  };

  return { render, cleanup, platform };
}

test('设置页：连接开关并进概览，分组收敛为六个（2026-11-22 信息架构）', async () => {
  const app = await mountClient();
  try {
    const first = app.render(); // 首帧：还没有 Host 状态（离线渲染）
    for (const key of ['overview', 'display', 'interaction', 'faces', 'devices', 'advanced']) {
      assert.ok(first.elements.some((el) => el.props?.title === key), `应有「${key}」分组`);
    }
    assert.ok(!first.elements.some((el) => el.props?.title === 'connection'),
      '连接开关已上移进概览，不该再单独成组');

    /* 两个通道开关渲染在「概览」与「设备」之间 —— 即真的落在概览分组里 */
    const overviewAt = first.elements.findIndex((el) => el.props?.title === 'overview');
    const devicesAt = first.elements.findIndex((el) => el.props?.title === 'devices');
    const channelSwitches = first.elements
      .map((el, index) => ({ el, index }))
      .filter(({ el }) => el?.props?.role === 'switch' && ['usbSwitch', 'bleSwitch'].includes(el.props['aria-label']));
    assert.deepEqual(channelSwitches.map(({ el }) => el.props['aria-label']).sort(), ['bleSwitch', 'usbSwitch']);
    for (const { index } of channelSwitches) {
      assert.ok(index > overviewAt && index < devicesAt, '通道开关应渲染在概览分组内');
    }
    for (const key of ['probeUsb', 'probeBle']) {
      assert.ok(first.elements.some((el) => el?.props?.children === key && typeof el.props.onClick === 'function'),
        `概览里应有探测按钮「${key}」`);
    }
    assert.match(first.html, /statusOffline/);
    assert.match(first.html, /needWatch/);

    // 等 Host 状态回来再渲染一帧：概览要显示 USB / 编号 / 端口
    await settle(30);
    const second = app.render();
    assert.match(second.html, /statusUsb/);
    assert.match(second.html, /2BEA4E/);
    assert.match(second.html, /COM8/);
    assert.match(second.html, /87%/, '电量');
    assert.match(second.html, /v6/, '协议版本');
    assert.doesNotMatch(second.html, /needWatch/, '在线时不再提示需要连接手表');
  } finally {
    app.cleanup();
  }
});

test('设置页：布尔项统一是拨动开关（role=switch），不再有 checkbox', async () => {
  const app = await mountClient();
  try {
    app.render();
    await settle(30);
    const closed = app.render();
    const switches = closed.elements.filter((el) => el?.props?.role === 'switch');
    assert.ok(switches.length >= 2, `显示分组应渲染拨动开关，实际 ${switches.length}`);
    assert.ok(switches.every((el) => typeof el.props['aria-checked'] === 'boolean'), '开关应带 aria-checked');
    assert.ok(!closed.elements.some((el) => el?.props?.type === 'checkbox'), '不应再有 checkbox');

    /* 展开「提醒与审批」后开关更多：3 类事件 + 2 个提醒通道 + 审批 + 问答 */
    closed.elements.find((el) => el.props?.title === 'interaction').props.onToggle();
    const opened = app.render();
    const more = opened.elements.filter((el) => el?.props?.role === 'switch');
    assert.ok(more.length >= 9, `展开提醒后应有更多开关，实际 ${more.length}`);
    assert.ok(more.every((el) => typeof el.props['aria-checked'] === 'boolean'));

    assert.match(CLIENT_SOURCE, /role: 'switch'/, '源码里用 role=switch 而不是 checkbox');
    assert.doesNotMatch(CLIENT_SOURCE, /type: 'checkbox'/);
  } finally {
    app.cleanup();
  }
});

test('设置页：设备分组单表已绑定时默认折叠，多台/未绑定时展开（§8 展示规则）', async () => {
  const app = await mountClient();
  try {
    app.render(); // 先渲染一帧，useHostState 才会去拉 /state
    await settle(30);
    const withSingle = app.render();
    const devices = withSingle.elements.find((el) => el.props?.title === 'devices');
    assert.equal(devices.props.open, false, '单表 + 已绑定 → 默认折叠');

    // 换成"未绑定 + 两个候选"的 Host 状态
    globalThis.fetch = async () => ({
      json: async () => hostState({
        device: { id: '', bound: '', source: null, label: 'PROTORIG ES', mismatch: false },
        candidates: [
          { path: 'COM8', port: 'COM8', admin: '2BEA4E', connected: false, label: 'PROTORIG ES · 2BEA4E · USB COM8' },
          { path: 'COM5', port: 'COM5', admin: '657837', connected: false, label: 'PROTORIG ES · 657837 · USB COM5' },
        ],
      }),
    });
    app.render(); // 换状态后再渲染一帧，effect 才会去拉新的 /state
    await settle(30);
    const withMulti = app.render();
    const devicesMulti = withMulti.elements.find((el) => el.props?.title === 'devices');
    assert.equal(devicesMulti.props.open, true, '多台/未绑定 → 展开');
    assert.match(withMulti.html, /657837/);
    assert.match(withMulti.html, /2BEA4E/);
    assert.match(withMulti.html, /\bbind\b/, '未绑定的候选带「绑定」按钮');
  } finally {
    app.cleanup();
  }
});

test('设置页：高级默认折叠；展开后有链路/烧录/关于（Windows）', async () => {
  const app = await mountClient();
  try {
    app.render();
    await settle(30);
    const closed = app.render();
    assert.doesNotMatch(closed.html, /flashBin/, '高级折叠时不渲染烧录内容');
    const advanced = closed.elements.find((el) => el.props?.title === 'advanced');
    assert.equal(advanced.props.open, false);

    advanced.props.onToggle(); // 用户点开「高级」
    const opened = app.render();
    assert.match(opened.html, /flashBin/, 'Windows 展开后应出现烧录');
    assert.match(opened.html, /resetCache/);
    assert.match(opened.html, /aboutText/);
    assert.match(opened.html, /dsh-protorig-panel v\d+\.\d+\.\d+/, '关于里带版本号（随 package.json 更新）');
  } finally {
    app.cleanup();
  }
});

test('设置页：表盘分组只留 0/3 两个卡片 + 图片安装 + 默认表盘下半区（research/76）', async () => {
  const app = await mountClient();
  try {
    app.render();
    await settle(30);
    const closed = app.render();
    /* 表盘分组默认展开；安装类表单收在两个子分区里，展开后才渲染。 */
    const facesGroup = closed.elements.find((el) => el.props?.title === 'faces');
    assert.equal(facesGroup.props.open, true, '表盘分组默认展开');
    const first = app.render();
    const html = first.html;
    for (const key of ['faceDefault', 'faceImage']) {
      assert.match(html, new RegExp(key), `应有「${key}」表盘按钮`);
    }
    assert.doesNotMatch(html, /faceMinimal|faceMonitor/, '已删除的 1/2 表盘卡片不能出现');
    assert.match(html, /facesImageTitle/, '图片安装收在子分区里');
    assert.match(html, /facesDoubleClickHint/, '应提示单击切换/双击预览');
    assert.match(html, /facesNoThumb/, '没有本地缓存时显示占位');
    assert.doesNotMatch(html, /facesPickImage/, '子分区默认折叠时不渲染安装表单');

    const imageSub = first.elements.find((el) => el.props?.title === 'facesImageTitle');
    imageSub.props.onToggle();
    const installOpen = app.render();
    assert.match(installOpen.html, /facesImageHint/);
    assert.match(installOpen.html, /facesInstall/, '应有图片表盘安装按钮');
    assert.match(installOpen.html, /facesPickImage/, '应有选图控件');

    /* 默认表盘下半区（research/76 §2.3）：说明/选图/安装/恢复内置 都在 */
    const panelSub = installOpen.elements.find((el) => el.props?.title === 'facesPanelTitle');
    panelSub.props.onToggle();
    const panelOpen = app.render();
    for (const key of ['facePanelHint', 'facePanelPick', 'facePanelInstall', 'facePanelClear']) {
      assert.match(panelOpen.html, new RegExp(key), `下半区控件应有「${key}」`);
    }

    /* 只有 0/3 两个表盘卡片可双击预览 */
    const cards = panelOpen.elements.filter((el) => typeof el.props?.onDoubleClick === 'function');
    assert.equal(cards.length, 2, '只有 0/3 两个表盘卡片可双击');
    cards[0].props.onDoubleClick();
    const preview = app.render();
    assert.match(preview.html, /facesPreview/, '应出现预览块');
    assert.match(preview.html, /facesBuiltinPreview/);
  } finally {
    app.cleanup();
  }
});

test('客户端源码：1/2 表盘的入口与文案已彻底移除（research/76 §1）', () => {
  assert.doesNotMatch(CLIENT_SOURCE, /faceMinimal|faceMonitor/, '1/2 表盘的文案键必须删掉');
  assert.doesNotMatch(CLIENT_SOURCE, /极简|监控/, '1/2 表盘的中文说明必须删掉');
  assert.match(
    CLIENT_SOURCE,
    /faceOptions = \[\[0, t\('faceDefault'\)\], \[3, t\('faceImage'\)\]\]/,
    '表盘卡片只允许 0/3',
  );
  /* 下半区面板接的是研究 76 的 facepanel 路由，不是新协议 */
  for (const path of [
    '/api/dsh-protorig-panel/facepanel',
    '/api/dsh-protorig-panel/facepanel/install',
    '/api/dsh-protorig-panel/facepanel/clear',
  ]) {
    assert.ok(CLIENT_SOURCE.includes(path), `设置页应调用 ${path}`);
  }
});

test('设置页：macOS 渲染烧录（BLFlashCommand-macos 后端，2026-11-08 起）', async () => {
  const app = await mountClient({
    state: hostState({
      platform: { os: 'darwin', kind: 'macos', name: 'macOS', flash: true },
      port: '/dev/cu.usbmodem14201',
    }),
  });
  try {
    app.render();
    await settle(30);
    const closed = app.render();
    const advanced = closed.elements.find((el) => el.props?.title === 'advanced');
    advanced.props.onToggle();
    const opened = app.render();
    assert.match(opened.html, /flashBin/, 'macOS 有烧录分组');
    assert.match(opened.html, /flashHintMac/, 'macOS 用 BLFlashCommand 的说明（t() 在测试里返回 key）');
    assert.doesNotMatch(opened.html, /flashUnsupported/);
    assert.match(opened.html, /\/dev\/cu\.usbmodem14201/, 'macOS 端口路径照常显示');
  } finally {
    app.cleanup();
  }
});

test('设置页：无烧录后端的平台（Linux）只显示一行说明', async () => {
  const app = await mountClient({
    state: hostState({
      platform: { os: 'linux', kind: 'linux', name: 'Linux', flash: false },
    }),
  });
  try {
    app.render();
    await settle(30);
    const closed = app.render();
    const advanced = closed.elements.find((el) => el.props?.title === 'advanced');
    advanced.props.onToggle();
    const opened = app.render();
    assert.match(opened.html, /flashUnsupported/, 'Linux 只显示"只在 Windows/macOS 可用"（t() 在测试里返回 key）');
    assert.doesNotMatch(opened.html, /flashBin/);
  } finally {
    app.cleanup();
  }
});
