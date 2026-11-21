/*
 * settings_codec.h —— 版本化设置记录的纯逻辑编解码（R2b）
 *
 * 不依赖 SDK / FreeRTOS / SD3010：同一份代码可以在真机与 host 单测里跑
 * （tests/host/test_settings_codec.c）。
 *
 * SD3010 用户 RAM（0x2C..0x71）实际分配（V1.6 硬件既有布局）：
 *
 *   0x2C..0x2F  RTC 掉电魔数 "PRTC"      ← sd3010.c 使用，不能占用
 *   0x30..0x3D  本设置记录 v2（14 B，兼容 v1）
 *   0x3E        battlog 写指针
 *   0x40..0x69  battlog 记录 21 × 2 B
 *   0x6A..0x71  pdslog 计数 8 B
 *
 * 记录格式（小端）：
 *   [0..1] magic  'S','2'
 *   [2]    version = 2（向下兼容 v1）
 *   [3]    payload_len = 8
 *   [4..11] payload (settings_payload_t)
 *   [12..13] CRC-16/CCITT-FALSE，覆盖 [0..11]
 *
 * 为什么不是从 0x2C 开始：0x2C..0x2F 是 RTC 掉电魔数（"PRTC"），被
 * sd3010_check_magic() 用来判断 RTC 是否丢过电；覆盖它会导致每次开机
 * 都被当成掉电、用编译时间重播 RTC —— 这是功能回归，所以新表从 0x30
 * 开始，battlog 顺延（28→21 条，诊断数据），pdslog 不动。
 * 旧格式（0x31 单字节，0xA0|亮度档<<2|息屏档）在 0x30 魔数不存在时迁移。
 */
#ifndef SETTINGS_CODEC_H
#define SETTINGS_CODEC_H

#include <stdint.h>

#define SETTINGS_MAGIC0        'S'
#define SETTINGS_MAGIC1        '2'
#define SETTINGS_VERSION       2u
#define SETTINGS_PAYLOAD_LEN   8u
#define SETTINGS_RECORD_LEN    (2u + 1u + 1u + SETTINGS_PAYLOAD_LEN + 2u)  /* 14 */

/* SD3010 用户 RAM 地址 */
#define SETTINGS_RAM_ADDR      0x30u
#define SETTINGS_LEGACY_ADDR   0x31u

typedef struct {
    uint8_t brightness_idx;     /* 0..2 -> power_brightness_levels[] */
    uint8_t idle_idx;           /* 0..2 -> power_idle_levels[] */
    uint8_t face_id;            /* 0/3 -> 表盘注册表（76 §1 起 1/2 已删除） */
    uint8_t flags;              /* bit0 always_on ... */
    uint8_t alarm_h;            /* 0..23（2026-11-08 起由闹钟服务写入） */
    uint8_t alarm_m;            /* 0..59 */
    uint8_t alarm_flags;        /* bit0 enabled ... */
    uint8_t ble_power_mode;     /* v2: 0=省电 1=常连 2=智能（v1 该字节保留=0，解码按默认 2） */
} settings_payload_t;

/* CRC-16/CCITT-FALSE（poly 0x1021, init 0xFFFF, refin/refout=false, xorout 0） */
uint16_t settings_crc16(const uint8_t *data, uint32_t len);

/* 默认值：亮度档 2（255）、息屏档 0（15000 ms）、连接模式 2（智能），其余 0 */
void settings_payload_defaults(settings_payload_t *out);

/* 字段范围校验（0 成功 / -1 非法） */
int settings_payload_valid(const settings_payload_t *p);

/* 编码到 14 B 记录（out 必须 >= SETTINGS_RECORD_LEN）。0 成功 / -1 失败 */
int settings_encode(const settings_payload_t *p, uint8_t *out);

/* 解码并校验 magic/version/len/CRC/字段范围。接受 v1/v2；
 * v1 的 ble_power_mode 默认 2（智能）。0 成功 / -1 失败 */
int settings_decode(const uint8_t *in, settings_payload_t *out);

/* 旧 0x31 单字节迁移：bit7..4=0xA 标记，bit3..2=亮度档，bit1..0=息屏档。
 * 连接模式默认 2（智能）。0 成功 / -1 不是合法旧格式。 */
int settings_migrate_legacy(uint8_t old_byte, settings_payload_t *out);

#endif /* SETTINGS_CODEC_H */
