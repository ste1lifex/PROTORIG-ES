/*
 * ui_theme.c —— 统一 UI 主题（颜色/字体/间距都在 ui_theme.h，这里只有空的主题级初始化钩子）
 */
#include "ui_theme.h"

void ui_theme_init(void)
{
    /* 目前无副作用：颜色/字体/间距都是 ui_theme.h 的宏，没有全局 LVGL 样式。
     * 以后要加全局样式，在这里创建一次并缓存（main.c 开机调一次）。 */
}
