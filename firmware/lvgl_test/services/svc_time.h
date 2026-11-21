/*
 * svc_time.h —— RTC 时间服务（R3c，2026-10-29）
 *
 * 职责（见 docs/R3c_DESIGN.md §2）：
 *   - RTC 中断（IO17，AON）、轮询、掉电播种、分钟报警清理；
 *   - **唯一**写 app_state 时间字段的服务（time_seq）；
 *   - hh:mm 变化投 EV_RTC_MINUTE，svc_time_set() 成功投 EV_TIME_SET；
 *   - PDS 醒来通过 board_power 注册的回调恢复 IRQ / 清报警；
 *   - 不 include lvgl、不碰任何 UI。
 *
 * 前置：i2c_bus_init()/sd3010_init() 已完成（仍在启动序列里）。
 * 线程：svc_time_tick()/svc_time_set() 只在 UI 任务或 shell 任务调用；
 *       ISR 只投信号量，不做业务。
 */
#ifndef SVC_TIME_H
#define SVC_TIME_H

#include <stdint.h>

#include "sd3010.h"

#define SVC_TIME_RTC_INT_PIN 17

/* 创建互斥量/信号量、挂 RTC 中断、播种（PDS15 判据不变）、设分钟报警，
 * 并向 board_power 注册 PDS 醒来回调。启动序列调用一次。 */
void svc_time_init(void);

/* UI 任务 500 ms 调一次：节流后读 RTC，更新 app_state，必要时投 EV_RTC_MINUTE。 */
void svc_time_tick(uint32_t now_ms);

/* 真实对时路径（settime 命令 / BLE CTS / demo 3 共用）：
 * sd3010_set_time + clear_osf + write_magic + app_state + EV_TIME_SET。
 * 返回 sd3010_set_time 的返回值（>=0 成功）。 */
int svc_time_set(const sd3010_time_t *tm);

/* 只读当前时间（临界区）。0 成功。
 * ⚠️ BLE（R4 起已接入，svc_ble.c 的 CTS 同步就走这里）：BLE 的「当前时间」
 *    必须调本函数读 RTC，**不要读 app_state.time**
 *    —— 息屏/停 tick 时 app_state 里的时间是冻结的，只有 svc_time_get() 可信。 */
int svc_time_get(sd3010_time_t *out);

/* PDS 醒来恢复 RTC GPIO 中断（board_power 回调）。 */
void svc_time_restore_irq(void);

/* PDS 醒来清 RTC 报警标志（board_power 回调，原 rtc_int_service）。 */
void svc_time_alarm_service(void);

/* 给 shell 命令/诊断用的小接口（保持 settime/rtcstat/rtcalarm 行为不变）。 */
void svc_time_lock(void);
void svc_time_unlock(void);
int  svc_time_int_level(void);
void svc_time_get_irq_stats(uint32_t *irq_count, uint32_t *serviced_count);

#endif /* SVC_TIME_H */
