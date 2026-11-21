/*
 * face_image.c —— 图片表盘 App（face id 3，表盘可替换，2026-11-08；
 * 下文"契约 §x"均指本地 research/70_表盘可替换_实现方案.md，local/ 不发布）
 *
 * 版面来源：media 分区单槽（header 256 B + 1024 B palette + 480x480 I8 像素）。
 * 图片不拷进 RAM：lv_image_dsc_t.data 直接指向 XIP 地址 0xA0379000，
 * LVGL 的 bin decoder 按行解码成 ARGB8888 再混到 RGB565（和 app_notify 的
 * I1/I2 位图通知同一条路径，见 SDK libs/bin_decoder/lv_bin_decoder.c）。
 *
 * 叠层：时间（必选）+ 日期/电量（header.flags 控制），全部从 app_state 快照读。
 * 刷新节流 500 ms；息屏时主循环不跑 tick / lv_timer_handler，所以不耗电。
 *
 * 注意：本 App 绝不能调 face_lvgl_create() —— 那会重建默认表盘并 lv_obj_clean
 * 掉所有子对象；切换只做 show/hide（契约 §3.2）。
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "ui_theme.h"

#include "app_framework.h"
#include "app_state.h"
#include "face_lvgl.h"
#include "svc_face_store.h"
#include "face_image.h"

/* header.flags 的位定义（与 svc_face_store.h 同一份，别各写一份） */
#define IMG_FLAG_DATE   FACE_FLAG_SHOW_DATE
#define IMG_FLAG_BATT   FACE_FLAG_SHOW_BATT

static lv_obj_t *s_root;      /* 480x480 容器（face_lvgl_page 的子对象） */
static lv_obj_t *s_img;       /* 图片层（无图时为 NULL） */
static lv_obj_t *s_time;
static lv_obj_t *s_date;
static lv_obj_t *s_batt;

static lv_image_dsc_t s_dsc;
static face_store_info_t s_layout;   /* 建面时的 header 快照 */
static int s_has_image;              /* 1 = 当前是图片页，0 = 提示页 */
static int s_visible;
static uint32_t s_last_tick;

/* 上次显示的文本/颜色：只有真变了才 lv_label_set_text，避免无谓重绘
 * （重绘 label 区域会把图片底下那几行再解码一遍）。 */
static int s_last_hh = -1, s_last_mm = -1;
static int s_last_day = -1;
static int s_last_pct = -1;
static int s_last_lvl = -1;

static const lv_font_t *img_font_by_id(uint8_t font_id)
{
    switch (font_id) {
    case 1:  return UI_FONT_CLOCK;   /* num104 */
    case 2:  return UI_FONT_HEAD;    /* ui28 */
    case 3:  return UI_FONT_TITLE;   /* ui20 */
    default: return UI_FONT_HUGE;    /* ui48 */
    }
}

static lv_obj_t *img_root_create(void)
{
    lv_obj_t *o = lv_obj_create(face_lvgl_page());

    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, FACE_IMAGE_W, FACE_IMAGE_H);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static lv_obj_t *img_label(lv_obj_t *parent, const lv_font_t *font,
                           uint32_t color, int x, int y, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, text);
    return l;
}

/* 无图时的提示页：黑底灰字，顺便给出安装命令，省得翻文档。
 * 2026-11-08：中文只用现有字库已有的字（表/盘/未/设/置/请/用/电/脑/连/接），
 * 不用「安装/工具/切换」——这几个字不在当前字库里，会显示成方框。 */
static void img_build_hint(void)
{
    lv_obj_t *t1 = img_label(s_root, UI_FONT_HEAD, UI_COLOR_MUTED, 0, 0, "表盘未设置");
    lv_obj_t *t2 = img_label(s_root, UI_FONT_BODY, UI_COLOR_MUTED, 0, 0, "请用电脑连接");
    lv_obj_t *t3 = img_label(s_root, UI_FONT_SMALL, UI_COLOR_LINE, 0, 0,
                              "tools/face_install.py upload");

    lv_obj_align(t1, LV_ALIGN_CENTER, 0, -48);
    lv_obj_align(t2, LV_ALIGN_CENTER, 0, 4);
    lv_obj_align(t3, LV_ALIGN_CENTER, 0, 44);
}

static void img_build_image(const face_store_info_t *info)
{
    const lv_font_t *font = img_font_by_id(info->font_id);
    int line_h;

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_I8;
    s_dsc.header.w = (uint16_t)info->width;
    s_dsc.header.h = (uint16_t)info->height;
    s_dsc.header.stride = (uint16_t)info->width;      /* I8：1 B/px */
    s_dsc.data_size = FACE_IMAGE_MAX;                 /* palette + 像素 */
    s_dsc.data = face_store_image_ptr();

    s_img = lv_image_create(s_root);
    lv_image_set_src(s_img, &s_dsc);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_CLICKABLE);

    s_time = img_label(s_root, font, info->time_color & 0x00FFFFFFu,
                       info->time_x, info->time_y, "--:--");

    line_h = lv_font_get_line_height(font);
    if ((info->flags & IMG_FLAG_DATE) != 0) {
        s_date = img_label(s_root, UI_FONT_BODY, UI_COLOR_MUTED,
                           info->time_x, info->time_y + line_h + 8, "0000-00-00");
    }
    if ((info->flags & IMG_FLAG_BATT) != 0) {
        s_batt = img_label(s_root, UI_FONT_SMALL, UI_COLOR_MUTED,
                           info->time_x, info->time_y - 28, "--%");
    }
}

/* 重建 root 的全部子对象（root 本身保留，避免反复创建/删除容器） */
static void img_build(void)
{
    face_store_info_t info;
    int ok;

    if (s_root == NULL) {
        s_root = img_root_create();
    }
    lv_obj_clean(s_root);          /* 只删子对象；root 的样式/位置保留 */
    s_img = NULL;
    s_time = NULL;
    s_date = NULL;
    s_batt = NULL;

    ok = (face_store_info(&info, 0) == FACE_STORE_OK);
    if (!ok) {
        s_has_image = 0;
        memset(&s_layout, 0, sizeof(s_layout));
        img_build_hint();
    } else {
        s_has_image = 1;
        s_layout = info;
        img_build_image(&info);
    }
    s_last_hh = -1;             /* 强制下一拍刷新 */
    s_last_mm = -1;
    s_last_day = -1;
    s_last_pct = -1;
    s_last_lvl = -1;
}

/* 时间/日期/电量文本只在变化时更新 */
static void img_update(const app_state_t *st)
{
    char buf[32];

    if (s_time != NULL && (st->hh != s_last_hh || st->mm != s_last_mm)) {
        snprintf(buf, sizeof(buf), "%02d:%02d", st->hh, st->mm);
        lv_label_set_text(s_time, buf);
        s_last_hh = st->hh;
        s_last_mm = st->mm;
    }
    if (s_date != NULL && st->day != s_last_day) {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", st->year, st->mon, st->day);
        lv_label_set_text(s_date, buf);
        s_last_day = st->day;
    }
    if (s_batt != NULL && (st->battery_pct != s_last_pct ||
                           st->battery_level != s_last_lvl)) {
        uint32_t color = s_layout.time_color & 0x00FFFFFFu;

        if (st->battery_level >= 2) {
            color = UI_COLOR_ALERT;
        } else if (st->battery_level == 1) {
            color = UI_COLOR_WARN;
        }
        snprintf(buf, sizeof(buf), "%d%%", st->battery_pct);
        lv_label_set_text(s_batt, buf);
        lv_obj_set_style_text_color(s_batt, lv_color_hex(color), 0);
        s_last_pct = st->battery_pct;
        s_last_lvl = st->battery_level;
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

int face_image_available(void)
{
    return (face_store_init() == FACE_STORE_OK) ? 1 : 0;
}

void face_image_show(void)
{
    face_store_info_t info;
    int have;

    if (face_lvgl_page() == NULL) {
        return;
    }
    have = (face_store_info(&info, 0) == FACE_STORE_OK);

    if (s_root == NULL || have != s_has_image ||
        (have && memcmp(&info, &s_layout, sizeof(info)) != 0)) {
        img_build();
    }

    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    s_visible = 1;
    s_last_tick = 0;
    printf("[face_image] show %s (%dx%d)\r\n", s_has_image ? "image" : "hint",
           (int)FACE_IMAGE_W, (int)FACE_IMAGE_H);
    face_image_tick();
}

void face_image_hide(void)
{
    if (s_root != NULL) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    s_visible = 0;
}

void face_image_tick(void)
{
    app_state_t st;
    uint32_t now;

    if (!s_visible || s_root == NULL) {
        return;
    }
    now = lv_tick_get();
    if (s_last_tick != 0 && (uint32_t)(now - s_last_tick) < 500u) {
        return;
    }
    s_last_tick = now;
    if (!s_has_image) {
        return;
    }
    app_state_get(&st);
    img_update(&st);
}

void face_image_on_face_recreated(void)
{
    /* face_lvgl_create() 的 lv_obj_clean() 已经把整棵子树删掉，这里只清指针，
     * 不要再访问旧对象（契约 §3.3）。 */
    s_root = NULL;
    s_img = NULL;
    s_time = NULL;
    s_date = NULL;
    s_batt = NULL;
    s_has_image = 0;
    s_visible = 0;
    memset(&s_layout, 0, sizeof(s_layout));
}

static const app_desc_t s_face_image = {
    .id = "face_image",
    .name = "图片表盘",
    .has_gui = 1,
    .allow_bg = 0,
    .page_id = -1,
    .init = NULL,
    .load = face_image_show,
    .close = face_image_hide,
    .kill = NULL,
    .power_off = NULL,
    .event = NULL,
};

void face_image_register(void)
{
    (void)app_register(&s_face_image);
}
