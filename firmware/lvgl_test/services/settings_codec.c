/*
 * settings_codec.c —— 版本化设置记录编解码（纯逻辑，host 可测）
 */
#include <string.h>

#include "settings_codec.h"

uint16_t settings_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    uint32_t i;

    if (data == NULL) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        int b;
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

void settings_payload_defaults(settings_payload_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->brightness_idx = 2;    /* 255 */
    out->idle_idx = 0;          /* 15000 ms */
    out->ble_power_mode = 2;    /* 智能 */
}

int settings_payload_valid(const settings_payload_t *p)
{
    if (p == NULL) {
        return -1;
    }
    if (p->brightness_idx > 2 || p->idle_idx > 2) {
        return -1;
    }
    if (p->alarm_h > 23 || p->alarm_m > 59) {
        return -1;
    }
    if (p->ble_power_mode > 2) {
        return -1;
    }
    return 0;
}

int settings_encode(const settings_payload_t *p, uint8_t *out)
{
    uint16_t crc;

    if (p == NULL || out == NULL || settings_payload_valid(p) != 0) {
        return -1;
    }

    out[0] = SETTINGS_MAGIC0;
    out[1] = SETTINGS_MAGIC1;
    out[2] = (uint8_t)SETTINGS_VERSION;
    out[3] = (uint8_t)SETTINGS_PAYLOAD_LEN;

    out[4] = p->brightness_idx;
    out[5] = p->idle_idx;
    out[6] = p->face_id;
    out[7] = p->flags;
    out[8] = p->alarm_h;
    out[9] = p->alarm_m;
    out[10] = p->alarm_flags;
    out[11] = p->ble_power_mode;

    crc = settings_crc16(out, SETTINGS_RECORD_LEN - 2u);
    out[12] = (uint8_t)(crc & 0xFF);
    out[13] = (uint8_t)((crc >> 8) & 0xFF);
    return 0;
}

int settings_decode(const uint8_t *in, settings_payload_t *out)
{
    settings_payload_t p;
    uint16_t want, got;

    if (in == NULL || out == NULL) {
        return -1;
    }
    if (in[0] != SETTINGS_MAGIC0 || in[1] != SETTINGS_MAGIC1) {
        return -1;
    }
    /* v2 解码器同时接受 v1（该字节曾是 reserved，按默认智能模式处理） */
    if ((in[2] != 1u && in[2] != (uint8_t)SETTINGS_VERSION) ||
        in[3] != (uint8_t)SETTINGS_PAYLOAD_LEN) {
        return -1;
    }

    want = (uint16_t)((uint16_t)in[12] | ((uint16_t)in[13] << 8));
    got = settings_crc16(in, SETTINGS_RECORD_LEN - 2u);
    if (want != got) {
        return -1;
    }

    p.brightness_idx = in[4];
    p.idle_idx = in[5];
    p.face_id = in[6];
    p.flags = in[7];
    p.alarm_h = in[8];
    p.alarm_m = in[9];
    p.alarm_flags = in[10];
    p.ble_power_mode = (in[2] == 1u) ? 2u : in[11];   /* v1 -> 默认智能 */

    if (settings_payload_valid(&p) != 0) {
        return -1;
    }
    *out = p;
    return 0;
}

int settings_migrate_legacy(uint8_t old_byte, settings_payload_t *out)
{
    uint8_t bi, ti;

    if (out == NULL) {
        return -1;
    }
    if ((old_byte & 0xF0u) != 0xA0u) {
        return -1;
    }
    bi = (uint8_t)((old_byte >> 2) & 0x03u);
    ti = (uint8_t)(old_byte & 0x03u);
    if (bi >= 3 || ti >= 3) {
        return -1;
    }

    settings_payload_defaults(out);
    out->brightness_idx = bi;
    out->idle_idx = ti;
    return 0;
}
