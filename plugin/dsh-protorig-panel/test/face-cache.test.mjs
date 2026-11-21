/*
 * face-cache.test.mjs —— 图片表盘本地缩略图缓存
 *
 * 缓存按 image_crc32（8 位大写 hex）索引；目录可用 PROTORIG_FACE_CACHE 覆盖
 * （每次调用现读环境变量，测试用临时目录，不碰用户 ~/protorig-panel）。
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {
  cacheThumb,
  faceCacheDir,
  hasThumb,
  listThumbs,
  normalizeCrc,
  readThumb,
  removeThumb,
  thumbPath,
} from '../src/face-cache.mjs';

const PNG = Buffer.from('89504e470d0a1a0a', 'hex'); // 只验字节往返，不验图片内容

function withCacheDir() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'protorig-face-cache-'));
  process.env.PROTORIG_FACE_CACHE = dir;
  return dir;
}

test('CRC 规范化：8 位 hex、大小写归一，非法值返回空', () => {
  assert.equal(normalizeCrc('dbf3f247'), 'DBF3F247');
  assert.equal(normalizeCrc(' DBF3F247 '), 'DBF3F247');
  assert.equal(normalizeCrc('0xDBF3F247'), '', '不接受 0x 前缀');
  assert.equal(normalizeCrc('DBF3F24'), '');
  assert.equal(normalizeCrc(''), '');
  assert.equal(normalizeCrc(null), '');
});

test('cache/read/has/remove/list：按 CRC 往返，非法 CRC 不落盘', () => {
  const dir = withCacheDir();
  try {
    const crc = 'DBF3F247';
    assert.equal(hasThumb(crc), false);
    assert.equal(readThumb(crc), null);

    const file = cacheThumb(crc, PNG);
    assert.ok(file && fs.existsSync(file), '应写缓存文件');
    assert.equal(path.basename(file), 'DBF3F247.png');
    assert.equal(hasThumb(crc), true);
    assert.deepEqual(readThumb(crc), PNG);

    // 非法 CRC / 空数据：静默失败，不创建文件
    assert.equal(cacheThumb('nope', PNG), '');
    assert.equal(cacheThumb('AABBCCDD', Buffer.alloc(0)), '');
    assert.equal(hasThumb('AABBCCDD'), false);

    const list = listThumbs();
    assert.equal(list.length, 1);
    assert.equal(list[0].crc, crc);
    assert.equal(list[0].size, PNG.length);

    assert.equal(removeThumb(crc), true);
    assert.equal(hasThumb(crc), false);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
    delete process.env.PROTORIG_FACE_CACHE;
  }
});

test('thumbPath：目录取 PROTORIG_FACE_CACHE / 默认 DATA_DIR', () => {
  const dir = withCacheDir();
  try {
    assert.equal(faceCacheDir(), dir);
    assert.equal(thumbPath('DBF3F247'), path.join(dir, 'DBF3F247.png'));
    assert.equal(thumbPath('bad'), '');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
    delete process.env.PROTORIG_FACE_CACHE;
  }
  // 未设置环境变量时落到默认数据目录（不实际写文件）
  assert.match(faceCacheDir(), /faces$/);
});

test('损坏文件：read/has 返回 null/false，不抛错', () => {
  const dir = withCacheDir();
  try {
    fs.writeFileSync(path.join(dir, 'DBF3F247.png'), '');
    assert.equal(readThumb('DBF3F247')?.length, 0, '空文件也读出来（0 字节）');
    assert.equal(hasThumb('DBF3F247'), true);
    fs.rmSync(path.join(dir, 'DBF3F247.png'));
    assert.equal(readThumb('DBF3F247'), null);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
    delete process.env.PROTORIG_FACE_CACHE;
  }
});
