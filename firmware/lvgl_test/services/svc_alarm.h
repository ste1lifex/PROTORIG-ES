/*
 * svc_alarm.h —— 闹钟服务（2026-11-08，APP 第一批）
 *
 * 职责：闹钟配置（HH:MM + 开关 + 每天重复）、到点判定、RTC 报警arming。
 * 不碰 LVGL、不碰页面、不直接读写 SD3010 —— 持久化/arming 通过回调注入，
 * 这样服务本身可以在 host 上单测（不链接 I2C/FreeRTOS）。
 *
 * 时间来源：调用方（main.c，UI 任务）把 RTC 读到的日期时间传进来。
 *
 * 到点策略（关键，见 research/80 §5）：
 *   - 醒着：UI 每拍 poll，HH:MM 匹配且当天没触发过 → 返回 1；
 *   - 息屏但没深睡：main 的息屏分支节流 poll，到点由 main 亮屏 + 震动 + 通知；
 *   - PDS15 深睡：睡前把 SD3010 的 HH:MM 报警寄存器设好（IO17 = AON 唤醒脚），
 *     到点 RTC 拉低 INT → PDS 唤醒（level>=7 是复位启动），
 *     main 启动后用 boot_check() 按"当前时间是否落在闹钟那一分钟"补触发。
 *
 * 不持久化"今天已响过"：boot 时 RAM 清空，靠"只在 HH:MM 那一分钟触发"收窄
 * 重复窗口；PDS15 唤醒后下一次报警是明天，不会连响。断电/复位恰好落在同一
 * 分钟内会多响一次，属可接受边界（见 research/80 §7 风险）。
 */
#ifndef SVC_ALARM_H
#define SVC_ALARM_H

#include <stdint.h>

typedef struct {
    uint16_t year;      /* 2026 */
    uint8_t month;      /* 1..12 */
    uint8_t day;        /* 1..31 */
    uint8_t hour;       /* 0..23 */
    uint8_t min;        /* 0..59 */
    uint8_t sec;        /* 0..59（判定只用时分，秒仅用于诊断） */
} svc_alarm_time_t;

/* 持久化回调：把 (时, 分, 使能) 写进 settings v2 的 alarm_h/alarm_m/alarm_flags。
 * 返回 0 成功。main.c 注册 settings_save_alarm()。 */
typedef int (*svc_alarm_save_cb_t)(uint8_t hour, uint8_t minute, uint8_t enabled);

/* RTC 报警 arming 回调：enabled=1 时把 SD3010 报警设成每天 hour:minute，
 * enabled=0 时恢复原来的整分报警。main.c 注册（直接调 sd3010 驱动）。 */
typedef void (*svc_alarm_arm_cb_t)(int enabled, uint8_t hour, uint8_t minute);

void svc_alarm_init(uint8_t hour, uint8_t minute, int enabled);
void svc_alarm_set_save_cb(svc_alarm_save_cb_t cb);
void svc_alarm_set_arm_cb(svc_alarm_arm_cb_t cb);

int svc_alarm_enabled(void);
uint8_t svc_alarm_hour(void);
uint8_t svc_alarm_minute(void);

void svc_alarm_adjust_hour(int delta);
void svc_alarm_adjust_minute(int delta);
void svc_alarm_set_enabled(int enabled);

/* 直接设定时/分（shell 上板测试用；越界忽略）。 */
void svc_alarm_set_time(uint8_t hour, uint8_t minute);

/* 按当前配置重设 RTC 报警（启动/唤醒/开关变化时调用）。 */
void svc_alarm_apply_rtc(void);

/* 睡前问询：1 = 闹钟已 arm（board_power 不要 disable RTC 报警/INT）；
 * 0 = 没开闹钟，沿用原来的"整分报警 disable"。 */
int svc_alarm_arm_for_sleep(void);

/* 醒着时检查：HH:MM 匹配且当天没响过 → 1（调用方负责震动/通知）。
 * now 为 NULL 或闹钟关闭时返回 0。 */
int svc_alarm_poll(const svc_alarm_time_t *now);

/* PDS15 复位唤醒后的补触发：判定规则与 poll 相同（只在 HH:MM 那一分钟）。 */
int svc_alarm_boot_check(const svc_alarm_time_t *now);

#endif /* SVC_ALARM_H */
