/*
 * [接口桩·未接线] svc_demo.h —— `demo` 命令接口桩（R3a，编进固件但没人调用）
 *
 * 现状（截至 2026-11-15）：CMakeLists 编入，main.c 只调 svc_demo_init()；
 * shell 里没有 `demo` 命令，也没有 UI 消费者调 svc_demo_tick()/pop_event()，
 * 所以固件跑起来本模块除 init 之外什么都不做。步骤执行
 * （face/notify/time/find/sleep/monitor）从未实现；当时的规格 video/demo-mode.md
 * 已不在仓库，现存的映射文档是 docs/DEMO_IMPLEMENTATION_MAP.md（本地规格）。
 *
 * 线程模型（设计意图，尚未接线）：
 *   - svc_demo_start/stop/lock 本来打算由 shell 任务调用（只置状态，不阻塞）；
 *   - svc_demo_tick(now_ms) 本来打算由 UI 任务 50 ms 调一次；
 *   - 需要 UI/马达/息屏的动作通过 demo_evt_t 交给 UI 任务，本模块不碰 LVGL。
 */
#ifndef SVC_DEMO_H
#define SVC_DEMO_H

#include <stdint.h>

typedef enum {
    DEMO_STEP_NONE = 0,
    DEMO_STEP_FACE = 1,         /* demo 1：表盘轮播 */
    DEMO_STEP_NOTIFY = 2,       /* demo 2：通知 */
    DEMO_STEP_TIME = 3,         /* demo 3：对时 */
    DEMO_STEP_FIND = 4,         /* demo 4：找表 */
    DEMO_STEP_SLEEP = 5,        /* demo 5：息屏唤醒 */
    DEMO_STEP_MONITOR = 6,      /* demo 6：监控页 */
    DEMO_STEP_SEQ_ALL = 100,    /* demo seq all（至今未实现，start 返回 -1） */
} demo_step_t;

typedef enum {
    DEMO_EVT_NONE = 0,
    DEMO_EVT_STEP,              /* 步骤开始/进行中（arg = 子步骤） */
    DEMO_EVT_DONE,              /* 步骤结束 */
    DEMO_EVT_ABORT,             /* demo 0 / 冲突中止 */
} demo_evt_type_t;

typedef struct {
    demo_evt_type_t type;
    demo_step_t step;
    uint32_t t_ms;              /* 相对 demo 开始的毫秒数 */
    uint8_t arg;                /* 子步骤（face idx / find buzz 序号等） */
} demo_evt_t;

void svc_demo_init(void);

/* 启动单个 demo 步骤；0 成功，-1 = 未知/未实现步骤（当前仅 SEQ_ALL 返回 -1）。 */
int svc_demo_start(demo_step_t step);

/* UI 任务调用：推进时钟并产生事件（不阻塞、不碰 LVGL）。 */
void svc_demo_tick(uint32_t now_ms);

/* demo 0：立即中止（后续 tick 不再产生事件）。 */
void svc_demo_stop(void);

int svc_demo_active(void);

/* 拍摄锁定（亮度 100/关息屏/PDS 抑制）；只保存一个标志，没有任何策略应用方
 * （截至 2026-11-15 未实现）。 */
int svc_demo_lock(int on);
int svc_demo_locked(void);

/* 取一条待处理事件；1 = 有，0 = 无。 */
int svc_demo_pop_event(demo_evt_t *out);

#endif /* SVC_DEMO_H */
