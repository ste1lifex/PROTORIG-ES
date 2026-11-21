/*
 * event_queue.h —— 离散事件队列（已编入，生产/消费两端都在跑）
 *
 * 方向：ISR / 服务 / BLE / shell -> event_t 队列 -> UI 任务消费。
 * 原则（本地 research/16 号方案 §4，不发布）：ISR 只投队列；服务不碰 LVGL；UI 单任务。
 *
 * 现状（2026-11-15）：
 *   生产者 = services/svc_time（EV_RTC_MINUTE / EV_TIME_SET）、
 *            svc_battery（EV_BATTERY / EV_CHARGING / EV_LOW_BATTERY）、
 *            svc_ble（EV_NOTIFY / EV_NOTIFY_BMP / EV_FIND / EV_BLE_*）；
 *   消费者 = main.c 的 UI 主循环，pop 到后 app_event_dispatch(&ev) 分给 App。
 * EV_TOUCH / EV_POWER_OFF / EV_PANEL_SLEEP / EV_DEMO / EV_UI_REFRESH 仍没有投递方，属预留。
 */
#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <stdint.h>

#define EVENT_QUEUE_LEN     16
#define EVENT_DATA_MAX      64

typedef enum {
    EV_NONE = 0,
    EV_TOUCH,               /* 触摸唤醒 / 点击（截至 2026-11-15 仍无投递方，预留） */
    EV_RTC_MINUTE,          /* 整分（R2c） */
    EV_BATTERY,             /* 电量变化（R2c） */
    EV_CHARGING,            /* 充电状态变化（R2c） */
    EV_LOW_BATTERY,         /* 低电等级 0/1/2 变化（R3c） */
    EV_BLE_CONNECTED,       /* R4 */
    EV_BLE_DISCONNECTED,    /* R4 */
    EV_NOTIFY,              /* 文本通知（R4，payload 按 06 定稿） */
    EV_NOTIFY_BMP,          /* 位图通知（R2.1，显示在 app_notify） */
    EV_FIND,                /* 找表（R4） */
    EV_TIME_SET,            /* 手机对时（R4） */
    EV_POWER_OFF,           /* 低电关机（R2c） */
    EV_PANEL_SLEEP,         /* panelsleep（R2c） */
    EV_DEMO,                /* demo 模式（R3/R6） */
    EV_UI_REFRESH,          /* 请求 UI 重画 */
} event_type_t;

typedef struct {
    event_type_t type;
    uint16_t len;                   /* data[] 已用字节数，0..64 */
    uint8_t data[EVENT_DATA_MAX];
} event_t;

typedef struct {
    uint32_t pushed;
    uint32_t popped;
    uint32_t dropped;               /* 队列满时丢最旧 */
    uint32_t depth;                 /* 当前积压 */
} event_queue_stats_t;

/* 创建队列；在调度器启动后的任务上下文调用一次。返回 0 成功。 */
int event_queue_init(void);

/* 任务上下文投递。队列满时丢最旧的一条并计数。返回 0 成功。 */
int event_queue_push(const event_t *ev);

/* ISR 上下文投递（xQueueSendFromISR；不阻塞）。返回 0 成功。 */
int event_queue_push_isr(const event_t *ev);

/* 非阻塞取一条。返回 1 = 拿到，0 = 空。只在 UI/消费任务里调用。 */
int event_queue_pop(event_t *out);

/* 统计快照（诊断用；计数器为普通累加，允许瞬时竞争）。 */
void event_queue_get_stats(event_queue_stats_t *out);

#endif /* EVENT_QUEUE_H */
