/*
 * pages.c —— 8 个前台页面 App（page_id 0..7，对应 eez_ui_glue.c 的 PAGE_N=8）
 *
 * load 统一做 eez_ui_goto_raw(page)：
 *   - settings：load 时 eez_ui_refresh_settings()，把亮度/息屏选中态刷新；
 *   - monitor ：load/close 调 pc_monitor_page_enter/exit + svc_pc_enter/leave，接管监控会话；
 *   - network/meets：只做 UI 所有权，不接 BLE（协议 v1 未接入，App 列表里 hidden=1 不显示），
 *     布局由 ui/device_pages.c 按 page 1/2 创建；
 *   - stopwatch/timer/alarm：load 再调 app_page_open()，布局在 ui/app_pages.c。
 */
#include <stdio.h>

#include "app_framework.h"
#include "eez_ui_glue.h"
#include "app_pages.h"
#include "pc_monitor.h"
#include "svc_pc.h"
#include "pages.h"

static void page_enter(int page, const char *id)
{
    printf("[app] %s load -> raw page %d\r\n", id, page);
    eez_ui_goto_raw(page);
}

static void page_close(const char *id)
{
    printf("[app] %s close\r\n", id);
}

/* ---- 0 表盘 ---- */
static void page_default_load(void) { page_enter(0, "default"); }
static void page_default_close(void) { page_close("default"); }

/* ---- 1 协议网络（只做 UI 所有权）---- */
static void page_network_load(void) { page_enter(1, "network"); }
static void page_network_close(void) { page_close("network"); }

/* ---- 2 协议网络记录（只做 UI 所有权）---- */
static void page_meets_load(void) { page_enter(2, "meets"); }
static void page_meets_close(void) { page_close("meets"); }

/* ---- 3 终端设置：load 刷新选中态 ---- */
static void page_settings_load(void)
{
    printf("[app] settings load -> refresh + raw page 3\r\n");
    eez_ui_refresh_settings();
    eez_ui_goto_raw(3);
}
static void page_settings_close(void) { page_close("settings"); }

/* ---- 4 USB 监控：load/close 接管会话可见性 ---- */
static void page_monitor_load(void)
{
    printf("[app] monitor load -> raw page 4 + session enter\r\n");
    eez_ui_goto_raw(4);
    pc_monitor_page_enter();
    /* 电脑模式：透传显示 / 强制亮屏不息屏 / USB>BLE 选路 */
    svc_pc_enter();
}
static void page_monitor_close(void)
{
    svc_pc_leave();
    pc_monitor_page_exit();
    page_close("monitor");
}

/* ---- 5 秒表 / 6 计时器 / 7 闹钟：2026-11-08 APP 第一批 ----
 * 状态在 services/svc_*，页面由 ui/app_pages.c 创建；这里只做页面所有权。 */
static void page_stopwatch_load(void)
{
    page_enter(APP_PAGE_STOPWATCH, "stopwatch");
    app_page_open(APP_PAGE_STOPWATCH);
}
static void page_stopwatch_close(void) { page_close("stopwatch"); }

static void page_timer_load(void)
{
    page_enter(APP_PAGE_TIMER, "timer");
    app_page_open(APP_PAGE_TIMER);
}
static void page_timer_close(void) { page_close("timer"); }

static void page_alarm_load(void)
{
    page_enter(APP_PAGE_ALARM, "alarm");
    app_page_open(APP_PAGE_ALARM);
}
static void page_alarm_close(void) { page_close("alarm"); }

#define DEFINE_PAGE_APP(fn, id_str, page_no, display_name, load_fn, close_fn) \
    static const app_desc_t s_##fn = {                                        \
        .id = id_str,                                                         \
        .name = display_name,                                                 \
        .has_gui = 1,                                                         \
        .allow_bg = 0,                                                        \
        .page_id = page_no,                                                   \
        .init = NULL,                                                         \
        .load = load_fn,                                                      \
        .close = close_fn,                                                    \
        .kill = NULL,                                                         \
        .power_off = NULL,                                                    \
        .event = NULL,                                                        \
    };                                                                        \
    void fn##_register(void) { (void)app_register(&s_##fn); }

DEFINE_PAGE_APP(page_default,  "default",  0, "表盘",         page_default_load,  page_default_close)
DEFINE_PAGE_APP(page_network,  "network",  1, "协议网络",     page_network_load,  page_network_close)
DEFINE_PAGE_APP(page_meets,    "meets",    2, "协议网络记录", page_meets_load,    page_meets_close)
DEFINE_PAGE_APP(page_settings, "settings", 3, "终端设置",     page_settings_load, page_settings_close)
DEFINE_PAGE_APP(page_monitor,  "monitor",  4, "USB 监控",     page_monitor_load,  page_monitor_close)
DEFINE_PAGE_APP(page_stopwatch, "stopwatch", APP_PAGE_STOPWATCH, "秒表",
                page_stopwatch_load, page_stopwatch_close)
DEFINE_PAGE_APP(page_timer, "timer", APP_PAGE_TIMER, "计时器",
                page_timer_load, page_timer_close)
DEFINE_PAGE_APP(page_alarm, "alarm", APP_PAGE_ALARM, "闹钟",
                page_alarm_load, page_alarm_close)

void pages_register(void)
{
    page_default_register();
    page_network_register();
    page_meets_register();
    page_settings_register();
    page_monitor_register();
    page_stopwatch_register();
    page_timer_register();
    page_alarm_register();
}
