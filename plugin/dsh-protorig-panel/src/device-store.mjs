/*
 * device-store.mjs —— 已见过的手表登记表（**自动登记**，不提供手动输入）
 *
 * 触发点（任一即可自动登记）：
 *   · USB 握手 `adminid` 问出编号（含"不是绑定表"的那块表）；
 *   · 设置页扫描候选口时逐口探活问出编号；
 *   · BLE 广播名 `PROTORIG ES <ADMIN_ID>` 解析出编号。
 *
 * 作用：设置页「设备」把这份列表给用户**选择**连接目标（可留空 = 自动第一个）；
 * 选了以后 USB/BLE 都只连这一块（拦截在 panel-service / ble-bridge）。
 *
 * 存储：`<DATA_DIR>/devices.json`（与烧录锁、表盘缓存同一个目录；
 * DATA_DIR = $PROTORIG_DATA_DIR 或 %LOCALAPPDATA%/protorig-panel）。
 * 文件损坏/权限失败不影响连接功能：读失败当空表，写失败只记 debug。
 */
import fs from 'node:fs';
import path from 'node:path';
import { DATA_DIR } from './flash.mjs';
import { normalizeAdminId } from './device-id.mjs';

export const DEVICES_FILE = path.join(DATA_DIR, 'devices.json');
const MAX_DEVICES = 16;

const copy = (entry) => ({ ...entry });

export class DeviceStore {
  /**
   * @param {{file?:string, logger?:{debug?:Function}}} opts
   *   file 可注入（测试/多实例）；默认 $PROTORIG_DEVICES_FILE 或 DATA_DIR/devices.json
   */
  constructor({ file = process.env.PROTORIG_DEVICES_FILE || DEVICES_FILE, logger = null } = {}) {
    this.file = file;
    this.logger = logger;
    this.loaded = false;
    this.items = [];
  }

  #load() {
    if (this.loaded) return;
    this.loaded = true;
    try {
      const raw = fs.readFileSync(this.file, 'utf8');
      const parsed = JSON.parse(raw);
      const list = Array.isArray(parsed) ? parsed : Array.isArray(parsed?.devices) ? parsed.devices : [];
      this.items = list
        .map((item) => {
          const id = normalizeAdminId(item?.id);
          if (!id) return null;
          return {
            id,
            firstSeen: Number(item.firstSeen) || 0,
            lastSeen: Number(item.lastSeen) || 0,
            usbAt: Number(item.usbAt) || 0,
            bleAt: Number(item.bleAt) || 0,
          };
        })
        .filter(Boolean);
    } catch (error) {
      if (error?.code !== 'ENOENT') this.logger?.debug?.(`设备登记表读取失败（按空表处理）：${error.message}`);
      this.items = [];
    }
  }

  #save() {
    try {
      fs.mkdirSync(path.dirname(this.file), { recursive: true });
      const tmp = `${this.file}.tmp`;
      fs.writeFileSync(tmp, JSON.stringify(this.items, null, 2));
      fs.renameSync(tmp, this.file);
    } catch (error) {
      this.logger?.debug?.(`设备登记表写入失败（不影响连接）：${error.message}`);
    }
  }

  /** 全部已登记设备（最近见过的在前）。 */
  list() {
    this.#load();
    return [...this.items]
      .sort((a, b) => (b.lastSeen || 0) - (a.lastSeen || 0))
      .map(copy);
  }

  /**
   * 自动登记一次出现：新设备插入；老设备只刷新 lastSeen / 来源时间戳。
   * @returns {{id:string,firstSeen:number,lastSeen:number,usbAt:number,bleAt:number}|null}
   */
  note(admin, source, now = Date.now()) {
    const id = normalizeAdminId(admin);
    if (!id) return null;
    this.#load();
    const stamp = Number(now) || Date.now();
    let entry = this.items.find((item) => item.id === id);
    if (!entry) {
      entry = { id, firstSeen: stamp, lastSeen: 0, usbAt: 0, bleAt: 0 };
      this.items.push(entry);
    }
    entry.lastSeen = Math.max(entry.lastSeen || 0, stamp);
    if (source === 'ble') entry.bleAt = Math.max(entry.bleAt || 0, stamp);
    else entry.usbAt = Math.max(entry.usbAt || 0, stamp);
    // 只留最近见过的若干块，防止文件无限增长
    if (this.items.length > MAX_DEVICES) {
      this.items.sort((a, b) => (b.lastSeen || 0) - (a.lastSeen || 0));
      this.items = this.items.slice(0, MAX_DEVICES);
    }
    this.#save();
    return copy(entry);
  }
}
