/*
 * board_power.c —— 息屏 / 触摸唤醒 / 空闲超时 / PDS（BSP 层）
 *
 * ===========================================================================
 * 为什么这件事在这个产品里是"形态"而不是"优化"
 *
 * 这块表【没有姿态传感器】（没有加速度计/陀螺仪），所以：
 *   · 没有「抬腕亮屏」—— 唯一的"用户在场"信号就是【手指碰到屏】
 *   · 因此：屏幕默认应该是关的，触摸才是开屏动作；
 *     息屏省下来的电不是锦上添花，而是这块表能不能戴一天的生死线。
 *
 * 当前实现（V1.6）：
 *   · 熄屏 = WRDISBV(0x51)=0 + DISPOFF(0x28)，屏停止扫描且全黑
 *   · 手表模式满足条件时，熄屏后进 PDS15（CPU 掉电、复位唤醒）：触摸
 *     INT(IO16) 拉低后走完整 boot，约 1 秒亮屏。电脑模式和 USB 供电时禁止 PDS。
 *   · 空闲超时（默认 15s）自动熄屏
 *   · 亮度/息屏档持久化在 SD3010 用户 RAM（settings v2，services/svc_settings.c）：
 *     用户改设置即落盘、开机 settings_load() 读回 —— PDS15 复位唤醒不会回默认值
 *     （只有电池彻底耗尽 / RTC 掉电时 SD3010 的 RAM 才丢，那时才回落默认）
 *
 * ⚠️ 硬件事实（V1.6 改板后）：
 *    触摸 INT = IO16、RTC INT = IO17，都是 AON 脚，可以硬唤醒；
 *    PDS1/2/3 CPU 不掉电但底电流 8~18 mA，PDS15 才够省电。
 *    TP_RST(IO22) 的 10k 下拉 R25 已拆，否则 PDS15 期间触摸会一直被按在复位里。
 * ===========================================================================
 */
#include "board_power.h"

#include "st7802.h"

#include <stdio.h>

#ifndef PROTORIG_HOST_TEST
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "bflb_gpio.h"
#include "bflb_mtimer.h"
#include "bl616_pds.h"
#include "bl616_hbn.h"
#include "bl616_aon.h"
#include "bl616_pm.h"

#include "board.h"          /* GPIO_PIN_16/17 */
#include "board_adc.h"
#include "sd3010.h"
#include "usbd_core.h"
#include "tp.h"
#include "motor.h"

/* main.c 里尚未迁移到服务的 diag/记录函数（截至 2026-11-15 仍在 main.c，只能 extern） */
extern int pc_link_active(void);
extern void batt_log_force(void);
extern void batt_log_tick(void);

/* R3c：RTC 服务通过回调注入（BSP 不再 extern rtc_int_isr/rtc_int_service） */
static void (*s_time_restore_cb)(void);
static void (*s_time_alarm_cb)(void);

void board_power_set_time_callbacks(void (*restore_irq)(void), void (*alarm_service)(void))
{
    s_time_restore_cb = restore_irq;
    s_time_alarm_cb = alarm_service;
}

/* R3c：息屏低电兜底由 svc_battery 实现，BSP 通过回调调用，避免反向依赖 */
static void (*s_batt_guard_cb)(void);
static int (*s_ble_pds_cb)(void);   /* R2.1: 返回 1 = 允许 PDS15 */

void board_power_set_battery_guard_cb(void (*guard)(void))
{
    s_batt_guard_cb = guard;
}

void board_power_set_ble_policy_cb(int (*cb)(void))
{
    s_ble_pds_cb = cb;
}

/* 2026-11-08：App 服务（闹钟/计时器）与深睡策略的两条通用钩子。
 * 目的：BSP 不反向 include services/，服务把策略注入进来。
 *   sleep_veto：返回 1 = 这一轮不要进 PDS（例如倒计时正在跑，PDS15 复位会丢状态）；
 *   alarm_arm ：睡前问一次；返回 1 = 已经 arm 了 RTC 报警（IO17 唤醒），
 *               调用方**不要**再 sd3010_alarm_disable()；
 *   alarm_apply：唤醒/启动后按当前闹钟配置重设 RTC 报警（替代原来固定写
 *                sd3010_alarm_set_sec(0) 的整分报警）。 */
static int (*s_sleep_veto_cb)(void);
static int (*s_alarm_arm_cb)(void);
static void (*s_alarm_apply_cb)(void);

void board_power_set_sleep_veto_cb(int (*cb)(void))
{
    s_sleep_veto_cb = cb;
}

void board_power_set_alarm_callbacks(int (*arm)(void), void (*apply)(void))
{
    s_alarm_arm_cb = arm;
    s_alarm_apply_cb = apply;
}

int board_power_alarm_armed(void)
{
    return (s_alarm_arm_cb != NULL && s_alarm_arm_cb() > 0) ? 1 : 0;
}

void board_power_alarm_apply(void)
{
    if (s_alarm_apply_cb != NULL) {
        s_alarm_apply_cb();
    }
}

int board_power_ble_pds_allowed(void)
{
    return (s_ble_pds_cb != NULL) ? s_ble_pds_cb() : 1;
}

/* R2c：触摸轮询回调（由 main.c 注册 tp_lvgl_poll_raw），BSP 不依赖 UI 层 */
static board_power_touch_poll_t s_touch_poll_cb;

void board_power_set_touch_poll_cb(board_power_touch_poll_t cb)
{
    s_touch_poll_cb = cb;
}

static int touch_poll(void)
{
    return (s_touch_poll_cb != NULL) ? s_touch_poll_cb() : 0;
}

#endif

/* WRDISBV(0x51) 亮度；DISPOFF(0x28) / DISPON(0x29) */
#define REG_WRDISBV  0x51
#define CMD_DISPOFF  0x28
#define CMD_DISPON   0x29

static uint8_t s_brightness = 255;
static int s_monitor_active, s_monitor_always_on;
static uint8_t s_monitor_brightness = 153;

/* 设置页三档（与 eez_ui_glue 共用，避免两处数值不一致） */
const uint8_t  power_brightness_levels[3] = { 64, 153, 255 };
const uint32_t power_idle_levels[3]       = { 15000, 30000, 60000 };

uint8_t power_effective_brightness(void)
{ return s_monitor_active ? s_monitor_brightness : s_brightness; }
int power_monitor_active(void) { return s_monitor_active; }
static bool     s_on = true;
static int      s_always_on;                /* 诊断用：屏永不自动息（power_tick 里短路） */
static int      s_wake_redraw;              /* 唤醒时是否整屏重画（默认不）*/
static void   (*s_wake_cb)(void);           /* 唤醒回调：补时间等小区域 */
static uint32_t s_idle_ms = 15000;
/* 电脑模式独立息屏超时（0 = 用全局 s_idle_ms）；由 PC 帧 0x16 设置，不持久化。 */
static uint32_t s_monitor_idle_ms = 0;
static uint32_t s_last_activity;
static uint32_t s_off_count;
static uint32_t s_on_count;
static uint32_t s_last_on_ms;               /* 最近一次"熄->亮"的时刻（唤醒防抖用） */

/* R2c：由 UI 任务每轮传入的时间（替代 lv_tick_get，让 BSP 脱离 LVGL） */
static uint32_t s_now_ms;
static volatile int s_full_redraw_req;

/* R2c-fix：息屏期间 svc_power_tick 不运行，s_now_ms 冻结；唤醒后第一次 tick
 * 必须用真实 now 重新对齐 idle 计时，否则 s_now_ms - s_last_activity 会等于
 * 整个睡眠时长，唤醒后立刻又被息屏（G0 实测亮 3/46 ms）。 */
static int s_wake_pending;

/* 持久化钩子由 main.c 注册；host 测试不注册，request 变成空操作 */
static power_persist_cb_t s_persist_cb;
void power_set_persist_cb(power_persist_cb_t cb) { s_persist_cb = cb; }
void power_request_persist(void)
{
    if (s_persist_cb != NULL) {
        (void)s_persist_cb(s_brightness, s_idle_ms);
    }
}

/* 屏幕开关本身只允许在 LVGL 任务里执行（避免和刷屏抢 DBI） */
static void panel_blank(bool blank)
{
    uint8_t b;

    if (blank) {
        b = 0x00;
        st7802_write_cmd(REG_WRDISBV, &b, 1);
        st7802_write_cmd(CMD_DISPOFF, NULL, 0);
    } else {
        st7802_write_cmd(CMD_DISPON, NULL, 0);
        b = power_effective_brightness();
        st7802_write_cmd(REG_WRDISBV, &b, 1);
    }
}

void power_set_wake_cb(void (*cb)(void))
{
    s_wake_cb = cb;
}

void power_set_wake_redraw(int on)
{
    s_wake_redraw = on;
    printf("[power] wake mode = %s\r\n", on ? "full redraw" : "gram kept (fast)");
}

int power_get_wake_redraw(void)
{
    return s_wake_redraw;
}

void power_init(void)
{
    s_last_activity = s_now_ms;
    s_on = true;
    s_off_count = 0;
    s_on_count = 0;
    panel_blank(false);
    printf("[power] init: idle timeout = %u ms\r\n", (unsigned)s_idle_ms);
}

bool power_is_on(void)
{
    return s_on;
}

void power_screen_off(void)
{
    if (!s_on) {
        return;
    }
    s_on = false;
    s_off_count++;
    panel_blank(true);
    /* R3c policy2：屏灭立刻发一次 D1 07（LP scan），在 auto_sleep/PDS 之前；
     * 失败则保持 flag=0，auto_sleep 仍可走原有 gesture 回退。 */
#ifndef PROTORIG_HOST_TEST
    if (!s_tp_lp_scan) {
        int r = tp_enter_lp_scan();
        printf("[tp] enter LP scan (D1 07) = %d\r\n", r);
        if (r >= 0) {
            s_tp_lp_scan = 1;
        }
    }
#endif
    printf("[power] screen OFF  (idle %u ms, off#%lu)\r\n",
           (unsigned)s_idle_ms, (unsigned long)s_off_count);
}

void power_screen_on(void)
{
    if (s_on) {
        return;
    }
    s_on = true;
    s_on_count++;
    s_last_on_ms = s_now_ms;
    panel_blank(false);
    s_last_activity = s_now_ms;
    s_wake_pending = 1;     /* 下一次 svc_power_tick 用真实 now 重新对齐 */

    /* 默认【不】整屏重画：面板 GRAM 里还留着上一帧，DISPON 就原样回来了。
     * 只让上层补一下时间这类会变的小区域。 */
    if (s_wake_redraw) {
        s_full_redraw_req = 1;  /* R2c: BSP 不碰 LVGL，由 UI 任务取请求后重画 */
    } else if (s_wake_cb) {
        s_wake_cb();
    }
    printf("[power] screen ON   (touch wake, on#%lu, %s)\r\n",
           (unsigned long)s_on_count, s_wake_redraw ? "full redraw" : "gram kept");
}

void power_note_touch(void)
{
    s_last_activity = s_now_ms;
}

/*
 * 最近一次"熄屏 -> 亮屏"的时刻。
 * 用途：唤醒屏幕的那一下触摸，不应该顺手触发 UI 上的点击/导航，
 * 否则一切到表盘就会立刻又跳进三页。上层用它做 400ms 防抖。
 */
uint32_t power_last_on_ms(void)
{
    return s_last_on_ms;
}

/* R2c：UI 任务把当前时间传进来（原 power_tick 里的 lv_tick_get 替代）。 */
void svc_power_tick(uint32_t now_ms)
{
    s_now_ms = now_ms;

    /* R2c-fix：息屏期间本函数不会跑，s_now_ms 冻结；唤醒后的第一拍必须把
     * idle/防抖计时锚到真实 now，否则会被睡眠时长直接判成超时。 */
    if (s_wake_pending) {
        s_wake_pending = 0;
        s_last_activity = now_ms;
        s_last_on_ms = now_ms;
    }

    power_tick();
}

/* UI 任务取一次"整屏重画"请求（唤醒时 wake_redraw=1 的路径）。 */
int power_take_full_redraw(void)
{
    int r = s_full_redraw_req;
    s_full_redraw_req = 0;
    return r;
}

void power_tick(void)
{
    /*
     * 常亮模式（2026-10-16 晚加）：诊断时屏老是自动息掉，把"刚画上去的图案"
     * 一起刷没了 —— 因为息屏后 LVGL 完全停渲染，而"整分花屏"这类取证又必须
     * 盯着屏看几十秒。所以给一个**永久常亮**开关，不走 idle 计时。
     */
    if (s_monitor_active ? s_monitor_always_on : s_always_on) {
        if (!s_on) {
            power_screen_on();
        }
        return;
    }
    if (!s_on) {
        return;
    }
    uint32_t idle = (s_monitor_active && s_monitor_idle_ms) ? s_monitor_idle_ms : s_idle_ms;
    if (s_now_ms - s_last_activity >= idle) {
        power_screen_off();
    }
}

void power_set_always_on(int on)
{
    s_always_on = on ? 1 : 0;
    if (s_always_on) {
        /* 立刻点亮，并且不要再被 idle 关掉 */
        if (!s_on) {
            power_screen_on();
        }
        s_last_activity = s_now_ms;
    }
    printf("[power] always-on = %s\r\n", s_always_on ? "ON（屏永不自动息）" : "OFF");
}

int power_get_always_on(void)
{
    return s_always_on;
}

void power_set_idle_ms(uint32_t ms)
{
    s_idle_ms = ms;
    s_last_activity = s_now_ms;
    printf("[power] idle timeout = %u ms\r\n", (unsigned)ms);
}

uint32_t power_get_idle_ms(void)
{
    return s_idle_ms;
}

uint32_t power_get_off_count(void)
{
    return s_off_count;
}

uint32_t power_get_on_count(void)
{
    return s_on_count;
}

uint32_t power_idle_left_ms(void)
{
    uint32_t e;
    if (!s_on) {
        return 0;
    }
    e = s_now_ms - s_last_activity;
    return (e >= s_idle_ms) ? 0 : (s_idle_ms - e);
}

void power_set_brightness(uint8_t level)
{
    s_brightness = level < 32 ? 32 : level;
    uint8_t effective = power_effective_brightness();
    if(s_on) st7802_write_cmd(REG_WRDISBV, &effective, 1);
    power_note_touch();
}
uint8_t power_get_brightness(void) { return s_brightness; }

/* boot 时 st7802_init 会把亮度写成 0xFF（最亮）；读回用户设置后调一次这个，
 * 把实际亮度写回面板。不要用 power_set_brightness 代劳：它会重置空闲计时。 */
void power_apply_brightness(void)
{
    uint8_t effective = power_effective_brightness();
    if (s_on) st7802_write_cmd(REG_WRDISBV, &effective, 1);
}

void power_set_monitor_idle_ms(uint32_t ms)
{
    s_monitor_idle_ms = ms;
    s_last_activity = s_now_ms;
}

uint32_t power_get_monitor_idle_ms(void)
{
    return s_monitor_idle_ms;
}

void power_set_monitor_policy(int active, uint8_t brightness, int always_on)
{
    active=!!active;always_on=!!always_on;
    if(brightness<32) brightness=32;
    bool changed=active!=s_monitor_active || (active &&
                 (brightness!=s_monitor_brightness || always_on!=s_monitor_always_on));
    if(!changed) return;
    s_monitor_active=active;s_monitor_brightness=brightness;s_monitor_always_on=always_on;
    if(!active) s_monitor_idle_ms=0;   /* 退出电脑模式：息屏超时回到手表模式设置 */
    uint8_t effective=power_effective_brightness();
    if(s_on) st7802_write_cmd(REG_WRDISBV,&effective,1);
    /* Frames do not count as touch: only entering/leaving or changing policy resets idle. */
    power_note_touch();
    if(active && always_on && !s_on) power_screen_on();
}

/* ================================================================== */
/* PDS 深睡（V1.6）                                                    */
/*                                                                    */
/* 唤醒路径：IO16(触摸) / IO17(RTC) 都是 AON pad，用 HBN IRQ 唤醒。    */
/* 关键点：                                                            */
/*   · 进 PDS 前 HBN_Pin_WakeUp_Mask 先全 1，再配置 16/17 使能；      */
/*   · AON pad 用 ctrlEn=1 + pull-up + 低电平触发；                    */
/*   · 只使能 PDS_WAKEUP_BY_HBN_IRQ_OUT_EN，别让其它源乱唤醒；        */
/*   · 醒来后把 AON pad 控制还给 GLB GPIO，再恢复 IRQ。                */
/* ================================================================== */
#ifndef PROTORIG_HOST_TEST
static int s_pds_level = 1;
static int s_pds_ready;
static int s_pds_last_wake;
static uint32_t s_pds_count;
static void (*s_pds_restore_cb)(void);

void power_pds_set_restore_cb(void (*cb)(void))
{
    s_pds_restore_cb = cb;
}

void power_pds_init(int level)
{
    if (level != 1 && level != 2 && level != 3 && level != 7 && level != 15) {
        level = 1;
    }
    s_pds_level = level;
    pm_pds_irq_register();      /* PDS1 必须，别删 */
    s_pds_ready = 1;
    printf("[pds] init: level=%d (1/2/3=CPU 不掉电, 7/15=复位唤醒)\r\n", level);
}

int power_pds_level(void) { return s_pds_level; }

void power_pds_set_level(int level)
{
    if (level == 1 || level == 2 || level == 3 || level == 7 || level == 15) {
        s_pds_level = level;
        printf("[pds] level = %d\r\n", level);
    } else {
        printf("[pds] level must be 1/2/3/7/15\r\n");
    }
}

uint32_t power_pds_count(void) { return s_pds_count; }
int power_pds_last_wake(void) { return s_pds_last_wake; }

static void pds_pads_wake_cfg(void)
{
    HBN_AON_PAD_CFG_Type cfg;

    cfg.ctrlEn = 1;
    cfg.ie = 1;
    cfg.oe = 0;
    cfg.pullUp = 1;
    cfg.pullDown = 0;
    (void)HBN_Aon_Pad_Cfg(ENABLE, HBN_AON_PAD_GPIO16, &cfg);
    (void)HBN_Aon_Pad_Cfg(ENABLE, HBN_AON_PAD_GPIO17, &cfg);

    /* bit0/1 = GPIO16/17 使能，bit2/3 = 18/19 屏蔽；低电平触发 */
    (void)HBN_Aon_Pad_WakeUpCfg(DISABLE, HBN_GPIO_INT_TRIGGER_SYNC_LOW_LEVEL,
                                0x0C, DISABLE, 7);
}

static void pds_pads_restore(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");
    HBN_AON_PAD_CFG_Type cfg;

    cfg.ctrlEn = 0;
    cfg.ie = 0;
    cfg.oe = 0;
    cfg.pullUp = 0;
    cfg.pullDown = 0;
    (void)HBN_Aon_Pad_Cfg(ENABLE, HBN_AON_PAD_GPIO16, &cfg);
    (void)HBN_Aon_Pad_Cfg(ENABLE, HBN_AON_PAD_GPIO17, &cfg);
    (void)HBN_Pin_WakeUp_Mask(0x0F);

    if (gpio != NULL) {
        bflb_gpio_init(gpio, 16, GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
        bflb_gpio_init(gpio, 17, GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    }
}

int power_pds_enter(uint32_t sleep_ticks)
{
    struct bflb_device_s *gpio;
    uint32_t tmpVal;
    int src;

    if (!s_pds_ready) {
        return -1;
    }

    /* USB PHY 供电先关（PDS 本身也会关，这里省一点过渡电流）
     *
     * 2026-11-15 现状：PDS 关掉 USB PHY 后，唤醒/重新插线仍不会自动重新枚举，
     * 要复位才能恢复串口。PC 外接屏（services/svc_pc.c）已经做完了，但靠下面
     * 两条规避活着 —— 别指望从 PDS 回来 USB 还在：
     *   · 插着 USB 绝不进 PDS（board_power_auto_sleep() + main.c 的守卫）；
     *   · shell `pds usbreset 1` 打开 s_pds_usb_reset：PDS1/3 醒来检测到 VBUS
     *     就软复位重枚举（PDS7/15 本来就是复位启动，自然重枚举）。
     */
    tmpVal = BL_RD_REG(PDS_BASE, PDS_USB_PHY_CTRL);
    tmpVal = BL_CLR_REG_BIT(tmpVal, PDS_REG_PU_USB20_PSW);
    BL_WR_REG(PDS_BASE, PDS_USB_PHY_CTRL, tmpVal);

    pm_pds_mask_all_wakeup_src();
    (void)AON_Output_Float_LDO15_RF();
    (void)HBN_Pin_WakeUp_Mask(0x0F);
    pds_pads_wake_cfg();
    (void)pm_pds_wakeup_src_en(PDS_WAKEUP_BY_HBN_IRQ_OUT_EN_POS);

    s_pds_count++;
    pm_pds_mode_enter((enum pm_pds_sleep_level)s_pds_level, sleep_ticks);

    /* ---- PDS1/2/3 醒来后会走到这里；7/15 直接复位 ---- */
    pds_pads_restore();
    if (s_pds_restore_cb) {
        s_pds_restore_cb();
    }

    gpio = bflb_device_get_by_name("gpio");
    if (gpio != NULL) {
        int t = (bflb_gpio_read(gpio, 16) == 0);
        int r = (bflb_gpio_read(gpio, 17) == 0);
        src = t ? 1 : (r ? 2 : 0);
    } else {
        src = 0;
    }
    s_pds_last_wake = src;
    return src;
}

/*
 * 3.2V 低电关机态。
 *
 * 为什么需要：电池保护板在 ~3.0V 硬切，BAT+ 一掉 SD3010 就停走/丢时间。
 * 这里在 3.2V 主动进 PDS15，并且：
 *   · 触摸提前进 D1 05 深睡（2µA）→ 不会被误唤醒；
 *   · 面板 VCI_EN 拉低 → 面板 PMIC 断电；
 *   · 只使能 PDS GPIO 唤醒：CHRG(IO3) 下降沿 = USB 插入；
 *   · AON pad 16/17 全部 mask 掉。
 * 于是电流 ≈ PDS15 几十 µA + 触摸 2µA，BAT+ 还能让 SD3010 走很久。
 * USB 一插进来 CHRG 被 TP4056 拉低 → 复位启动 → boot 看到 VBUS 正常充电。
 */
int power_pds_shutdown(void)
{
    uint32_t tmpVal;

    /* 面板 PMIC 断电（醒来自动重初始化） */
    st7802_vci_power(0);

    /* USB PHY 关 */
    tmpVal = BL_RD_REG(PDS_BASE, PDS_USB_PHY_CTRL);
    tmpVal = BL_CLR_REG_BIT(tmpVal, PDS_REG_PU_USB20_PSW);
    BL_WR_REG(PDS_BASE, PDS_USB_PHY_CTRL, tmpVal);

    pm_pds_mask_all_wakeup_src();
    (void)AON_Output_Float_LDO15_RF();
    (void)HBN_Pin_WakeUp_Mask(0x0F);     /* 不靠 AON pad（触摸已深睡） */

    /*
     * PDS GPIO 唤醒：IO3 = CHRG。
     * 不插 USB 时 CHRG 被 100k 上拉为高；USB 插入/开始充电 → 拉低 → 下降沿。
     * 注意：PDS GPIO 的 SYNC_LOW_LEVEL 在 SDK 里是不支持的，必须用下降沿。
     */
    (void)PDS_Set_GPIO_Pad_Pn_Pu_Pd_Ie(0, 0, 0, 1);
    (void)PDS_Set_GPIO_Pad_IntClr(0);
    (void)PDS_Set_GPIO_Pad_IntMode(0, PDS_GPIO_INT_SYNC_FALLING_EDGE);
    tmpVal = BL_RD_REG(PDS_BASE, PDS_GPIO_PD_SET);
    tmpVal &= ~(1u << 3);                /* IO3 使能（PD_SET 对应位清 0） */
    BL_WR_REG(PDS_BASE, PDS_GPIO_PD_SET, tmpVal);
    (void)pm_pds_wakeup_src_en(PDS_WAKEUP_BY_PDS_GPIO_IRQ_EN_POS);

    printf("[pds] SHUTDOWN 3.2V: PDS15, only VBUS(CHRG IO3) falling-edge wake\r\n");
    pm_pds_mode_enter(PM_PDS_LEVEL_15, 0);

    while (1) {
    }
}

/*
 * HBN（hibernate）实验：MCU 进 <1µA 的 HBN，只留 AON pad IO16/17 低电平唤醒。
 * 触摸芯片保持 normal（不能 D1 05 深睡，否则没有 INT 叫不醒）。
 * 醒来是 POR 复位启动；boot 会重新初始化面板/USB/触摸。
 * 注意：HBN 的电流优势主要在 MCU；触摸 1.4mA 和面板 3V3 仍是底噪。
 */
int power_hbn_enter(void)
{
    HBN_AON_PAD_CFG_Type cfg;

    /* 面板 PMIC 断电 */
    st7802_vci_power(0);

    cfg.ctrlEn = 1;
    cfg.ie = 1;
    cfg.oe = 0;
    cfg.pullUp = 1;
    cfg.pullDown = 0;
    (void)HBN_Aon_Pad_Cfg(ENABLE, HBN_AON_PAD_GPIO16, &cfg);
    (void)HBN_Aon_Pad_Cfg(ENABLE, HBN_AON_PAD_GPIO17, &cfg);
    (void)HBN_Aon_Pad_WakeUpCfg(DISABLE, HBN_GPIO_INT_TRIGGER_SYNC_LOW_LEVEL,
                                0x0C, DISABLE, 7);
    (void)HBN_Pin_WakeUp_Mask(0x0C);
    (void)HBN_GPIO_INT_Enable(HBN_GPIO_INT_TRIGGER_SYNC_LOW_LEVEL);

    printf("[hbn] enter level 0 (touch IO16 low-level wake)\r\n");
    pm_hbn_mode_enter(PM_HBN_LEVEL_0, 0);

    while (1) {
    }
}
#endif /* PROTORIG_HOST_TEST: hardware sleep is never executed on the host. */

/* ================================================================== */
/* R1c: 从 main.c 搬入的 board/power 胶水（纯搬移，函数体原样）        */
/*   charge pins / PDS 状态与持久计数 / 面板与触摸电源 / 低电关机 /     */
/*   息屏电量兜底 / 触摸唤醒 ISR / PDS 唤醒后 IRQ 恢复                  */
/* ================================================================== */

#ifndef PROTORIG_HOST_TEST
/* ---- charge ---- */
/* 充电状态（V1.6：CHRG=IO3 / STDBY=IO21，TP4056 开漏低有效）          */
/*                                                                    */
/*   CHRG  STDBY   含义                                               */
/*    低    高     USB 在 + 正在充电                                  */
/*    高    低     USB 在 + 已充满                                    */
/*    高    高     没插 USB（或充电器故障 / 无电池）                  */
/* ------------------------------------------------------------------ */
#define PIN_CHRG  3
#define PIN_STDBY 21

/* ---- pds_state ---- */
/* 自动 PDS 开关：默认开（息屏 + 没插 USB 就睡；插着 USB 永不睡）。
 * R25 已拆：默认走 PDS15（CPU 掉电，醒来复位启动，USB 自然重枚举）。
 */
int s_pds_enabled = 1;

/* PDS 期间的选项：
 *   s_pds_panel_off = 1：睡前拉低 VCI_EN；PDS15 醒来 boot 会重新 st7802_init
 *   s_tp_lp         = 1：CST9220 gesture 低功耗（实测叫不醒，别开）
 *   s_pds_usb_reset = 1：PDS1/3 醒来检测 VBUS → 软复位重枚举（PDS7/15 不需要）
 */
int s_pds_panel_off = 1;
int s_panel_powered_off;
int s_tp_lp;
int s_tp_in_lp;
int s_tp_lp_scan;   /* R3c policy2: chip already in D1 07 LP scan */
int s_pds_usb_reset;
int s_pds_hbn;      /* HBN 手动测试通过，但自动模式没跑过整夜；默认 0=用验证过的 PDS15 */

/* ------------------------------------------------------------------ */
/* PDS 持久计数（SD3010 用户 RAM 0x6A~0x71，电池在就一直保存）        */
/*   [0]=boot [1]=enter [2]=wake_touch [3]=wake_rtc [4]=wake_other     */
/*   [5]=last_src [6]=last_level [7]=valid(0x5A)                       */
/* 用途：拔 USB 后没有串口，靠它区分"真进了 PDS"还是"复位重启了"。     */
/* ------------------------------------------------------------------ */
uint8_t s_pds_log[8];

void pds_log_load(void)
{
    if (sd3010_ram_read(PDSLOG_ADDR, s_pds_log, sizeof(s_pds_log)) < 0 ||
        s_pds_log[7] != PDSLOG_VALID) {
        memset(s_pds_log, 0, sizeof(s_pds_log));
        s_pds_log[7] = PDSLOG_VALID;
    }
}

void pds_log_save(void)
{
    (void)sd3010_ram_write(PDSLOG_ADDR, s_pds_log, sizeof(s_pds_log));
}

void pds_log_bump(int idx)
{
    if (idx >= 0 && idx < 7) {
        s_pds_log[idx]++;
        pds_log_save();
    }
}

/* ---- panel_glue ---- */
/* 面板在 PDS 里被断电（VCI_EN=0）后，醒来必须完整重初始化：GRAM 已经丢了。
 * st7802_init() 内部已经做了：VCI_EN 上电 + 20ms + 复位 + init_cmds + fmt565(40MHz)，
 * 这里只补 px_swap 和整屏重画标记，尽量缩短第二段睡眠的唤醒时间。
 */
void panel_reinit_after_pds(void)
{
    printf("[panel] re-init after PDS (VCI_EN was off)\r\n");
    st7802_init();
    st7802_set_px_swap(0);
    s_panel_powered_off = 0;
    power_set_wake_redraw(1);       /* GRAM 丢了，必须整屏重画 */
}

/* LVGL 任务里统一的开屏入口：先恢复面板/触摸，再 power_screen_on */
void screen_on_safe(void)
{
    if (s_panel_powered_off) {
        panel_reinit_after_pds();
    }
    if (!s_tp_lp_scan && s_tp_in_lp) {
        int r = tp_lowpower_exit();
        printf("[tp] low-power exit = %d\r\n", r);
        s_tp_in_lp = 0;
    }
    power_screen_on();
}

/*
 * PDS 醒来如果检测到 VBUS（USB 插着），直接软复位一次。
 *
 * 为什么不在 PDS1 里重新初始化 USB：PDS 会关 PD_USB，CherryUSB 的
 * desc/interface/endpoint 状态和 console task 都不好原地恢复；软复位后
 * 走完整启动，USB 100% 重新枚举。代价 ~1 秒，和 PDS15 的复位唤醒一致。
 * 插着 USB 时 auto-PDS 守卫本来就不让睡，所以不会出现反复复位。
 */
void usb_vbus_reboot(void)
{
    printf("[usb] VBUS present after PDS -> reset to re-enumerate\r\n");
    bflb_mtimer_delay_ms(50);
    (void)GLB_SW_System_Reset();
    while (1) {
    }
}

/* 3.2V 低电关机：触摸深睡 2µA + 面板断电 + PDS15，只留 CHRG 下降沿唤醒 */
void low_battery_shutdown(void)
{
    /*
     * 关机态的唤醒源只有 CHRG(IO3) 的下降沿（见本文件 power_pds_shutdown()）。
     * 只有“真的在充电 / 已充满”才跳过；不能再拿 usb_device_is_configured 判断：
     * 物理拔线后 CherryUSB 的 configured 可能仍是 1，会把这里和 PDS 守卫一起卡死。
     */
    if (chrg_usb_present()) {
        printf("[batt] low voltage but charging/full -> shutdown skipped\r\n");
        return;
    }

    printf("[batt] low battery -> shutdown (VBUS wake only)\r\n");
    (void)tp_set_mode(TP_ADDR_DEFAULT, TP_CMD_DEBUG_INFO);
    bflb_mtimer_delay_ms(10);
    (void)tp_set_mode(TP_ADDR_DEFAULT, TP_CMD_DEEP_SLEEP);
    bflb_mtimer_delay_ms(20);
    (void)sd3010_alarm_disable();
    (void)sd3010_int_clear();
    power_pds_shutdown();
}

/* ---- adc_ready ---- */
int s_adc_ready;

/* ---- battery_guard ---- */

/* ---- tp_isr ---- */
/* 触摸中断 → 二分信号量（息屏时 CPU 靠它被叫醒，不再轮询 I2C） */
SemaphoreHandle_t s_tp_wake_sem;

void tp_int_isr(uint8_t pin)
{
    BaseType_t woken = pdFALSE;

    (void)pin;
    if (s_tp_wake_sem) {
        xSemaphoreGiveFromISR(s_tp_wake_sem, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* ---- pds_restore ---- */
/* PDS 醒来后：把 IO16/IO17 的 GPIO 中断重新挂回去（AON pad 控制已交还 GLB） */
void pds_restore_irq(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    if (gpio == NULL) {
        return;
    }
    bflb_gpio_irq_attach(GPIO_PIN_16, tp_int_isr);
    if (s_time_restore_cb != NULL) {
        s_time_restore_cb();        /* RTC ISR 由 svc_time 恢复 */
    }
    bflb_irq_enable(gpio->irq_num);
}

/* ---- charge functions ---- */
static struct bflb_device_s *s_chrg_gpio;

int chrg_charging(void);
int chrg_full(void);
int chrg_usb_present(void);

void chrg_init(void)
{
    s_chrg_gpio = bflb_device_get_by_name("gpio");
    if (s_chrg_gpio == NULL) {
        return;
    }
    /* 板上已有 100k 上拉（R27/R28）；内部弱上拉只是给悬空期兜底 */
    bflb_gpio_init(s_chrg_gpio, PIN_CHRG,
                   GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(s_chrg_gpio, PIN_STDBY,
                   GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    printf("[chrg] CHRG=IO%d STDBY=IO%d  (charging=%d full=%d usb=%d)\r\n",
           PIN_CHRG, PIN_STDBY, chrg_charging(), chrg_full(), chrg_usb_present());
}

int chrg_charging(void)
{
    return s_chrg_gpio && bflb_gpio_read(s_chrg_gpio, PIN_CHRG) == 0;
}

int chrg_full(void)
{
    return s_chrg_gpio && bflb_gpio_read(s_chrg_gpio, PIN_STDBY) == 0;
}

int chrg_usb_present(void)
{
    return chrg_charging() || chrg_full();
}

/*
 * "USB 在不在"的软件判据：充电中/已充满（CHRG/STDBY 任一为低）或已经枚举成功。
 * 局限：USB 插着但既没在充电、也没枚举（无电池/充电器故障）时看不出来 ——
 * 这种组合下 3.2V 关机态没有可等的 CHRG 下降沿，所以关机路径要尽量保守。
 */
int usb_present_any(void)
{
    return chrg_usb_present() || usb_device_is_configured(0);
}

int chrg_raw(int pin)
{
    return s_chrg_gpio ? bflb_gpio_read(s_chrg_gpio, pin) : -1;
}


/* R2c：原 lvgl_task 里的自动睡决策块（纯搬移；continue -> 返回值）。
 * 前置条件：power_is_on() == false。不碰 LVGL。
 * POWER_IDLE_SLEPT 表示这次调用真的进过 PDS 并返回（PDS1/2/3）。 */
power_idle_ret_t board_power_auto_sleep(bool pc_mode)
{
    int veto;

    /* LVGL timers are paused here; keep a low-battery safety net even
     * when PDS is blocked for another reason. */
    if (s_batt_guard_cb != NULL) {
        s_batt_guard_cb();
    }

    if (tp_int_level() == 0) {
        if (touch_poll()) {
            screen_on_safe();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        return POWER_IDLE_CONTINUE;
    }

    /* 2026-11-08：倒计时运行中不进 PDS（PDS15 唤醒=复位，RAM 状态会丢）。
     * 禁睡期间把下面的阻塞等待缩短到 200 ms，让主循环按拍轮询计时器。 */
    veto = (s_sleep_veto_cb != NULL && s_sleep_veto_cb() > 0);

    /*
     * PDS 深睡：屏幕已息 + 没插 USB + 无电脑会话 + 没触摸 + 马达没转。
     * 息屏时关掉分钟报警（不用刷时间），只留 IO16/IO17 唤醒；
     * 醒来：触摸 → 亮屏；RTC → 清标志后继续睡。
     * ⚠️ 插着 USB 绝不进 PDS（PDS 会关 USB，串口就没了）。
     */
    if (s_pds_enabled && !veto && !pc_mode && !chrg_usb_present() && !pc_link_active() &&
        !tp_int_asserted() && motor_pct() == 0 && board_power_ble_pds_allowed()) {
        int wake_src = 0;
        const uint32_t pds_ticks = (uint32_t)32768u * (uint32_t)BATTLOG_SEC;

        /* 睡前再查一次电压：低于 3.2V 直接进低电关机态，不普通深睡 */
        if (s_adc_ready) {
            int mv = board_adc_battery_mv();
            if (mv > 0 && mv <= 3200) {
                low_battery_shutdown();
            }
        }

        if (power_pds_level() >= 7) {
            /* 深度睡眠：CPU 掉电，醒来是完整启动，USB 自然重枚举。
             * 默认走 PDS15（s_pds_hbn 默认 0）；HBN（MCU <1µA）只做过手动测试，
             * 自动模式没跑过整夜，所以它是实验/回退路径，不是"优先"。
             *
             * 闹钟（2026-11-08）：开了闹钟就 arm SD3010 的 HH:MM 报警，
             * IO17 是 AON 唤醒脚（pds_pads_wake_cfg 里已使能低电平唤醒），
             * 到点 RTC 拉低 INT → PDS15 复位启动 → main 的 boot_check 补触发。
             * HBN 只配了触摸 pad，没验证过 RTC 唤醒，所以 arm 时强制走 PDS。 */
            int alarm_armed = board_power_alarm_armed();

            if (!alarm_armed) {
                (void)sd3010_alarm_disable();
                (void)sd3010_int_clear();
            } else {
                printf("[pds] sleep with RTC alarm armed (IO17 AON wake)\r\n");
            }

            if (s_pds_panel_off && !s_panel_powered_off) {
                st7802_vci_power(0);
                s_panel_powered_off = 1;
            }
            pds_log_bump(1);
            s_pds_log[6] = (s_pds_hbn && !alarm_armed) ? 0xE0 : (uint8_t)power_pds_level();
            pds_log_save();

            if (s_pds_hbn && !alarm_armed) {
                printf("[pds] auto HBN (touch IO16 wake)\r\n");
                power_hbn_enter();      /* 不返回 */
            }

            (void)power_pds_enter(0);   /* PDS15 回退，不返回 */
            vTaskDelay(pdMS_TO_TICKS(1000));
            return POWER_IDLE_SLEPT;
        }

        {
            int alarm_armed = board_power_alarm_armed();
            if (!alarm_armed) {
                (void)sd3010_alarm_disable();
                (void)sd3010_int_clear();
            }
        }

        /* 睡前后：面板先不动（前 2 分钟保持 GRAM，摸一下快亮）；
         * 第一次 2 分钟定时唤醒后才切 VCI_EN，进入省电的第二阶段。
         */
        if (s_tp_lp && !s_tp_in_lp && !s_tp_lp_scan) {
            int lp = tp_lowpower_enter();
            printf("[tp] low-power enter = 0x%02X\r\n", lp);
            if (lp >= 0 && (lp & 0x80)) {
                s_tp_in_lp = 1;
            }
        }

        for (;;) {
            pds_log_bump(1);                /* enter */
            s_pds_log[6] = (uint8_t)power_pds_level();
            pds_log_save();

            wake_src = power_pds_enter(pds_ticks);

            pds_log_bump(wake_src == 1 ? 2 : (wake_src == 2 ? 3 : 4));
            s_pds_log[5] = (uint8_t)wake_src;
            pds_log_save();

            if (s_pds_usb_reset && chrg_usb_present()) {
                usb_vbus_reboot();      /* 实验项：默认关，避免重启卡死风险 */
            }

            if (wake_src == 0) {
                /* 定时醒来：记一条电压；没别的事就继续睡 */
                batt_log_force();
                if (!chrg_usb_present()) {
                    int mv = board_adc_battery_mv();
                    if (mv > 0 && mv <= 3200) {
                        low_battery_shutdown();
                    }
                }
                if (chrg_usb_present() || tp_int_asserted() || motor_pct() > 0) {
                    break;
                }
                /* 第二阶段：已经睡过 2 分钟，面板 PMIC 可以断电了 */
                if (s_pds_panel_off && !s_panel_powered_off) {
                    st7802_vci_power(0);
                    s_panel_powered_off = 1;
                    printf("[pds] stage 2: panel VCI off\r\n");
                }
                continue;
            }
            if (wake_src == 2 && s_time_alarm_cb != NULL) {
                s_time_alarm_cb();      /* = svc_time_alarm_service */
            }
            break;
        }

        if (wake_src == 1 || tp_int_asserted()) {
            screen_on_safe();       /* 面板/VCI + 触摸低功耗恢复都在里面 */
            (void)touch_poll();
        }
        if (power_is_on()) {
            /* 原来是固定写整分报警；现在交给闹钟服务按配置重设（每天 HH:MM 或整分） */
            board_power_alarm_apply();
        }
        return POWER_IDLE_SLEPT;
    }

    {
        /* 息屏也要及时消费电脑帧和跑链路看门狗。 */
        TickType_t wait = veto ? pdMS_TO_TICKS(200)
                               : pdMS_TO_TICKS(usb_device_is_configured(0) ? 50 : 20000);
        if (pc_mode) {
            wait = pdMS_TO_TICKS(50);
        }

        if (xSemaphoreTake(s_tp_wake_sem, wait) == pdFALSE) {
            /* 超时：醒来记一条电压（飞行记录仪），然后继续睡 */
            batt_log_tick();
            return POWER_IDLE_CONTINUE;
        }
    }
    if (power_is_on()) {
        return POWER_IDLE_CONTINUE;      /* 这一秒里被别的路径点亮了 */
    }
    if (touch_poll()) {
        screen_on_safe();
    }
    return POWER_IDLE_CONTINUE;
}

#endif /* PROTORIG_HOST_TEST: board/power glue is hardware-only */
