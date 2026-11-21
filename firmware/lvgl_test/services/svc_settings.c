/*
 * svc_settings.c —— 版本化设置服务（R2b）
 *
 * 存储布局见 settings_codec.h 顶部注释（0x30 起 14 B，避开 0x2C 的 RTC 魔数）。
 */
#include <stdio.h>
#include <string.h>

#include "svc_settings.h"
#include "settings_codec.h"

#include "sd3010.h"
#include "board_power.h"

static settings_payload_t s_payload;

static int find_u8(const uint8_t *arr, int n, uint8_t v)
{
    int i;
    for (i = 0; i < n; i++) {
        if (arr[i] == v) {
            return i;
        }
    }
    return -1;
}

static int find_u32(const uint32_t *arr, int n, uint32_t v)
{
    int i;
    for (i = 0; i < n; i++) {
        if (arr[i] == v) {
            return i;
        }
    }
    return -1;
}

/* 应用 payload 到 power 模块。只在 LVGL 任务上电初始化阶段调用。 */
static void settings_apply(const settings_payload_t *p)
{
    power_set_brightness(power_brightness_levels[p->brightness_idx]);
    power_set_idle_ms(power_idle_levels[p->idle_idx]);
}

int settings_load(void)
{
    uint8_t rec[SETTINGS_RECORD_LEN];
    uint8_t old_byte = 0xFF;
    settings_payload_t p;
    int rd;

    /* 1) 新格式 v1 */
    rd = sd3010_ram_read(SETTINGS_RAM_ADDR, rec, sizeof(rec));
    if (rd == 0 && settings_decode(rec, &p) == 0) {
        s_payload = p;
        settings_apply(&p);
        printf("[settings] v2 loaded: brightness=%u idle=%lu face=%u flags=0x%02X\r\n",
               power_get_brightness(), (unsigned long)power_get_idle_ms(),
               p.face_id, p.flags);
        return 0;
    }

    /* 2) 旧格式：0x31 单字节（0xA0 | 亮度档<<2 | 息屏档） */
    rd = sd3010_ram_read(SETTINGS_LEGACY_ADDR, &old_byte, 1);
    if (rd == 0 && settings_migrate_legacy(old_byte, &p) == 0) {
        s_payload = p;
        settings_apply(&p);
        printf("[settings] legacy 0x31=0x%02X -> v1 (brightness_idx=%u idle_idx=%u)\r\n",
               old_byte, p.brightness_idx, p.idle_idx);
        (void)settings_save(power_get_brightness(), power_get_idle_ms());
        return 0;
    }

    /* 3) 没有任何有效记录：用默认值（与 V1.6 上电默认一致） */
    settings_payload_defaults(&s_payload);
    settings_apply(&s_payload);
    printf("[settings] no valid record -> defaults (brightness=%u idle=%lu)\r\n",
           power_get_brightness(), (unsigned long)power_get_idle_ms());
    return -1;
}

int settings_save(uint8_t brightness, uint32_t idle_ms)
{
    uint8_t rec[SETTINGS_RECORD_LEN];
    uint8_t verify[SETTINGS_RECORD_LEN];
    int bi, ti;

    bi = find_u8(power_brightness_levels, 3, brightness);
    ti = find_u32(power_idle_levels, 3, idle_ms);
    if (bi < 0 || ti < 0) {
        return -1;              /* 非设置页档位（例如低电临时值）不落盘 */
    }

    s_payload.brightness_idx = (uint8_t)bi;
    s_payload.idle_idx = (uint8_t)ti;

    if (settings_encode(&s_payload, rec) != 0) {
        return -1;
    }
    if (sd3010_ram_write(SETTINGS_RAM_ADDR, rec, sizeof(rec)) != 0) {
        printf("[settings] write failed\r\n");
        return -1;
    }

    /* 写后立即回读校验（掉电/总线错误都能在这里发现） */
    if (sd3010_ram_read(SETTINGS_RAM_ADDR, verify, sizeof(verify)) != 0 ||
        memcmp(rec, verify, sizeof(rec)) != 0 ||
        settings_decode(verify, &s_payload) != 0) {
        printf("[settings] verify failed\r\n");
        return -1;
    }

    printf("[settings] saved v2 @0x%02X brightness_idx=%u idle_idx=%u\r\n",
           (unsigned)SETTINGS_RAM_ADDR, (unsigned)bi, (unsigned)ti);
    return 0;
}

int settings_save_power_mode(uint8_t mode)
{
    uint8_t rec[SETTINGS_RECORD_LEN];
    uint8_t verify[SETTINGS_RECORD_LEN];

    if (mode > 2) {
        return -1;
    }
    s_payload.ble_power_mode = mode;

    if (settings_encode(&s_payload, rec) != 0) {
        return -1;
    }
    if (sd3010_ram_write(SETTINGS_RAM_ADDR, rec, sizeof(rec)) != 0) {
        printf("[settings] mode write failed\r\n");
        return -1;
    }
    if (sd3010_ram_read(SETTINGS_RAM_ADDR, verify, sizeof(verify)) != 0 ||
        memcmp(rec, verify, sizeof(rec)) != 0 ||
        settings_decode(verify, &s_payload) != 0) {
        printf("[settings] mode verify failed\r\n");
        return -1;
    }
    printf("[settings] saved v2 mode=%u\r\n", mode);
    return 0;
}

void settings_get_payload(settings_payload_t *out)
{
    if (out != NULL) {
        *out = s_payload;
    }
}

/* B（表盘可替换）：切表盘后把 face_id 落盘（0..3）。
 * 值与当前 payload 相同则直接返回，避免开机恢复表盘时多写一次 I2C。 */
int settings_save_face_id(uint8_t face_id)
{
    uint8_t rec[SETTINGS_RECORD_LEN];
    uint8_t verify[SETTINGS_RECORD_LEN];

    if (face_id > 3) {
        return -1;
    }
    if (s_payload.face_id == face_id) {
        return 0;
    }
    s_payload.face_id = face_id;

    if (settings_encode(&s_payload, rec) != 0) {
        return -1;
    }
    if (sd3010_ram_write(SETTINGS_RAM_ADDR, rec, sizeof(rec)) != 0) {
        printf("[settings] face write failed\r\n");
        return -1;
    }
    if (sd3010_ram_read(SETTINGS_RAM_ADDR, verify, sizeof(verify)) != 0 ||
        memcmp(rec, verify, sizeof(rec)) != 0 ||
        settings_decode(verify, &s_payload) != 0) {
        printf("[settings] face verify failed\r\n");
        return -1;
    }
    printf("[settings] saved v2 face=%u\r\n", face_id);
    return 0;
}

/* D（闹钟，2026-11-08）：settings v2 payload 里的 alarm_h/alarm_m/alarm_flags（已启用）。
 * 与 face_id 一样：值没变不写 I2C，写后回读校验。 */
int settings_save_alarm(uint8_t hour, uint8_t minute, uint8_t enabled)
{
    uint8_t rec[SETTINGS_RECORD_LEN];
    uint8_t verify[SETTINGS_RECORD_LEN];
    uint8_t on = enabled ? 1u : 0u;

    if (hour > 23 || minute > 59 || on > 1) {
        return -1;
    }
    if (s_payload.alarm_h == hour && s_payload.alarm_m == minute &&
        (s_payload.alarm_flags & 0x01u) == on) {
        return 0;
    }
    s_payload.alarm_h = hour;
    s_payload.alarm_m = minute;
    s_payload.alarm_flags = (uint8_t)((s_payload.alarm_flags & ~0x01u) | on);

    if (settings_encode(&s_payload, rec) != 0) {
        return -1;
    }
    if (sd3010_ram_write(SETTINGS_RAM_ADDR, rec, sizeof(rec)) != 0) {
        printf("[settings] alarm write failed\r\n");
        return -1;
    }
    if (sd3010_ram_read(SETTINGS_RAM_ADDR, verify, sizeof(verify)) != 0 ||
        memcmp(rec, verify, sizeof(rec)) != 0 ||
        settings_decode(verify, &s_payload) != 0) {
        printf("[settings] alarm verify failed\r\n");
        return -1;
    }
    printf("[settings] saved v2 alarm %02u:%02u %s\r\n",
           (unsigned)hour, (unsigned)minute, on ? "on" : "off");
    return 0;
}
