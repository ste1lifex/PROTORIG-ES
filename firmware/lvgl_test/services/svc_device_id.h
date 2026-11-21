/*
 * svc_device_id.h —— 设备身份（管理员编号，F1，2026-11-08）
 *
 * 默认表盘左下角显示的 6 位十六进制编号，就是本文件算出的 admin_id：
 *   eFuse chip ID (8 B) -> FNV-1a -> 低 24 bit（本机 2BEA4E）；
 *   eFuse 读失败才回落固定值 0x7C3E82。
 * 算法从 face_lvgl.c::derive_admin_id() 原样搬入，结果不变。
 *
 * 为什么要独立成服务：UI（表盘）、BLE（广播名后缀 / GET_INFO）、
 * shell（adminid，USB 探测）三处都取同一个编号；本文件不依赖 LVGL/BLE，
 * 任意任务上下文可调，首次调用时懒读 eFuse 并缓存。
 */
#ifndef SVC_DEVICE_ID_H
#define SVC_DEVICE_ID_H

#include <stdint.h>

/* 管理员编号只有 24 bit（表盘按 %06X 显示）。 */
#define SVC_DEVICE_ID_ADMIN_MASK 0xFFFFFFu
/* eFuse 读失败（或哈希落到 0 / 0xFFFFFF）时的固定回落编号，与旧实现一致。 */
#define SVC_DEVICE_ID_FALLBACK   0x7C3E82u
/* eFuse chip ID 长度（字节）。 */
#define SVC_DEVICE_ID_CHIP_LEN   8

/* 管理员编号（24 bit）。自初始化；UI / BLE / shell 任务都能调。 */
uint32_t svc_device_id_admin(void);

/* 8 B eFuse chip ID 指针（静态缓冲，勿修改/释放；读失败时全 0）。 */
const uint8_t *svc_device_id_chip(void);

/* 1 = eFuse chip ID 读取成功；0 = 失败（此时 chip 全 0、admin 为回落值）。 */
int svc_device_id_chip_ok(void);

#endif /* SVC_DEVICE_ID_H */
