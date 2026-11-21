/*
 * svc_ble.c —— BLE 服务桥接（R4 第一里程碑）
 *
 * BLE 回调（ble_svc.c，host task）只把 ble_evt_t 放进自己的队列；
 * 本文件只在 UI 任务（main.c 主循环）里 drain 它，然后：
 *   - app_state_set_ble_connected()
 *   - svc_time_set()（CTS / 自定义 SET_TIME）
 *   - event_queue_push(EV_NOTIFY / EV_FIND / EV_TIME_SET / EV_BLE_*)
 * 绝不碰 LVGL。电池/时间都走服务读真实值，不读 app_state 时间（息屏会冻）。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "sd3010.h"
#include "svc_time.h"
#include "svc_battery.h"
#include "svc_ble.h"
#include "board_power.h"
#include "app_notify.h"

#include "app_state.h"
#include "event_queue.h"
#include "ble_svc.h"
#include "ble_link.h"
#include "pc_monitor.h"   /* BLE numeric channel -> local monitor page */
#include "svc_pc.h"       /* 广播只在电脑模式开（手表模式省电 + 手表主动权） */

#define SVC_BLE_SYNC_MS 1000u

static uint32_t s_last_sync_ms;
static int s_synced;
static int s_last_batt = -1;
static uint32_t s_cmd_count;
static uint32_t s_cts_count;

/* R2.1 power */
#define SVC_BLE_SMART_TIMEOUT_MS (5u * 60u * 1000u)
static int s_power_mode = 2;
static svc_ble_mode_persist_t s_persist_cb;
static uint32_t s_boot_ms;
static uint32_t s_last_disconnect_ms;
static int s_ever_connected;

/* R2.1-BMP：320x160 2bpp 最大 12,800 B；前 16 B 给 I2 调色板 */
#define SVC_BMP_PIX_MAX   12800u
#define SVC_BMP_BUF_SIZE  (SVC_BMP_PIX_MAX + 16u)
static uint8_t s_bmp_buf[SVC_BMP_BUF_SIZE];
static int s_bmp_active;
static int s_bmp_bad;
static uint8_t s_bmp_cat;
static uint16_t s_bmp_w, s_bmp_h, s_bmp_total, s_bmp_next_seq;
static uint8_t s_bmp_bpp;
static uint32_t s_bmp_got;
static uint32_t s_bmp_start_ms;
static uint16_t s_bmp_crc;

/* BLE Current Time 10 B -> sd3010_time_t */
static void ble_time_to_tm(const uint8_t v[10], sd3010_time_t *tm)
{
    uint16_t year;

    memset(tm, 0, sizeof(*tm));
    year = (uint16_t)(v[0] | ((uint16_t)v[1] << 8));
    tm->year = (uint8_t)((year >= 2000) ? (year - 2000) : 0);
    tm->month = v[2];
    tm->day = v[3];
    tm->hour = v[4];
    tm->min = v[5];
    tm->sec = v[6];
    /* BLE weekday 1=Mon..7=Sun；SD3010 0=Sun..6=Sat */
    tm->week = (uint8_t)(v[7] == 7 ? 0 : (v[7] ? v[7] : 0));
}

/* sd3010_time_t -> BLE Current Time 10 B */
static void tm_to_ble_time(const sd3010_time_t *tm, uint8_t v[10])
{
    uint16_t year = (uint16_t)(2000 + tm->year);
    uint8_t wd = tm->week;

    v[0] = (uint8_t)(year & 0xFF);
    v[1] = (uint8_t)((year >> 8) & 0xFF);
    v[2] = tm->month;
    v[3] = tm->day;
    v[4] = tm->hour;
    v[5] = tm->min;
    v[6] = tm->sec;
    v[7] = (uint8_t)(wd == 0 ? 7 : wd);   /* 0=Sun -> 7 */
    v[8] = 0;                             /* fractions */
    v[9] = 0;                             /* reason: manual */
}

static void push_event(event_type_t type, const uint8_t *data, uint16_t len)
{
    event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    if (data != NULL && len > 0) {
        if (len > EVENT_DATA_MAX) {
            len = EVENT_DATA_MAX;
        }
        memcpy(ev.data, data, len);
        ev.len = len;
    }
    (void)event_queue_push(&ev);
}

/* 手机写时间：CTS 和自定义 0x03 共用。只在 UI 任务调用。 */
static void apply_ble_time(const uint8_t v[10])
{
    sd3010_time_t tm;
    uint8_t cache[10];

    ble_time_to_tm(v, &tm);
    if (svc_time_set(&tm) < 0) {
        printf("[ble] time set FAILED\r\n");
        return;
    }
    printf("[ble] time set %04d-%02d-%02d %02d:%02d:%02d (week %u)\r\n",
           2000 + tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec, tm.week);

    memcpy(cache, v, sizeof(cache));
    ble_svc_cts_cache_set(cache);
    push_event(EV_TIME_SET, v, 10);
}

static void handle_notify(const uint8_t *payload, uint8_t len)
{
    uint8_t cat;
    uint8_t tlen;

    if (len < 3) {
        printf("[ble] NOTIFY too short (%u)\r\n", len);
        return;
    }
    cat = payload[0];
    tlen = payload[1];
    if ((uint16_t)(2u + tlen + 1u) > len || payload[2 + tlen] != 0) {
        printf("[ble] NOTIFY bad title (cat=%u tlen=%u len=%u)\r\n", cat, tlen, len);
        return;
    }
    /* 原始 payload（含 cat/tlen/title/0/body）交给 UI，UI 再切标题/正文 */
    push_event(EV_NOTIFY, payload, len);
}

void svc_ble_init(void)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    s_boot_ms = now;
    s_last_disconnect_ms = now;
    s_ever_connected = 0;
    board_power_set_ble_policy_cb(svc_ble_pds_allowed);
    ble_svc_set_power_mode(s_power_mode);
    s_last_batt = -1;
    s_last_sync_ms = 0;
    s_synced = 0;
    s_cmd_count = 0;
    s_cts_count = 0;
    ble_link_start();
    /* 2026-11-14：手表模式默认不广播（用户要求：划到 -1 屏才让蓝牙握手；
     * 手表模式维持自己的电源策略，不因蓝牙多看/多连而耗电）。PC 模式进入时
     * svc_pc_enter() 会重新 ble_link_start_adv()。 */
    ble_link_stop();
    printf("[ble] svc init (svc_ble bridge ready, adv off in watch mode)\r\n");
}

void svc_ble_battery_update(int pct)
{
    if (pct < 0) {
        return;
    }
    if (pct == s_last_batt) {
        return;
    }
    s_last_batt = pct;
    ble_svc_battery_update((uint8_t)(pct > 100 ? 100 : pct));
}

static void sync_periodic(uint32_t now_ms)
{
    sd3010_time_t tm;

    if (s_synced && (uint32_t)(now_ms - s_last_sync_ms) < SVC_BLE_SYNC_MS) {
        return;
    }
    s_last_sync_ms = now_ms;
    s_synced = 1;

    if (svc_time_get(&tm) == 0) {
        uint8_t v[10];
        tm_to_ble_time(&tm, v);
        ble_svc_cts_cache_set(v);
    } else {
        /* 读失败也不清缓存：避免 CTS 回读突然变成 0 */
    }

    {
        int mv = 0;
        int pct = 0;
        /* 第一次电池采样前 app_state 还是 0，别把 BAS 写成 0% */
        if (svc_battery_get(&mv, &pct, NULL) == 0 && mv > 0) {
            svc_ble_battery_update(pct);
        }
    }
}

/* ------------------------------------------------------------------ */
/* R2.1-BMP：位图通知状态机（0x21/22/23/24）                          */
/* ------------------------------------------------------------------ */

static uint16_t bmp_crc16_update(uint16_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t bmp_palette_size(void)
{
    return (s_bmp_bpp == 2) ? 16u : 8u;
}

static void bmp_palette_init(void)
{
    if (s_bmp_bpp == 2) {
        const uint8_t pal[16] = {
            0x00, 0x00, 0x00, 0xFF,
            0x3A, 0x3A, 0x3A, 0xFF,
            0x5B, 0xEB, 0xF2, 0xFF,
            0xEA, 0xF2, 0xF1, 0xFF,
        };
        memcpy(s_bmp_buf, pal, sizeof(pal));
    } else {
        const uint8_t pal[8] = {
            0x00, 0x00, 0x00, 0xFF,
            0xEA, 0xF2, 0xF1, 0xFF,
        };
        memcpy(s_bmp_buf, pal, sizeof(pal));
    }
}

static void bmp_reset(void)
{
    s_bmp_active = 0;
    s_bmp_bad = 0;
    s_bmp_got = 0;
    s_bmp_next_seq = 0;
}

static void bmp_begin(const uint8_t *p, uint8_t len)
{
    uint16_t w, h, total;
    uint8_t bpp;

    if (len != 8 || app_notify_bitmap_visible()) {
        ble_svc_send_ack(BLE_CMD_BMP_BEGIN, 1);
        return;
    }
    w = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));
    h = (uint16_t)(p[3] | ((uint16_t)p[4] << 8));
    bpp = p[5];
    total = (uint16_t)(p[6] | ((uint16_t)p[7] << 8));
    if (w == 0 || w > 320 || h == 0 || h > 160 || (bpp != 1 && bpp != 2) ||
        total != (uint16_t)(((uint32_t)w * h * bpp + 7u) / 8u) ||
        total > SVC_BMP_PIX_MAX) {
        ble_svc_send_ack(BLE_CMD_BMP_BEGIN, 1);
        return;
    }
    s_bmp_cat = p[0];
    s_bmp_w = w;
    s_bmp_h = h;
    s_bmp_bpp = bpp;
    s_bmp_total = total;
    s_bmp_got = 0;
    s_bmp_next_seq = 0;
    s_bmp_bad = 0;
    s_bmp_crc = 0xFFFF;
    s_bmp_active = 1;
    s_bmp_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bmp_palette_init();
    printf("[bmp] begin %ux%u %ubpp total=%u cat=%u\r\n", w, h, bpp, total, s_bmp_cat);
    ble_svc_send_ack(BLE_CMD_BMP_BEGIN, 0);
}

static void bmp_data(const uint8_t *p, uint8_t len)
{
    uint16_t seq;
    uint8_t n;

    if (!s_bmp_active || s_bmp_bad || len < 3) {
        return;
    }
    seq = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    n = p[2];
    if ((uint16_t)(3u + n) > len || n > 200) {
        s_bmp_bad = 1;
        return;
    }
    if (seq < s_bmp_next_seq) {
        return;
    }
    if (seq != s_bmp_next_seq || s_bmp_got + n > s_bmp_total) {
        s_bmp_bad = 1;
        return;
    }
    memcpy(s_bmp_buf + bmp_palette_size() + s_bmp_got, p + 3, n);
    s_bmp_crc = bmp_crc16_update(s_bmp_crc, p + 3, n);
    s_bmp_got += n;
    s_bmp_next_seq++;
}

static void bmp_end(const uint8_t *p, uint8_t len)
{
    uint16_t want = 0;

    if (len == 2) {
        want = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    }
    if (!s_bmp_active || s_bmp_bad || s_bmp_got != s_bmp_total ||
        (want != 0 && want != s_bmp_crc)) {
        printf("[bmp] end fail active=%d bad=%d got=%u/%u crc=%04X/%04X\r\n",
               s_bmp_active, s_bmp_bad, (unsigned)s_bmp_got, s_bmp_total,
               s_bmp_crc, want);
        ble_svc_send_ack(BLE_CMD_BMP_END, 1);
        bmp_reset();
        return;
    }
    if (app_notify_show_bitmap(s_bmp_cat, s_bmp_w, s_bmp_h, s_bmp_bpp,
                               s_bmp_buf, s_bmp_total) != 0) {
        ble_svc_send_ack(BLE_CMD_BMP_END, 1);
        bmp_reset();
        return;
    }
    ble_svc_send_ack(BLE_CMD_BMP_END, 0);
    push_event(EV_NOTIFY_BMP, NULL, 0);
    printf("[bmp] end ok %ux%u %ubpp total=%u\r\n", s_bmp_w, s_bmp_h, s_bmp_bpp, s_bmp_total);
    bmp_reset();
}

void svc_ble_bmp_feed(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (s_bmp_active && (uint32_t)(now - s_bmp_start_ms) > 5000u) {
        printf("[bmp] timeout; abort\r\n");
        bmp_reset();
    }
    switch (cmd) {
    case BLE_CMD_BMP_BEGIN:
        bmp_begin(payload, len);
        break;
    case BLE_CMD_BMP_DATA:
        bmp_data(payload, len);
        break;
    case BLE_CMD_BMP_END:
        bmp_end(payload, len);
        break;
    case BLE_CMD_BMP_ABORT:
        bmp_reset();
        break;
    default:
        break;
    }
}

/* Advertising self-heal: one failed restart after disconnect used to leave
 * the watch invisible until reboot. Retry every 2 s while idle. */
static void ble_adv_watchdog(void)
{
    static uint32_t s_last_ms;
    static int s_idle_rounds;
    uint32_t now = (uint32_t)xTaskGetTickCount();
    if ((uint32_t)(now - s_last_ms) < 2000u) {
        return;
    }
    s_last_ms = now;
    /* 手表模式：广播关、连接断（切回 -1 屏时 svc_pc_enter 会重开）。 */
    if (!svc_pc_active()) {
        if (ble_link_is_active()) {
            printf("[ble] watch mode: adv/conn off (BLE only in PC mode)\r\n");
            ble_link_stop();
        }
        s_idle_rounds = 0;
        return;
    }
    if (ble_link_is_connected()) {
        s_idle_rounds = 0;
        return;
    }
    /* 2026-11-08 修：只在广播真的没开时才补。
     * 原来的逻辑不看 ble_link_adv_on()，健康广播状态下也每 4 s 无条件
     * stop+start 一次（夜里相当于一直折腾 BLE 控制器，串口每 4 s 一条
     * "[ble] advertising as"）。现在健康时什么都不做；只有确实掉了才
     * 重试一次，仍不行再 force stop+start。 */
    if (ble_link_adv_on()) {
        s_idle_rounds = 0;
        return;
    }
    if (++s_idle_rounds >= 2) {
        s_idle_rounds = 0;
        ble_link_force_adv();
    } else {
        ble_link_start_adv();
    }
}

void svc_ble_tick(void)
{
    ble_adv_watchdog();
    /* Stale-link guard: if the peer vanished (Windows keeps the link object
     * alive), drop it after 8 s of silence so we advertise again. */
    if (ble_link_is_connected() && ble_link_idle_ms() > 8000u) {
        printf("[ble] idle 8s, dropping stale link\r\n");
        ble_link_drop_conn();
    }
    ble_evt_t ev;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    while (ble_svc_evt_pop(&ev)) {
        switch (ev.type) {
        case BLE_EVT_CONNECTED:
            s_ever_connected = 1;
            app_state_set_ble_connected(1);
            push_event(EV_BLE_CONNECTED, NULL, 0);
            printf("[ble] phone connected\r\n");
            break;

        case BLE_EVT_DISCONNECTED:
            s_last_disconnect_ms = now_ms;
            app_state_set_ble_connected(0);
            push_event(EV_BLE_DISCONNECTED, NULL, 0);
            printf("[ble] phone disconnected\r\n");
            break;

        case BLE_EVT_CTS_WRITE:
            s_cts_count++;
            apply_ble_time(ev.data);
            break;

        case BLE_EVT_COMMAND:
            s_cmd_count++;
            /* BLE numeric channel: no USB, LVGL draws the rings locally */
            if (ev.cmd == BLE_CMD_METRICS && ev.len >= 15) {
                const uint8_t *m = ev.data;
                int cpu = (int)(m[0] | (m[1] << 8));
                int gpu = (int)(m[2] | (m[3] << 8));
                int ram = (int)(m[4] | (m[5] << 8));
                int vram = (int)(m[6] | (m[7] << 8));
                int brand = m[8];
                uint32_t rx = (uint32_t)m[9] | ((uint32_t)m[10] << 8) | ((uint32_t)m[11] << 16) | ((uint32_t)m[12] << 24);
                uint32_t tx = (uint32_t)m[13] | ((uint32_t)m[14] << 8);
                pc_monitor_set_metrics(cpu, gpu, ram, vram, brand, rx, tx);
                break;
            }
            switch (ev.cmd) {
            case BLE_CMD_SET_TIME:
                if (ev.len == 10) {
                    apply_ble_time(ev.data);
                } else {
                    printf("[ble] SET_TIME bad len=%u\r\n", ev.len);
                }
                break;

            case BLE_CMD_NOTIFY:
                handle_notify(ev.data, ev.len);
                break;

            case BLE_CMD_FIND:
                push_event(EV_FIND, NULL, 0);
                printf("[ble] FIND\r\n");
                break;

            case BLE_CMD_SET_POWER_MODE:
                if (ev.len == 1 && ev.data[0] <= 2) {
                    (void)svc_ble_power_mode_set(ev.data[0]);
                } else {
                    printf("[ble] SET_POWER_MODE bad payload\r\n");
                }
                break;

            default:
                break;
            }
            break;

        default:
            break;
        }
    }

    sync_periodic(now_ms);
}


/* R2.1 power：连接模式 / 启动停止 / PDS 策略                          */
/* ------------------------------------------------------------------ */

void svc_ble_set_persist_cb(svc_ble_mode_persist_t cb)
{
    s_persist_cb = cb;
}

int svc_ble_power_mode_get(void)
{
    return s_power_mode;
}

void svc_ble_power_mode_load(int mode)
{
    if (mode < 0 || mode > 2) {
        mode = 2;
    }
    s_power_mode = mode;
    ble_svc_set_power_mode(mode);
    printf("[ble] power mode loaded = %d\r\n", mode);
}

int svc_ble_power_mode_set(int mode)
{
    if (mode < 0 || mode > 2) {
        return -1;
    }
    if (mode == 0 && !power_is_on()) {
        svc_ble_stop();
    }
    s_power_mode = mode;
    ble_svc_set_power_mode(mode);
    if (s_persist_cb != NULL) {
        (void)s_persist_cb((uint8_t)mode);
    }
    printf("[ble] power mode = %d\r\n", mode);
    return 0;
}

void svc_ble_start(void)
{
    ble_link_start_adv();
}

void svc_ble_stop(void)
{
    /* 省电模式的息屏动作不能断开电脑模式，退出电脑模式由 svc_pc_leave() 停链路。 */
    if (!svc_pc_active()) {
        ble_link_stop();
    }
}

int svc_ble_is_active(void)
{
    return ble_link_is_active();
}

int svc_ble_pds_allowed(void)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t since;

    if (s_power_mode == 0) {
        return 1;                       /* 省电：手表模式屏灭停 BLE；电脑会话另有 PDS 守卫 */
    }
    if (ble_link_is_connected()) {
        return 0;                       /* 常连/智能：连接中禁止 PDS */
    }
    if (s_power_mode == 1) {
        return !ble_link_is_active();   /* 常连：广播中也禁止 */
    }

    /* 智能：仅广播；自上次断开（或开机）起 5 min 无连接才允许 */
    since = s_ever_connected ? s_last_disconnect_ms : s_boot_ms;
    return (uint32_t)(now - since) >= SVC_BLE_SMART_TIMEOUT_MS;
}
