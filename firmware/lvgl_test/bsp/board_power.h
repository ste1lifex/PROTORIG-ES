/*
 * board_power.h —— 息屏 / 触摸唤醒 / 空闲超时 / PDS / 面板电源胶水
 *
 * R1c（W2）：原 power.h 搬入 bsp/，并把 main.c 里的充电脚、PDS 状态、
 * 面板/触摸电源、低电关机、息屏电量兜底等胶水一并收进来（纯搬移）。
 */
#ifndef BOARD_POWER_H
#define BOARD_POWER_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "semphr.h"

/* V1.6 充电状态脚：CHRG=IO3 / STDBY=IO21（TP4056 开漏低有效） */
#define PIN_CHRG  3
#define PIN_STDBY 21

/* PDS 持久计数在 SD3010 用户 RAM 的地址 / valid 标记（shell 也会读写） */
#define PDSLOG_ADDR  0x6A
#define PDSLOG_VALID 0x5A
#define BATTLOG_SEC   120        /* battlog 周期；PDS 定时唤醒也用它 */

void power_init(void);
/* Temporary monitor-page policy; never overwrites watch preferences. LVGL task only. */
void power_set_monitor_policy(int active, uint8_t brightness, int always_on);
/* 电脑模式专用息屏超时；0 = 恢复全局 idle（不写手表设置）。 */
void power_set_monitor_idle_ms(uint32_t ms);
uint32_t power_get_monitor_idle_ms(void);
int power_monitor_active(void);
uint8_t power_effective_brightness(void);
/* LVGL task only：立刻写 WRDISBV(0x51)。本函数只改 RAM；持久化由设置页调
 * settings_save() 完成（SD3010 用户 RAM / settings v2），开机 settings_load() 读回。 */
void power_set_brightness(uint8_t level);
uint8_t power_get_brightness(void);

/* 设置页的三档亮度 / 三档息屏时间（唯一来源：EEZ 设置页、持久化、shell 共用） */
extern const uint8_t power_brightness_levels[3];
extern const uint32_t power_idle_levels[3];

/* 设置持久化钩子：main.c 在 SD3010 就绪后注册；host 测试不注册则为空操作。
 * 只有"用户显式改设置"才调用 request，低电策略改 idle 不会写 RTC RAM。 */
typedef int (*power_persist_cb_t)(uint8_t brightness, uint32_t idle_ms);
void power_set_persist_cb(power_persist_cb_t cb);
void power_request_persist(void);

/* 开机读回设置后，把当前亮度真正写到面板（st7802_init 默认写 0xFF） */
void power_apply_brightness(void);

/* 唤醒时是否整屏重画。
 *   0（默认）= 面板 GRAM 保留了上一帧，DISPON 即刻恢复 —— 唤醒最快
 *   1        = 唤醒后整屏重画（万一这块屏其实没有 GRAM 就切到这个） */
void power_set_wake_redraw(int on);
int power_get_wake_redraw(void);

/* 唤醒回调（非整屏重画模式下调用，用来补时间这类小区域） */
void power_set_wake_cb(void (*cb)(void));

bool power_is_on(void);

/* 只在 LVGL 任务上下文里调用（会和刷屏抢 DBI） */
void power_screen_off(void);
void power_screen_on(void);

/* 由触摸驱动调用：刷新"用户在场"计时 */
void power_note_touch(void);

/* 最近一次"熄屏 -> 亮屏"的时刻（lv_tick_get 时间轴）。
 * 返回 0 表示开机以来还没熄过屏。用于"唤醒的那一下触摸不要触发导航"防抖。 */
uint32_t power_last_on_ms(void);

/* 由 LVGL 周期定时器调用：检查空闲超时（时间由 svc_power_tick 提供） */
void power_tick(void);

/* R2c：BSP 去 LVGL 化。
 *   svc_power_tick(now_ms) —— UI 任务每轮传入时间（替代原 power.c / 现本文件里的
 *                             lv_tick_get，BSP 不再依赖 LVGL）；
 *   power_take_full_redraw() —— 取一次"整屏重画"请求（wake_redraw 唤醒路径）。 */
void svc_power_tick(uint32_t now_ms);
int power_take_full_redraw(void);

/* R2c：息屏后自动睡决策（原 lvgl_task 内联块）。前置 power_is_on()==false；
 * pc_mode 为真时保留电脑会话，只等触摸或短超时，以便及时处理 USB/BLE。
 * 返回值语义：CONTINUE = 本次没进 PDS（或已处理完）；SLEPT = 进过 PDS 并返回。
 * 调用方无论返回值都 continue 主循环。不碰 LVGL。 */
typedef enum {
    POWER_IDLE_CONTINUE = 0,
    POWER_IDLE_SLEPT = 1,
} power_idle_ret_t;
power_idle_ret_t board_power_auto_sleep(bool pc_mode);
/* R2c：触摸轮询由 UI 侧注入（tp_lvgl_poll_raw），BSP 不 include tp_lvgl.h。
 * 返回 1 = 有触摸事件（语义同 tp_lvgl_poll_raw）。 */
typedef int (*board_power_touch_poll_t)(void);
void board_power_set_touch_poll_cb(board_power_touch_poll_t cb);




void power_set_idle_ms(uint32_t ms);
uint32_t power_get_idle_ms(void);
uint32_t power_get_off_count(void);
uint32_t power_get_on_count(void);
uint32_t power_idle_left_ms(void);

/* 诊断用：永久常亮（屏永不自动息，power_tick 直接短路）。
 * 为什么要它：息屏后 LVGL 完全停止渲染，而"整分花屏"这类取证必须盯着屏看
 * 几十秒到一分钟；靠 `idle <ms>` 抢时间总会被各种重启/复位打回 15000ms。
 * shell: `alwayson 1|0` */
void power_set_always_on(int on);
int  power_get_always_on(void);

/* ------------------------------------------------------------------ */
/* PDS 深睡（V1.6，IO16 触摸 / IO17 RTC 只能在 AON 脚唤醒）            */
/*                                                                    */
/* level: 1/2/3 = CPU 不掉电，醒来继续执行；7/15 = 醒来复位重启。      */
/* sleep_ticks: 0 = 只等 GPIO/RTC 唤醒；>0 = RC32K 定时唤醒（单位      */
/*              1/32768 s，且必须 > PDS_WARMUP_LATENCY_CNT）。         */
/* 返回唤醒源：1=IO16 触摸，2=IO17 RTC，0=其它/定时。                  */
/* ------------------------------------------------------------------ */
void power_pds_init(int level);
int  power_pds_level(void);
void power_pds_set_level(int level);
uint32_t power_pds_count(void);
int  power_pds_last_wake(void);
void power_pds_set_restore_cb(void (*cb)(void));
int  power_pds_enter(uint32_t sleep_ticks);

/* 3.2V 低电关机态：面板断电 + PDS15 + 只留 CHRG(IO3) 下降沿唤醒（USB 插入）。
 * 触摸会提前进 D1 05 深睡（2µA），所以不会误唤醒；RTC/SD3010 由 BAT+ 继续供电。
 * 不返回（PDS15 复位唤醒时直接重启）。 */
int  power_pds_shutdown(void);

/* HBN 实验：MCU <1µA，IO16 触摸低电平唤醒；醒来 POR 复位启动。
 * 面板先断电，触摸保持 normal（不深睡，否则叫不醒）。 */
int  power_hbn_enter(void);

/* ------------------------------------------------------------------ */
/* R1c：从 main.c 搬入的 board/power 胶水（跨文件先 extern，R2 再收 API） */
/* ------------------------------------------------------------------ */

/* 充电状态脚（IO3/IO21） */
void chrg_init(void);
int  chrg_charging(void);
int  chrg_full(void);
int  chrg_usb_present(void);
int  usb_present_any(void);      /* chrg 或 USB 已枚举 */
int  chrg_raw(int pin);

/* PDS 选项（shell 与 lvgl_task 直接读写；R2c 收进 svc_power） */
extern int s_pds_enabled;
extern int s_pds_panel_off;
extern int s_panel_powered_off;
extern int s_tp_lp;
extern int s_tp_in_lp;
extern int s_tp_lp_scan;   /* policy2：D1 07 LP scan 已生效 */
extern int s_pds_usb_reset;
extern int s_pds_hbn;

/* PDS 持久计数（SD3010 用户 RAM 0x6A~0x71） */
extern uint8_t s_pds_log[8];
void pds_log_load(void);
void pds_log_save(void);
void pds_log_bump(int idx);

/* 面板/触摸电源胶水（只在 LVGL 任务/关机路径调用） */
void panel_reinit_after_pds(void);
void screen_on_safe(void);
void usb_vbus_reboot(void);
void low_battery_shutdown(void);

/* 触摸中断唤醒：ISR 只给信号量；PDS 醒来后恢复 GPIO 中断 */
extern SemaphoreHandle_t s_tp_wake_sem;
void tp_int_isr(uint8_t pin);
void pds_restore_irq(void);

/* R3c：RTC 服务注册 PDS 回调（恢复 RTC IRQ / 清分钟报警），BSP 不再 extern
 * rtc_int_isr/rtc_int_service。任一回调可为 NULL。 */
void board_power_set_time_callbacks(void (*restore_irq)(void), void (*alarm_service)(void));

/* R3c：息屏低电兜底（实现搬到 services/svc_battery.c），BSP 只负责回调调用 */
void board_power_set_battery_guard_cb(void (*guard)(void));

/* R2.1：BLE 电源策略回调；返回 1 = 允许 PDS15。NULL = 允许（兼容旧行为） */
void board_power_set_ble_policy_cb(int (*cb)(void));
int  board_power_ble_pds_allowed(void);

/* 2026-11-08：App 服务（闹钟/计时器）注入的深睡策略钩子。
 *   sleep_veto  —— 返回 1 = 这一轮不要进 PDS（倒计时运行中；PDS15 复位会丢状态）。
 *                  禁睡期间阻塞等待缩短到 200 ms，让调用方按时轮询倒计时。
 *   alarm_arm   —— 睡前问询：返回 1 = 已 arm SD3010 RTC 报警（IO17 低电平唤醒），
 *                  调用方会跳过 sd3010_alarm_disable()；返回 0 走原来的整分报警 disable。
 *   alarm_apply —— 唤醒/启动后按当前闹钟配置重设 RTC 报警
 *                  （替代原来固定 sd3010_alarm_set_sec(0) 的整分报警）。 */
void board_power_set_sleep_veto_cb(int (*cb)(void));
void board_power_set_alarm_callbacks(int (*arm)(void), void (*apply)(void));

/* 供 main.c 的 shell / 睡前逻辑直接问一次"闹钟 arm 了吗" */
int  board_power_alarm_armed(void);
void board_power_alarm_apply(void);

/* 电量采样就绪标志（batt_timer_cb 设置，guard / PDS 读取） */
extern int s_adc_ready;

#endif /* BOARD_POWER_H */
