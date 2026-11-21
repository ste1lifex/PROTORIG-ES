/*
 * svc_battery.h —— 电池/充电服务（R3c 第二步，2026-10-29）
 *
 * 职责（docs/R3c_DESIGN.md §3）：
 *   - ADC 延迟初始化、采样/一阶滤波、百分比、充电脚检测；
 *   - 低电 3 级（1=3.45V / 2=3.35V）与 3.2V 连续 3 次关机；
 *   - 息屏期间 20 s 低电兜底（原 screen_off_battery_guard）；
 *   - **唯一**写 app_state 电池字段的服务（batt_seq）；
 *   - 变化 ≥1% 或 ≥10 mV 投 EV_BATTERY，充电边沿投 EV_CHARGING，
 *     低电等级变化投 EV_LOW_BATTERY，并通过 policy 回调交给 power 侧；
 *   - 不 include lvgl、不直接改表盘/马达；UI 只看 app_state / 事件。
 *
 * 低电策略解耦（评审要求）：本服务不直调 power_set_idle_ms()，而是调用
 * main 启动代码注册的 policy 回调（内部再调 board_power）。3.2V 关机
 * 仍直接调 board_power 的 low_battery_shutdown()（bsp 调用，不是服务互调）。
 */
#ifndef SVC_BATTERY_H
#define SVC_BATTERY_H

#include <stdint.h>

/* 低电等级变化时回调（level 0/1/2，mv 为滤波后电压） */
typedef void (*svc_battery_policy_cb_t)(int level, int mv);

/* 注册在 board_power 的息屏兜底回调 + 清状态；启动序列调用一次。 */
void svc_battery_init(void);

/* UI 任务每 3 s 调一次。返回 1 = 本次真正采样并更新了状态。 */
int svc_battery_tick(uint32_t now_ms);

/* 立即采一次（唤醒/进页面时用）。返回 1 = 采样成功。 */
int svc_battery_sample_now(void);

/* 读 app_state 快照；指针可为 NULL。0 成功。 */
int svc_battery_get(int *mv, int *pct, int *charging);

/* shell `lowbatt` 用：当前等级 / 当前强制值（-1 = auto）。 */
int svc_battery_get_level(void);
int svc_battery_get_force(void);
void svc_battery_force_level(int level);   /* 0/1/2；-1 = auto */

/* 息屏 20 s 兜底（由 board_power auto_sleep 通过回调调用）。 */
void svc_battery_off_guard(void);

/* 低电策略回调（main 启动时注入 power 侧实现）。 */
void svc_battery_set_policy_cb(svc_battery_policy_cb_t cb);

#endif /* SVC_BATTERY_H */
