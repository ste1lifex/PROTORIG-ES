/*
 * app_notify.c —— 通知队列 + 圆屏居中卡片（文本 368x136 / 位图短卡+展开）
 *
 * - 队列 4 条、连续重复去重；队列满丢最旧；
 * - UI 任务每 tick 从队首取一条，显示 3 s，再显示下一条；
 * - 本文件只在 UI 任务调用 LVGL；app_notify_show 本身不碰 LVGL，BLE 侧安全。
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "ui_theme.h"

#include "app_notify.h"

#define NOTIFY_QUEUE_LEN 4

typedef struct {
    char title[APP_NOTIFY_TITLE_MAX];
    char body[APP_NOTIFY_BODY_MAX];
} notify_entry_t;

static notify_entry_t s_q[NOTIFY_QUEUE_LEN];
static int s_q_head;
static int s_q_count;

static char s_title[APP_NOTIFY_TITLE_MAX];
static char s_body[APP_NOTIFY_BODY_MAX];
static char s_last_title[APP_NOTIFY_TITLE_MAX];
static char s_last_body[APP_NOTIFY_BODY_MAX];
static volatile int s_visible;

static void copy_trunc(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void app_notify_init(void)
{
    memset(s_q, 0, sizeof(s_q));
    s_q_head = 0;
    s_q_count = 0;
    s_title[0] = '\0';
    s_body[0] = '\0';
    s_last_title[0] = '\0';
    s_last_body[0] = '\0';
    s_visible = 0;
}

int app_notify_show(const char *title, const char *body)
{
    char t[APP_NOTIFY_TITLE_MAX];
    char b[APP_NOTIFY_BODY_MAX];
    notify_entry_t *slot;

    copy_trunc(t, sizeof(t), title);
    copy_trunc(b, sizeof(b), body);

    /* 去重：与上一条入队相同，或与当前显示相同，直接丢弃 */
    if ((s_q_count > 0 && strcmp(t, s_last_title) == 0 &&
         strcmp(b, s_last_body) == 0) ||
        (s_visible && strcmp(t, s_title) == 0 && strcmp(b, s_body) == 0)) {
        printf("[notify] duplicate dropped\r\n");
        return 0;
    }

    if (s_q_count >= NOTIFY_QUEUE_LEN) {
        s_q_head = (s_q_head + 1) % NOTIFY_QUEUE_LEN;   /* 丢最旧 */
        s_q_count--;
    }
    slot = &s_q[(s_q_head + s_q_count) % NOTIFY_QUEUE_LEN];
    copy_trunc(slot->title, sizeof(slot->title), t);
    copy_trunc(slot->body, sizeof(slot->body), b);
    s_q_count++;

    copy_trunc(s_last_title, sizeof(s_last_title), t);
    copy_trunc(s_last_body, sizeof(s_last_body), b);
    printf("[notify] queued title=\"%s\" body=\"%s\" (depth=%d)\r\n",
           slot->title, slot->body, s_q_count);
    return 0;
}

int app_notify_pop(char *title, unsigned title_cap, char *body, unsigned body_cap)
{
    notify_entry_t *slot;

    if (s_q_count <= 0) {
        return 0;
    }
    slot = &s_q[s_q_head];
    copy_trunc(s_title, sizeof(s_title), slot->title);
    copy_trunc(s_body, sizeof(s_body), slot->body);
    copy_trunc(title, title_cap, s_title);
    copy_trunc(body, body_cap, s_body);
    s_q_head = (s_q_head + 1) % NOTIFY_QUEUE_LEN;
    s_q_count--;
    s_visible = 1;
    return 1;
}

void app_notify_hide(void)
{
    s_visible = 0;
    printf("[notify] hide\r\n");
}

int app_notify_visible(void) { return s_visible; }
int app_notify_pending(void) { return s_q_count > 0; }
void app_notify_clear_pending(void) { s_q_head = 0; s_q_count = 0; }

const char *app_notify_title(void) { return s_title; }
const char *app_notify_body(void) { return s_body; }

/* ------------------------------------------------------------------ */
/* 通知小卡片（LVGL；只在 UI 任务执行）                                 */
/*                                                                    */
/* 2026-11-08：改成圆屏适配 —— 卡片**居中**（屏幕中心 240,240），       */
/* 368x136、圆角 24。四角到圆心：sqrt(184^2+68^2)=196.2 < 207，         */
/* 整张卡都在安全圆内（改前是 440x96 贴顶 y=24，两角直接出界）。        */
/* 正文用 LV_LABEL_LONG_WRAP 折行（宽 336、高 76，14px 约 4 行）。      */
/* ------------------------------------------------------------------ */

#define NOTIFY_CARD_W      368
#define NOTIFY_CARD_H      136
#define NOTIFY_CARD_RADIUS 24
#define NOTIFY_CARD_PAD    16

static lv_obj_t *s_card;
static lv_obj_t *s_card_title;
static lv_obj_t *s_card_body;
static uint32_t s_hide_at;
static int s_ui_ready;

/* R2.1-BMP：位图通知卡（短卡 → 点击展开） */
static lv_obj_t *s_bmp_short;
static lv_obj_t *s_bmp_full;
static lv_obj_t *s_bmp_img_short;
static lv_obj_t *s_bmp_img_full;
static lv_obj_t *s_bmp_close;
static lv_image_dsc_t s_bmp_dsc;
static const uint8_t *s_bmp_data;
static uint16_t s_bmp_w, s_bmp_h;
static uint32_t s_bmp_total;
static uint8_t s_bmp_bpp, s_bmp_cat;
static int s_bmp_pending, s_bmp_visible, s_bmp_expanded;
static uint32_t s_bmp_hide_at;

/* 点按关闭（可选交互）：卡片显示期间会吞掉它覆盖区域的这一下触摸，
 * 隐藏时卡片不参与命中测试，手势层/页面触摸不受影响。 */
static void notify_card_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!s_visible || s_card == NULL) {
        return;
    }
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    app_notify_hide();
}

static void notify_ui_create(void)
{
    if (s_ui_ready) {
        return;
    }
    s_card = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, NOTIFY_CARD_W, NOTIFY_CARD_H);
    lv_obj_center(s_card);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, NOTIFY_CARD_RADIUS, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_pad_all(s_card, NOTIFY_CARD_PAD, 0);
    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_card, notify_card_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    s_card_title = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_card_title, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_card_title, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_width(s_card_title, NOTIFY_CARD_W - 2 * NOTIFY_CARD_PAD);
    lv_obj_set_height(s_card_title, lv_font_get_line_height(UI_FONT_BODY));
    lv_obj_align(s_card_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_long_mode(s_card_title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_card_title, "");

    s_card_body = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_card_body, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_card_body, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_size(s_card_body, NOTIFY_CARD_W - 2 * NOTIFY_CARD_PAD,
                    NOTIFY_CARD_H - 2 * NOTIFY_CARD_PAD - 28);   /* 76 */
    lv_label_set_long_mode(s_card_body, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_card_body, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_label_set_text(s_card_body, "");

    s_ui_ready = 1;
}

void app_notify_ui_init(void)
{
    notify_ui_create();
}

static void bmp_ui_tick(uint32_t now_ms);

void app_notify_ui_tick(uint32_t now_ms)
{
    bmp_ui_tick(now_ms);
    if (s_bmp_visible) {
        return;                 /* 位图卡优先，文本队列等它收起 */
    }
    notify_ui_create();
    if (!s_ui_ready) {
        return;
    }

    if (!app_notify_visible() && app_notify_pending()) {
        char title[APP_NOTIFY_TITLE_MAX];
        char body[APP_NOTIFY_BODY_MAX];

        if (app_notify_pop(title, sizeof(title), body, sizeof(body))) {
            lv_label_set_text(s_card_title, title);
            lv_label_set_text(s_card_body, body);
            lv_obj_remove_flag(s_card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_card);
            s_hide_at = now_ms + 3000u;
        }
    }

    if (app_notify_visible() && (int32_t)(now_ms - s_hide_at) >= 0) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        app_notify_hide();
    }
}


/* ------------------------------------------------------------------ */
/* R2.1-BMP：位图卡（LVGL；只在 UI 任务）                              */
/* ------------------------------------------------------------------ */

static void bmp_short_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_bmp_visible) return;
    s_bmp_expanded = 1;
    lv_obj_add_flag(s_bmp_short, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_bmp_full, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_bmp_full);
}

static void bmp_dismiss_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    app_notify_bmp_dismiss();
}

static void bmp_ui_create(void)
{
    if (s_bmp_short != NULL) return;

    s_bmp_short = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_bmp_short);
    lv_obj_set_pos(s_bmp_short, 84, 104);
    lv_obj_set_size(s_bmp_short, 312, 240);
    lv_obj_set_style_bg_color(s_bmp_short, lv_color_hex(0x121512), 0);
    lv_obj_set_style_bg_opa(s_bmp_short, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bmp_short, 16, 0);
    lv_obj_set_style_border_width(s_bmp_short, 1, 0);
    lv_obj_set_style_border_color(s_bmp_short, lv_color_hex(0xF2EB5B), 0);
    lv_obj_add_flag(s_bmp_short, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_bmp_short, bmp_short_cb, LV_EVENT_CLICKED, NULL);

    s_bmp_img_short = lv_image_create(s_bmp_short);
    lv_obj_set_pos(s_bmp_img_short, 16, 12);
    lv_obj_set_size(s_bmp_img_short, 280, 123);
    lv_image_set_scale(s_bmp_img_short, 224);
    lv_obj_remove_flag(s_bmp_img_short, LV_OBJ_FLAG_CLICKABLE);

    {
        lv_obj_t *hint = lv_label_create(s_bmp_short);
        lv_obj_set_style_text_font(hint, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(0xA0A69A), 0);
        lv_label_set_text(hint, "点按展开");
        lv_obj_set_pos(hint, 16, 204);
    }

    s_bmp_full = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_bmp_full);
    lv_obj_set_pos(s_bmp_full, 84, 120);
    lv_obj_set_size(s_bmp_full, 312, 248);
    lv_obj_set_style_bg_color(s_bmp_full, lv_color_hex(0x121512), 0);
    lv_obj_set_style_bg_opa(s_bmp_full, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bmp_full, 16, 0);
    lv_obj_set_style_border_width(s_bmp_full, 1, 0);
    lv_obj_set_style_border_color(s_bmp_full, lv_color_hex(0xF2EB5B), 0);
    lv_obj_add_flag(s_bmp_full, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_bmp_full, bmp_dismiss_cb, LV_EVENT_CLICKED, NULL);

    s_bmp_img_full = lv_image_create(s_bmp_full);
    lv_obj_set_pos(s_bmp_img_full, 16, 12);
    lv_obj_set_size(s_bmp_img_full, 280, 123);
    lv_image_set_scale(s_bmp_img_full, 224);
    lv_obj_remove_flag(s_bmp_img_full, LV_OBJ_FLAG_CLICKABLE);

    s_bmp_close = lv_obj_create(s_bmp_full);
    lv_obj_remove_style_all(s_bmp_close);
    lv_obj_set_pos(s_bmp_close, 68, 188);
    lv_obj_set_size(s_bmp_close, 176, 48);
    lv_obj_set_style_bg_color(s_bmp_close, lv_color_hex(0x242A20), 0);
    lv_obj_set_style_bg_opa(s_bmp_close, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bmp_close, 12, 0);
    lv_obj_set_style_border_width(s_bmp_close, 1, 0);
    lv_obj_set_style_border_color(s_bmp_close, lv_color_hex(0xF2EB5B), 0);
    lv_obj_add_event_cb(s_bmp_close, bmp_dismiss_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lbl = lv_label_create(s_bmp_close);
        lv_obj_set_style_text_font(lbl, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xF1F2EA), 0);
        lv_label_set_text(lbl, "关闭");
        lv_obj_center(lbl);
    }
}

int app_notify_show_bitmap(uint8_t cat, uint16_t w, uint16_t h, uint8_t bpp,
                           const uint8_t *data, uint32_t total)
{
    uint32_t pal;
    uint32_t need;

    if (data == NULL || w == 0 || w > 320 || h == 0 || h > 160 ||
        (bpp != 1 && bpp != 2)) {
        return -1;
    }
    pal = (bpp == 2) ? 16u : 8u;
    need = pal + total;
    if (need > sizeof(s_bmp_dsc) + 0) { /* 仅做明显上界检查 */ }
    if (total != ((uint32_t)w * h * bpp + 7u) / 8u || need > 12816u) {
        return -1;
    }
    s_bmp_cat = cat;
    s_bmp_w = w;
    s_bmp_h = h;
    s_bmp_bpp = bpp;
    s_bmp_total = total;
    s_bmp_data = data;
    s_bmp_pending = 1;
    return 0;
}

int app_notify_bitmap_visible(void)
{
    return s_bmp_visible || s_bmp_pending;
}

int app_notify_bmp_expanded(void)
{
    return s_bmp_expanded;
}

void app_notify_bmp_dismiss(void)
{
    if (s_bmp_short == NULL) {
        s_bmp_visible = 0;
        s_visible = 0;
        return;
    }
    lv_obj_add_flag(s_bmp_short, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bmp_full, LV_OBJ_FLAG_HIDDEN);
    s_bmp_visible = 0;
    s_bmp_expanded = 0;
    s_visible = 0;
}

static void bmp_ui_tick(uint32_t now_ms)
{
    bmp_ui_create();
    if (s_bmp_pending && !s_bmp_visible) {
        uint32_t pal = (s_bmp_bpp == 2) ? 16u : 8u;
        uint32_t stride = ((uint32_t)s_bmp_w * s_bmp_bpp + 7u) / 8u;

        /* 文本卡若正在显示，位图优先；文本队列保留到下一轮 */
        if (s_visible) {
            if (s_card != NULL) lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
            app_notify_hide();
        }

        memset(&s_bmp_dsc, 0, sizeof(s_bmp_dsc));
        s_bmp_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_bmp_dsc.header.cf = (s_bmp_bpp == 2) ? LV_COLOR_FORMAT_I2 : LV_COLOR_FORMAT_I1;
        s_bmp_dsc.header.w = s_bmp_w;
        s_bmp_dsc.header.h = s_bmp_h;
        s_bmp_dsc.header.stride = (uint16_t)stride;
        s_bmp_dsc.data_size = pal + s_bmp_total;
        s_bmp_dsc.data = s_bmp_data;

        lv_image_set_src(s_bmp_img_short, &s_bmp_dsc);
        lv_image_set_src(s_bmp_img_full, &s_bmp_dsc);
        lv_obj_add_flag(s_bmp_full, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_bmp_short, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_bmp_short);
        s_bmp_visible = 1;
        s_bmp_expanded = 0;
        s_visible = 1;
        s_bmp_pending = 0;
        s_bmp_hide_at = now_ms + 3000u;
        printf("[bmp] show %ux%u %ubpp (cat=%u)\r\n",
               s_bmp_w, s_bmp_h, s_bmp_bpp, s_bmp_cat);
    }
    if (s_bmp_visible && !s_bmp_expanded &&
        (int32_t)(now_ms - s_bmp_hide_at) >= 0) {
        app_notify_bmp_dismiss();
    }
}
