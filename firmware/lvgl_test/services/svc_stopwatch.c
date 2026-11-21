/*
 * svc_stopwatch.c —— 秒表服务实现（2026-11-08）
 *
 * 只用单调毫秒，不读 RTC：
 *   s_acc_ms   暂停/开始之前累计的部分；
 *   s_start_ms RUNNING 时这一段的起点；
 *   s_now_ms   最近一次 tick 传入的时间戳。
 */
#include <stdio.h>

#include "svc_stopwatch.h"

static svc_stopwatch_state_t s_state;
static uint32_t s_acc_ms;
static uint32_t s_start_ms;
static uint32_t s_now_ms;
static uint32_t s_elapsed_ms;

static uint32_t saturate(uint32_t ms)
{
    return (ms > SVC_STOPWATCH_MAX_MS) ? SVC_STOPWATCH_MAX_MS : ms;
}

static void recompute(void)
{
    uint32_t run;

    if (s_state == SVC_STOPWATCH_RUNNING) {
        run = (uint32_t)(s_now_ms - s_start_ms);
        s_elapsed_ms = saturate(s_acc_ms + run);
    } else {
        s_elapsed_ms = saturate(s_acc_ms);
    }
}

void svc_stopwatch_init(void)
{
    s_state = SVC_STOPWATCH_IDLE;
    s_acc_ms = 0;
    s_start_ms = 0;
    s_now_ms = 0;
    s_elapsed_ms = 0;
}

void svc_stopwatch_tick(uint32_t now_ms)
{
    s_now_ms = now_ms;
    recompute();
}

void svc_stopwatch_toggle(void)
{
    switch (s_state) {
    case SVC_STOPWATCH_IDLE:
        s_acc_ms = 0;
        s_start_ms = s_now_ms;
        s_state = SVC_STOPWATCH_RUNNING;
        break;
    case SVC_STOPWATCH_RUNNING:
        /* 暂停：把这一段折进累计值 */
        s_acc_ms = s_elapsed_ms;
        s_state = SVC_STOPWATCH_PAUSED;
        break;
    case SVC_STOPWATCH_PAUSED:
    default:
        s_start_ms = s_now_ms;
        s_state = SVC_STOPWATCH_RUNNING;
        break;
    }
    recompute();
    printf("[stopwatch] %s elapsed=%lu ms\r\n",
           (s_state == SVC_STOPWATCH_RUNNING) ? "run" : "pause",
           (unsigned long)s_elapsed_ms);
}

void svc_stopwatch_reset(void)
{
    s_state = SVC_STOPWATCH_IDLE;
    s_acc_ms = 0;
    s_start_ms = s_now_ms;
    s_elapsed_ms = 0;
    printf("[stopwatch] reset\r\n");
}

svc_stopwatch_state_t svc_stopwatch_state(void)
{
    return s_state;
}

uint32_t svc_stopwatch_elapsed_ms(void)
{
    return s_elapsed_ms;
}

void svc_stopwatch_format(char *out, uint32_t cap)
{
    uint32_t ms = s_elapsed_ms;
    uint32_t mm, ss, cc;

    if (out == NULL || cap == 0) {
        return;
    }
    mm = ms / 60000u;
    ss = (ms / 1000u) % 60u;
    cc = (ms % 1000u) / 10u;
    snprintf(out, cap, "%02lu:%02lu.%02lu",
             (unsigned long)mm, (unsigned long)ss, (unsigned long)cc);
}
