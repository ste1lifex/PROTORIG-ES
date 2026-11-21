/*
 * app_list.h — R2.1 圆形 App 列表（F1）
 *
 * 独立全屏容器，不占用 eez_ui_glue 的 8 个 page 数组；输入仍由 eez_ui_glue 的
 * 全屏手势层统一转发（press/swipe/release），本模块只负责圆形列表 UI、
 * 3 项/页、上下整页、返回与 App 启动。
 */
#ifndef APP_LIST_H
#define APP_LIST_H

#include <stdint.h>

void app_list_init(void);

int  app_list_visible(void);
int  app_list_return_armed(void);

void app_list_show(void);
void app_list_hide(void);

/* 输入转发：坐标/位移/按下时长（ms） */
void app_list_press(int x, int y);
void app_list_swipe(int dx, int dy);
void app_list_release(int dx, int dy, uint32_t press_ms);
/* P0-1：PRESS_LOST / 被吞掉的触摸 -> 只取消候选，绝不启动 App */
void app_list_cancel(void);

#endif /* APP_LIST_H */
