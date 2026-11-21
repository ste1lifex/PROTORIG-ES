/*
 * event_queue.c —— 离散事件队列实现（FreeRTOS 队列；满时丢最旧）
 */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "event_queue.h"

static QueueHandle_t s_evt_q;
static volatile uint32_t s_pushed;
static volatile uint32_t s_popped;
static volatile uint32_t s_dropped;

int event_queue_init(void)
{
    if (s_evt_q != NULL) {
        return 0;
    }
    s_evt_q = xQueueCreate(EVENT_QUEUE_LEN, sizeof(event_t));
    return (s_evt_q != NULL) ? 0 : -1;
}

static void evt_drop_oldest(void)
{
    event_t old;

    if (xQueueReceive(s_evt_q, &old, 0) == pdTRUE) {
        s_dropped++;
    }
}

int event_queue_push(const event_t *ev)
{
    if (s_evt_q == NULL || ev == NULL) {
        return -1;
    }
    if (xQueueSend(s_evt_q, ev, 0) != pdTRUE) {
        /* 队列满：丢最旧，再放新的（本地 research/16 号方案 §4，不发布） */
        evt_drop_oldest();
        if (xQueueSend(s_evt_q, ev, 0) != pdTRUE) {
            s_dropped++;
            return -1;
        }
    }
    s_pushed++;
    return 0;
}

int event_queue_push_isr(const event_t *ev)
{
    BaseType_t woken = pdFALSE;

    if (s_evt_q == NULL || ev == NULL) {
        return -1;
    }
    if (xQueueSendFromISR(s_evt_q, ev, &woken) != pdTRUE) {
        /* ISR 里不做"丢最旧"的二次操作：宁可丢新事件，也不在 ISR 里循环 */
        s_dropped++;
        return -1;
    }
    s_pushed++;
    portYIELD_FROM_ISR(woken);
    return 0;
}

int event_queue_pop(event_t *out)
{
    if (s_evt_q == NULL || out == NULL) {
        return 0;
    }
    if (xQueueReceive(s_evt_q, out, 0) == pdTRUE) {
        s_popped++;
        return 1;
    }
    return 0;
}

void event_queue_get_stats(event_queue_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->pushed = s_pushed;
    out->popped = s_popped;
    out->dropped = s_dropped;
    out->depth = (s_evt_q != NULL) ? (uint32_t)uxQueueMessagesWaiting(s_evt_q) : 0;
}
