/*
 * ui_theme.h —— 统一 UI 颜色/字体/间距
 *
 * 只定义常量和字体引用。新页面/表盘统一从这里取，
 * 避免继续往 eez_ui_glue/face_lvgl 里写散落的色值。
 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl.h"

/* 终末地风格：黑底 + 信号黄 + 工业灰 */
#define UI_COLOR_BG        0x000000u
#define UI_COLOR_SURFACE   0x141414u
#define UI_COLOR_LINE      0x2A2A2Au
#define UI_COLOR_TEXT      0xF0F0F0u
#define UI_COLOR_MUTED     0xA8A8A8u
#define UI_COLOR_ACCENT    0xFFFA00u   /* 信号黄（ctx 条/高亮） */
#define UI_COLOR_WARN      0xFF8A00u
#define UI_COLOR_ALERT     0xFF3B30u
#define UI_COLOR_OK        0x35C759u

/* 字体（fonts_gen/ 生成；LV_FONT_DECLARE 只是 extern 声明） */
LV_FONT_DECLARE(lv_font_ui14);
LV_FONT_DECLARE(lv_font_ui16);
LV_FONT_DECLARE(lv_font_ui20);
LV_FONT_DECLARE(lv_font_ui28);
LV_FONT_DECLARE(lv_font_ui48);
LV_FONT_DECLARE(lv_font_num104);
LV_FONT_DECLARE(lv_font_monitor20);

#define UI_FONT_SMALL   (&lv_font_ui14)
#define UI_FONT_BODY    (&lv_font_ui16)
#define UI_FONT_TITLE   (&lv_font_ui20)
#define UI_FONT_HEAD    (&lv_font_ui28)
#define UI_FONT_HUGE    (&lv_font_ui48)
#define UI_FONT_CLOCK   (&lv_font_num104)
#define UI_FONT_MONO    (&lv_font_monitor20)

/* 常用间距/圆角 */
#define UI_PAD_S        4
#define UI_PAD_M        8
#define UI_PAD_L        16
#define UI_RADIUS_S     6
#define UI_RADIUS_M     12

/* 目前无副作用；以后的主题级初始化（如全局样式）放这里 */
void ui_theme_init(void);

#endif /* UI_THEME_H */
