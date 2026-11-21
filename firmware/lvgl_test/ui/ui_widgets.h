#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include "ui_theme.h"

typedef struct {
    int x, y, w, h;
} ui_rect_t;

typedef enum {
    UI_ICON_BACK, UI_ICON_NEXT, UI_ICON_UP, UI_ICON_DOWN, UI_ICON_CHECK,
    UI_ICON_SETTINGS, UI_ICON_SUN, UI_ICON_LINK, UI_ICON_ARCHIVE,
    UI_ICON_STOPWATCH, UI_ICON_TIMER, UI_ICON_ALARM,
    UI_ICON_NONE            /* 纯文字条目：不画图标 */
} ui_icon_t;

typedef struct {
    lv_obj_t *root, *icon, *title, *subtitle, *mark, *underline;
} ui_row_t;

/* 纯文字按钮（无边框，v4 的"文字 + 底边线"风格）：秒表/计时器/闹钟页用。 */
typedef struct {
    lv_obj_t *root, *label;
} ui_button_t;


extern const ui_rect_t ui_back_rect;
extern const ui_rect_t ui_row_rects[3];

int ui_rect_hit(const ui_rect_t *rect, int x, int y);
lv_obj_t *ui_page_create(lv_obj_t *parent);
lv_obj_t *ui_text(lv_obj_t *parent, const char *text, const lv_font_t *font,
                  uint32_t color, int x, int y, int width, lv_text_align_t align);
lv_obj_t *ui_icon_create(lv_obj_t *parent, ui_icon_t kind, int x, int y,
                         int size, uint32_t color);
void ui_icon_set_kind(lv_obj_t *icon, ui_icon_t kind);
lv_obj_t *ui_page_header(lv_obj_t *parent, const char *title);
void ui_row_create(ui_row_t *row, lv_obj_t *parent, int index, ui_icon_t icon);
/* 自定义热区/高度的条目（App 页用；ui_row_create 就是 index 版包装）。 */
void ui_row_create_at(ui_row_t *row, lv_obj_t *parent, const ui_rect_t *rect,
                      ui_icon_t icon);
/* selected -1 draws a navigation chevron, 0 a choice, 1 a selected choice. */
void ui_row_set(ui_row_t *row, const char *title, const char *subtitle,
                int enabled, int selected);
void ui_row_press(ui_row_t *row, int pressed);

/* 文字按钮：创建一次，运行时只改文字/按下态。 */
void ui_button_create(ui_button_t *btn, lv_obj_t *parent, const char *text,
                      int x, int y, int w, int h, const lv_font_t *font);
void ui_button_set(ui_button_t *btn, const char *text, uint32_t color);
void ui_button_press(ui_button_t *btn, int pressed);

#endif
