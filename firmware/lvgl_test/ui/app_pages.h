/*
 * app_pages.h —— 秒表 / 计时器 / 闹钟三个原生 App 页（2026-11-08）
 *
 * 已接入：eez_ui_glue.c 的 PAGE_N = 8，页 5..7 由 app_page_create() 建；
 * 页面和触摸都由本模块负责，手势层只转发 press/release。
 *
 * 页号必须与 apps/pages.c 的 app_desc.page_id、apps/app_list.c 的条目一致。
 */
#ifndef APP_PAGES_H
#define APP_PAGES_H

#include "lvgl.h"

#define APP_PAGE_STOPWATCH 5
#define APP_PAGE_TIMER     6
#define APP_PAGE_ALARM     7
#define APP_PAGE_FIRST     APP_PAGE_STOPWATCH
#define APP_PAGE_LAST      APP_PAGE_ALARM

/* 返回 1 = 是 App 页 */
int app_page_is(int page);

/* 创建页容器（eez_ui_init 里一次） */
lv_obj_t *app_page_create(lv_obj_t *parent, int page);

/* 进入页面时调用：重置按下态并按当前服务状态刷一遍显示 */
void app_page_open(int page);

/* 刷新显示（100 ms 节拍；只改变化了的标签） */
void app_page_tick(int page);

/* 秒表的百分秒需要更快刷新（33 ms 节拍） */
void app_page_tick_fast(int page);

/* 触摸：返回目标号（0 = 没命中，1 = 返回键留给手势层） */
int  app_page_target(int page, int x, int y);
void app_page_press(int page, int target, int pressed);
void app_page_activate(int page, int target);

#endif /* APP_PAGES_H */
