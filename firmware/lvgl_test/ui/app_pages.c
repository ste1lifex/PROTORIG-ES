/*
 * app_pages.c —— 秒表 / 计时器 / 闹钟三个原生 App 页（2026-11-08，用户第一批）
 *
 * 分工（本地 research/80 §2，不发布）：
 *   服务层 svc_stopwatch/svc_timer/svc_alarm 管状态与判定；
 *   本文件只管"画出来 + 把触摸翻成服务调用"，不保存业务状态。
 *
 * 布局约束：480x480 圆屏，安全半径 207。大号时间标签 320x56 放在 y=112，
 * 四个角到圆心的距离都 <= 205；操作行都收在 y<=400 的圆内（主机侧
 * tools/device_ui_test.c 的 check_labels() 会逐标签验证）。
 *
 * 触摸目标号（每页独立，1 = 返回键由 eez_ui_glue 处理）：
 *   秒表  10 开始/暂停/继续   11 复位
 *   计时器 10 开始/暂停/继续   11 取消   12 预设-   13 预设+   14 预设档位循环
 *   闹钟  10 时-   11 时+   12 分-   13 分+   14 闹钟开关（每天）
 */
#include <stdio.h>
#include <string.h>

#include "app_pages.h"
#include "ui_widgets.h"
#include "ui_theme.h"

#include "svc_alarm.h"
#include "svc_stopwatch.h"
#include "svc_timer.h"

/* ---------------- 秒表 ---------------- */
static struct {
    lv_obj_t *root;
    lv_obj_t *time;
    ui_row_t main;
    ui_row_t reset;
    uint32_t shown_ms;
    int shown_state;
    int inited;
} s_sw;

static const ui_rect_t s_sw_time_rect = {80, 112, 320, 56};
static const ui_rect_t s_sw_main_rect = {112, 208, 256, 96};
static const ui_rect_t s_sw_reset_rect = {112, 304, 256, 96};

/* ---------------- 计时器 ---------------- */
static struct {
    lv_obj_t *root;
    lv_obj_t *time;
    lv_obj_t *preset;
    ui_button_t minus;
    ui_button_t plus;
    ui_row_t main;
    ui_button_t cancel;
    char shown_time[16];
    uint32_t shown_preset;
    int shown_state;
    int inited;
} s_tm;

static const ui_rect_t s_tm_time_rect = {80, 112, 320, 56};
static const ui_rect_t s_tm_minus_rect = {112, 184, 64, 64};
static const ui_rect_t s_tm_preset_rect = {176, 192, 128, 48};
static const ui_rect_t s_tm_plus_rect = {304, 184, 64, 64};
static const ui_rect_t s_tm_main_rect = {112, 264, 256, 72};
static const ui_rect_t s_tm_cancel_rect = {112, 344, 256, 56};

/* ---------------- 闹钟 ---------------- */
static struct {
    lv_obj_t *root;
    lv_obj_t *time;
    lv_obj_t *hour;
    lv_obj_t *minute;
    ui_button_t hour_minus;
    ui_button_t hour_plus;
    ui_button_t min_minus;
    ui_button_t min_plus;
    ui_button_t repeat;
    uint8_t shown_hour;
    uint8_t shown_minute;
    int shown_enabled;
    int inited;
} s_al;

static const ui_rect_t s_al_time_rect = {80, 112, 320, 56};
static const ui_rect_t s_al_h_minus_rect = {112, 184, 56, 56};
static const ui_rect_t s_al_h_plus_rect = {320, 184, 56, 56};
static const ui_rect_t s_al_m_minus_rect = {112, 256, 56, 56};
static const ui_rect_t s_al_m_plus_rect = {320, 256, 56, 56};
static const ui_rect_t s_al_repeat_rect = {112, 332, 256, 64};

int app_page_is(int page)
{
    return page >= APP_PAGE_FIRST && page <= APP_PAGE_LAST;
}

/* ------------------------------------------------------------------ */
/* 秒表                                                                */
/* ------------------------------------------------------------------ */

static void stopwatch_refresh(int force)
{
    char buf[16];
    svc_stopwatch_state_t st;
    uint32_t ms;

    if (!s_sw.inited) {
        return;
    }
    /* 秒表是"读当前时间"型状态：页面刷新（33 ms 节拍）顺手把时间喂给服务，
     * 不产生任何事件副作用（到点事件只有计时器/闹钟有）。 */
    svc_stopwatch_tick(lv_tick_get());
    st = svc_stopwatch_state();
    ms = svc_stopwatch_elapsed_ms();
    if (force || ms != s_sw.shown_ms) {
        svc_stopwatch_format(buf, sizeof(buf));
        lv_label_set_text(s_sw.time, buf);
        s_sw.shown_ms = ms;
    }
    if (force || (int)st != s_sw.shown_state) {
        const char *title;
        const char *sub;

        s_sw.shown_state = (int)st;
        switch (st) {
        case SVC_STOPWATCH_RUNNING:
            title = "暂停";
            sub = "计时中";
            break;
        case SVC_STOPWATCH_PAUSED:
            title = "继续";
            sub = "已暂停";
            break;
        case SVC_STOPWATCH_IDLE:
        default:
            title = "开始";
            sub = "未开始";
            break;
        }
        ui_row_set(&s_sw.main, title, sub, 1, -1);
        ui_row_set(&s_sw.reset, "复位", "回到 00:00.00",
                   (ms > 0u || st != SVC_STOPWATCH_IDLE) ? 1 : 0, -1);
    }
}

static void stopwatch_create(lv_obj_t *parent)
{
    s_sw.root = ui_page_create(parent);
    ui_page_header(s_sw.root, "秒表");
    s_sw.time = ui_text(s_sw.root, "00:00.00", UI_FONT_HUGE, UI_COLOR_TEXT,
                        s_sw_time_rect.x, s_sw_time_rect.y, s_sw_time_rect.w,
                        LV_TEXT_ALIGN_CENTER);
    ui_row_create_at(&s_sw.main, s_sw.root, &s_sw_main_rect, UI_ICON_STOPWATCH);
    ui_row_create_at(&s_sw.reset, s_sw.root, &s_sw_reset_rect, UI_ICON_NONE);
    s_sw.shown_ms = 0xFFFFFFFFu;
    s_sw.shown_state = -1;
    s_sw.inited = 1;
    stopwatch_refresh(1);
}

/* ------------------------------------------------------------------ */
/* 计时器                                                              */
/* ------------------------------------------------------------------ */

static void timer_refresh(int force)
{
    char buf[16];
    char preset[24];
    svc_timer_state_t st;
    uint32_t preset_min;

    if (!s_tm.inited) {
        return;
    }
    /* 只更新、不报事件：到点事件由 main.c 的 tick 取走（会亮屏+震动+通知） */
    svc_timer_update(lv_tick_get());
    st = svc_timer_state();
    preset_min = svc_timer_preset_min();
    svc_timer_format(buf, sizeof(buf));
    if (force || strcmp(buf, s_tm.shown_time) != 0) {
        lv_label_set_text(s_tm.time, buf);
        strncpy(s_tm.shown_time, buf, sizeof(s_tm.shown_time) - 1u);
        s_tm.shown_time[sizeof(s_tm.shown_time) - 1u] = '\0';
    }
    if (force || preset_min != s_tm.shown_preset) {
        s_tm.shown_preset = preset_min;
        snprintf(preset, sizeof(preset), "%lu 分钟", (unsigned long)preset_min);
        lv_label_set_text(s_tm.preset, preset);
    }
    if (force || (int)st != s_tm.shown_state) {
        s_tm.shown_state = (int)st;
        switch (st) {
        case SVC_TIMER_RUNNING:
            ui_row_set(&s_tm.main, "暂停", "倒计时中", 1, -1);
            break;
        case SVC_TIMER_PAUSED:
            ui_row_set(&s_tm.main, "继续", "已暂停", 1, -1);
            break;
        case SVC_TIMER_DONE:
            ui_row_set(&s_tm.main, "重新开始", "时间到", 1, -1);
            break;
        case SVC_TIMER_IDLE:
        default:
            ui_row_set(&s_tm.main, "开始", "点按开始倒计时", 1, -1);
            break;
        }
        ui_button_set(&s_tm.cancel, "取消",
                      (st == SVC_TIMER_RUNNING || st == SVC_TIMER_PAUSED)
                          ? UI_COLOR_TEXT : UI_COLOR_MUTED);
    }
}

static void timer_create(lv_obj_t *parent)
{
    s_tm.root = ui_page_create(parent);
    ui_page_header(s_tm.root, "计时器");
    s_tm.time = ui_text(s_tm.root, "05:00", UI_FONT_HUGE, UI_COLOR_TEXT,
                        s_tm_time_rect.x, s_tm_time_rect.y, s_tm_time_rect.w,
                        LV_TEXT_ALIGN_CENTER);
    ui_button_create(&s_tm.minus, s_tm.root, "-", s_tm_minus_rect.x, s_tm_minus_rect.y,
                     s_tm_minus_rect.w, s_tm_minus_rect.h, UI_FONT_HEAD);
    s_tm.preset = ui_text(s_tm.root, "5 分钟", UI_FONT_HEAD, UI_COLOR_TEXT,
                          s_tm_preset_rect.x, s_tm_preset_rect.y + 8,
                          s_tm_preset_rect.w, LV_TEXT_ALIGN_CENTER);
    ui_button_create(&s_tm.plus, s_tm.root, "+", s_tm_plus_rect.x, s_tm_plus_rect.y,
                     s_tm_plus_rect.w, s_tm_plus_rect.h, UI_FONT_HEAD);
    ui_row_create_at(&s_tm.main, s_tm.root, &s_tm_main_rect, UI_ICON_TIMER);
    ui_button_create(&s_tm.cancel, s_tm.root, "取消", s_tm_cancel_rect.x,
                     s_tm_cancel_rect.y, s_tm_cancel_rect.w, s_tm_cancel_rect.h,
                     UI_FONT_TITLE);
    s_tm.shown_time[0] = '\0';
    s_tm.shown_preset = 0xFFFFFFFFu;
    s_tm.shown_state = -1;
    s_tm.inited = 1;
    timer_refresh(1);
}

/* ------------------------------------------------------------------ */
/* 闹钟                                                                */
/* ------------------------------------------------------------------ */

static void alarm_refresh(int force)
{
    char buf[16];
    uint8_t hour;
    uint8_t minute;
    int enabled;
    int changed = 0;

    if (!s_al.inited) {
        return;
    }
    hour = svc_alarm_hour();
    minute = svc_alarm_minute();
    enabled = svc_alarm_enabled();
    if (force || hour != s_al.shown_hour) {
        s_al.shown_hour = hour;
        snprintf(buf, sizeof(buf), "%02u", (unsigned)hour);
        lv_label_set_text(s_al.hour, buf);
        changed = 1;
    }
    if (force || minute != s_al.shown_minute) {
        s_al.shown_minute = minute;
        snprintf(buf, sizeof(buf), "%02u", (unsigned)minute);
        lv_label_set_text(s_al.minute, buf);
        changed = 1;
    }
    if (force || enabled != s_al.shown_enabled) {
        s_al.shown_enabled = enabled;
        changed = 1;
        ui_button_set(&s_al.repeat, enabled ? "闹钟开关 · 已开启" : "闹钟开关 · 已关闭",
                      enabled ? UI_COLOR_ACCENT : UI_COLOR_MUTED);
    }
    if (changed) {
        snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)hour, (unsigned)minute);
        lv_label_set_text(s_al.time, buf);
        lv_obj_set_style_text_color(s_al.time,
                                    lv_color_hex(enabled ? UI_COLOR_TEXT : UI_COLOR_MUTED), 0);
    }
}

static void alarm_create(lv_obj_t *parent)
{
    s_al.root = ui_page_create(parent);
    ui_page_header(s_al.root, "闹钟");
    s_al.time = ui_text(s_al.root, "07:30", UI_FONT_HUGE, UI_COLOR_TEXT,
                        s_al_time_rect.x, s_al_time_rect.y, s_al_time_rect.w,
                        LV_TEXT_ALIGN_CENTER);

    ui_button_create(&s_al.hour_minus, s_al.root, "-", s_al_h_minus_rect.x,
                     s_al_h_minus_rect.y, s_al_h_minus_rect.w, s_al_h_minus_rect.h,
                     UI_FONT_HEAD);
    ui_text(s_al.root, "小时", UI_FONT_TITLE, UI_COLOR_MUTED,
            176, 198, 56, LV_TEXT_ALIGN_LEFT);
    s_al.hour = ui_text(s_al.root, "07", UI_FONT_HUGE, UI_COLOR_TEXT,
                        232, s_al_h_minus_rect.y, 80, LV_TEXT_ALIGN_LEFT);
    ui_button_create(&s_al.hour_plus, s_al.root, "+", s_al_h_plus_rect.x,
                     s_al_h_plus_rect.y, s_al_h_plus_rect.w, s_al_h_plus_rect.h,
                     UI_FONT_HEAD);

    ui_button_create(&s_al.min_minus, s_al.root, "-", s_al_m_minus_rect.x,
                     s_al_m_minus_rect.y, s_al_m_minus_rect.w, s_al_m_minus_rect.h,
                     UI_FONT_HEAD);
    ui_text(s_al.root, "分钟", UI_FONT_TITLE, UI_COLOR_MUTED,
            176, 270, 56, LV_TEXT_ALIGN_LEFT);
    s_al.minute = ui_text(s_al.root, "30", UI_FONT_HUGE, UI_COLOR_TEXT,
                          232, s_al_m_minus_rect.y, 80, LV_TEXT_ALIGN_LEFT);
    ui_button_create(&s_al.min_plus, s_al.root, "+", s_al_m_plus_rect.x,
                     s_al_m_plus_rect.y, s_al_m_plus_rect.w, s_al_m_plus_rect.h,
                     UI_FONT_HEAD);

    ui_button_create(&s_al.repeat, s_al.root, "闹钟开关 · 已关闭", s_al_repeat_rect.x,
                     s_al_repeat_rect.y, s_al_repeat_rect.w, s_al_repeat_rect.h,
                     UI_FONT_TITLE);
    s_al.shown_hour = 0xFF;
    s_al.shown_minute = 0xFF;
    s_al.shown_enabled = -1;
    s_al.inited = 1;
    alarm_refresh(1);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

lv_obj_t *app_page_create(lv_obj_t *parent, int page)
{
    switch (page) {
    case APP_PAGE_STOPWATCH:
        stopwatch_create(parent);
        return s_sw.root;
    case APP_PAGE_TIMER:
        timer_create(parent);
        return s_tm.root;
    case APP_PAGE_ALARM:
        alarm_create(parent);
        return s_al.root;
    default:
        return NULL;
    }
}

void app_page_open(int page)
{
    switch (page) {
    case APP_PAGE_STOPWATCH: stopwatch_refresh(1); break;
    case APP_PAGE_TIMER:     timer_refresh(1);     break;
    case APP_PAGE_ALARM:     alarm_refresh(1);     break;
    default: break;
    }
}

void app_page_tick(int page)
{
    switch (page) {
    case APP_PAGE_STOPWATCH: stopwatch_refresh(0); break;
    case APP_PAGE_TIMER:     timer_refresh(0);     break;
    case APP_PAGE_ALARM:     alarm_refresh(0);     break;
    default: break;
    }
}

void app_page_tick_fast(int page)
{
    /* 秒表的百分秒只有它们需要 30 fps；计时器/闹钟 100 ms 足够 */
    if (page == APP_PAGE_STOPWATCH) {
        stopwatch_refresh(0);
    }
}

int app_page_target(int page, int x, int y)
{
    switch (page) {
    case APP_PAGE_STOPWATCH:
        if (ui_rect_hit(&s_sw_main_rect, x, y)) return 10;
        if (ui_rect_hit(&s_sw_reset_rect, x, y)) return 11;
        break;
    case APP_PAGE_TIMER:
        if (ui_rect_hit(&s_tm_main_rect, x, y)) return 10;
        if (ui_rect_hit(&s_tm_cancel_rect, x, y)) return 11;
        if (ui_rect_hit(&s_tm_minus_rect, x, y)) return 12;
        if (ui_rect_hit(&s_tm_plus_rect, x, y)) return 13;
        if (ui_rect_hit(&s_tm_preset_rect, x, y)) return 14;
        break;
    case APP_PAGE_ALARM:
        if (ui_rect_hit(&s_al_h_minus_rect, x, y)) return 10;
        if (ui_rect_hit(&s_al_h_plus_rect, x, y)) return 11;
        if (ui_rect_hit(&s_al_m_minus_rect, x, y)) return 12;
        if (ui_rect_hit(&s_al_m_plus_rect, x, y)) return 13;
        if (ui_rect_hit(&s_al_repeat_rect, x, y)) return 14;
        break;
    default:
        break;
    }
    return 0;
}

void app_page_press(int page, int target, int pressed)
{
    switch (page) {
    case APP_PAGE_STOPWATCH:
        if (target == 10) ui_row_press(&s_sw.main, pressed);
        else if (target == 11) ui_row_press(&s_sw.reset, pressed);
        break;
    case APP_PAGE_TIMER:
        if (target == 10) ui_row_press(&s_tm.main, pressed);
        else if (target == 11) ui_button_press(&s_tm.cancel, pressed);
        else if (target == 12) ui_button_press(&s_tm.minus, pressed);
        else if (target == 13) ui_button_press(&s_tm.plus, pressed);
        break;
    case APP_PAGE_ALARM:
        if (target == 10) ui_button_press(&s_al.hour_minus, pressed);
        else if (target == 11) ui_button_press(&s_al.hour_plus, pressed);
        else if (target == 12) ui_button_press(&s_al.min_minus, pressed);
        else if (target == 13) ui_button_press(&s_al.min_plus, pressed);
        else if (target == 14) ui_button_press(&s_al.repeat, pressed);
        break;
    default:
        break;
    }
}

void app_page_activate(int page, int target)
{
    switch (page) {
    case APP_PAGE_STOPWATCH:
        if (target == 10) {
            /* 先喂一次当前时间：按钮可能比第一次 33 ms 刷新更早被按到 */
            svc_stopwatch_tick(lv_tick_get());
            svc_stopwatch_toggle();
            stopwatch_refresh(1);
        } else if (target == 11) {
            svc_stopwatch_reset();
            stopwatch_refresh(1);
        }
        break;
    case APP_PAGE_TIMER:
        if (target == 10) {
            svc_timer_update(lv_tick_get());
            svc_timer_toggle();
            timer_refresh(1);
        } else if (target == 11) {
            svc_timer_cancel();
            timer_refresh(1);
        } else if (target == 12) {
            svc_timer_adjust(-1);
            timer_refresh(1);
        } else if (target == 13) {
            svc_timer_adjust(1);
            timer_refresh(1);
        } else if (target == 14) {
            svc_timer_next_preset();
            timer_refresh(1);
        }
        break;
    case APP_PAGE_ALARM:
        if (target == 10) {
            svc_alarm_adjust_hour(-1);
            alarm_refresh(1);
        } else if (target == 11) {
            svc_alarm_adjust_hour(1);
            alarm_refresh(1);
        } else if (target == 12) {
            svc_alarm_adjust_minute(-1);
            alarm_refresh(1);
        } else if (target == 13) {
            svc_alarm_adjust_minute(1);
            alarm_refresh(1);
        } else if (target == 14) {
            svc_alarm_set_enabled(!svc_alarm_enabled());
            alarm_refresh(1);
        }
        break;
    default:
        break;
    }
}
