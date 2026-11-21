/*
 * svc_battery.c —— 电池/充电服务（R3c 第二步）
 *
 * 从 main.c 的 batt_timer_cb + bsp/board_power.c 的 screen_off_battery_guard
 * 搬入；差异只有：
 *   - 不碰 LVGL/motor：表盘/警告色/震动交给 UI 按 app_state/事件处理；
 *   - 低电 idle 策略改走 policy 回调（不在服务里调 power_set_idle_ms）；
 *   - 写 app_state（batt_seq）+ EV_BATTERY / EV_CHARGING / EV_LOW_BATTERY。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board_adc.h"
#include "board_power.h"
#include "motor.h"
#include "app_state.h"
#include "event_queue.h"
#include "svc_battery.h"

#define BATT_ADC_ENABLE 1
#define BATT_SAMPLE_MS  3000u

/* ---- 采样/分级状态（原 batt_timer_cb 的 static） ---- */
static int s_filt = -1;
static int s_last_log = -100000;
static int s_low_strikes;
static int s_low_lvl;
static int s_force_low = -1;

/* ---- 状态变化检测（事件用） ---- */
static int s_have_last;
static int s_last_mv;
static int s_last_pct;
static int s_last_chg;
static int s_last_lvl;

/* ---- 节流 ---- */
static uint32_t s_last_tick_ms;
static int s_ticked;

/* ---- 息屏兜底（原 screen_off_battery_guard 的 static） ---- */
static uint32_t s_guard_next;
static int s_guard_strikes;

/* ---- 低电策略回调（main 注入） ---- */
static svc_battery_policy_cb_t s_policy_cb;

void svc_battery_set_policy_cb(svc_battery_policy_cb_t cb)
{
    s_policy_cb = cb;
}

static void battery_publish(int mv, int pct, int chg, int lvl)
{
    event_t ev;
    int data_changed;
    int chg_changed;
    int changed;

    data_changed = !s_have_last || abs(mv - s_last_mv) >= 10 || pct != s_last_pct ||
                   lvl != s_last_lvl;
    chg_changed = !s_have_last || chg != s_last_chg;
    changed = data_changed || chg_changed;

    /* 只在真的有变化时写 app_state（batt_seq 才递增），避免 UI 每 3 s 无谓重画 */
    if (changed) {
        app_state_set_battery(mv, pct, (uint8_t)chg, (uint8_t)lvl);
    }

    if (chg_changed) {
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_CHARGING;
        ev.len = 1;
        ev.data[0] = (uint8_t)chg;
        (void)event_queue_push(&ev);
    }
    if (changed) {
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_BATTERY;
        ev.len = 4;
        ev.data[0] = (uint8_t)(mv & 0xFF);
        ev.data[1] = (uint8_t)((mv >> 8) & 0xFF);
        ev.data[2] = (uint8_t)pct;
        ev.data[3] = (uint8_t)lvl;
        (void)event_queue_push(&ev);
    }

    s_have_last = 1;
    s_last_mv = mv;
    s_last_pct = pct;
    s_last_chg = chg;
    s_last_lvl = lvl;
}

static void battery_set_level(int lvl, int mv)
{
    event_t ev;

    if (lvl == s_low_lvl) {
        return;
    }
    s_low_lvl = lvl;

    if (lvl == 1) {
        printf("[batt] LOW 1: %d mV -> idle 8 s\r\n", mv);
    } else if (lvl == 2) {
        printf("[batt] LOW 2: %d mV -> idle 3 s\r\n", mv);
    } else {
        printf("[batt] battery recovered: %d mV -> normal\r\n", mv);
    }

    /* 服务之间不直接调用：idle 策略交给 main 注入的 power 侧回调 */
    if (s_policy_cb != NULL) {
        s_policy_cb(lvl, mv);
    }

    memset(&ev, 0, sizeof(ev));
    ev.type = EV_LOW_BATTERY;
    ev.len = 3;
    ev.data[0] = (uint8_t)lvl;
    ev.data[1] = (uint8_t)(mv & 0xFF);
    ev.data[2] = (uint8_t)((mv >> 8) & 0xFF);
    (void)event_queue_push(&ev);
}

/* 返回 1 = 本次采样成功，0 = 跳过/失败 */
static int battery_sample(void)
{
#if BATT_ADC_ENABLE
    int mv, pct, chg, lvl;

    if (!s_adc_ready) {
        printf("[batt] adc_init = %d\r\n", board_adc_init());
        s_adc_ready = 1;
    }

    /* 马达在震的时候 BAT+ 会被拉低十几 mV —— 这时候采样只会得到噪声 */
    if (motor_pct() > 0) {
        return 0;
    }

    mv = board_adc_battery_mv();
    if (mv < 0) {
        return 0;
    }

    /* 一阶低通（新值占 1/4） */
    s_filt = (s_filt < 0) ? mv : (s_filt + (mv - s_filt) / 4);
    pct = board_adc_battery_percent(s_filt);
    chg = chrg_charging();

    /*
     * 3.2V 主动关机：连续 3 次（约 9 秒）都在阈值以下才执行；
     * USB 一插着就清零。关机动作仍在 board_power（bsp）。
     */
    if (!chg && !chrg_usb_present() && s_filt > 0 && s_filt <= 3200) {
        if (++s_low_strikes >= 3) {
            low_battery_shutdown();
        }
    } else {
        s_low_strikes = 0;
    }

    /* 只在真的变了 10 mV 以上才打日志 */
    if (s_last_log < 0 || abs(s_filt - s_last_log) >= 10) {
        printf("[batt] %d mV  %d%%  %s\r\n", s_filt, pct, chg ? "CHG" : "discharging");
        s_last_log = s_filt;
    }

    /* 低电分级：充电中自动清零（force 调试除外） */
    if (chg && s_force_low < 0) {
        lvl = 0;
        if (s_low_lvl != 0) {
            printf("[batt] charging -> low-battery state cleared\r\n");
        }
    } else if (s_force_low >= 0) {
        lvl = s_force_low;
    } else if (s_filt <= 3350) {
        lvl = 2;
    } else if (s_filt <= 3450) {
        lvl = 1;
    } else {
        lvl = 0;
    }
    battery_set_level(lvl, s_filt);
    battery_publish(s_filt, pct, chg, lvl);
    return 1;
#else
    return 0;
#endif
}

int svc_battery_tick(uint32_t now_ms)
{
    if (s_ticked && (uint32_t)(now_ms - s_last_tick_ms) < BATT_SAMPLE_MS) {
        return 0;
    }
    s_ticked = 1;
    s_last_tick_ms = now_ms;
    return battery_sample();
}

int svc_battery_sample_now(void)
{
    return battery_sample();
}

int svc_battery_get(int *mv, int *pct, int *charging)
{
    app_state_t st;

    app_state_get(&st);
    if (mv != NULL) {
        *mv = st.battery_mv;
    }
    if (pct != NULL) {
        *pct = st.battery_pct;
    }
    if (charging != NULL) {
        *charging = st.charging;
    }
    return 0;
}

int svc_battery_get_level(void)
{
    return s_low_lvl;
}

int svc_battery_get_force(void)
{
    return s_force_low;
}

void svc_battery_force_level(int level)
{
    if (level < -1) {
        level = -1;
    }
    if (level > 2) {
        level = 2;
    }
    s_force_low = level;
}

/*
 * 息屏期间 20 s 低电兜底（原 board_power 的 screen_off_battery_guard）。
 * 只在 UI 任务上下文（board_power auto_sleep 回调）调用。
 */
void svc_battery_off_guard(void)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int mv;

    if (s_guard_next && now < s_guard_next) {
        return;
    }
    s_guard_next = now + 20000u;

    if (!s_adc_ready) {
        (void)board_adc_init();
        s_adc_ready = 1;
    }
    mv = board_adc_battery_mv();
    if (mv <= 0) {
        return;
    }
    if (!chrg_usb_present() && mv <= 3200) {
        if (++s_guard_strikes >= 3) {
            low_battery_shutdown();
            s_guard_strikes = 0;
        }
    } else {
        s_guard_strikes = 0;
    }
}

void svc_battery_init(void)
{
    s_filt = -1;
    s_last_log = -100000;
    s_low_strikes = 0;
    s_low_lvl = 0;
    s_force_low = -1;
    s_have_last = 0;
    s_ticked = 0;
    s_last_tick_ms = 0;
    s_guard_next = 0;
    s_guard_strikes = 0;

    /* 息屏兜底仍由 board_power 的 auto_sleep 触发，但实现由本服务接管：
     * 通过回调注入，避免 bsp 反向依赖 services。 */
    board_power_set_battery_guard_cb(svc_battery_off_guard);
}
