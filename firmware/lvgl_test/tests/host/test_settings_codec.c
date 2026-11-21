/*
 * test_settings_codec.c —— host 单元测试（R2b）
 *
 * 编译运行（不依赖 SDK）：
 *   gcc -std=c99 -Wall -Wextra -I services \
 *       tests/host/test_settings_codec.c services/settings_codec.c \
 *       -o tests/host/build_host/test_settings_codec.exe
 *   ./tests/host/build_host/test_settings_codec.exe
 */
#include <stdio.h>
#include <string.h>

#include "settings_codec.h"

static int s_failures;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        s_failures++;
    }
}

int main(void)
{
    uint8_t rec[SETTINGS_RECORD_LEN];
    uint8_t bad[SETTINGS_RECORD_LEN];
    settings_payload_t p, q;
    uint16_t crc;

    printf("settings_codec host test\n");

    /* 1. CRC-16/CCITT-FALSE 标准向量 */
    crc = settings_crc16((const uint8_t *)"123456789", 9);
    printf("  crc(123456789) = 0x%04X\n", crc);
    check(crc == 0x29B1, "CRC-16/CCITT-FALSE vector 0x29B1");

    /* 2. 默认值合法 */
    settings_payload_defaults(&p);
    check(p.brightness_idx == 2 && p.idle_idx == 0 && p.ble_power_mode == 2,
          "defaults = brightness 2 / idle 0 / mode 2");
    check(settings_payload_valid(&p) == 0, "defaults valid");

    /* 3. 编码头字段 */
    p.face_id = 3;
    p.flags = 1;
    p.alarm_h = 7;
    p.alarm_m = 30;
    p.alarm_flags = 1;
    check(settings_encode(&p, rec) == 0, "encode succeeds");
    check(rec[0] == 'S' && rec[1] == '2' && rec[2] == 2 && rec[3] == 8,
          "encoded header = S,2,2,8");

    /* 4. 编解码往返 */
    memset(&q, 0, sizeof(q));
    check(settings_decode(rec, &q) == 0, "decode succeeds");
    check(q.brightness_idx == p.brightness_idx && q.idle_idx == p.idle_idx &&
          q.face_id == p.face_id && q.flags == p.flags &&
          q.alarm_h == p.alarm_h && q.alarm_m == p.alarm_m &&
          q.alarm_flags == p.alarm_flags && q.ble_power_mode == p.ble_power_mode,
          "roundtrip payload equal (incl. alarm/mode)");

    /* 5-7. magic / version / len 损坏 */
    memcpy(bad, rec, sizeof(bad));
    bad[0] = 'X';
    check(settings_decode(bad, &q) != 0, "bad magic rejected");

    memcpy(bad, rec, sizeof(bad));
    bad[2] = 3;
    check(settings_decode(bad, &q) != 0, "bad version rejected");

    memcpy(bad, rec, sizeof(bad));
    bad[3] = 4;
    check(settings_decode(bad, &q) != 0, "bad payload len rejected");

    /* 8. CRC 损坏 */
    memcpy(bad, rec, sizeof(bad));
    bad[13] ^= 0x01;
    check(settings_decode(bad, &q) != 0, "bad crc rejected");

    /* 8b. v1 记录兼容：version=1 + reserved=0 -> mode=2 */
    memcpy(bad, rec, sizeof(bad));
    bad[2] = 1;
    bad[11] = 0;
    crc = settings_crc16(bad, SETTINGS_RECORD_LEN - 2u);
    bad[12] = (uint8_t)(crc & 0xFF);
    bad[13] = (uint8_t)((crc >> 8) & 0xFF);
    check(settings_decode(bad, &q) == 0 && q.ble_power_mode == 2,
          "v1 record decodes with mode=2 default");

    /* 9. 字段越界但 CRC 正确 -> 仍拒绝 */
    settings_payload_defaults(&p);
    p.brightness_idx = 3;
    check(settings_encode(&p, bad) != 0, "encode rejects brightness_idx=3");

    memcpy(bad, rec, sizeof(bad));
    bad[4] = 3;                                     /* brightness_idx = 3 */
    crc = settings_crc16(bad, SETTINGS_RECORD_LEN - 2u);
    bad[12] = (uint8_t)(crc & 0xFF);
    bad[13] = (uint8_t)((crc >> 8) & 0xFF);
    check(settings_decode(bad, &q) != 0, "decode rejects out-of-range field with valid crc");

    /* 9b. 闹钟字段越界（CRC 正确也拒绝） */
    settings_payload_defaults(&p);
    p.alarm_h = 24;
    check(settings_encode(&p, bad) != 0, "encode rejects alarm_h=24");
    settings_payload_defaults(&p);
    p.alarm_m = 60;
    check(settings_encode(&p, bad) != 0, "encode rejects alarm_m=60");

    /* 10. 旧 0x31 单字节迁移 */
    check(settings_migrate_legacy(0xA9, &q) == 0 &&
          q.brightness_idx == 2 && q.idle_idx == 1 && q.ble_power_mode == 2,
          "legacy 0xA9 -> brightness 2 / idle 1 / mode 2");
    check(settings_migrate_legacy(0xA0, &q) == 0 &&
          q.brightness_idx == 0 && q.idle_idx == 0,
          "legacy 0xA0 -> brightness 0 / idle 0");

    /* 11. 非旧格式 / 档位越界 */
    settings_payload_defaults(&p);
    p.ble_power_mode = 3;
    check(settings_encode(&p, bad) != 0, "encode rejects mode=3");

    check(settings_migrate_legacy(0xFF, &q) != 0, "legacy 0xFF rejected");
    check(settings_migrate_legacy(0xAC, &q) != 0, "legacy tier 3 rejected");

    printf("%s (%d failure%s)\n",
           s_failures ? "FAILED" : "PASSED",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
