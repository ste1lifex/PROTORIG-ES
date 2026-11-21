/*
 * svc_settings.h —— 版本化设置服务（R2b）
 *
 * 职责：把 settings_codec（纯逻辑）与 SD3010 用户 RAM 读写粘起来。
 *   - 新格式 v2（向下兼容 v1）：0x30 起 14 B（magic/version/len/payload/crc16）
 *   - 旧格式 0x31 单字节：检测到新记录无效时迁移一次
 *   - 写后立即回读校验；失败返回 -1（调用方保持 RAM 现值，不回滚）
 *
 * 与 bsp/board_power.c 的接口保持原样：settings_load() 读回并 apply；
 * settings_save(brightness, idle_ms) 由 power_set_persist_cb 注册。
 */
#ifndef SVC_SETTINGS_H
#define SVC_SETTINGS_H

#include <stdint.h>

#include "settings_codec.h"

/* 读设置并应用（亮度/息屏时间）。返回 0 = 新格式或旧格式迁移成功；
 * -1 = 无有效记录，已按默认值应用（= 上电默认，行为与 V1.6 相同）。 */
int settings_load(void);

/* 保存当前亮度/息屏时间（只接受设置页三档；非档位值不写）。
 * 返回 0 成功，-1 = 非档位 / 写失败 / 回读校验失败。 */
int settings_save(uint8_t brightness, uint32_t idle_ms);

/* 保存连接模式（0=省电 1=常连 2=智能）；保留当前亮度/息屏档。
 * 返回 0 成功，-1 = 越界/写失败/回读校验失败。 */
int settings_save_power_mode(uint8_t mode);

/* B（表盘可替换）：保存 face_id（76 §1 起只有 0=默认表盘 3=图片表盘；
 * 旧数据里的 1/2 由 main.c 开机时回退 0 并写回）。
 * 值未变化时直接返回 0（不写 I2C）。返回 0 成功，-1 = 越界/写失败/回读校验失败。 */
int settings_save_face_id(uint8_t face_id);

/* D（闹钟，2026-11-08）：保存 alarm_h / alarm_m / alarm_flags bit0。
 * settings v2 的 payload 里这三个字节早就留好了（R3a 定稿时预留，2026-11-08 起启用），
 * 所以**不需要升 codec 版本**，也不影响旧记录解码。
 * 值未变化时直接返回 0（不写 I2C）。0 成功，-1 = 越界/写失败/回读校验失败。 */
int settings_save_alarm(uint8_t hour, uint8_t minute, uint8_t enabled);

/* 最近一次成功 load/save 的 payload（诊断用） */
void settings_get_payload(settings_payload_t *out);

#endif /* SVC_SETTINGS_H */
