/*
 * usb_pc_link.c —— USB 原始二进制上行组帧（电脑模式）
 *
 * 运行上下文：SDK 的 usb_console 任务（与 BLE 主机任务同级别）。这里
 * **只组帧 + 塞进 svc_pc 的记录环**，不碰 LVGL、不碰面板 —— 真正的
 * 解析和 blit 在 UI 任务的 svc_pc_tick() 里做，和 BLE 的 0x50 完全同一条路。
 *
 * 帧：A5 5A len payload(len)  —— len 1..240，payload 首字节是 op。
 * 一帧可以跨多个 USB 包（FS 批量端点 64 B/包），所以状态机要能续。
 */
#include <string.h>

#include "usb_pc_link.h"
#include "svc_pc.h"

#define PCUSB_SYNC0   0xA5
#define PCUSB_SYNC1   0x5A
#define PCUSB_MAX     240       /* 与 svc_pc_stage() 的 PC_CHUNK_MAX 一致 */

enum { ST_SYNC0 = 0, ST_SYNC1, ST_LEN, ST_PAYLOAD };

int protorig_usb_pc_active = 0;

static int s_state = ST_SYNC0;
static uint16_t s_need, s_have;
static uint8_t s_buf[PCUSB_MAX];

void usb_pc_link_reset(void)
{
    s_state = ST_SYNC0;
    s_need = 0;
    s_have = 0;
}

void usb_pc_link_set_active(int on)
{
    if (on) {
        usb_pc_link_reset();
        protorig_usb_pc_active = 1;
    } else {
        protorig_usb_pc_active = 0;
        usb_pc_link_reset();        /* 半截帧不留给下一次会话 */
    }
}

/* 逐字节喂状态机；一帧收满就交给 svc_pc_stage() */
static void feed_byte(uint8_t b)
{
    switch (s_state) {
    case ST_SYNC0:
        if (b == PCUSB_SYNC0) {
            s_state = ST_SYNC1;
        }
        break;

    case ST_SYNC1:
        if (b == PCUSB_SYNC1) {
            s_state = ST_LEN;
        } else if (b != PCUSB_SYNC0) {
            s_state = ST_SYNC0;     /* 不是同步头，丢掉重新找 */
        }
        break;

    case ST_LEN:
        if (b == 0 || b > PCUSB_MAX) {
            s_state = ST_SYNC0;     /* 长度非法：丢，等电脑重发整块 */
            break;
        }
        s_need = b;
        s_have = 0;
        s_state = ST_PAYLOAD;
        break;

    case ST_PAYLOAD:
        s_buf[s_have++] = b;
        if (s_have >= s_need) {
            s_state = ST_SYNC0;
            /* 与 BLE 同一条入口，但这里可以等：整屏连发时环满只等 UI 抽片，
             * 不等就会丢片 → TILE_END 字节不足 → 整块被丢。
             * 上限给到 500 ms：一次瓦片 blit 本身就要 ~6 ms，整屏 30 条带
             * 光刷面板就 ~180 ms；20 ms 的上限实测仍会周期性超车（丢 11 片）。 */
            (void)svc_pc_stage_wait(s_buf, s_need, 500);
        }
        break;

    default:
        s_state = ST_SYNC0;
        break;
    }
}

int protorig_usb_pc_rx(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    if (!protorig_usb_pc_active || buf == NULL || len == 0) {
        return 0;
    }

    /* 只在两种情况下把这一包当二进制：
     *   ① 正在收一帧的中间（s_state != ST_SYNC0）；
     *   ② 这一包以同步头 0xA5 开头。
     * 其余情况整包交还 shell —— 电脑模式下敲的 `pcmode 0` 走的就是这里。 */
    if (s_state == ST_SYNC0 && buf[0] != PCUSB_SYNC0) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        feed_byte(buf[i]);
    }
    return 1;
}
