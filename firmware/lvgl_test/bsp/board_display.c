/*
 * board_display.c — ST7802 LVGL display port (BSP layer)
 *
 * R1 pure move (2026-10-28).  The functions and state below are copied
 * verbatim from firmware/lvgl_test/main.c (called firmware_m2/ back then):
 *
 *     lv_tick_ms()
 *     disp_flush()
 *     disp_render_start_cb()
 *     port_disp_init()
 *     s_buf1[], TE/flush timing statics, minute-capture counter
 *
 * Only the file boundary changed (plus the LCD mutex declaration, which R1
 * still leaves owned by main.c).  No logic was modified.
 */
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "bflb_mtimer.h"

#include "lvgl.h"
#include "flush_policy.h"

#include "board_display.h"
#include "st7802.h"

/* LCD mutex is created by main.c in R1 (kept there to keep the move pure);
 * R2 打算把 mutex 和 LCD_LOCK/UNLOCK 一起搬进 BSP —— 截至 2026-11-15 仍未搬，
 * main.c 还是唯一创建者，所以这里只能 extern。 */
extern SemaphoreHandle_t s_lcd_mtx;
#define LCD_LOCK()   xSemaphoreTake(s_lcd_mtx, portMAX_DELAY)
#define LCD_UNLOCK() xSemaphoreGive(s_lcd_mtx)

static uint8_t s_buf1[BAND_BYTES] __attribute__((aligned(64)));

/* TE sync and flush timing (shared with diag commands via board_display.h) */
#define TE_WAIT_US 20000        /* 60Hz 一帧 16.7ms；等不到沿就照常推，不能卡死 */

volatile int s_te_sync = 1;     /* te 0/1 可切，现场 A/B */
uint32_t s_te_ok, s_te_to;      /* 等到沿 / 超时 的次数 */
uint32_t s_flush_us_sum, s_flush_us_max, s_flush_n;

/* R2c：整分取证钩子由 main.c（diag 侧）注册。BSP 不再 include face_lvgl.h，
 * 保持 drivers -> bsp -> services/ui 的单向依赖。 */
static board_display_flush_hook_t s_flush_hook;

void board_display_set_flush_hook(board_display_flush_hook_t hook)
{
    s_flush_hook = hook;
}

uint32_t lv_tick_ms(void)
{
    return (uint32_t)bflb_mtimer_get_time_ms();
}

/*
 * LVGL 会把"这块脏区"渲染进 px_map（紧密排列，宽 × 高），
 * 我们要做的就是把它推进 ST7802 对应的窗口里。
 * 这里是同步的（st7802_push_pixels565 内部轮询 DBI FIFO），
 * 所以 flush 返回后立刻 flush_ready。
 */
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t t0 = (uint32_t)bflb_mtimer_get_time_us();

    /* panelsleep 实验期间面板已 DISPOFF/SLPIN/断电：丢掉这一帧，别往 DBI
     * 灌数据（flush_ready 必须照调，否则 LVGL 会卡住）。 */
    if (st7802_panel_sleep_level() > 0) {
        lv_display_flush_ready(disp);
        return;
    }

    LCD_LOCK();
    st7802_blit565((uint16_t)area->x1, (uint16_t)area->y1,
                   (uint16_t)area->x2, (uint16_t)area->y2,
                   (const uint16_t *)px_map);
    LCD_UNLOCK();

    /* 每次 blit 完成后调用 UI/diag 侧注册的钩子（整分取证等）。
     * 钩子只在 armed 时做事；没有钩子时零开销。 */
    if (s_flush_hook != NULL) {
        s_flush_hook();
    }

    /* 送屏耗时统计：一帧的活 = 送屏 + 渲染，判断"是不是带宽到顶"就靠这个数 */
    {
        uint32_t dt = (uint32_t)bflb_mtimer_get_time_us() - t0;
        s_flush_us_sum += dt;
        s_flush_n++;
        if (dt > s_flush_us_max) s_flush_us_max = dt;
    }

    lv_display_flush_ready(disp);
}

/*
 * 挂在 LV_EVENT_RENDER_START 上：**每渲染一帧只等一次 TE**。
 * 绝不能写在 disp_flush 里 —— 一块整屏脏区会被 LVGL 拆成十几个带，
 * 那样每帧要等十几次 V-Blank，直接变成 4fps。
 */
static void disp_render_start_cb(lv_event_t *e)
{
    (void)e;
    if (!s_te_sync) {
        return;
    }
    if (st7802_te_wait(TE_WAIT_US) == 0) {
        s_te_ok++;
    } else {
        s_te_to++;
    }
}

lv_display_t *port_disp_init(void)
{
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(disp, disp_flush);
    lv_display_add_event_cb(disp, flush_policy_event, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, disp_render_start_cb, LV_EVENT_RENDER_START, NULL);

    /* 部分刷新：只把脏区推给面板，而不是整帧 */
    lv_display_set_buffers(disp, s_buf1, NULL, BAND_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    return disp;
}
