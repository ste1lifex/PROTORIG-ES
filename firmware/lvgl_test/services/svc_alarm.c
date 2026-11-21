/*
 * svc_alarm.c —— 闹钟服务实现（2026-11-08）
 *
 * "每天重复"是唯一模式（alarm_flags bit0 = 使能），所以到点判定只看时/分，
 * 不比较星期/日期。
 */
#include <stdio.h>

#include "svc_alarm.h"

static svc_alarm_save_cb_t s_save_cb;
static svc_alarm_arm_cb_t s_arm_cb;

static uint8_t s_hour;
static uint8_t s_minute;
static uint8_t s_enabled;

/* "今天已经响过"：只在 RAM，重启会清空（见头文件说明） */
static int s_have_fired;
static int s_fired_yday;

/* 1..366（闰年 366）；月份天数表 */
static int day_of_year(const svc_alarm_time_t *t)
{
    static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int yday = t->day;
    int i;
    int leap = ((t->year % 4 == 0 && t->year % 100 != 0) || (t->year % 400 == 0));

    for (i = 1; i < (int)t->month && i <= 12; i++) {
        yday += mdays[i - 1];
        if (i == 2 && leap) {
            yday += 1;
        }
    }
    return yday;
}

static void persist(void)
{
    if (s_save_cb != NULL) {
        int r = s_save_cb(s_hour, s_minute, s_enabled);
        printf("[alarm] save %02u:%02u %s -> %d\r\n",
               (unsigned)s_hour, (unsigned)s_minute, s_enabled ? "on" : "off", r);
    }
}

void svc_alarm_init(uint8_t hour, uint8_t minute, int enabled)
{
    s_save_cb = NULL;
    s_arm_cb = NULL;
    s_hour = (hour > 23u) ? 7u : hour;
    s_minute = (minute > 59u) ? 0u : minute;
    s_enabled = enabled ? 1u : 0u;
    s_have_fired = 0;
    s_fired_yday = -1;
    printf("[alarm] init %02u:%02u %s\r\n",
           (unsigned)s_hour, (unsigned)s_minute, s_enabled ? "on" : "off");
}

void svc_alarm_set_save_cb(svc_alarm_save_cb_t cb)
{
    s_save_cb = cb;
}

void svc_alarm_set_arm_cb(svc_alarm_arm_cb_t cb)
{
    s_arm_cb = cb;
}

int svc_alarm_enabled(void)
{
    return s_enabled ? 1 : 0;
}

uint8_t svc_alarm_hour(void)
{
    return s_hour;
}

uint8_t svc_alarm_minute(void)
{
    return s_minute;
}

void svc_alarm_adjust_hour(int delta)
{
    int h = (int)s_hour + delta;

    while (h < 0) {
        h += 24;
    }
    h %= 24;
    if ((uint8_t)h == s_hour) {
        return;
    }
    s_hour = (uint8_t)h;
    s_have_fired = 0;           /* 改时间 = 重新算"今天响过" */
    s_fired_yday = -1;
    persist();
    svc_alarm_apply_rtc();
}

void svc_alarm_adjust_minute(int delta)
{
    int m = (int)s_minute + delta;

    while (m < 0) {
        m += 60;
    }
    m %= 60;
    if ((uint8_t)m == s_minute) {
        return;
    }
    s_minute = (uint8_t)m;
    s_have_fired = 0;
    s_fired_yday = -1;
    persist();
    svc_alarm_apply_rtc();
}

void svc_alarm_set_time(uint8_t hour, uint8_t minute)
{
    if (hour > 23u || minute > 59u) {
        return;
    }
    if (hour == s_hour && minute == s_minute) {
        return;
    }
    s_hour = hour;
    s_minute = minute;
    s_have_fired = 0;
    s_fired_yday = -1;
    printf("[alarm] time -> %02u:%02u\r\n", (unsigned)hour, (unsigned)minute);
    persist();
    svc_alarm_apply_rtc();
}

void svc_alarm_set_enabled(int enabled)
{
    uint8_t on = enabled ? 1u : 0u;

    if (on == s_enabled) {
        return;
    }
    s_enabled = on;
    s_have_fired = 0;
    s_fired_yday = -1;
    printf("[alarm] %s %02u:%02u\r\n", on ? "enable" : "disable",
           (unsigned)s_hour, (unsigned)s_minute);
    persist();
    svc_alarm_apply_rtc();
}

void svc_alarm_apply_rtc(void)
{
    if (s_arm_cb != NULL) {
        s_arm_cb(s_enabled ? 1 : 0, s_hour, s_minute);
    }
}

int svc_alarm_arm_for_sleep(void)
{
    if (!s_enabled) {
        return 0;
    }
    /* 睡前再写一次：怕醒来后的整分报警/别的路径覆盖过报警寄存器 */
    svc_alarm_apply_rtc();
    return 1;
}

static int check_due(const svc_alarm_time_t *now, const char *why)
{
    int yday;

    if (!s_enabled || now == NULL) {
        return 0;
    }
    if (now->hour != s_hour || now->min != s_minute) {
        return 0;
    }
    yday = day_of_year(now);
    if (s_have_fired && s_fired_yday == yday) {
        return 0;
    }
    s_have_fired = 1;
    s_fired_yday = yday;
    printf("[alarm] FIRE %s at %04u-%02u-%02u %02u:%02u:%02u\r\n", why,
           (unsigned)now->year, (unsigned)now->month, (unsigned)now->day,
           (unsigned)now->hour, (unsigned)now->min, (unsigned)now->sec);
    return 1;
}

int svc_alarm_poll(const svc_alarm_time_t *now)
{
    return check_due(now, "poll");
}

int svc_alarm_boot_check(const svc_alarm_time_t *now)
{
    return check_due(now, "boot");
}
