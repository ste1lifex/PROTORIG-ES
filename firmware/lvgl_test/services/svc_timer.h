/*
 * svc_timer.h —— 倒计时服务（2026-11-08，APP 第一批）
 *
 * 职责：预设分钟数 + 倒计时状态机，不碰 LVGL、不碰页面、不管震动/通知。
 *   - 页面 → svc_timer_toggle()/cancel()/adjust()/next_preset()；
 *   - UI 任务每拍 svc_timer_tick(now_ms)：运行中时到点返回 1（只报一次），
 *     由调用方负责震动 + 通知弹层 + 亮屏；
 *   - 页面按 svc_timer_format() 显示剩余时间。
 *
 * 状态：
 *   IDLE    未开始（显示预设时长）
 *   RUNNING 倒计时中
 *   PAUSED  已暂停（剩余时间冻结）
 *   DONE    到点（显示 00:00，按开始会重新从预设时长开始）
 *
 * 后台语义：状态在 RAM，离开页面继续倒计时；**运行期间 board_power 不进入
 * PDS 深睡**（PDS15 唤醒是复位，RAM 状态会丢），到点后恢复正常睡眠。
 * 代价见 research/80 §6。
 */
#ifndef SVC_TIMER_H
#define SVC_TIMER_H

#include <stdint.h>

typedef enum {
    SVC_TIMER_IDLE = 0,
    SVC_TIMER_RUNNING,
    SVC_TIMER_PAUSED,
    SVC_TIMER_DONE
} svc_timer_state_t;

#define SVC_TIMER_PRESET_MIN_MIN 1u
#define SVC_TIMER_PRESET_MIN_MAX 99u

void svc_timer_init(void);

/* 只更新剩余时间/到点状态，**不产生事件**：页面 100 ms 刷新用（显示要跟手）。
 * 到点后状态变 DONE、置 pending，由 svc_timer_tick() 取走并报一次。 */
void svc_timer_update(uint32_t now_ms);

/* 每拍更新：返回 1 = 这一拍刚好到点（之后不会再返回 1，直到重新开始）。
 * main.c 的 500 ms 节拍调用（醒着）与息屏轮询调用，负责"报事件"。 */
int svc_timer_tick(uint32_t now_ms);

/* 开始 / 暂停 / 继续 / 到点后重新开始。 */
void svc_timer_toggle(void);

/* 取消：回 IDLE，剩余时间重新等于预设时长。 */
void svc_timer_cancel(void);

/* 预设 +/- 调整（delta 单位：分钟，可为负）。 */
void svc_timer_adjust(int delta_min);

/* 点按预设值：在 1/3/5/10/30 分钟之间循环。 */
void svc_timer_next_preset(void);

uint32_t svc_timer_preset_min(void);
uint32_t svc_timer_remaining_ms(void);
svc_timer_state_t svc_timer_state(void);

/* 1 = 正在倒计时（PDS 睡眠拦截用） */
int svc_timer_is_running(void);

/* 剩余时间 "mm:ss"；out 至少 8 字节。 */
void svc_timer_format(char *out, uint32_t cap);

#endif /* SVC_TIMER_H */
