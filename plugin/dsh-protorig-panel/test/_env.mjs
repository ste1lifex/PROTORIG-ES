/* 测试环境隔离：设备登记表默认写到临时目录，避免 npm test 污染用户的 devices.json。 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

if (!process.env.PROTORIG_DEVICES_FILE) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-test-devices-'));
  process.env.PROTORIG_DEVICES_FILE = path.join(dir, 'devices.json');
}

/* 烧录锁/烧录落盘标记都写在 DATA_DIR 下：测试必须指到临时目录，
 * 否则会写用户的 ~/protorig-panel（工作区外还会被沙箱拒绝）。 */
if (!process.env.PROTORIG_DATA_DIR) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-test-data-'));
  process.env.PROTORIG_DATA_DIR = dir;
}
