/*
 * device-store.test.mjs —— 已登记设备表（自动发现）
 *
 * 设计约束（2026-11-14 用户要求）：**不允许用户手动输入序列号**；
 * 设备只能通过 USB 握手 / 串口探活 / BLE 广播被自动登记，用户只做"选择连接哪一块"。
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { DeviceStore } from '../src/device-store.mjs';

const tmpFile = () => path.join(
  fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-devices-')),
  'devices.json',
);

test('自动登记：同一块表 USB/BLE 各出现一次 → 一条记录，两个来源都记上', () => {
  const store = new DeviceStore({ file: tmpFile() });
  store.note('2bea4e', 'usb', 1000);
  store.note('2BEA4E', 'ble', 2000);
  const list = store.list();
  assert.equal(list.length, 1, '同一编号只应有一条');
  assert.equal(list[0].id, '2BEA4E');
  assert.ok(list[0].usbAt > 0, 'USB 来源要记');
  assert.ok(list[0].bleAt > 0, 'BLE 来源要记');
});

test('非法编号不登记（空/位数不对/非 hex）', () => {
  const store = new DeviceStore({ file: tmpFile() });
  assert.equal(store.note('', 'usb'), null);
  assert.equal(store.note('2BE', 'usb'), null);
  assert.equal(store.note('ZZZZZZ', 'usb'), null);
  assert.equal(store.list().length, 0);
});

test('持久化：新实例读回，最近见过的在前', () => {
  const file = tmpFile();
  new DeviceStore({ file }).note('2BEA4E', 'usb', 1000);
  new DeviceStore({ file }).note('657837', 'ble', 2000);
  const list = new DeviceStore({ file }).list();
  assert.deepEqual(list.map((d) => d.id), ['657837', '2BEA4E']);
});

test('坏文件容错：垃圾内容按空表处理，仍可继续登记', () => {
  const file = tmpFile();
  fs.writeFileSync(file, '{not json');
  const store = new DeviceStore({ file });
  assert.deepEqual(store.list(), []);
  store.note('2BEA4E', 'usb');
  assert.equal(store.list().length, 1);
});

test('只保留最近 16 块，防文件无限增长', () => {
  const store = new DeviceStore({ file: tmpFile() });
  for (let i = 0; i < 20; i += 1) {
    store.note((0x100000 + i).toString(16).toUpperCase(), 'usb', 1000 + i);
  }
  const list = store.list();
  assert.equal(list.length, 16);
  // 最新的那块仍在
  assert.equal(list[0].id, (0x100000 + 19).toString(16).toUpperCase());
});
