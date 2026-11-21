/*
 * svc_time.c —— RTC 时间服务（R3c）
 *
 * 从 main.c 原样搬入：rtc_int_isr / rtc_int_level / rtc_int_service /
 * rtc_int_init / rtc_poll / rtc_timer_cb / rtc_seed_if_needed。
 * 差异（设计允许的最小改动）：
 *   - rtc_poll 不再调 face_lvgl_set_time，改为写 app_state + 事件；
 *   - rtc_timer_cb 变成 svc_time_tick(now_ms)，由 UI 定时器驱动；
 *   - PDS 恢复/报警清理改成 board_power 注册回调；
 *   - 多一个 svc_time_set() 供 shell/BLE/demo 共用。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "bflb_gpio.h"

#include "sd3010.h"
#include "rtc_seed.h"
#include "board_power.h"
#include "app_state.h"
#include "event_queue.h"
#include "svc_time.h"

#define RTC_POLL_MS 500u

static SemaphoreHandle_t s_rtc_mtx;
static SemaphoreHandle_t s_rtc_wake_sem;
static volatile uint32_t s_rtc_irq_count;
static volatile uint32_t s_rtc_service_count;
static volatile int s_alarm_pending;    /* R3c-time2：ISR 到过就置位，serviced 可信 */
static volatile int s_rtc_int_ready;

static uint32_t s_last_poll_ms;
static int s_polled;
static int s_last_hhmm = -1;
static int s_have_last;

/* ------------------------------------------------------------------ */
/* 锁 / 中断                                                           */
/* ------------------------------------------------------------------ */

void svc_time_lock(void)
{
    if (s_rtc_mtx != NULL) {
        (void)xSemaphoreTake(s_rtc_mtx, portMAX_DELAY);
    }
}

void svc_time_unlock(void)
{
    if (s_rtc_mtx != NULL) {
        (void)xSemaphoreGive(s_rtc_mtx);
    }
}

static void rtc_int_isr(uint8_t pin)
{
    BaseType_t woken = pdFALSE;

    (void)pin;
    s_rtc_irq_count++;
    /* R3c-time2：报警脉冲可能短于 500 ms 轮询周期，轮询到达时 INT 已回高；
     * 先记 pending，让 svc_time_alarm_service() 至少尝试清一次 INTAF。 */
    s_alarm_pending = 1;
    if (s_rtc_wake_sem != NULL) {
        xSemaphoreGiveFromISR(s_rtc_wake_sem, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

int svc_time_int_level(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    return gpio ? (bflb_gpio_read(gpio, SVC_TIME_RTC_INT_PIN) ? 1 : 0) : 1;
}

void svc_time_get_irq_stats(uint32_t *irq_count, uint32_t *serviced_count)
{
    if (irq_count != NULL) {
        *irq_count = s_rtc_irq_count;
    }
    if (serviced_count != NULL) {
        *serviced_count = s_rtc_service_count;
    }
}

void svc_time_alarm_service(void)
{
    if (!s_rtc_int_ready) {
        return;
    }
    /* R3c-time2：ISR 到过（pending=1）就一定要尝试清一次，即使此刻 INT 已回高。
     * 否则报警脉冲早于 500 ms 轮询时会被别的读路径清掉、serviced 永远不增长。 */
    if (!s_alarm_pending && svc_time_int_level() != 0) {
        return;
    }
    if (sd3010_int_clear() == 0) {
        s_alarm_pending = 0;
        s_rtc_service_count++;
    }
}

void svc_time_restore_irq(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    if (gpio == NULL) {
        return;
    }
    bflb_gpio_irq_attach(SVC_TIME_RTC_INT_PIN, rtc_int_isr);
    bflb_irq_enable(gpio->irq_num);
}

static void rtc_int_init(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    s_rtc_wake_sem = xSemaphoreCreateBinary();
    bflb_gpio_init(gpio, SVC_TIME_RTC_INT_PIN,
                   GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_irq_attach(SVC_TIME_RTC_INT_PIN, rtc_int_isr);
    bflb_irq_enable(gpio->irq_num);
    s_rtc_int_ready = 1;
    printf("[rtc] int armed on IO%d (falling edge, AON)\r\n", SVC_TIME_RTC_INT_PIN);
}

/* ------------------------------------------------------------------ */
/* 播种（PDS15 / 掉电判据保持 V1.6 原样）                              */
/* ------------------------------------------------------------------ */

static void rtc_seed_if_needed(void)
{
    sd3010_time_t tm;
    sd3010_time_t seed;
    uint8_t ctr1 = 0;
    int osf;
    int lost;

    svc_time_lock();
    if (sd3010_read(&tm) != 0) {
        svc_time_unlock();
        printf("[rtc] read failed\r\n");
        return;
    }
    (void)sd3010_get_status(&ctr1, NULL);
    lost = sd3010_check_magic();
    svc_time_unlock();

    osf = (ctr1 & 0x40) ? 1 : 0;
    if (osf) {
        printf("[rtc] OSF=1 -> oscillator stopped, time is lost\r\n");
    }
    if (lost != 0) {
        printf("[rtc] power-loss marker %s\r\n",
               (lost < 0) ? "read FAILED" : "MISSING -> RTC lost its backup power");
    }

    if (!osf && lost == 0 &&
        !rtc_seed_looks_invalid(2000 + tm.year, tm.month, tm.day, tm.week)) {
        printf("[rtc] ok: %04d-%02d-%02d %02d:%02d:%02d (week %d, ctr1 0x%02X)\r\n",
               2000 + tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec,
               tm.week, ctr1);
        return;
    }

    printf("[rtc] INVALID (%04d-%02d-%02d week=%d, osf=%d, lost=%d, ctr1=0x%02X) -> seed from build time\r\n",
           2000 + tm.year, tm.month, tm.day, tm.week, osf, lost, ctr1);
    if (rtc_seed_from_build(&seed)) {
        svc_time_lock();
        printf("[rtc] seed=%d\r\n", sd3010_set_time(&seed));
        (void)sd3010_clear_osf();
        (void)sd3010_write_magic();
        svc_time_unlock();

        svc_time_lock();
        if (sd3010_read(&tm) == 0) {
            printf("[rtc] now: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                   2000 + tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec);
        }
        svc_time_unlock();
    }
}

/* ------------------------------------------------------------------ */
/* 轮询 / 事件                                                         */
/* ------------------------------------------------------------------ */

static void svc_time_publish(const sd3010_time_t *tm)
{
    int hhmm = (int)tm->hour * 100 + (int)tm->min;

    app_state_set_time(2000 + tm->year, tm->month, tm->day,
                       tm->week & 0x07, tm->hour, tm->min, tm->sec);

    if (s_have_last && hhmm != s_last_hhmm) {
        event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_RTC_MINUTE;
        ev.len = 2;
        ev.data[0] = tm->hour;
        ev.data[1] = tm->min;
        (void)event_queue_push(&ev);
    }
    s_last_hhmm = hhmm;
    s_have_last = 1;
}

/* 读 RTC、更新 app_state、清分钟报警。只由 UI/shell 任务调用。 */
static int svc_time_poll(void)
{
    sd3010_time_t tm;

    svc_time_lock();
    if (sd3010_read(&tm) != 0) {
        svc_time_unlock();
        return -1;
    }
    svc_time_unlock();

    if (tm.week > 6) {
        tm.week = sd3010_calc_week((uint16_t)(2000 + tm.year), tm.month, tm.day);
    }
    svc_time_publish(&tm);

    /* 分钟报警：INT(IO17) 低了就把 INTAF 清掉，否则线会一直低 */
    svc_time_alarm_service();
    return 0;
}

void svc_time_tick(uint32_t now_ms)
{
    if (s_polled && (uint32_t)(now_ms - s_last_poll_ms) < RTC_POLL_MS) {
        return;
    }
    s_last_poll_ms = now_ms;
    s_polled = 1;
    (void)svc_time_poll();
}

int svc_time_get(sd3010_time_t *out)
{
    int r;

    if (out == NULL) {
        return -1;
    }
    svc_time_lock();
    r = sd3010_read(out);
    svc_time_unlock();
    return r;
}

int svc_time_set(const sd3010_time_t *tm)
{
    sd3010_time_t cur;
    int r;

    if (tm == NULL) {
        return -1;
    }

    svc_time_lock();
    r = sd3010_set_time(tm);
    if (r >= 0) {
        (void)sd3010_clear_osf();
        (void)sd3010_write_magic();
    }
    svc_time_unlock();
    printf("settime -> %d\r\n", r);

    if (r >= 0 && svc_time_get(&cur) == 0) {
        int wk = cur.week & 0x07;
        if (wk > 6) {
            wk = sd3010_calc_week((uint16_t)(2000 + cur.year), cur.month, cur.day);
        }
        cur.week = (uint8_t)wk;
        svc_time_publish(&cur);

        event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_TIME_SET;
        ev.len = 6;
        ev.data[0] = cur.year;
        ev.data[1] = cur.month;
        ev.data[2] = cur.day;
        ev.data[3] = cur.hour;
        ev.data[4] = cur.min;
        ev.data[5] = cur.sec;
        (void)event_queue_push(&ev);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* 启动                                                                */
/* ------------------------------------------------------------------ */

void svc_time_init(void)
{
    s_rtc_mtx = xSemaphoreCreateMutex();

    rtc_int_init();
    rtc_seed_if_needed();

    /* PDS 醒来：board_power 恢复 GPIO 中断时回调 svc_time_restore_irq，
     * 定时醒来/触摸醒来清报警时回调 svc_time_alarm_service。 */
    board_power_set_time_callbacks(svc_time_restore_irq, svc_time_alarm_service);

    printf("[rtc] minute alarm = %d (sec=0)\r\n", sd3010_alarm_set_sec(0));

    /* 首次读一次，填 app_state（不投分钟事件，避免开机假事件） */
    (void)svc_time_poll();
    s_last_poll_ms = 0;
    s_polled = 0;
}
