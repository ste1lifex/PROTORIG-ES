/*
 * board_display.h — ST7802 LVGL display port (BSP layer)
 *
 * R1 pure move (2026-10-28): extracted verbatim from main.c
 * ("LVGL 显示端口" + "TE 同步" sections).  No logic change.
 *
 * Dependency direction: drivers/st7802 <- bsp/board_display <- ui (lvgl_task).
 * This module only pushes LVGL dirty areas to the panel; no business logic.
 */
#ifndef BOARD_DISPLAY_H
#define BOARD_DISPLAY_H

#include <stdint.h>
#include "lvgl.h"

/*
 * Draw buffer: full width x BAND_LINES rows, RGB565 (2 B/px).
 *
 * 128 rows is the smallest value that lets the ~174-row watch-face time label
 * fit into one LVGL chunk (64 rows split it into two blits, which was the
 * "smearing at the minute roll" bug).  See V1.6 main.c history / docs.
 */
#define BAND_LINES  128
#define BAND_BYTES  (LCD_W * BAND_LINES * 2)

/*
 * TE sync + flush timing statistics.
 *
 * Still shared with the diagnostic shell commands in main.c (stats / te /
 * minroll), exactly as before the move ("cross-file sharing via extern").
 * R2 本来要把它们改成 board_display_*() 访问器 —— 截至 2026-11-15 还没做，仍是 extern。
 */
extern volatile int s_te_sync;
extern uint32_t s_te_ok, s_te_to;
extern uint32_t s_flush_us_sum, s_flush_us_max, s_flush_n;

/* 每次 blit 完成后的可选钩子（整分取证等诊断用）。R2c 起由 main.c 注册，
 * BSP 不反向依赖 face_lvgl/UI。回调在 LVGL 任务上下文、已释放 LCD 锁之后执行。 */
typedef void (*board_display_flush_hook_t)(void);
void board_display_set_flush_hook(board_display_flush_hook_t hook);

/* LVGL tick callback (lv_tick_set_cb) */
uint32_t lv_tick_ms(void);

/* Create the LVGL display, register flush / flush-policy / TE callbacks and
 * the partial-refresh draw buffer.  Call from the UI task after lv_init(). */
lv_display_t *port_disp_init(void);

#endif /* BOARD_DISPLAY_H */
