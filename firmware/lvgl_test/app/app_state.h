/*
 * app_state.h —— 全局状态快照（time/batt/ui 三域 seq；已编入，不是桩）
 *
 * 目的（本地 research/16 号方案 §4，不发布）：把"时间 / 电量 / 充电 / BLE / 屏幕 / 页面 / 表盘"
 * 这些会被多个服务与 UI 读写的状态集中到一个快照里，避免继续散落
 * 成 main.c 的 static 全局。
 *
 * 现状（2026-11-15）：main.c（init / `app` 诊断命令 / 屏幕状态）、services/svc_time、
 * svc_battery、svc_ble、apps/app_framework.c 都在读写。
 *
 * 并发模型：
 *   - 写：服务/UI 任务调用 setter，临界区内递增所属域的 seq；
 *   - 读：app_state_get() 一次拷贝整个快照，调用方比较对应域的 seq；
 *   - 三域 seq 互不牵连（评审修订）：电量 3 s 写入不会让时间 UI 整帧重画。
 *
 * 写入规则：
 *   - time_seq  ← 只有 svc_time 标；
 *   - batt_seq  ← 只有 svc_battery 标；
 *   - ui_seq    ← screen/page/face/ble/notify；page/face 唯一写入者 = app_launch_page()/app_launch_face()（apps/app_framework.c，仅 UI 任务）；
 *   - 服务之间不直接调用；禁止把 lv_* 或回调指针放进本结构。
 */
#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdint.h>

typedef struct {
    /* 三个独立域，各自递增；UI 只看自己关心的域 */
    uint32_t time_seq;      /* svc_time 写 */
    uint32_t batt_seq;      /* svc_battery 写 */
    uint32_t ui_seq;        /* screen/page/face/ble/notify 写 */

    /* 时间（RTC） */
    int year, mon, day, week;
    int hh, mm, ss;

    /* 电池 / 充电 */
    int battery_mv;
    int battery_pct;
    uint8_t charging;       /* 0/1 */
    uint8_t battery_level;  /* 低电等级 0/1/2（svc_battery 写） */

    /* 连接 / 显示 */
    uint8_t ble_connected;
    uint8_t screen_on;
    uint8_t page_id;        /* eez_ui 当前页 */
    uint8_t face_id;        /* 当前表盘 */
    uint8_t notify_count;   /* 未读通知数 */
} app_state_t;

/* 清零并把三个 seq 置 1；在调度器启动后的任务上下文调用一次。 */
void app_state_init(void);

/* 拷贝当前快照（临界区，原子 wrt setter）。 */
void app_state_get(app_state_t *out);

/* 用 *in 覆盖整个快照（三个 seq 各自递增，调用方无需维护）。 */
void app_state_set(const app_state_t *in);

/* 细粒度 setter（每个都是临界区 + 所属域 seq++）。 */
void app_state_set_time(int year, int mon, int day, int week,
                        int hh, int mm, int ss);
void app_state_set_battery(int mv, int pct, uint8_t charging, uint8_t level);
void app_state_set_screen(uint8_t on);
void app_state_set_page(uint8_t page_id);
void app_state_set_face(uint8_t face_id);
void app_state_set_ble_connected(uint8_t connected);
void app_state_notify_inc(void);
void app_state_notify_clear(void);

#endif /* APP_STATE_H */
