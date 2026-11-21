/*
 * svc_stopwatch.h —— 秒表服务（2026-11-08，APP 第一批）
 *
 * 职责：只保存秒表状态机与累计时间，不碰 LVGL、不碰页面。
 *   - 页面/触摸 → svc_stopwatch_toggle()/reset()；
 *   - UI 任务每拍传入单调毫秒（lv_tick_get()）→ svc_stopwatch_tick()；
 *   - 页面只负责按 svc_stopwatch_format() 显示。
 *
 * 状态：
 *   IDLE    未开始（显示 00:00.00）
 *   RUNNING 计时中（开始/继续后）
 *   PAUSED  已暂停（显示暂停时的累计值）
 *
 * 后台语义：状态全在 RAM，离开页面（回列表/切别的 App）继续计时；
 * PDS15 深睡唤醒是复位启动，RAM 状态丢失 —— 不承诺跨 PDS 精确（用户已定）。
 */
#ifndef SVC_STOPWATCH_H
#define SVC_STOPWATCH_H

#include <stdint.h>

typedef enum {
    SVC_STOPWATCH_IDLE = 0,
    SVC_STOPWATCH_RUNNING,
    SVC_STOPWATCH_PAUSED
} svc_stopwatch_state_t;

/* 99:59.99 封顶：显示位数固定，超过就停在这里，不让格式串溢出 */
#define SVC_STOPWATCH_MAX_MS 5999990u

void svc_stopwatch_init(void);

/* 每拍更新（UI 任务）。now_ms = lv_tick_get()。 */
void svc_stopwatch_tick(uint32_t now_ms);

/* 开始 / 暂停 / 继续（同一颗按钮循环）。 */
void svc_stopwatch_toggle(void);

/* 复位到 00:00.00（任意状态都可以按）。 */
void svc_stopwatch_reset(void);

svc_stopwatch_state_t svc_stopwatch_state(void);

/* 当前累计毫秒（运行中会含"这一拍"的增量）。 */
uint32_t svc_stopwatch_elapsed_ms(void);

/* 格式化成 "mm:ss.cc"；out 至少 10 字节。 */
void svc_stopwatch_format(char *out, uint32_t cap);

#endif /* SVC_STOPWATCH_H */
