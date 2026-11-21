/*
 * pages.h —— 8 个前台页面 App（page_id 0..7）
 *
 * page_id 0..7 = 表盘 / 协议网络 / 协议网络记录 / 终端设置 / USB 监控 /
 *                秒表 / 计时器 / 闹钟。
 * 只登记 + load 时调 eez_ui_goto_raw()；原生布局：0 在 face_lvgl.c，
 * 1..3 在 ui/device_pages.c，4 在 pc_monitor.c，5..7 在 ui/app_pages.c。
 */
#ifndef APP_PAGES_H
#define APP_PAGES_H

void pages_register(void);

#endif /* APP_PAGES_H */
