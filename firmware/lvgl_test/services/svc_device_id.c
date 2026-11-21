/*
 * svc_device_id.c —— 设备身份（管理员编号，F1，2026-11-08）
 *
 * 算法逐字保留自 face_lvgl.c::derive_admin_id()（bflb_efuse_get_chipid ->
 * FNV-1a -> 低 24 bit，失败回落 0x7C3E82），所以默认表盘上的编号不变。
 * 搬到独立服务的原因是同一个编号现在有四个消费者：
 *   表盘（face_lvgl.c）、BLE 广播名（ble_link.c）、GET_INFO（ble_svc.c）、
 *   shell adminid（main.c）—— 不能各算各的，也不能让 BLE 依赖 LVGL。
 *
 * 线程模型：UI 任务和 BLE 任务都可能先到，首次读 eFuse 放在临界区里
 * “只做一次”；eFuse 读只是几条寄存器操作（BL616 ROM API），临界区很短。
 * 之后只返回缓存，不再进临界区。
 */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bflb_efuse.h"

#include "svc_device_id.h"

#define FNV1A_OFFSET_BASIS 0x811C9DC5u
#define FNV1A_PRIME        0x01000193u

static uint8_t  s_chip[SVC_DEVICE_ID_CHIP_LEN];  /* eFuse chip ID；失败全 0 */
static uint32_t s_admin;                         /* 24 bit 管理员编号 */
static int      s_chip_ok;                       /* 1 = eFuse 读成功 */
static int      s_ready;                         /* 1 = 已读完并算完 */

static void svc_device_id_ensure(void)
{
    uint32_t h;
    int ok;
    int i;

    taskENTER_CRITICAL();
    if (s_ready) {
        taskEXIT_CRITICAL();
        return;
    }

    memset(s_chip, 0, sizeof(s_chip));
    ok = (bflb_efuse_get_chipid(s_chip) == 0);
    if (!ok) {
        /* 读失败：chip 对外全 0，编号用回落值（与旧 derive_admin_id 一致） */
        memset(s_chip, 0, sizeof(s_chip));
        s_admin = SVC_DEVICE_ID_FALLBACK;
    } else {
        h = FNV1A_OFFSET_BASIS;
        for (i = 0; i < SVC_DEVICE_ID_CHIP_LEN; i++) {
            h ^= s_chip[i];
            h *= FNV1A_PRIME;
        }
        h &= SVC_DEVICE_ID_ADMIN_MASK;
        s_admin = (h == 0u || h == SVC_DEVICE_ID_ADMIN_MASK) ? SVC_DEVICE_ID_FALLBACK : h;
    }
    s_chip_ok = ok;
    s_ready = 1;

    taskEXIT_CRITICAL();
}

uint32_t svc_device_id_admin(void)
{
    svc_device_id_ensure();
    return s_admin;
}

const uint8_t *svc_device_id_chip(void)
{
    svc_device_id_ensure();
    return s_chip;
}

int svc_device_id_chip_ok(void)
{
    svc_device_id_ensure();
    return s_chip_ok;
}
