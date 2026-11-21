/*
 * device_pages.h —— 原生设备页布局接口（终端设置 / 协议网络占位 / 相遇记录占位）
 *
 * page 3 = 终端设置（亮度 / 自动息屏 / 连接模式），由 device_settings_* 这批函数
 *          驱动选中态与返回；
 * page 1 = 协议网络、page 2 = 相遇记录：页面布局还在，但入口在 App 列表里
 *          hidden=1（协议 v1 未接入，2026-11-15）。恢复时把 app_list.c 对应条目
 *          的 hidden 改回 0；布局本身在 device_page_create() 里按 page 号建。
 */
#ifndef DEVICE_PAGES_H
#define DEVICE_PAGES_H

#include "lvgl.h"

lv_obj_t *device_page_create(lv_obj_t *parent, int page);
void device_settings_open(void);
void device_settings_tick(void);
int device_settings_back(void);
int device_settings_target(int x, int y);
void device_settings_press(int target, int pressed);
void device_settings_activate(int target);

#endif
