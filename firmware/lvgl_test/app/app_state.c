/*
 * app_state.c —— 全局状态快照实现（time/batt/ui 三域 seq；setter 全是临界区）
 */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app_state.h"

static app_state_t s_state;

void app_state_init(void)
{
    taskENTER_CRITICAL();
    memset(&s_state, 0, sizeof(s_state));
    s_state.time_seq = 1;
    s_state.batt_seq = 1;
    s_state.ui_seq = 1;
    taskEXIT_CRITICAL();
}

void app_state_get(app_state_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL();
    *out = s_state;
    taskEXIT_CRITICAL();
}

void app_state_set(const app_state_t *in)
{
    uint32_t t, b, u;

    if (in == NULL) {
        return;
    }
    taskENTER_CRITICAL();
    t = s_state.time_seq;
    b = s_state.batt_seq;
    u = s_state.ui_seq;
    s_state = *in;
    s_state.time_seq = t + 1;
    s_state.batt_seq = b + 1;
    s_state.ui_seq = u + 1;
    taskEXIT_CRITICAL();
}

void app_state_set_time(int year, int mon, int day, int week,
                        int hh, int mm, int ss)
{
    taskENTER_CRITICAL();
    s_state.year = year;
    s_state.mon = mon;
    s_state.day = day;
    s_state.week = week;
    s_state.hh = hh;
    s_state.mm = mm;
    s_state.ss = ss;
    s_state.time_seq++;
    taskEXIT_CRITICAL();
}

void app_state_set_battery(int mv, int pct, uint8_t charging, uint8_t level)
{
    taskENTER_CRITICAL();
    s_state.battery_mv = mv;
    s_state.battery_pct = pct;
    s_state.charging = charging ? 1 : 0;
    s_state.battery_level = level > 2 ? 2 : level;
    s_state.batt_seq++;
    taskEXIT_CRITICAL();
}

void app_state_set_screen(uint8_t on)
{
    taskENTER_CRITICAL();
    s_state.screen_on = on ? 1 : 0;
    s_state.ui_seq++;
    taskEXIT_CRITICAL();
}

void app_state_set_page(uint8_t page_id)
{
    taskENTER_CRITICAL();
    s_state.page_id = page_id;
    s_state.ui_seq++;
    taskEXIT_CRITICAL();
}

void app_state_set_face(uint8_t face_id)
{
    taskENTER_CRITICAL();
    s_state.face_id = face_id;
    s_state.ui_seq++;
    taskEXIT_CRITICAL();
}

void app_state_set_ble_connected(uint8_t connected)
{
    taskENTER_CRITICAL();
    s_state.ble_connected = connected ? 1 : 0;
    s_state.ui_seq++;
    taskEXIT_CRITICAL();
}

void app_state_notify_inc(void)
{
    taskENTER_CRITICAL();
    s_state.notify_count++;
    s_state.ui_seq++;
    taskEXIT_CRITICAL();
}

void app_state_notify_clear(void)
{
    taskENTER_CRITICAL();
    s_state.notify_count = 0;
    s_state.ui_seq++;
    taskEXIT_CRITICAL();
}
