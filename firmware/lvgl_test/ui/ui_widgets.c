#include <stdint.h>
#include "ui_widgets.h"

const ui_rect_t ui_back_rect = {112, 64, 80, 48};
const ui_rect_t ui_row_rects[3] = {
    {112, 112, 256, 96}, {112, 208, 256, 96}, {112, 304, 256, 96}
};

int ui_rect_hit(const ui_rect_t *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

lv_obj_t *ui_page_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 480, 480);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return page;
}

lv_obj_t *ui_text(lv_obj_t *parent, const char *text, const lv_font_t *font,
                  uint32_t color, int x, int y, int width, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    return label;
}

static void icon_line(lv_layer_t *layer, const lv_area_t *area, lv_color_t color,
                       int x1, int y1, int x2, int y2)
{
    int size = lv_area_get_width(area);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = size >= 48 ? 3 : 2;
    line.p1.x = area->x1 + x1 * size / 24;
    line.p1.y = area->y1 + y1 * size / 24;
    line.p2.x = area->x1 + x2 * size / 24;
    line.p2.y = area->y1 + y2 * size / 24;
    lv_draw_line(layer, &line);
}

static void icon_circle(lv_layer_t *layer, const lv_area_t *area, lv_color_t color,
                         int x, int y, int radius)
{
    int size = lv_area_get_width(area);
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = color;
    arc.width = size >= 48 ? 3 : 2;
    arc.center.x = area->x1 + x * size / 24;
    arc.center.y = area->y1 + y * size / 24;
    arc.radius = radius * size / 24;
    arc.start_angle = 0;
    arc.end_angle = 360;
    lv_draw_arc(layer, &arc);
}

static void icon_draw(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    lv_color_t color = lv_obj_get_style_text_color(obj, 0);
    ui_icon_t kind = (ui_icon_t)(uintptr_t)lv_obj_get_user_data(obj);
#define LINE(a,b,c,d) icon_line(layer, &area, color, a,b,c,d)
#define CIRCLE(a,b,r) icon_circle(layer, &area, color, a,b,r)
    switch (kind) {
    case UI_ICON_BACK: LINE(15, 5, 8, 12); LINE(8, 12, 15, 19); break;
    case UI_ICON_NEXT: LINE(9, 5, 16, 12); LINE(16, 12, 9, 19); break;
    case UI_ICON_UP: LINE(5, 15, 12, 8); LINE(12, 8, 19, 15); break;
    case UI_ICON_DOWN: LINE(5, 9, 12, 16); LINE(12, 16, 19, 9); break;
    case UI_ICON_CHECK: LINE(5, 12, 10, 17); LINE(10, 17, 20, 6); break;
    case UI_ICON_SETTINGS:
        LINE(3, 6, 21, 6); LINE(3, 12, 21, 12); LINE(3, 18, 21, 18);
        LINE(8, 3, 8, 9); LINE(16, 9, 16, 15); LINE(10, 15, 10, 21);
        break;
    case UI_ICON_SUN:
        CIRCLE(12, 12, 5);
        LINE(12, 1, 12, 4); LINE(12, 20, 12, 23);
        LINE(1, 12, 4, 12); LINE(20, 12, 23, 12);
        LINE(4, 4, 6, 6); LINE(18, 18, 20, 20);
        LINE(4, 20, 6, 18); LINE(18, 6, 20, 4);
        break;
    case UI_ICON_LINK:
        CIRCLE(5, 6, 3); CIRCLE(19, 6, 3); CIRCLE(12, 19, 3);
        LINE(8, 6, 16, 6); LINE(6, 9, 10, 16); LINE(18, 9, 14, 16);
        break;
    case UI_ICON_ARCHIVE:
        LINE(3, 5, 21, 5); LINE(3, 12, 21, 12); LINE(3, 19, 16, 19);
        break;
    case UI_ICON_STOPWATCH:
        CIRCLE(12, 14, 8); LINE(9, 2, 15, 2);
        LINE(12, 3, 12, 6); LINE(12, 14, 16, 10);
        break;
    case UI_ICON_TIMER:
        LINE(5, 3, 19, 3); LINE(5, 21, 19, 21);
        LINE(6, 4, 18, 20); LINE(18, 4, 6, 20);
        break;
    case UI_ICON_ALARM:
        CIRCLE(12, 13, 7);
        LINE(12, 13, 12, 8); LINE(12, 13, 16, 15);
        LINE(6, 3, 9, 6); LINE(18, 3, 15, 6);
        break;
    case UI_ICON_NONE:
    default:
        break;
    }
#undef LINE
#undef CIRCLE
}

lv_obj_t *ui_icon_create(lv_obj_t *parent, ui_icon_t kind, int x, int y,
                         int size, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, size, size);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, (void *)(uintptr_t)kind);
    lv_obj_add_event_cb(obj, icon_draw, LV_EVENT_DRAW_MAIN, NULL);
    return obj;
}

lv_obj_t *ui_page_header(lv_obj_t *parent, const char *title)
{
    ui_icon_create(parent, UI_ICON_BACK, 136, 76, 28, UI_COLOR_TEXT);
    return ui_text(parent, title, UI_FONT_HEAD, UI_COLOR_TEXT,
                   192, 69, 156, LV_TEXT_ALIGN_CENTER);
}

void ui_icon_set_kind(lv_obj_t *icon, ui_icon_t kind)
{
    lv_obj_set_user_data(icon, (void *)(uintptr_t)kind);
    lv_obj_invalidate(icon);
}

void ui_row_create(ui_row_t *row, lv_obj_t *parent, int index, ui_icon_t icon)
{
    ui_row_create_at(row, parent, &ui_row_rects[index], icon);
}

/* 高度 >= 88 用标准三行布局（标题/副标题/底边线），更矮的条目收紧凑布局。 */
void ui_row_create_at(ui_row_t *row, lv_obj_t *parent, const ui_rect_t *r,
                      ui_icon_t icon)
{
    int compact = r->h < 88;
    int title_y = compact ? 6 : 12;
    int sub_y = compact ? 40 : 54;

    row->root = lv_obj_create(parent);
    lv_obj_remove_style_all(row->root);
    lv_obj_set_pos(row->root, r->x, r->y);
    lv_obj_set_size(row->root, r->w, r->h);
    lv_obj_set_style_bg_color(row->root, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_remove_flag(row->root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    row->icon = ui_icon_create(row->root, icon, 4, compact ? (r->h - 28) / 2 : 27,
                               28, UI_COLOR_ACCENT);
    row->title = ui_text(row->root, "", UI_FONT_HEAD, UI_COLOR_TEXT,
                         52, title_y, 172, LV_TEXT_ALIGN_LEFT);
    row->subtitle = (r->h >= 64)
        ? ui_text(row->root, "", UI_FONT_BODY, UI_COLOR_MUTED,
                  52, sub_y, 196, LV_TEXT_ALIGN_LEFT)
        : NULL;
    row->mark = ui_icon_create(row->root, UI_ICON_NEXT, 232,
                               compact ? (r->h - 20) / 2 : 32, 20, UI_COLOR_MUTED);
    row->underline = lv_obj_create(row->root);
    lv_obj_remove_style_all(row->underline);
    lv_obj_set_pos(row->underline, 52, compact ? (r->h - 8) : 86);
    lv_obj_set_size(row->underline, 40, 2);
    lv_obj_set_style_bg_color(row->underline, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(row->underline, LV_OPA_COVER, 0);
    lv_obj_remove_flag(row->underline, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

void ui_row_set(ui_row_t *row, const char *title, const char *subtitle,
                int enabled, int selected)
{
    lv_label_set_text(row->title, title);
    if (row->subtitle != NULL) {
        lv_label_set_text(row->subtitle, subtitle ? subtitle : "");
    }
    uint32_t ink = enabled ? UI_COLOR_TEXT : UI_COLOR_MUTED;
    lv_obj_set_style_text_color(row->title, lv_color_hex(selected > 0 ? UI_COLOR_ACCENT : ink), 0);
    lv_obj_set_style_text_color(row->icon, lv_color_hex(enabled ? UI_COLOR_ACCENT : UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_color(row->mark, lv_color_hex(selected > 0 ? UI_COLOR_ACCENT : UI_COLOR_MUTED), 0);
    ui_icon_set_kind(row->mark, selected > 0 ? UI_ICON_CHECK : UI_ICON_NEXT);
    lv_obj_set_flag(row->mark, LV_OBJ_FLAG_HIDDEN, !enabled || selected == 0);
    lv_obj_set_flag(row->underline, LV_OBJ_FLAG_HIDDEN, selected <= 0);
    ui_row_press(row, 0);
}

void ui_row_press(ui_row_t *row, int pressed)
{
    lv_obj_set_style_bg_opa(row->root, pressed ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

/* ------------------------------------------------------------------ */
/* 文字按钮（App 页：秒表/计时器/闹钟的操作键）                        */
/* ------------------------------------------------------------------ */

void ui_button_create(ui_button_t *btn, lv_obj_t *parent, const char *text,
                      int x, int y, int w, int h, const lv_font_t *font)
{
    if (btn == NULL) {
        return;
    }
    btn->root = lv_obj_create(parent);
    lv_obj_remove_style_all(btn->root);
    lv_obj_set_pos(btn->root, x, y);
    lv_obj_set_size(btn->root, w, h);
    lv_obj_set_style_bg_color(btn->root, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn->root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn->root, UI_RADIUS_M, 0);
    lv_obj_remove_flag(btn->root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    btn->label = lv_label_create(btn->root);
    lv_obj_set_style_text_font(btn->label, font, 0);
    lv_obj_set_style_text_color(btn->label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_label_set_long_mode(btn->label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(btn->label, text ? text : "");
    lv_obj_center(btn->label);
}

void ui_button_set(ui_button_t *btn, const char *text, uint32_t color)
{
    if (btn == NULL || btn->label == NULL) {
        return;
    }
    lv_label_set_text(btn->label, text ? text : "");
    lv_obj_set_style_text_color(btn->label, lv_color_hex(color), 0);
}

void ui_button_press(ui_button_t *btn, int pressed)
{
    if (btn == NULL || btn->root == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(btn->root,
                              lv_color_hex(pressed ? UI_COLOR_LINE : UI_COLOR_SURFACE), 0);
    lv_obj_set_style_text_color(btn->label,
                                lv_color_hex(pressed ? UI_COLOR_ACCENT : UI_COLOR_TEXT), 0);
}

