/*
 * app_notify.h —— 通知卡（文本队列 + 圆屏居中卡片 + 位图卡）
 *
 * 文本：app_notify_show() 只入队、不碰 LVGL（BLE/服务侧安全），
 *       卡片由 UI 任务的 app_notify_ui_tick() 取出显示 3 s；
 * 位图：svc_ble 收完分片后调 app_notify_show_bitmap()，短卡点按展开。
 */
#ifndef APP_NOTIFY_H
#define APP_NOTIFY_H

#include <stdint.h>

#define APP_NOTIFY_TITLE_MAX 32
#define APP_NOTIFY_BODY_MAX  96

void app_notify_init(void);

/* 简单通知卡片（居中 368x136，整张卡都在圆屏安全圆内）。只在 UI 任务调用；首次 tick 时懒创建。 */
void app_notify_ui_init(void);
void app_notify_ui_tick(uint32_t now_ms);

/* 保存一条通知（截断 UTF-8 安全由调用方保证；这里只按字节截断）。
 * 返回 0 成功；不阻塞、不碰 LVGL。 */
int app_notify_show(const char *title, const char *body);

/* 标记弹层已隐藏（UI 调用）。 */
void app_notify_hide(void);

int app_notify_visible(void);
int app_notify_pending(void);       /* 1 = UI 需要构建/刷新弹层 */
void app_notify_clear_pending(void);

/* R2.1-BMP：位图通知（data 含 LVGL 调色板，后面跟像素；由 svc_ble 静态缓冲持有） */
int app_notify_show_bitmap(uint8_t cat, uint16_t w, uint16_t h, uint8_t bpp,
                           const uint8_t *data, uint32_t total);
int app_notify_bitmap_visible(void);
void app_notify_bmp_dismiss(void);
int  app_notify_bmp_expanded(void);

const char *app_notify_title(void);
const char *app_notify_body(void);

#endif /* APP_NOTIFY_H */
