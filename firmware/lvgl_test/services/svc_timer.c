/*
 * svc_timer.c —— 倒计时服务实现（2026-11-08）
 *
 * 时间基准与秒表一致：只用单调毫秒（lv_tick_get），不读 RTC。
 *   s_duration_ms 点"开始"那一刻锁定的总时长；
 *   s_acc_ms      暂停之前已经走过的部分；
 *   s_start_ms    RUNNING 时这一段的起点。
 * 剩余 = duration - (acc + (now - start))。
 */
#include <stdio.h>

#include "svc_timer.h"

/* 预设档位（用户 2026-11-08：至少 1/3/5/10/30 分钟） */
static const uint32_t s_presets[] = {1u, 3u, 5u, 10u, 30u};
#define PRESET_N ((int)(sizeof(s_presets) / sizeof(s_presets[0])))

static svc_timer_state_t s_state;
static uint32_t s_preset_min = 5u;
static uint32_t s_duration_ms;
static uint32_t s_acc_ms;
static uint32_t s_start_ms;
static uint32_t s_now_ms;
static uint32_t s_remaining_ms;
static int s_expire_pending;

static uint32_t preset_ms(void)
{
    return s_preset_min * 60000u;
}

static void recompute(void)
{
    uint32_t used;

    switch (s_state) {
    case SVC_TIMER_RUNNING:
        used = s_acc_ms + (uint32_t)(s_now_ms - s_start_ms);
        s_remaining_ms = (used >= s_duration_ms) ? 0u : (s_duration_ms - used);
        break;
    case SVC_TIMER_PAUSED:
        used = s_acc_ms;
        s_remaining_ms = (used >= s_duration_ms) ? 0u : (s_duration_ms - used);
        break;
    case SVC_TIMER_DONE:
        s_remaining_ms = 0u;
        break;
    case SVC_TIMER_IDLE:
    default:
        s_remaining_ms = preset_ms();
        break;
    }
}

void svc_timer_init(void)
{
    s_state = SVC_TIMER_IDLE;
    s_preset_min = 5u;
    s_duration_ms = preset_ms();
    s_acc_ms = 0;
    s_start_ms = 0;
    s_now_ms = 0;
    s_expire_pending = 0;
    recompute();
}

int svc_timer_tick(uint32_t now_ms)
{
    svc_timer_update(now_ms);
    if (s_expire_pending) {
        s_expire_pending = 0;
        return 1;
    }
    return 0;
}

void svc_timer_update(uint32_t now_ms)
{
    s_now_ms = now_ms;
    recompute();
    if (s_state != SVC_TIMER_RUNNING || s_remaining_ms > 0u) {
        return;
    }
    /* 到点：状态先切 DONE（页面立刻显示 00:00），事件留给 tick 报一次 */
    s_state = SVC_TIMER_DONE;
    s_acc_ms = s_duration_ms;
    s_expire_pending = 1;
    printf("[timer] expired (%lu min)\r\n", (unsigned long)s_preset_min);
}

void svc_timer_toggle(void)
{
    switch (s_state) {
    case SVC_TIMER_RUNNING:
        s_acc_ms += (uint32_t)(s_now_ms - s_start_ms);
        s_state = SVC_TIMER_PAUSED;
        break;
    case SVC_TIMER_PAUSED:
        s_start_ms = s_now_ms;
        s_state = SVC_TIMER_RUNNING;
        break;
    case SVC_TIMER_IDLE:
    case SVC_TIMER_DONE:
    default:
        s_duration_ms = preset_ms();
        s_acc_ms = 0;
        s_start_ms = s_now_ms;
        s_state = SVC_TIMER_RUNNING;
        break;
    }
    recompute();
    printf("[timer] %s remaining=%lu ms (preset %lu min)\r\n",
           (s_state == SVC_TIMER_RUNNING) ? "run" : "pause",
           (unsigned long)s_remaining_ms, (unsigned long)s_preset_min);
}

void svc_timer_cancel(void)
{
    s_state = SVC_TIMER_IDLE;
    s_acc_ms = 0;
    s_expire_pending = 0;
    s_start_ms = s_now_ms;
    recompute();
    printf("[timer] cancel -> preset %lu min\r\n", (unsigned long)s_preset_min);
}

void svc_timer_adjust(int delta_min)
{
    int32_t next = (int32_t)s_preset_min + delta_min;

    /* 运行中只允许改"下一轮"的预设，不打断当前倒计时 */
    if (next < (int32_t)SVC_TIMER_PRESET_MIN_MIN) {
        next = (int32_t)SVC_TIMER_PRESET_MIN_MIN;
    }
    if (next > (int32_t)SVC_TIMER_PRESET_MIN_MAX) {
        next = (int32_t)SVC_TIMER_PRESET_MIN_MAX;
    }
    if ((uint32_t)next == s_preset_min) {
        return;
    }
    s_preset_min = (uint32_t)next;
    if (s_state == SVC_TIMER_IDLE) {
        s_duration_ms = preset_ms();
    }
    recompute();
    printf("[timer] preset -> %lu min\r\n", (unsigned long)s_preset_min);
}

void svc_timer_next_preset(void)
{
    int i;

    for (i = 0; i < PRESET_N; i++) {
        if (s_presets[i] == s_preset_min) {
            s_preset_min = s_presets[(i + 1) % PRESET_N];
            break;
        }
    }
    if (i >= PRESET_N) {
        /* 自定义分钟数（+/- 调出来的）：切到第一个比它大的档位，没有就回到 1 分钟 */
        uint32_t next = s_presets[0];
        for (i = 0; i < PRESET_N; i++) {
            if (s_presets[i] > s_preset_min) {
                next = s_presets[i];
                break;
            }
        }
        s_preset_min = next;
    }
    if (s_state == SVC_TIMER_IDLE) {
        s_duration_ms = preset_ms();
    }
    recompute();
    printf("[timer] preset -> %lu min (cycle)\r\n", (unsigned long)s_preset_min);
}

uint32_t svc_timer_preset_min(void)
{
    return s_preset_min;
}

uint32_t svc_timer_remaining_ms(void)
{
    return s_remaining_ms;
}

svc_timer_state_t svc_timer_state(void)
{
    return s_state;
}

int svc_timer_is_running(void)
{
    return s_state == SVC_TIMER_RUNNING;
}

void svc_timer_format(char *out, uint32_t cap)
{
    uint32_t ms = s_remaining_ms;
    uint32_t mm, ss;

    if (out == NULL || cap == 0) {
        return;
    }
    mm = ms / 60000u;
    ss = (ms / 1000u) % 60u;
    snprintf(out, cap, "%02lu:%02lu", (unsigned long)mm, (unsigned long)ss);
}
