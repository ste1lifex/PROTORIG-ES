/*
 * svc_pc.c —— 电脑连接模式（透传显示）实现
 *
 * 见 svc_pc.h。三件事：
 *   1. 链路：USB 已配置 > BLE 已连接 > 无；
 *   2. 显示通路：有链路 → 冻结 LVGL、整屏交给电脑分片 blit；
 *                无链路 → 解冻 LVGL，显示本地"等待电脑/连接丢失"界面；
 *   3. 帧解析：亮度 / 震动 / 整屏填充 / 像素块（BEGIN+DATA×N+END）。
 *
 * 传输：所有帧都是 BLE 命令 0x50 的 payload；BLE 单次写入 ≤ MTU-3，
 * 所以像素走分片。BLE 任务只把每片丢进小记录环，真正的 LVGL/面板操作
 * 在 UI 任务的 svc_pc_tick() 里做。
 *
 * 帧格式（op 之后的字段一律小端）：
 *   0x01 HELLO                 -> 回 0x81 [ver, maxW lo,hi, maxH lo,hi, fmt]
 *   0x02 BRIGHT   u8           (32..255)
 *   0x03 VIBRATE  u16 ms
 *   0x10 TILE_BEGIN x,y,w,h 各 u16
 *   0x11 TILE_DATA 原始 RGB565 字节（每片 ≤ 240 B）
 *   0x12 TILE_END              -> 整块 blit 到面板
 *   0x13 CLEAR    r,g,b
 *   0x14 STATUS_REQ            -> 回 0x91 [link, batt, bright, page]
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lvgl.h"
#include "usbd_core.h"

#include "svc_pc.h"
#include "ble_link.h"
#include "ble_svc.h"
#include "board_power.h"
#include "st7802.h"
#include "motor.h"
#include "app_state.h"
#include "tp_lvgl.h"
#include "usb_pc_link.h"        /* USB 原始二进制上行：A5 5A len → svc_pc_stage */
#include "pc_monitor.h"         /* BLE 卡片层：0x53/0x54/0x55/0x56/0x57 落屏 */
#include "eez_ui_glue.h"        /* 掉线安全阀：走正常 page close 退出电脑模式 */

#define PC_TILE_W_MAX   480
#define PC_TILE_H_MAX   16      /* 480*16*2 = 15360 B，一块像素缓冲 */
#define PC_CHUNK_MAX    240     /* 单片像素；留余量给 op 头 */
#define PC_RING_N       32      /* 记录环：吸收 USB 整带突发（8 片时实测会丢） */
#define PC_LINK_LOST_EXIT_MS 60000u  /* 无链路多久自动退出电脑模式（防耗电/防卡死） */
#define PC_HOST_GONE_MS 3000u        /* USB IN 连续多久没完成 = PC 端进程没了（关掉 3100 会卡死） */
#define PC_HOST_IDLE_MS 10000u       /* 非透传（电脑空闲不推像素）时的同款超时，放宽到 10 s */
#define PC_PROTO_VER    1

#define PC_OP_HELLO     0x01
#define PC_OP_BRIGHT    0x02
#define PC_OP_VIBRATE   0x03
#define PC_OP_TILE_BEG  0x10
#define PC_OP_TILE_DATA 0x11
#define PC_OP_TILE_END  0x12
#define PC_OP_CLEAR     0x13
#define PC_OP_STATUS    0x14
#define PC_OP_ALWAYS_ON 0x15   /* payload[1]: 0=PC 模式下也会息屏, 1=常亮 */
#define PC_OP_SCREEN_OFF 0x16  /* payload[1..2]: 小端秒数；0=恢复默认 idle（电脑模式专用，不写手表设置） */

/* BLE 卡片层（research/65）：无像素链路时由 PC 下发，本机 LVGL 画下半屏。
 * 这些 op 在 USB 下也接受（台架测试用），只是透传时屏幕被电脑接管、看不见。 */
#define PC_OP_SESSION    0x53  /* state, steps, ctx(i8), toolLen+tool, titleLen+title, footerLen+footer */
#define PC_OP_APPROVAL   0x54  /* canAllow, computer, countdown, toolLen+tool, cmdLen+cmd */
#define PC_OP_QUESTION   0x55  /* qIdx,qCnt,multi,selMask u16,page,optCount,promptLen+prompt,optLen+option */
#define PC_OP_CARD_CLEAR 0x56  /* 无载荷：撤卡 */
#define PC_OP_CARD_BMP   0x57  /* slot, w u16, h u16, index, total, bpp, data... */

#define PC_EVT_HELLO    0x81
#define PC_EVT_STATUS   0x91
#define PC_EVT_ZONES    0x58   /* 卡片热区自报：kind, count, {id,x,y,w,h}×count */
#define PC_EVT_ACK      0x59   /* 卡片帧回执：what, code（1=card 2=bmp ok 3=bmp bad 4=card bad） */
#define PC_EVT_REDRAW   0x92   /* 2026-11-08：本地弹窗结束 -> 请主机整屏重发。
                                * payload: [reason u8]  1 = 闹钟/计时器弹窗结束。
                                * 主机不认识时应直接忽略（多一条事件不影响旧插件）。 */

/* 与 pc_monitor.c 的卡片版式一一对应（热区以固件自报为准，插件不再猜） */
#define PC_ZONE_DENY     1
#define PC_ZONE_ALLOW    2
#define PC_ZONE_OPTION   3
#define PC_ZONE_NEXT     4
#define PC_ZONE_SUBMIT   5

#define PC_ACK_CARD     1
#define PC_ACK_BMP_OK   2
#define PC_ACK_BMP_BAD  3
#define PC_ACK_CARD_BAD 4

#define PC_BMP_CHUNK_MAX 231   /* 240 - (op + slot + w2 + h2 + index + total + bpp) */

static int s_active;
static int s_passthrough;
static int s_await_first_frame;
/* 2026-11-08：本地弹窗期间暂停接收 PC 像素帧（PC 模式/session 保持） */
static int s_frames_suspended;
static int s_popup_unfrozen;
static int s_transport;
static uint8_t s_bright = 128;
static uint8_t s_always_on = 1;   /* 默认常亮，可用 0x15 关掉 */
static uint32_t s_tick_ms;
static uint32_t s_last_status_ms;
static uint32_t s_rx_frames, s_rx_drops, s_bad_frames, s_ignored;

/* 掉线安全阀（见 svc_pc_tick 2b） */
static int s_no_link;
static uint32_t s_no_link_since;

/* 2026-11-28：会话在、监控页不在的"对账"计时（见 svc_pc_tick 3b） */
static uint32_t s_page_gap_since;
static uint8_t s_page_gap_fix_tried;
static uint8_t s_page_gap_warned;

/* BLE 请求进/出电脑模式（-1 = 无请求）；由 svc_pc_tick 在 UI 任务里消费 */
static volatile int s_pending = -1;

/* shell `pcmode 0` 之后把 UI 退回表盘（2026-11-09 P0）。
 * shell 任务不能直接碰 LVGL（LV_OS_NONE），所以只置标志，由 svc_pc_tick 执行。 */
static volatile int s_pending_ui_leave;

/* 2026-11-14：主机存活判定。SDK 补丁在 usb_console_bulk_in_cb 里对每次 USB IN
 * 完成计数；PC 端进程被杀（3100 关掉）后端口关闭、没人再收我们的状态帧，
 * 计数停止增长。透传时判定主机消失 -> 自动退出电脑模式回表盘，不再卡死。 */
extern volatile uint32_t protorig_usb_console_in_events;
static int s_host_watch_on;
static uint32_t s_host_last_in;
static uint32_t s_host_last_change_ms;
static uint32_t s_host_gone_count;

/* 消费进度（单调递增）。注意：不能用 s_ring_tail 判断"UI 有没有在抽"——
 * 它是 mod PC_RING_N 的索引，满载时 32 片一绕回原值，软阈值会误判。 */
static volatile uint32_t s_ring_drained;

/* USB 总线挂起（拔线）判定用：起点 + 诊断计数 */
static int s_usb_susp_active;
static uint32_t s_usb_susp_since;
static uint32_t s_usb_susp_ticks;

/* 像素缓冲：先按字节累积，TILE_END 时原地转成 uint16_t 再 blit */
static uint16_t s_pix[PC_TILE_W_MAX * PC_TILE_H_MAX];

/* 正在拼的瓦片 */
static int s_tile_open;
static uint16_t s_tx, s_ty, s_tw, s_th;
static uint32_t s_tile_filled;

/* BLE 任务 -> UI 任务的记录环 */
typedef struct {
    uint16_t len;
    uint8_t data[PC_CHUNK_MAX];
} pc_chunk_t;
static pc_chunk_t s_ring[PC_RING_N];
static volatile int s_ring_head, s_ring_tail;
static uint32_t s_ring_drop;

static void send_mode(int passthrough);
static void pc_touch_hook(int pressed, uint16_t x, uint16_t y, uint32_t taps);

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ------------------------------------------------------------------ 链路 */

/* USB 是否"真的还在"。
 *
 * 坑（2026-11-06 上板实测）：拔掉 USB 线时 `usb_device_is_configured(0)`
 * **不会**立刻变 0 —— 它只在总线 reset / 下一次枚举时清。于是固件一直以为
 * 链路还在：屏幕停在最后一帧（用户看到"绿屏不消失"）、不解冻、60 s 安全阀
 * 也不启动。
 *
 * 拔线后 3 ms 内没有 SOF，USB 设备必然收到 suspend；CHERRYUSB 会置
 * `usb_device_is_suspend(0)`。所以"已配置且挂起"就是链路没了。
 * 兼容主机真的把设备挂起的情况：挂起要持续 SUSP_DEBOUNCE_MS 才算掉线，
 * 期间仍按 USB 处理（主机一唤醒 SOF 回来就恢复）。
 */
#define PC_USB_SUSP_DEBOUNCE_MS  500u

static int usb_link_ok(uint32_t now_ms)
{
    if (!usb_device_is_configured(0)) {
        s_usb_susp_active = 0;
        return 0;
    }
    if (!usb_device_is_suspend(0)) {
        s_usb_susp_active = 0;
        return 1;
    }
    s_usb_susp_ticks++;
    if (!s_usb_susp_active) {
        s_usb_susp_active = 1;
        s_usb_susp_since = now_ms;
    }
    if ((uint32_t)(now_ms - s_usb_susp_since) > PC_USB_SUSP_DEBOUNCE_MS) {
        return 0;                       /* 挂起持续 500 ms：当成拔线 */
    }
    return 1;
}

static int detect_transport(void)
{
    if (usb_device_is_configured(0) && !usb_device_is_suspend(0)) {
        return SVC_PC_LINK_USB;         /* 插充电器不算：必须被主机配置 */
    }
    if (ble_link_is_connected()) {
        return SVC_PC_LINK_BLE;
    }
    return SVC_PC_LINK_NONE;
}

int svc_pc_transport(void) { return s_transport; }
/* 2026-11-14：USB 像素透传时，本地 LVGL 只是"冻结显示"，输入仍在跑 ——
 * 手势会在看不见的页面上悄悄导航（用户断 USB 后发现自己已经在设置页）。
 * 所以透传期间 UI 层要吞掉所有本地输入；触摸事件仍由 pc_touch_hook 转发给电脑。
 * 弹窗期间（frames_suspended）LVGL 解冻、需要本地点击关卡片，不拦。 */
int svc_pc_local_input_blocked(void)
{
    if (!s_active || s_transport != SVC_PC_LINK_USB) {
        return 0;
    }
    if (s_frames_suspended) {
        return 0;
    }
    return s_passthrough ? 1 : 0;
}

int svc_pc_active(void)    { return s_active; }
int svc_pc_link_up(void)   { return s_transport != SVC_PC_LINK_NONE; }

/* 从 BLE 回调里调：只置标志，不碰 LVGL/页面 */
void svc_pc_request(int on)
{
    s_pending = on ? 1 : 0;
}

void svc_pc_init(void)
{
    s_active = 0;
    s_passthrough = 0;
    s_frames_suspended = 0;
    s_popup_unfrozen = 0;
    s_await_first_frame = 0;
    s_transport = SVC_PC_LINK_NONE;
    s_bright = 128;
    s_tick_ms = 0;
    s_last_status_ms = 0;
    s_rx_frames = s_rx_drops = s_bad_frames = s_ignored = 0;
    s_ring_head = s_ring_tail = 0;
    s_ring_drop = 0;
    s_tile_open = 0;
    s_tile_filled = 0;
    s_no_link = 0;
    s_pending = -1;
    usb_pc_link_set_active(0);          /* 没进电脑模式时 USB RX 还是 shell 的 */
    tp_lvgl_set_hook(pc_touch_hook);    /* 电脑模式下转发触摸给电脑 */
    printf("[pc] pc-mode service ready (proto v%d, tile %dx%d, touch fwd on)\r\n",
           PC_PROTO_VER, PC_TILE_W_MAX, PC_TILE_H_MAX);
}

/* -------------------------------------------------------------- 显示通路 */

/* 冻结 LVGL：之后整屏像素由电脑 blit 进来 */
static void passthrough_on(void)
{
    if (s_passthrough) {
        return;
    }
    s_passthrough = 1;
    s_host_watch_on = 0;
    lv_display_enable_invalidation(lv_display_get_default(), false);
    st7802_fill(0, 0, 479, 479, 0x000000);
    if (s_transport == SVC_PC_LINK_USB) {
        send_mode(1);
    }
    printf("[pc] passthrough ON (transport=%d)\r\n", s_transport);
}

/* 解冻 LVGL：回到本地"连接丢失/等待电脑"界面 */
static void passthrough_off(void)
{
    if (!s_passthrough) {
        return;
    }
    s_passthrough = 0;
    s_host_watch_on = 0;
    s_tile_open = 0;
    s_tile_filled = 0;
    if (s_transport == SVC_PC_LINK_USB) {
        send_mode(0);
    }
    lv_display_enable_invalidation(lv_display_get_default(), true);
    lv_obj_invalidate(lv_screen_active());
    printf("[pc] passthrough OFF\r\n");
}

/* ------------------------------------------------------------------ 发送 */

/* mode 供电脑侧判读：
 *   0 = 串口调试模式（手表模式，RX 还是 shell，只能发命令）
 *   1 = 透传模式（电脑模式，RX 已切成二进制帧）
 * USB 侧用十六进制行承载（printf 不能直接吐 NUL/二进制）：
 *   @PCM1 MODE 1                     ← 模式变化时主动广播
 *   @PCM1 FRAME a55a5091<hex...>     ← 二进制帧的等价文本形式
 * 电脑侧规则：看到 @PCM1 MODE 1 才发像素；看到 MODE 0 只发命令。 */
static void usb_send_hex(const uint8_t *p, uint16_t len)
{
    uint16_t i;
    printf("@PCM1 FRAME a55a50");
    for (i = 0; i < len; i++) {
        printf("%02X", p[i]);
    }
    printf("\r\n");
}

static void send_mode(int passthrough)
{
    printf("@PCM1 MODE %d\r\n", passthrough ? 1 : 0);
}

static void send_event(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    if (s_transport == SVC_PC_LINK_BLE) {
        (void)ble_svc_send_frame(cmd, payload, len);
    } else if (s_transport == SVC_PC_LINK_USB) {
        /* USB：二进制帧转成十六进制行，文本日志与帧用前缀区分。
         * 卡片回执/热区最大 31 B，缓冲区必须够，否则台架上看不到 ACK。 */
        uint8_t buf[96];
        if (len + 1 > sizeof(buf)) {
            return;
        }
        buf[0] = cmd;
        if (len > 0) {
            memcpy(&buf[1], payload, len);
        }
        usb_send_hex(buf, (uint16_t)(len + 1));
    }
}

static void send_status(void)
{
    uint8_t b[4];
    app_state_t st;
    (void)app_state_get(&st);
    b[0] = (uint8_t)s_transport;
    b[1] = st.battery_pct;
    b[2] = power_effective_brightness();
    b[3] = (uint8_t)st.page_id;
    send_event(PC_EVT_STATUS, b, sizeof(b));
    s_last_status_ms = lv_tick_get();
}

/* 2026-11-08：弹窗结束，请主机整屏重发（插件 0.4.x 已处理 0x92；旧插件忽略即可） */
static void send_redraw_request(void)
{
    uint8_t reason = 1;             /* 1 = 本地弹窗结束 */

    if (s_transport == SVC_PC_LINK_NONE || !s_passthrough) {
        return;                     /* BLE/无链路没有像素流，不需要重发 */
    }
    send_event(PC_EVT_REDRAW, &reason, 1);
    printf("[pc] redraw request -> host (reason=1 popup)\r\n");
}

static void pc_touch_flush_release(void);

/*
 * 2026-11-08：PC 模式下的本地弹窗（闹钟/计时器到点）。
 *
 * 关键：**不退出 PC 模式、不拆 session、不向电脑发 MODE 0**。
 *   suspend=1：丢 0x10..0x13 像素帧 + 临时解冻 LVGL 画本地页/通知卡；
 *              同时把"弹窗之前可能还按着的那一笔"给电脑补一个抬起，
 *              避免电脑 UI 卡在按下状态；
 *   suspend=0：重新冻结 LVGL + 发 0x92 请主机整屏重发。
 * 只在 UI 任务调用；不在 PC 模式时直接返回（普通模式行为完全不变）。
 */
void svc_pc_suspend_frames(int on)
{
    if (on) {
        if (!s_active || s_frames_suspended) {
            return;
        }
        s_frames_suspended = 1;
        s_tile_open = 0;            /* 弹窗跨在瓦片中间：丢掉半截瓦片，别误 blit */
        s_tile_filled = 0;
        pc_touch_flush_release();   /* 电脑侧别卡在按下 */
        if (s_passthrough) {
            /* 只是临时解冻渲染：s_passthrough 保持 1（0x10..0x13 本来就被丢了）。
             * 不在这里 lv_refr_now()：本函数可能从 LVGL 定时器回调里进来，
             * 让 lv_timer_handler 本轮刷完即可（几 ms，通知卡也要下一拍才入屏）。 */
            s_popup_unfrozen = 1;
            lv_display_enable_invalidation(lv_display_get_default(), true);
            lv_obj_invalidate(lv_screen_active());
        }
        printf("[pc] popup START: frames suspended (active=%d passthrough=%d)\r\n",
               s_active, s_passthrough);
    } else {
        if (!s_frames_suspended) {
            return;
        }
        s_frames_suspended = 0;
        if (s_popup_unfrozen) {
            s_popup_unfrozen = 0;
            if (s_active && s_passthrough) {
                lv_display_enable_invalidation(lv_display_get_default(), false);
                send_redraw_request();
            }
        }
        printf("[pc] popup DONE: frames resumed (active=%d passthrough=%d)\r\n",
               s_active, s_passthrough);
    }
}

int svc_pc_frames_suspended(void)
{
    return s_frames_suspended;
}

/* ------------------------------------------------------------ 帧处理(UI) */

static void tile_begin(const uint8_t *p, uint16_t len)
{
    uint16_t x, y, w, h;
    if (len < 9) {
        s_bad_frames++;
        return;
    }
    x = rd16(&p[1]);
    y = rd16(&p[3]);
    w = rd16(&p[5]);
    h = rd16(&p[7]);
    if (w == 0 || h == 0 || w > PC_TILE_W_MAX || h > PC_TILE_H_MAX ||
        (uint32_t)x + w > 480u || (uint32_t)y + h > 480u) {
        s_bad_frames++;
        s_tile_open = 0;
        return;
    }
    s_tx = x;
    s_ty = y;
    s_tw = w;
    s_th = h;
    s_tile_filled = 0;
    s_tile_open = 1;
}

static void tile_data(const uint8_t *p, uint16_t len)
{
    uint32_t cap;
    uint32_t n;
    if (!s_tile_open) {
        s_bad_frames++;
        return;
    }
    cap = (uint32_t)s_tw * s_th * 2u;
    n = len - 1;
    if (s_tile_filled + n > cap) {
        s_bad_frames++;
        return;
    }
    memcpy(((uint8_t *)s_pix) + s_tile_filled, &p[1], n);
    s_tile_filled += n;
}

static void tile_end(void)
{
    uint32_t npix;
    uint32_t i;
    uint8_t *b = (uint8_t *)s_pix;

    if (!s_tile_open) {
        s_bad_frames++;
        return;
    }
    npix = (uint32_t)s_tw * s_th;
    if (s_tile_filled < npix * 2u) {
        s_bad_frames++;
        s_tile_open = 0;
        return;
    }
    /* 原地字节 -> uint16：先读 2i/2i+1，再写 u16[i]，不越界回看 */
    for (i = 0; i < npix; i++) {
        uint16_t v = (uint16_t)(b[i * 2] | ((uint16_t)b[i * 2 + 1] << 8));
        s_pix[i] = v;
    }
    st7802_blit565(s_tx, s_ty, (uint16_t)(s_tx + s_tw - 1),
                   (uint16_t)(s_ty + s_th - 1), s_pix);
    s_tile_open = 0;
    s_tile_filled = 0;
}

/* ------------------------------------------------------- 卡片层（BLE 数字通道） */

/* 当前卡片类型（只在这里更新，和 pc_monitor 的落屏保持一致） */
static int s_card_kind;

static void send_card_ack(uint8_t what, uint8_t code)
{
    uint8_t b[2];
    b[0] = what;
    b[1] = code;
    send_event(PC_EVT_ACK, b, sizeof(b));
}

/* 卡片热区自报：插件按这份矩形做 hit-test，版式改动只在固件这一处。
 * 会话卡/无卡也报一次 count=0，插件不用猜"现在是哪种卡"。 */
static void send_card_zones(void)
{
    uint8_t b[2 + 3 * 9];
    uint16_t n = 2;
    int count = 0;

    b[0] = (uint8_t)s_card_kind;
    b[1] = 0;
    if (s_card_kind == PC_CARD_APPROVAL) {
        const uint8_t deny[9] = { PC_ZONE_DENY, 106 & 0xFF, 106 >> 8, 296 & 0xFF, 296 >> 8,
                                  128 & 0xFF, 128 >> 8, 96, 0 };
        const uint8_t allow[9] = { PC_ZONE_ALLOW, 246 & 0xFF, 246 >> 8, 296 & 0xFF, 296 >> 8,
                                   128 & 0xFF, 128 >> 8, 96, 0 };
        memcpy(&b[n], deny, 9); n += 9;
        memcpy(&b[n], allow, 9); n += 9;
        count = 2;
    } else if (s_card_kind == PC_CARD_QUESTION) {
        const uint8_t opt[9] = { PC_ZONE_OPTION, 92 & 0xFF, 92 >> 8, 218 & 0xFF, 218 >> 8,
                                 296 & 0xFF, 296 >> 8, 96, 0 };
        const uint8_t next[9] = { PC_ZONE_NEXT, 128 & 0xFF, 128 >> 8, 314 & 0xFF, 314 >> 8,
                                  104 & 0xFF, 104 >> 8, 96, 0 };
        const uint8_t submit[9] = { PC_ZONE_SUBMIT, 248 & 0xFF, 248 >> 8, 314 & 0xFF, 314 >> 8,
                                    104 & 0xFF, 104 >> 8, 96, 0 };
        memcpy(&b[n], opt, 9); n += 9;
        memcpy(&b[n], next, 9); n += 9;
        memcpy(&b[n], submit, 9); n += 9;
        count = 3;
    }
    b[1] = (uint8_t)count;
    send_event(PC_EVT_ZONES, b, n);
}

/* 带游标的读取器：任何越界都把 ok 置 0，调用方只计数丢弃，绝不越界写。 */
typedef struct {
    const uint8_t *p;
    uint16_t len;
    uint16_t i;
    int ok;
} card_rd_t;

static uint8_t rd_u8(card_rd_t *r)
{
    if (!r->ok || r->i >= r->len) { r->ok = 0; return 0; }
    return r->p[r->i++];
}

static uint16_t rd_u16(card_rd_t *r)
{
    uint16_t lo, hi;
    lo = rd_u8(r);
    hi = rd_u8(r);
    return (uint16_t)(lo | (hi << 8));
}

/* 去掉尾部被截断的半个 UTF-8 序列（不能把半个汉字塞给 LVGL） */
static void utf8_trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0) {
        size_t start = n - 1;
        uint8_t lead;
        size_t need;
        while (start > 0 && (((uint8_t)s[start]) & 0xC0) == 0x80) start--;
        lead = (uint8_t)s[start];
        need = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
        if (start + need <= n) return;
        s[start] = 0;
        n = start;
    }
}

/* 读一个 u8 长度前缀的字符串，截断到 cap-1 并保证 UTF-8 完整 */
static void rd_str(card_rd_t *r, char *out, int cap)
{
    int n, copy;
    out[0] = 0;
    n = rd_u8(r);
    if (!r->ok) return;
    if (n > (int)r->len - (int)r->i) { r->ok = 0; return; }
    copy = n < cap - 1 ? n : cap - 1;
    if (copy > 0) memcpy(out, &r->p[r->i], (size_t)copy);
    out[copy] = 0;
    utf8_trim(out);
    r->i = (uint16_t)(r->i + n);
}

static void card_session(const uint8_t *p, uint16_t len)
{
    card_rd_t r = { p, len, 1, 1 };
    pm_card_t c;

    memset(&c, 0, sizeof(c));
    c.kind = PC_CARD_SESSION;
    c.state = rd_u8(&r);
    c.steps = rd_u8(&r);
    c.ctx = (int)(int8_t)rd_u8(&r);
    rd_str(&r, c.tool, (int)sizeof(c.tool));
    rd_str(&r, c.title, (int)sizeof(c.title));
    rd_str(&r, c.footer, (int)sizeof(c.footer));
    if (!r.ok || c.state > PM_STATE_FAILED) {
        s_bad_frames++;
        send_card_ack(PC_ACK_CARD_BAD, PC_CARD_SESSION);
        return;
    }
    if (c.ctx < -1) c.ctx = -1;
    if (c.ctx > 100) c.ctx = 100;
    s_card_kind = PC_CARD_SESSION;
    pc_monitor_set_card(&c);
    send_card_zones();
    send_card_ack(PC_ACK_CARD, PC_CARD_SESSION);
}

static void card_approval(const uint8_t *p, uint16_t len)
{
    card_rd_t r = { p, len, 1, 1 };
    pm_card_t c;

    memset(&c, 0, sizeof(c));
    c.kind = PC_CARD_APPROVAL;
    c.can_allow = rd_u8(&r) ? 1 : 0;
    c.computer = rd_u8(&r) ? 1 : 0;
    c.countdown = rd_u8(&r);
    rd_str(&r, c.tool, (int)sizeof(c.tool));
    rd_str(&r, c.cmd, (int)sizeof(c.cmd));
    if (!r.ok) {
        s_bad_frames++;
        send_card_ack(PC_ACK_CARD_BAD, PC_CARD_APPROVAL);
        return;
    }
    if (c.countdown > 99) c.countdown = 99;
    s_card_kind = PC_CARD_APPROVAL;
    pc_monitor_set_card(&c);
    send_card_zones();
    send_card_ack(PC_ACK_CARD, PC_CARD_APPROVAL);
    printf("[pc] card APPROVAL can=%d computer=%d cd=%d tool=%s\r\n",
           c.can_allow, c.computer, c.countdown, c.tool);
}

static void card_question(const uint8_t *p, uint16_t len)
{
    card_rd_t r = { p, len, 1, 1 };
    pm_card_t c;

    memset(&c, 0, sizeof(c));
    c.kind = PC_CARD_QUESTION;
    c.q_index = rd_u8(&r);
    c.q_count = rd_u8(&r);
    c.q_multi = rd_u8(&r) ? 1 : 0;
    c.q_sel = rd_u16(&r);
    c.q_page = rd_u8(&r);
    c.q_optcount = rd_u8(&r);
    rd_str(&r, c.cmd, (int)sizeof(c.cmd));
    rd_str(&r, c.choice, (int)sizeof(c.choice));
    /* 0.4.19：问答超时移交电脑的标志。旧插件没有这一字节，默认 0。 */
    if (r.i < r.len) c.computer = rd_u8(&r) ? 1 : 0;
    if (c.q_page < 0 || c.q_page > 15) c.q_page = 0;
    if (!r.ok || c.q_count < 1 || c.q_index >= c.q_count) {
        s_bad_frames++;
        send_card_ack(PC_ACK_CARD_BAD, PC_CARD_QUESTION);
        return;
    }
    s_card_kind = PC_CARD_QUESTION;
    pc_monitor_set_card(&c);
    send_card_zones();
    send_card_ack(PC_ACK_CARD, PC_CARD_QUESTION);
    printf("[pc] card QUESTION %d/%d page=%d/%d multi=%d sel=0x%04X computer=%d\r\n",
           c.q_index + 1, c.q_count, c.q_page + 1, c.q_optcount, c.q_multi,
           (unsigned)c.q_sel, c.computer);
}

static void card_clear(void)
{
    s_card_kind = PC_CARD_OFF;
    pc_monitor_card_clear();
    send_card_zones();
    send_card_ack(PC_ACK_CARD, PC_CARD_OFF);
}

static void card_bitmap(const uint8_t *p, uint16_t len)
{
    card_rd_t r = { p, len, 1, 1 };
    int slot, w, h, index, total, bpp, data_len, rc;

    slot = rd_u8(&r);
    w = rd_u16(&r);
    h = rd_u16(&r);
    index = rd_u8(&r);
    total = rd_u8(&r);
    bpp = rd_u8(&r);
    if (!r.ok) {
        s_bad_frames++;
        send_card_ack(PC_ACK_BMP_BAD, (uint8_t)(slot & 0xFF));
        return;
    }
    /* w=h=total=0 是"清槽"（插件标题/题目变空时用），允许没有数据 */
    data_len = (int)r.len - (int)r.i;
    if (data_len < 0) data_len = 0;
    if (data_len > PC_BMP_CHUNK_MAX) {
        s_bad_frames++;
        send_card_ack(PC_ACK_BMP_BAD, (uint8_t)(slot & 0xFF));
        return;
    }
    rc = pc_monitor_card_bitmap(slot, w, h, index, total, bpp, &r.p[r.i], data_len);
    if (rc < 0) {
        s_bad_frames++;
        send_card_ack(PC_ACK_BMP_BAD, (uint8_t)(slot & 0xFF));
        return;
    }
    if (rc > 0) send_card_ack(PC_ACK_BMP_OK, (uint8_t)(slot & 0xFF));
}

static int on_frame(const uint8_t *p, uint16_t len)
{
    if (p == NULL || len < 1) {
        return -1;
    }
    s_rx_frames++;

    /* 卡片帧同样证明"电脑还活着"：BLE 下三环数值偶尔断 3 s 不能把页面打回离线 */
    if (p[0] >= PC_OP_SESSION && p[0] <= PC_OP_CARD_BMP) {
        pc_monitor_note_link();
    }

    /* 2026-11-08：本地弹窗期间只丢像素帧；0x91 状态、卡片帧、HELLO 等照常处理 */
    if (s_frames_suspended) {
        switch (p[0]) {
        case PC_OP_TILE_BEG:
        case PC_OP_TILE_DATA:
        case PC_OP_TILE_END:
        case PC_OP_CLEAR:
            s_ignored++;
            return 0;
        default:
            break;
        }
    }

    /* 电脑证明自己活着了：USB 下这时才冻结 LVGL 切透传 */
    if (s_await_first_frame && p[0] == PC_OP_HELLO) {
        s_await_first_frame = 0;
        passthrough_on();
    }

    switch (p[0]) {
    /* 像素类帧只认 USB：BLE 只承载"实时小数据 + 执行器"，
     * BLE 下界面复用现有监控页（本地 LVGL 渲染）。 */
    case PC_OP_TILE_BEG:
    case PC_OP_TILE_DATA:
    case PC_OP_TILE_END:
    case PC_OP_CLEAR:
        if (!s_passthrough) {
            s_ignored++;
            return 0;
        }
        break;
    default:
        break;
    }

    switch (p[0]) {
    case PC_OP_HELLO: {
        uint8_t r[6];
        r[0] = PC_PROTO_VER;
        r[1] = (uint8_t)(PC_TILE_W_MAX & 0xFF);
        r[2] = (uint8_t)(PC_TILE_W_MAX >> 8);
        r[3] = (uint8_t)(PC_TILE_H_MAX & 0xFF);
        r[4] = (uint8_t)(PC_TILE_H_MAX >> 8);
        r[5] = 2;                       /* RGB565 LE */
        send_event(PC_EVT_HELLO, r, sizeof(r));
        return 0;
    }
    case PC_OP_BRIGHT:
        if (len < 2) {
            break;
        }
        s_bright = p[1] < 32 ? 32 : p[1];
        power_set_monitor_policy(1, s_bright, s_always_on ? 1 : 0);
        return 0;
    case PC_OP_VIBRATE:
        if (len < 3) {
            break;
        }
        {
            uint16_t ms = rd16(&p[1]);
            if (ms) {
                motor_pulse(70, ms > 2000 ? 2000 : ms);
            }
        }
        return 0;
    case PC_OP_TILE_BEG:
        tile_begin(p, len);
        return 0;
    case PC_OP_TILE_DATA:
        tile_data(p, len);
        return 0;
    case PC_OP_TILE_END:
        tile_end();
        return 0;
    case PC_OP_CLEAR:
        if (len >= 4) {
            uint32_t rgb = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
            st7802_fill(0, 0, 479, 479, rgb);
        }
        return 0;
    case PC_OP_ALWAYS_ON:
        if (len < 2) { s_bad_frames++; return -1; }
        s_always_on = p[1] ? 1 : 0;
        power_set_monitor_policy(1, s_bright, s_always_on ? 1 : 0);
        return 0;
    case PC_OP_SCREEN_OFF: {
        if (len < 3) { s_bad_frames++; return -1; }
        uint16_t sec = (uint16_t)(p[1] | (p[2] << 8));
        power_set_monitor_idle_ms(sec ? (uint32_t)sec * 1000u : 0u);
        printf("[pc] screen-off timeout = %u s\r\n", (unsigned)sec);
        return 0;
    }

    case PC_OP_STATUS:
        send_status();
        return 0;

    /* ---- BLE 卡片层（无像素链路时的下半屏内容） ---- */
    case PC_OP_SESSION:
        card_session(p, len);
        return 0;
    case PC_OP_APPROVAL:
        card_approval(p, len);
        return 0;
    case PC_OP_QUESTION:
        card_question(p, len);
        return 0;
    case PC_OP_CARD_CLEAR:
        card_clear();
        return 0;
    case PC_OP_CARD_BMP:
        card_bitmap(p, len);
        return 0;
    default:
        break;
    }
    s_bad_frames++;
    return -1;
}

/* ------------------------------------------------------------ 触摸转发 */

/* 电脑模式：把触点实时送给电脑。tp_lvgl 的 hook 在触摸读回调里同步触发，
 * 与 LVGL 是否冻结无关，所以 USB 透传时也照常工作。
 * 帧：0x04 TOUCH [pressed, x_lo, x_hi, y_lo, y_hi]
 * 节流：状态变化立刻发；移动 ≥2 px 才发；上限约 60 帧/秒。 */
/* 触摸状态放到文件作用域：本地弹窗开始时需要给电脑补一个"抬起"（见
 * pc_touch_flush_release），所以不能继续藏在函数里。 */
static int s_touch_last_pressed = -1;
static uint16_t s_touch_last_x, s_touch_last_y;
static uint32_t s_touch_last_ms;

/* 组一帧 0x04 TOUCH 发给电脑（USB 会转成 @PCM1 FRAME 十六进制行） */
static void pc_touch_send(int pressed, uint16_t x, uint16_t y)
{
    uint8_t f[6];

    f[0] = 0x04;
    f[1] = pressed ? 1 : 0;
    f[2] = (uint8_t)(x & 0xFF);
    f[3] = (uint8_t)(x >> 8);
    f[4] = (uint8_t)(y & 0xFF);
    f[5] = (uint8_t)(y >> 8);
    send_event(0x50, f, sizeof(f));
}

/* 弹窗开始：如果电脑侧还按着，补一个抬起，避免它的 UI 卡在按下态 */
static void pc_touch_flush_release(void)
{
    if (s_touch_last_pressed == 1) {
        pc_touch_send(0, s_touch_last_x, s_touch_last_y);
        s_touch_last_pressed = 0;
    }
}

static void pc_touch_hook(int pressed, uint16_t x, uint16_t y, uint32_t taps)
{
    uint32_t now;
    int dx, dy;

    (void)taps;
    if (!s_active) {
        return;
    }
    /* 2026-11-08：本地弹窗期间触摸只走本地（通知卡点按关闭），不转发给电脑，
     * 否则用户为了关通知卡的那一下会点到 PC 界面上（可能是审批/问答按钮）。 */
    if (s_frames_suspended) {
        return;
    }
    now = lv_tick_get();
    if (pressed == s_touch_last_pressed) {
        dx = (int)x - (int)s_touch_last_x;
        dy = (int)y - (int)s_touch_last_y;
        if (dx > -2 && dx < 2 && dy > -2 && dy < 2) {
            return;
        }
        if ((uint32_t)(now - s_touch_last_ms) < 16) {
            return;                 /* 最多 ~60 Hz */
        }
    }
    s_touch_last_pressed = pressed;
    s_touch_last_x = x;
    s_touch_last_y = y;
    s_touch_last_ms = now;

    /* 卡片上的按下/抬起只做视觉反馈（高亮描边），插件才是判定方 */
    pc_monitor_card_touch(pressed, (int)x, (int)y);
    pc_touch_send(pressed, x, y);
}

/* ------------------------------------------------------- BLE 任务：入环 */

/* 非阻塞入环：0 = 已入队，-1 = 环满 */
static int stage_push(const uint8_t *p, uint16_t len)
{
    int head = s_ring_head;

    if ((head + 1) % PC_RING_N == s_ring_tail) {
        return -1;
    }
    s_ring[head].len = len;
    memcpy(s_ring[head].data, p, len);
    s_ring_head = (head + 1) % PC_RING_N;
    return 0;
}

void svc_pc_stage(const uint8_t *p, uint16_t len)
{
    if (!s_active || p == NULL || len == 0 || len > PC_CHUNK_MAX) {
        if (s_active) {
            s_ring_drop++;
        }
        return;
    }
    if (stage_push(p, len) != 0) {
        s_ring_drop++;                  /* 环满：丢，不阻塞 BLE 任务 */
    }
}

/* USB 任务专用：环满时让出 CPU 等 UI 任务抽走旧片，最多等 timeout_ms。
 *
 * 为什么需要它：实测电脑一次灌整带（65 片）时，不等待就会丢片 →
 * TILE_END 字节数不足 → 整块被丢、屏幕缺带。USB console 是普通任务
 * （不是 ISR），可以安全 delay；BLE 那条路径仍然必须非阻塞。
 *
 * 防卡死：等待期间 s_active 被清（用户左滑退出 / 掉线自动退）立刻返回；
 * 其余情况由 500 ms 硬上限兜底。**不要**再加"UI 多久没抽片"的软阈值：
 * 实测 UI 循环是 2–20 ms 一轮、单次 blit 还要等 TE，50 ms 软阈值会误判
 * 并丢帧（整屏 30 条带丢 17 片 / bad=9），已回退。
 * 返回 0 = 已入队，-1 = 被丢弃（已计入 ring_drop）。 */
int svc_pc_stage_wait(const uint8_t *p, uint16_t len, uint32_t timeout_ms)
{
    TickType_t t0;
    uint32_t elapsed;

    if (!s_active || p == NULL || len == 0 || len > PC_CHUNK_MAX) {
        if (s_active) {
            s_ring_drop++;
        }
        return -1;
    }
    t0 = xTaskGetTickCount();
    for (;;) {
        if (!s_active) {
            return -1;                  /* 已退出电脑模式：不再往死环里塞 */
        }
        if (stage_push(p, len) == 0) {
            return 0;
        }
        elapsed = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        if (elapsed > timeout_ms) {
            s_ring_drop++;
            return -1;
        }
        vTaskDelay(1);
    }
}

/* -------------------------------------------------------------- 模式切换 */

void svc_pc_enter(void)
{
    if (s_active) {
        return;
    }
    s_active = 1;
    s_frames_suspended = 0;
    s_popup_unfrozen = 0;
    s_transport = detect_transport();
    s_ring_head = s_ring_tail = 0;
    s_tile_open = 0;
    s_no_link = 0;
    s_host_watch_on = 0;
    printf("[pc] enter (transport=%d usb=%d)\r\n", s_transport,
           (int)usb_device_is_configured(0));
    /* USB 链路：CDC OUT 端点从这里开始归二进制帧（A5 5A len），shell 只收
     * 不以 0xA5 开头的包 —— 所以电脑模式下敲 pcmode 0 仍然有效。
     * 电脑侧收到 @PCM1 PC 1 后发 HELLO 触发透传；MODE 1 才是发像素的许可。 */
    usb_pc_link_set_active(s_transport == SVC_PC_LINK_USB);
    /* PC mode entry must make us discoverable: the PC is the BLE central. */
    ble_link_start_adv();
    printf("@PCM1 PC 1\r\n");
    power_set_monitor_policy(1, s_bright, s_always_on ? 1 : 0);
    if (!power_is_on()) {
        power_screen_on();
    }
    /* 先保持本地监控页；USB 下等电脑真的发来第一帧再冻结画面切透传。
     * 这样"电脑没开/插件没连"时不会黑屏，也避开开机瞬间的进出抖动。 */
    passthrough_off();
    s_await_first_frame = (s_transport == SVC_PC_LINK_USB);
    send_status();
}

void svc_pc_leave(void)
{
    if (!s_active) {
        return;
    }
    s_active = 0;
    /* 2026-11-28：常亮策略必须在最前面解掉。
     * 原来这行排在函数末尾（ble_link_stop 之后），中间夹着 printf /
     * passthrough_off() / usb_pc_link_set_active()，而同一轮 UI 循环里
     * pc_monitor_service() 在 svc_pc_tick() **之前**跑 —— 它看到 s_active 已经是 0，
     * 会用上一帧的 session/frame 重新算一次 policy，把常亮又设回去。
     * 结果就是"已经退出电脑模式了，屏还常亮、拔线放一分钟也不熄"。 */
    power_set_monitor_policy(0, s_bright, 0);
    s_host_watch_on = 0;
    s_frames_suspended = 0;
    s_popup_unfrozen = 0;
    s_no_link = 0;
    s_card_kind = PC_CARD_OFF;
    pc_monitor_card_clear();        /* 卡片是电脑模式的东西，退出即清 */
    passthrough_off();
    usb_pc_link_set_active(0);          /* 半截帧丢弃，CDC OUT 读取权还给 shell */
    printf("@PCM1 PC 0\r\n");
    /* 2026-11-14：退出电脑模式 = 蓝牙数字通道也结束（手表模式不保持连接/广播）。 */
    ble_link_stop();
    printf("[pc] leave (rx=%lu bad=%lu drop=%lu)\r\n",
           (unsigned long)s_rx_frames, (unsigned long)s_bad_frames,
           (unsigned long)s_ring_drop);
}

/* ---------------------------------------------------------------- 周期 */

/* 主机消失检测（见文件头 s_host_watch_* 注释）。返回 1 = 已退出电脑模式，调用方应直接返回。 */
static int pc_host_watch(uint32_t now)
{
    uint32_t in;
    uint32_t limit;

    /* 注意：这里**不能**再用 `!s_passthrough` 提前返回。
     * 2026-11-21 实机卡死两次：电脑退出/进程没了之后 passthrough 已经关掉，
     * 但手表仍停在电脑模式的监控页 —— 看门狗不跑、60 s 链路阀也不触发
     * （USB 枚举还在，svc_pc_link_up() 为真），于是画面静止、触摸全被转发给
     * 已经不存在的主机，用户只能靠退出/重进电脑模式自救。
     * 只要本端是 USB 链路就应该盯着 IN 完成计数；透传中 3 s，空闲态 10 s。 */
    if (s_transport != SVC_PC_LINK_USB) {
        s_host_watch_on = 0;
        return 0;
    }
    limit = s_passthrough ? PC_HOST_GONE_MS : PC_HOST_IDLE_MS;
    in = protorig_usb_console_in_events;
    if (!s_host_watch_on) {
        s_host_watch_on = 1;
        s_host_last_in = in;
        s_host_last_change_ms = now;
        return 0;
    }
    if (in != s_host_last_in) {
        s_host_last_in = in;
        s_host_last_change_ms = now;
        return 0;
    }
    if ((uint32_t)(now - s_host_last_change_ms) < limit) {
        return 0;
    }
    s_host_gone_count++;
    printf("[pc] host gone (no USB IN ack for %u ms) -> back to watch mode\r\n",
           (unsigned)limit);
    s_host_watch_on = 0;
    s_pending_ui_leave = 1;         /* 页面在 -1 屏时，下一拍回表盘 */
    svc_pc_leave();
    return 1;
}

void svc_pc_tick(void)
{
    int t;
    uint32_t now;
    int req;

    /* 0) BLE 的进/出请求（svc_pc_request）：切换必须在 UI 任务里做 */
    req = s_pending;
    if (req >= 0) {
        s_pending = -1;
        printf("[pc] request %s via BLE\r\n", req ? "ENTER" : "LEAVE");
        if (req) {
            /* 2026-11-14：蓝牙不能把表从手表模式强行拽进电脑模式；必须用户
             * 自己划到 -1 屏（那时 s_active 已经是 1）。 */
            if (!s_active) {
                printf("[pc] BLE enter ignored (watch mode; swipe to -1 first)\r\n");
                return;
            }
            eez_ui_monitor();       /* app_launch_page(4) -> page_monitor_load -> svc_pc_enter */

            /* BLE enter: if the watch already sits on page 4, eez_ui_monitor()
             * is a no-op -> page_monitor_load does not rerun -> svc_pc_enter never
             * runs -> no always-on -> screen sleeps in 15 s and BLE stops with it.
             * Seen on hardware: 19 s after unplug nothing was advertising. */
            if (!s_active) {
                svc_pc_enter();
            }
        } else {
            eez_ui_leave();         /* app_launch_page(0) -> page_monitor_close -> svc_pc_leave */
        }
        return;
    }

    if (!s_active) {
        /* 0b) shell `pcmode 0` 已经把电脑模式关掉，但 UI 还停在 -1 屏：
         *     在 UI 任务里把页面退回表盘（放在 if(!s_active) return 之前消费，
         *     否则 s_active=0 时永远走不到这段）。 */
        if (s_pending_ui_leave) {
            app_state_t st;
            s_pending_ui_leave = 0;
            app_state_get(&st);
            if (st.page_id == 4) {
                printf("[pc] shell leave -> back to face 0\r\n");
                eez_ui_leave();     /* app_launch_page(0) -> page_monitor_close */
            }
        }
        return;
    }

    /* 1) 先把 BLE 任务塞进来的片全部处理掉（不受 100 ms 节流） */
    while (s_ring_tail != s_ring_head) {
        int tail = s_ring_tail;
        (void)on_frame(s_ring[tail].data, s_ring[tail].len);
        s_ring_tail = (tail + 1) % PC_RING_N;
        s_ring_drained++;               /* 给 stage_wait 的"UI 还在干活"证据 */
    }

    now = lv_tick_get();
    if (now - s_tick_ms < 100) {
        return;
    }
    s_tick_ms = now;

    /* 2) 链路变化 */
    t = detect_transport();
    /* 拔线：configured 不会立刻清，用"总线挂起持续 500 ms"判掉线（见 usb_link_ok） */
    if (t == SVC_PC_LINK_USB && !usb_link_ok(now)) {
        t = ble_link_is_connected() ? SVC_PC_LINK_BLE : SVC_PC_LINK_NONE;
    }
    if (t != s_transport) {
        printf("[pc] transport %d -> %d\r\n", s_transport, t);
        s_transport = t;
        /* 链路换了就等于帧边界重新开始：清掉半截帧；只有 USB 才接管 RX */
        usb_pc_link_set_active(t == SVC_PC_LINK_USB);
        /* 插拔 USB / BLE 连断都要重新告诉电脑："现在是/不是二进制模式"，
         * 否则拔线重插后电脑不知道可以重新握手（固件不能假设电脑一直在听）。 */
        printf("@PCM1 PC %d\r\n", (t == SVC_PC_LINK_USB) ? 1 : 0);
        if (t == SVC_PC_LINK_USB) {
            passthrough_off();      /* 等新链路的第一帧 */
            s_await_first_frame = 1;
        } else {
            s_await_first_frame = 0;
            passthrough_off();      /* BLE 或掉线：回本地监控页 */
        }
        s_no_link = 0;
        send_status();
        return;
    }

    /* 2a) 主机消失检测：PC 端进程被杀后自动退出电脑模式（不等 60 s 掉线阀） */
    if (pc_host_watch(now)) {
        return;
    }

    /* 2b) 掉线安全阀：电脑模式息屏后仍保留会话并禁止深睡，断线后一直挂着会耗电。
     * 无链路持续 PC_LINK_LOST_EXIT_MS 就自己退回表盘，
     * 用户要再用电脑模式就重新滑进去 —— 不会卡死，也不会把电耗光。 */
    if (!svc_pc_link_up()) {
        if (!s_no_link) {
            s_no_link = 1;
            s_no_link_since = now;
        } else if ((uint32_t)(now - s_no_link_since) > PC_LINK_LOST_EXIT_MS) {
            printf("[pc] no link for %u ms, leaving pc mode\r\n",
                   (unsigned)PC_LINK_LOST_EXIT_MS);
            s_no_link = 0;
            eez_ui_leave();         /* 停在 -1 屏：走 page close -> svc_pc_leave */
            if (s_active) {
                svc_pc_leave();     /* 不是从 -1 屏进来的（shell pcmode 1）：直接退 */
            }
            return;
        }
    } else {
        s_no_link = 0;
    }

    /* 3) 常亮策略重申（用户可能在设置里改过亮度/息屏）。
     *
     * 2026-11-28：以前这里写死 active=1，只认 s_active —— `pcmode 1` 直达、
     * 或页面切换漏掉 close 时，会话在而监控页不在，这行就会把屏一直钉成常亮
     * （实测症状：BLE 连着、屏幕停在表盘 0、拔线放一分钟也不熄；进出一遍
     * "蓝牙手表模式"才恢复）。常亮必须跟着**页面**走，不能只看会话。 */
    {
        int live = pc_monitor_page_active();
        power_set_monitor_policy(live, s_bright, (live && s_always_on) ? 1 : 0);
    }

    /* 3b) 状态对账：会话在、监控页不在，宽限 3 s 后把 UI 拉回监控页（只试一次）。
     * 拉不回来就只告警，**不**强行退会话 —— shell `pcmode 1` / 台架脚本要用这个组合。
     * svc_pc_tick 跑在 UI 任务里，调 eez_ui_monitor() 是安全的（2b 的 eez_ui_leave 同理）。 */
    if (pc_monitor_page_active() || s_passthrough) {
        s_page_gap_since = 0;
        s_page_gap_fix_tried = 0;
        s_page_gap_warned = 0;
    } else if (!s_page_gap_since) {
        s_page_gap_since = now ? now : 1;
    } else if (!s_page_gap_fix_tried &&
               (uint32_t)(now - s_page_gap_since) > 3000u) {
        s_page_gap_fix_tried = 1;
        printf("[pc] WARN active=%d but monitor page not shown -> show it\r\n", s_active);
        eez_ui_monitor();
    } else if (s_page_gap_fix_tried && !s_page_gap_warned &&
               (uint32_t)(now - s_page_gap_since) > 6000u) {
        s_page_gap_warned = 1;
        printf("[pc] WARN monitor page still not shown (active=%d); always-on released\r\n",
               s_active);
    }

    /* 4) 1 s 一次状态，让电脑知道手表还活着 */
    if (svc_pc_link_up() && (now - s_last_status_ms) > 1000) {
        send_status();
    }
}

/* ---------------------------------------------------------------- shell */

int svc_pc_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("[pc] active=%d transport=%d passthrough=%d usbraw=%d bright=%u rx=%lu "
               "bad=%lu ignored=%lu ring_drop=%lu tile_open=%d cfg=%d susp=%d susp_ticks=%lu "
               "frames_susp=%d popup=%d host_gone=%lu\r\n",
               s_active, s_transport, s_passthrough, protorig_usb_pc_active,
               (unsigned)s_bright,
               (unsigned long)s_rx_frames, (unsigned long)s_bad_frames,
               (unsigned long)s_ignored, (unsigned long)s_ring_drop, s_tile_open,
               (int)usb_device_is_configured(0), (int)usb_device_is_suspend(0),
               (unsigned long)s_usb_susp_ticks,
               s_frames_suspended, s_popup_unfrozen,
               (unsigned long)s_host_gone_count);
        return 0;
    }
    if (!strcmp(argv[1], "1")) {
        svc_pc_enter();
        /* 2026-11-21: echo the execution result back to the host.
         * The host could not tell "command never arrived" from "arrived but was
         * a no-op" (svc_pc_enter() returns early when s_active is already 1),
         * so a pcmode 1 could silently do nothing. Print the resulting state
         * so the PC side can verify instead of blindly resending. */
        printf("[pc] cmd pcmode 1 active=%d\r\n", s_active);
    } else if (!strcmp(argv[1], "0")) {
        int was_active = s_active;
        svc_pc_leave();
        /* 2026-11-09 P0：pcmode 0 之后 UI 原来停在 -1 屏 → 让 UI 任务退回表盘。 */
        if (was_active) {
            s_pending_ui_leave = 1;
        }
        printf("[pc] cmd pcmode 0 active=%d\r\n", s_active);
    } else {
        printf("usage: pcmode [0|1]\r\n");
        return -1;
    }
    return 0;
}
