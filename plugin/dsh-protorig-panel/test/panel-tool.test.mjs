/*
 * panel-tool.test.mjs —— protorig_panel 工具的输出契约。
 *
 * 真机踩过：panelStatus() 返回 len/rc/raw/withCrc 等调试字段，而工具输出 schema 是封闭的
 * （additionalProperties:false），宿主直接判非法、整次调用失败。这里锁住"只投影声明字段"。
 */
import './_env.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { panelTool, connTool } from '../src/tools.mjs';

const DECLARED = [
  'ok', 'action', 'supported', 'valid', 'crc', 'width', 'height', 'y', 'size',
  'stage', 'percent', 'seconds', 'error', 'warning',
];

function stubPanel(overrides = {}) {
  return {
    panelStatus: async () => ({
      known: true, supported: true, valid: true, crc: 'DEADBEEF',
      len: 112384, width: 480, height: 232, y: 248,
      checkedAt: 1, installing: false, rc: 0, reason: '', crcChecked: true, raw: 'x', withCrc: false,
      error: null,
      ...overrides,
    }),
    panelClear: async () => ({ ok: true, reply: '[facepanel] clear ok', panel: { valid: false } }),
    panelInstallAndWait: async () => ({ ok: true, view: { ok: true, stage: 'done', percent: 100, seconds: 42 } }),
  };
}

test('protorig_panel status：只投影声明字段，且 null 不能漏出去', async () => {
  const tool = panelTool({ panel: stubPanel() });
  const out = await tool.execute({ action: 'status' });
  for (const key of Object.keys(out)) {
    assert.ok(DECLARED.includes(key), `字段 ${key} 不在输出 schema 里`);
  }
  assert.equal(out.valid, true);
  assert.equal(out.crc, 'DEADBEEF');
  assert.equal(out.size, 112384, 'len 要映射到 size');
  assert.equal(typeof out.error, 'string', 'error 必须是字符串，不能是 null');
  assert.equal(typeof out.width, 'number');
});

test('protorig_panel status：查询出错时也满足 schema', async () => {
  const tool = panelTool({ panel: stubPanel({ valid: false, crc: '', width: null, height: null, y: null, error: '超时' }) });
  const out = await tool.execute({ action: 'status' });
  assert.equal(out.ok, false);
  assert.equal(out.width, 0, 'null 要归一成 0');
  assert.equal(out.error, '超时');
});

test('protorig_panel install：没有 confirm 必须拒绝（不写 media 分区）', async () => {
  const tool = panelTool({ panel: stubPanel() });
  const out = await tool.execute({ action: 'install', imagePath: '/nope.png' });
  assert.equal(out.ok, false);
  assert.match(out.error, /confirm/);
});

test('protorig_panel clear：投影后仍是合法输出', async () => {
  const tool = panelTool({ panel: stubPanel() });
  const out = await tool.execute({ action: 'clear' });
  assert.equal(out.ok, true);
  assert.equal(out.valid, false);
  for (const key of Object.keys(out)) assert.ok(DECLARED.includes(key));
});


test('protorig_conn：pause 让出串口、resume 收回（并带 TTL）', async () => {
  /* 用户/agent 跑自定义脚本前必须先让出串口，否则插件占着口、而且每 1.5 s 会抢回来。 */
  const calls = [];
  const panel = {
    stateForApi: () => ({
      paused: false, connected: true, ble: false, port: '/dev/tty.usbmodemX', device: { id: '2BEA4E' },
    }),
    pauseForScript: async (sec) => {
      calls.push(['pause', sec]);
      return { seconds: Math.min(3600, Math.max(5, Math.round(Number(sec) || 120))), paused: true };
    },
    reconnectNow: async () => { calls.push(['resume']); return { opened: true }; },
  };
  const tool = connTool({ panel });
  const paused = await tool.execute({ action: 'pause', seconds: 30 });
  assert.equal(paused.ok, true);
  assert.equal(paused.paused, true);
  assert.equal(paused.seconds, 30);
  const resumed = await tool.execute({ action: 'resume' });
  assert.equal(resumed.ok, true);
  assert.equal(resumed.paused, false);
  assert.deepEqual(calls, [['pause', 30], ['resume']]);
});

test('protorig_conn：没插 USB 但 BLE 通道可用时 resume 应成功', async () => {
  const tool = connTool({
    panel: {
      stateForApi: () => ({ paused: true, connected: false, ble: false, bleActive: false, port: '' }),
      pauseForScript: async () => ({ seconds: 120 }),
      reconnectNow: async () => ({ opened: false, ble: true, bleReason: '' }),
    },
  });
  const out = await tool.execute({ action: 'resume' });
  assert.equal(out.ok, true, 'BLE 可用就不该报 FAILED');
  const rendered = tool.output.render({ action: 'resume' }, out).map((block) => block.text).join('');
  assert.match(rendered, /BLE 通道已恢复/);
});

test('protorig_conn：手表不在线时 resume 要如实报错，不能假装成功', async () => {
  const tool = connTool({
    panel: {
      stateForApi: () => ({ paused: true, connected: false, ble: false, port: '' }),
      pauseForScript: async () => ({ seconds: 120 }),
      reconnectNow: async () => ({ opened: false }),
    },
  });
  const out = await tool.execute({ action: 'resume' });
  assert.equal(out.ok, false);
  assert.match(out.error, /没找到手表/);
});
