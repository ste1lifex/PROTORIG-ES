/*
 * [接口桩·未接线] svc_demo.c —— `demo` 接口桩实现（不碰 LVGL/马达/面板）
 *
 * 现状（截至 2026-11-15）：只有状态机骨架 + 每步时长 + STEP/DONE 事件 + lock 标志。
 * 编进固件，但 main.c 只调 svc_demo_init()：没有 shell 命令进来 start/tick，
 * 也没有 UI 消费者调 pop_event()，所以事件产生后没人取，屏幕上什么都不会发生。
 * 真正的动作（切表盘/弹层/对时/震动/息屏/监控页）本来要由 UI 任务消费 demo_evt_t
 * 后调用，这一段从未实现；本文件始终不直接调用任何 UI/硬件接口。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "svc_demo.h"

/* 每步总时长（ms），照 docs/DEMO_IMPLEMENTATION_MAP.md §3 的时序 */
static uint32_t demo_duration_ms(demo_step_t step)
{
    switch (step) {
    case DEMO_STEP_FACE:    return 9000;
    case DEMO_STEP_NOTIFY:  return 3400;
    case DEMO_STEP_TIME:    return 4000;
    case DEMO_STEP_FIND:    return 8000;
    case DEMO_STEP_SLEEP:   return 7000;   /* 软息屏降级版 */
    case DEMO_STEP_MONITOR: return 6000;
    default:                return 0;
    }
}

static const char *demo_step_name(demo_step_t step)
{
    switch (step) {
    case DEMO_STEP_FACE:    return "face";
    case DEMO_STEP_NOTIFY:  return "notify";
    case DEMO_STEP_TIME:    return "time";
    case DEMO_STEP_FIND:    return "find";
    case DEMO_STEP_SLEEP:   return "sleep";
    case DEMO_STEP_MONITOR: return "monitor";
    case DEMO_STEP_SEQ_ALL: return "seq";
    default:                return "none";
    }
}

static demo_step_t s_step;
static uint32_t s_t0;
static uint32_t s_now;
static int s_active;
static int s_locked;
static int s_done_sent;
static demo_evt_t s_pending;
static int s_has_pending;

void svc_demo_init(void)
{
    s_step = DEMO_STEP_NONE;
    s_t0 = 0;
    s_now = 0;
    s_active = 0;
    s_locked = 0;
    s_done_sent = 0;
    s_has_pending = 0;
    memset(&s_pending, 0, sizeof(s_pending));
}

int svc_demo_start(demo_step_t step)
{
    if (demo_duration_ms(step) == 0) {
        return -1;              /* NONE / SEQ_ALL（至今未实现） */
    }
    s_step = step;
    s_t0 = s_now;
    s_active = 1;
    s_done_sent = 0;
    s_has_pending = 0;
    printf("[DEMO] t=0 step=%s start (skeleton)\r\n", demo_step_name(step));

    s_pending.type = DEMO_EVT_STEP;
    s_pending.step = step;
    s_pending.t_ms = 0;
    s_pending.arg = 0;
    s_has_pending = 1;
    return 0;
}

void svc_demo_tick(uint32_t now_ms)
{
    uint32_t elapsed;
    uint32_t dur;

    s_now = now_ms;
    if (!s_active) {
        return;
    }
    dur = demo_duration_ms(s_step);
    elapsed = now_ms - s_t0;

    if (!s_done_sent && elapsed >= dur) {
        s_done_sent = 1;
        s_active = 0;
        s_pending.type = DEMO_EVT_DONE;
        s_pending.step = s_step;
        s_pending.t_ms = elapsed;
        s_pending.arg = 0;
        s_has_pending = 1;
        printf("[DEMO] t=%lu step=%s done\r\n",
               (unsigned long)elapsed, demo_step_name(s_step));
    }
}

void svc_demo_stop(void)
{
    if (s_active) {
        s_pending.type = DEMO_EVT_ABORT;
        s_pending.step = s_step;
        s_pending.t_ms = s_now - s_t0;
        s_pending.arg = 0;
        s_has_pending = 1;
    }
    s_active = 0;
    s_step = DEMO_STEP_NONE;
    printf("[DEMO] abort\r\n");
}

int svc_demo_active(void) { return s_active; }

int svc_demo_lock(int on)
{
    s_locked = on ? 1 : 0;
    printf("[DEMO] lock=%d (stub: no policy consumer)\r\n", s_locked);
    return 0;
}

int svc_demo_locked(void) { return s_locked; }

int svc_demo_pop_event(demo_evt_t *out)
{
    if (out == NULL || !s_has_pending) {
        return 0;
    }
    *out = s_pending;
    s_has_pending = 0;
    return 1;
}
