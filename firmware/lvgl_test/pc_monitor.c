#include "pc_monitor.h"
#include "misc/lv_text_private.h"
#include "svc_time.h"
#include "sd3010.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_ui14);
LV_FONT_DECLARE(lv_font_ui16);
LV_FONT_DECLARE(lv_font_ui20);
LV_FONT_DECLARE(lv_font_ui25);
LV_FONT_DECLARE(lv_font_ui28);
LV_FONT_DECLARE(lv_font_ui48);
LV_FONT_DECLARE(lv_font_monitor20);

static lv_obj_t *page, *content, *offline, *offline2, *arcs[3], *values[3], *units[3], *net[2], *vram_label, *gpu_caption;
/* v4 面板同款：顶部 48px 时钟 + 日期/星期，卡片态时数值切 compact 行 */
static lv_obj_t *clock_digits[5], *clock_date, *clock_week, *compact[3], *arc_captions[3];
static int shown_clock_min = -1, shown_clock_day = -1, shown_clock_hour = -1;
static const unsigned colors[]={0xFFFA00,0x8FB8FF,0xF0F0F0};
static const int radii[]={228,218,208};
static bool was_connected;

/* ------------------------------------------------------------------------- */
/* v1.1 lower half (y 250..390): single-focus card.                          */
/*   state (ui20, baseline 272) + big title bitmap (h 20, top 286) +         */
/*   tool/steps (ui16, baseline 334) + ctx text/bar (baseline 360 / 352).    */
/* Bitmap pixels come from monitor_protocol.c's slot-0 A8 buffer; only the    */
/* committed width/hash metadata travels in pm_link_t.                        */
/* ------------------------------------------------------------------------- */
static lv_obj_t *lower, *focus_state, *focus_img, *focus_ph, *focus_info;
static lv_obj_t *ctx_label, *ctx_bar;
static lv_image_dsc_t focus_dsc;

static char shown_state[16], shown_info[32], shown_ctx[16];
static int shown_metric[3] = {-2,-2,-2};
static int shown_vram = -2;
static int shown_gpu_brand = -1;
static const unsigned gpu_colors[]={0x8FB8FF,0x76B900,0xED1C24,0x0091FF};
static int64_t shown_rx = -2, shown_tx = -2;
static int shown_ctx_value = -2;
static bool shown_ctx_hot;
static uint16_t shown_bmp_w;
static uint32_t shown_bmp_hash;
static bool focus_img_set;
static bool lower_shown;

static const char *state_name(int state)
{
    switch(state) {
    case PM_STATE_RUNNING: return "RUNNING";
    case PM_STATE_NEEDS:   return "NEEDS YOU";
    case PM_STATE_DONE:    return "DONE";
    case PM_STATE_FAILED:  return "FAILED";
    default:               return "IDLE";
    }
}
static unsigned state_color(int state)
{
    switch(state) {
    case PM_STATE_RUNNING: return 0xF0F0F0;
    case PM_STATE_NEEDS:   return 0xFFFA00;
    case PM_STATE_DONE:    return 0x8FB8FF;
    case PM_STATE_FAILED:  return 0xFF6B6B;
    default:               return 0xAAAAAA;
    }
}
static int text_width(const lv_font_t *font, const char *text)
{
    lv_point_t size;
    lv_text_get_size(&size,text,font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    return size.x;
}
static void label(lv_obj_t *obj,const char *text,const lv_font_t *font,
                  int x,int baseline,int align)
{
    lv_label_set_text(obj,text);
    lv_obj_set_style_text_font(obj,font,0);
    int width=text_width(font,text);
    lv_obj_set_pos(obj,x-(align==1?width/2:align==2?width:0),
                   baseline-(font->line_height-font->base_line));
}
static lv_obj_t *new_label(lv_obj_t *parent,unsigned color)
{
    lv_obj_t *o=lv_label_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);
    return o;
}
static bool put(lv_obj_t *obj,char *cache,size_t cap,const char *text,
                const lv_font_t *font,int x,int baseline,int align)
{
    if(!strcmp(cache,text)) return false;
    snprintf(cache,cap,"%s",text);
    label(obj,text,font,x,baseline,align);
    return true;
}
static void tracks(lv_event_t *e)
{
    lv_area_t a;lv_obj_get_coords(lv_event_get_target_obj(e),&a);
    lv_draw_arc_dsc_t d;lv_draw_arc_dsc_init(&d);
    d.center.x=a.x1+240;d.center.y=a.y1+240;
    d.color=lv_color_hex(0x333333);d.width=3;
    for(int k=0;k<3;k++) for(int t=0;t<60;t++) {
        d.radius=radii[k];d.start_angle=120+t*5;d.end_angle=d.start_angle+1;
        lv_draw_arc(lv_event_get_layer(e),&d);
    }
}
static void lower_create(lv_obj_t *parent)
{
    lower=lv_obj_create(parent);lv_obj_remove_style_all(lower);
    lv_obj_set_size(lower,480,480);
    lv_obj_remove_flag(lower,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);

    focus_state=new_label(lower,0xAAAAAA);
    label(focus_state,"IDLE",&lv_font_ui20,240,272,1);

    focus_img=lv_image_create(lower);
    lv_obj_remove_style_all(focus_img);
    lv_obj_remove_flag(focus_img,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_image_recolor(focus_img,lv_color_hex(0xF0F0F0),0);
    lv_obj_set_style_image_recolor_opa(focus_img,LV_OPA_COVER,0);
    lv_obj_set_pos(focus_img,90,286);
    lv_obj_add_flag(focus_img,LV_OBJ_FLAG_HIDDEN);

    focus_ph=new_label(lower,0xAAAAAA);
    label(focus_ph,"S1",&lv_font_ui16,240,304,1);

    focus_info=new_label(lower,0xAAAAAA);
    ctx_label=new_label(lower,0xAAAAAA);

    ctx_bar=lv_bar_create(lower);
    lv_obj_remove_style_all(ctx_bar);
    /* Reserve the full width of "CTX 100%" plus a visible gap. */
    lv_obj_set_size(ctx_bar,211,6);
    lv_obj_set_pos(ctx_bar,180,352);
    lv_obj_remove_flag(ctx_bar,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ctx_bar,0,LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx_bar,lv_color_hex(0x333333),LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx_bar,LV_OPA_COVER,LV_PART_MAIN);
    lv_obj_set_style_radius(ctx_bar,0,LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ctx_bar,lv_color_hex(0x8FB8FF),LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ctx_bar,LV_OPA_COVER,LV_PART_INDICATOR);
    lv_bar_set_range(ctx_bar,0,100);
    lv_bar_set_value(ctx_bar,0,LV_ANIM_OFF);

    lv_obj_add_flag(lower,LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------- 卡片层 */
/*
 * BLE 数字通道的卡片层（research/66）：无像素链路时 PC 用
 * 0x53 SESSION / 0x54 APPROVAL / 0x55 QUESTION 驱动，本机 LVGL 画。
 *
 * 版式 **逐项照抄 v4 主机面板**（插件 render-monitor-canvas.mjs）：
 *   会话卡   state 20px @(240,276)  →  标题 25px 位图 @(240,310)
 *            → 工具·N步 18px @(94,338) → CTX 17px @(94,364) + 条(180,356,206,6)
 *            → 底部 17px @(240,416)
 *   审批卡   DSH 审批待确认 25px @(240,192) → <工具> 请求执行 21px @(240,229)
 *            → 命令 22px @(240,263) → 按钮 (106,296,128,96)/(246,296,128,96)
 *            → 备注 17px @(240,419)
 *   问答卡   标题行 17px @(240,177) → 题目 25px @(240,207)
 *            → 选项 (92,218,296,96) → 下一项(128,314,104,96) / 提交(248,314,104,96)
 *   空态     探索未至之境 @(240,308) → DSH 等待任务 @(240,363) → 底部
 *   按钮     文字 24px + 提示 16px + 下方一条 48px 短线（v4 的"细线+文字"，无边框）
 *
 * 固件字库只有 14/16/20/28/48 档（v4 是 14..25 连档），所以：
 *   25px 级 → ui28（+12%）；23/21/20 → ui20；18/17/16 → ui16；14 → ui14。
 * 任意文本（标题/命令/题目/选项）仍走 1-bit 位图，避免缺字。
 *
 * 子对象创建顺序（LVGL 后创建的画在上层）：底框 → 位图 → 文字 → 条 → 按钮 → 短线/勾。
 */
/* 最大一张：300×30 @2bpp = 2250 B（1bpp 只用一半） */
#define CARD_BMP_BYTES ((PC_CARD_BMP_W / 8) * PC_CARD_BMP_H * 2)

typedef struct {
    uint16_t w, h;
    uint16_t total;
    uint16_t index;
    uint16_t expect;
    uint16_t filled;
    uint8_t open;
    uint8_t valid;
    uint8_t bpp;         /* 1 = 黑白，2 = 4 级灰 */
    uint32_t ver;
    uint8_t bits[CARD_BMP_BYTES];
} card_bmp_t;

/* v4 语义色（render-monitor-canvas.mjs 同值） */
#define CARD_PAPER  0xF0F0F0
#define CARD_MUTED  0xA8A8A8
#define CARD_GOLD   0xFFFA00
#define CARD_BLUE   0x8FB8FF
#define CARD_RED    0xFF6B6B

/* v4 热区（design/dsh_panel_v4/tokens.json） */
#define CARD_DENY_X   106
#define CARD_DENY_Y   296
#define CARD_ALLOW_X  246
#define CARD_ALLOW_Y  296
#define CARD_BTN_W    128
#define CARD_BTN_H    96
#define CARD_OPT_X    92
#define CARD_OPT_Y    218
#define CARD_OPT_W    296
#define CARD_OPT_H    96
#define CARD_NEXT_X   128
#define CARD_NEXT_Y   314
#define CARD_SUBMIT_X 248
#define CARD_SUBMIT_Y 314
#define CARD_QBTN_W   104
#define CARD_QBTN_H   96

static pm_card_t s_card;
static lv_obj_t *card_head, *card_sub, *card_info, *card_ctx, *card_footer, *card_note;
static lv_obj_t *card_line, *card_line2;
static lv_obj_t *card_ctxbar;
static lv_obj_t *card_btnlab[2], *card_btnhint[2], *card_btnline[2], *card_btnicon[2];
static lv_obj_t *card_optlab, *card_opthint, *card_optline, *card_check;
static lv_obj_t *card_img[PC_CARD_BMP_SLOTS];
static lv_image_dsc_t card_dsc[PC_CARD_BMP_SLOTS];
static card_bmp_t s_bmp[PC_CARD_BMP_SLOTS];
/* A8 展开缓冲：每张 300×30=9000 B，两张同时存在（题目 + 选项） */
static uint8_t s_a8[PC_CARD_BMP_SLOTS][PC_CARD_BMP_W * PC_CARD_BMP_H];
static uint32_t s_a8_ver[PC_CARD_BMP_SLOTS];
static uint8_t s_a8_valid[PC_CARD_BMP_SLOTS];
static uint32_t s_card_rev, s_card_applied;
/* 按住的按钮（0/1 = 两个按钮，2 = 选项），只做"短线变白"反馈 */
static int s_card_press = -1;
/* 按钮/选项的当前状态，按下时用它重画样式 */
static char s_btn_text[2][24], s_btn_hint[2][28], s_btn_icon[2][8];
static uint8_t s_btn_enabled[2], s_btn_emphasis[2];
static uint8_t s_opt_selected, s_opt_enabled;
static uint8_t s_card_live;
static uint32_t s_card_ms;

/* 缺字的字符替换成 '?'，避免屏上出方块/空白（卡片文本兜底用）。 */
static void sanitize_text(const char *in, char *out, size_t cap, const lv_font_t *font)
{
    size_t o = 0;
    uint32_t i = 0;
    if (!in || !out || cap < 2) return;
    while (in[i] && o < cap - 5) {
        uint32_t start = i, cp = lv_text_encoded_next(in, &i);
        lv_font_glyph_dsc_t glyph;
        if (lv_font_get_glyph_dsc(font, &glyph, cp, 0)) {
            while (start < i) out[o++] = in[start++];
        } else {
            out[o++] = '?';
        }
    }
    out[o] = 0;
}

/* 定宽标签（超长裁切，不换行） */
static void label_box(lv_obj_t *obj, const char *text, const lv_font_t *font,
                      int x, int baseline, int align, int maxw)
{
    int width;
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(obj, maxw);
    width = text_width(font, text);
    if (width > maxw) width = maxw;
    lv_obj_set_pos(obj, x - (align == 1 ? width / 2 : align == 2 ? width : 0),
                   baseline - (font->line_height - font->base_line));
}

static const char *state_text(int state)
{
    switch (state) {
    case PM_STATE_RUNNING: return "DSH 运行中";
    case PM_STATE_NEEDS:   return "DSH 等待操作";
    case PM_STATE_DONE:    return "DSH 本轮完成";
    case PM_STATE_FAILED:  return "DSH 运行失败";
    default:               return "DSH 等待任务";
    }
}

/* 一条 48px 宽的按钮短线（v4 的底边线） */
static lv_obj_t *card_line_make(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 48, 2);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x757575), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static void card_create(lv_obj_t *parent)
{
    /* 1) 底框：问答选项的选中高亮块（不透明，必须最先建） */
    /* 这里只做一条选中态下划线 + 淡底，保持 v4 的"无边框"观感 */
    /* 2) 位图 */
    for (int k = 0; k < PC_CARD_BMP_SLOTS; k++) {
        card_img[k] = lv_image_create(parent);
        lv_obj_remove_style_all(card_img[k]);
        lv_obj_remove_flag(card_img[k], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_image_recolor(card_img[k], lv_color_hex(CARD_PAPER), 0);
        lv_obj_set_style_image_recolor_opa(card_img[k], LV_OPA_COVER, 0);
        lv_obj_add_flag(card_img[k], LV_OBJ_FLAG_HIDDEN);
    }

    /* 3) 文字 */
    card_head = new_label(parent, CARD_MUTED);
    card_sub = new_label(parent, CARD_PAPER);
    card_info = new_label(parent, CARD_MUTED);
    card_ctx = new_label(parent, CARD_MUTED);
    card_footer = new_label(parent, CARD_MUTED);
    card_note = new_label(parent, CARD_MUTED);
    card_line = new_label(parent, CARD_MUTED);
    card_line2 = new_label(parent, CARD_MUTED);
    card_optlab = new_label(parent, CARD_PAPER);
    card_opthint = new_label(parent, CARD_MUTED);
    card_check = new_label(parent, CARD_GOLD);
    for (int k = 0; k < 2; k++) {
        card_btnlab[k] = new_label(parent, CARD_PAPER);
        card_btnhint[k] = new_label(parent, CARD_MUTED);
        card_btnicon[k] = new_label(parent, CARD_PAPER);   /* v4 按钮上方的图标 */
    }

    /* 4) CTX 进度条（v4 同位置 x=180..386, y=356, 高 6） */
    card_ctxbar = lv_bar_create(parent);
    lv_obj_remove_style_all(card_ctxbar);
    lv_obj_set_size(card_ctxbar, 206, 6);
    lv_obj_set_pos(card_ctxbar, 180, 356);
    lv_obj_remove_flag(card_ctxbar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card_ctxbar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card_ctxbar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card_ctxbar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card_ctxbar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(card_ctxbar, lv_color_hex(CARD_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(card_ctxbar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(card_ctxbar, 0, 100);
    lv_bar_set_value(card_ctxbar, 0, LV_ANIM_OFF);

    /* 5) 按钮短线（v4 的底边线，最上层） */
    card_btnline[0] = card_line_make(parent);
    card_btnline[1] = card_line_make(parent);
    card_optline = card_line_make(parent);

    lv_obj_add_flag(card_head, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_sub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_ctx, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_note, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_line2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_ctxbar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_optlab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_opthint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_check, LV_OBJ_FLAG_HIDDEN);
    for (int k = 0; k < 2; k++) {
        lv_obj_add_flag(card_btnlab[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_btnhint[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_btnicon[k], LV_OBJ_FLAG_HIDDEN);
    }
}

/* 位图 -> A8 展开（只在换图后做一次） */
static void card_expand_a8(int slot)
{
    card_bmp_t *b;
    uint16_t stride;
    uint8_t *out;

    if (slot < 0 || slot >= PC_CARD_BMP_SLOTS) return;
    b = &s_bmp[slot];
    if (!b->valid) return;
    out = s_a8[slot];
    if (b->bpp == 2) {
        stride = (uint16_t)(b->w >> 2);       /* 每字节 4 个像素，每像素 2 bit */
        for (uint16_t y = 0; y < b->h; y++) {
            const uint8_t *row = &b->bits[(size_t)y * stride];
            for (uint16_t x = 0; x < b->w; x++) {
                uint8_t v = (uint8_t)((row[x >> 2] >> (6 - ((x & 3) << 1))) & 0x03u);
                out[(size_t)y * b->w + x] = (uint8_t)(v * 85u);
            }
        }
    } else {
        stride = (uint16_t)(b->w >> 3);
        for (uint16_t y = 0; y < b->h; y++) {
            const uint8_t *row = &b->bits[(size_t)y * stride];
            for (uint16_t x = 0; x < b->w; x++) {
                out[(size_t)y * b->w + x] = (row[x >> 3] & (0x80u >> (x & 7))) ? 0xFF : 0x00;
            }
        }
    }
    card_dsc[slot].header.magic = LV_IMAGE_HEADER_MAGIC;
    card_dsc[slot].header.cf = LV_COLOR_FORMAT_A8;
    card_dsc[slot].header.flags = 0;
    card_dsc[slot].header.w = b->w;
    card_dsc[slot].header.h = b->h;
    card_dsc[slot].header.stride = b->w;
    card_dsc[slot].data_size = (uint32_t)b->w * (uint32_t)b->h;
    card_dsc[slot].data = out;
    lv_image_set_src(card_img[slot], &card_dsc[slot]);
    lv_obj_set_size(card_img[slot], b->w, b->h);
    lv_obj_invalidate(card_img[slot]);
    s_a8_ver[slot] = b->ver;
    s_a8_valid[slot] = 1;
}

/* 位图摆到 (居中, top) 并染色；没有图返回 false（调用方画文本兜底） */
static bool card_show_bmp(int slot, int top, unsigned color)
{
    if (slot < 0 || slot >= PC_CARD_BMP_SLOTS) return false;
    if (!s_bmp[slot].valid) return false;
    if (!s_a8_valid[slot] || s_a8_ver[slot] != s_bmp[slot].ver) card_expand_a8(slot);
    lv_obj_set_style_image_recolor(card_img[slot], lv_color_hex(color), 0);
    lv_obj_set_pos(card_img[slot], (480 - s_bmp[slot].w) / 2, top);
    lv_obj_remove_flag(card_img[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(card_img[slot]);
    return true;
}

static void card_hide_all(void)
{
    lv_obj_add_flag(card_head, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_sub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_ctx, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_note, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_line2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_ctxbar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_optlab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_opthint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_check, LV_OBJ_FLAG_HIDDEN);
    for (int k = 0; k < PC_CARD_BMP_SLOTS; k++) lv_obj_add_flag(card_img[k], LV_OBJ_FLAG_HIDDEN);
    for (int k = 0; k < 2; k++) {
        lv_obj_add_flag(card_btnlab[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_btnhint[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_btnline[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_btnicon[k], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(card_optline, LV_OBJ_FLAG_HIDDEN);
}

/* v4 按钮：文字 + 提示 + 底边短线（无边框），强调=金色，禁用=暗灰 */
static void card_btn_apply(int idx, int x, int y, int w, int h, bool pressed)
{
    bool enabled = s_btn_enabled[idx] != 0;
    bool emphasis = s_btn_emphasis[idx] != 0;
    unsigned ink = !enabled ? 0x707070 : emphasis ? CARD_GOLD : CARD_PAPER;
    unsigned line = !enabled ? 0x333333 : pressed ? CARD_PAPER : emphasis ? CARD_GOLD : 0x757575;
    int mid = x + w / 2;

    if (s_btn_icon[idx][0]) {
        label(card_btnicon[idx], s_btn_icon[idx], &lv_font_ui20, mid, y + 27, 1);
        lv_obj_set_style_text_color(card_btnicon[idx], lv_color_hex(ink), 0);
        lv_obj_remove_flag(card_btnicon[idx], LV_OBJ_FLAG_HIDDEN);
    }
    label(card_btnlab[idx], s_btn_text[idx], &lv_font_ui25,
          mid, y + (s_btn_hint[idx][0] ? 50 : 58), 1);
    lv_obj_set_style_text_color(card_btnlab[idx], lv_color_hex(ink), 0);
    lv_obj_remove_flag(card_btnlab[idx], LV_OBJ_FLAG_HIDDEN);
    if (s_btn_hint[idx][0]) {
        label(card_btnhint[idx], s_btn_hint[idx], &lv_font_ui16, mid, y + 74, 1);
        lv_obj_set_style_text_color(card_btnhint[idx], lv_color_hex(enabled ? CARD_MUTED : 0x707070), 0);
        lv_obj_remove_flag(card_btnhint[idx], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_pos(card_btnline[idx], mid - 24, y + h - 9);
    lv_obj_set_style_bg_color(card_btnline[idx], lv_color_hex(line), 0);
    lv_obj_remove_flag(card_btnline[idx], LV_OBJ_FLAG_HIDDEN);
}

static void card_opt_apply(void)
{
    bool pressed = (s_card_press == 2);
    bool selected = s_opt_selected != 0;
    bool enabled = s_opt_enabled != 0;
    int mid = CARD_OPT_X + CARD_OPT_W / 2;
    unsigned ink = !enabled ? 0x707070 : selected ? CARD_GOLD : CARD_PAPER;
    unsigned line = !enabled ? 0x333333 : pressed ? CARD_PAPER : selected ? CARD_GOLD : 0x757575;

    bool bmp_shown = card_show_bmp(1, 244, ink);
    if (!bmp_shown) {
        if (s_card.choice[0]) {
            char clean[PC_CARD_TEXT_MAX + 8];
            sanitize_text(s_card.choice, clean, sizeof(clean), &lv_font_ui28);
            label_box(card_optlab, clean, &lv_font_ui28, mid, 268, 1, 280);
            lv_obj_set_style_text_color(card_optlab, lv_color_hex(ink), 0);
            lv_obj_remove_flag(card_optlab, LV_OBJ_FLAG_HIDDEN);
        }
    }
    {
        char hint[40];
        snprintf(hint, sizeof(hint), "%d / %d · %s",
                 s_card.q_page + 1,
                 s_card.q_optcount > 0 ? s_card.q_optcount : 1,
                 s_card.computer ? "已移交电脑"
                                  : selected ? "已选 · 点按取消" : "点按选择");
        label(card_opthint, hint, &lv_font_ui16, mid, 292, 1);
    }
    lv_obj_set_style_text_color(card_opthint, lv_color_hex(enabled ? CARD_MUTED : 0x707070), 0);
    lv_obj_remove_flag(card_opthint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(card_optline, mid - 24, CARD_OPT_Y + CARD_OPT_H - 9);
    lv_obj_set_style_bg_color(card_optline, lv_color_hex(line), 0);
    lv_obj_remove_flag(card_optline, LV_OBJ_FLAG_HIDDEN);
    if (selected && !bmp_shown) {
        /* v4 把勾画在选项文字上方（居中），不是左边。
         * 走多行位图时首行已经占掉那个位置，勾选状态改由金色文字 + 提示行表达。 */
        label(card_check, "✓", &lv_font_ui20, mid, CARD_OPT_Y + 27, 1);
        lv_obj_set_style_text_color(card_check, lv_color_hex(CARD_GOLD), 0);
        lv_obj_remove_flag(card_check, LV_OBJ_FLAG_HIDDEN);
    }
}

static void card_style_refresh(void)
{
    if (!card_head) return;
    if (s_card.kind == PC_CARD_APPROVAL) {
        card_btn_apply(0, CARD_DENY_X, CARD_DENY_Y, CARD_BTN_W, CARD_BTN_H, s_card_press == 0);
        card_btn_apply(1, CARD_ALLOW_X, CARD_ALLOW_Y, CARD_BTN_W, CARD_BTN_H, s_card_press == 1);
    } else if (s_card.kind == PC_CARD_QUESTION) {
        card_opt_apply();
        card_btn_apply(0, CARD_NEXT_X, CARD_NEXT_Y, CARD_QBTN_W, CARD_QBTN_H, s_card_press == 0);
        card_btn_apply(1, CARD_SUBMIT_X, CARD_SUBMIT_Y, CARD_QBTN_W, CARD_QBTN_H, s_card_press == 1);
    }
}

void pc_monitor_card_touch(int pressed, int x, int y)
{
    int zone = -1;

    if (!s_card_live) return;
    /* 已移交电脑（审批/问答）：卡片不再响应，避免"看起来还能点"。 */
    if (s_card.computer) return;
    if (pressed) {
        if (s_card.kind == PC_CARD_APPROVAL) {
            if (x >= CARD_DENY_X && x < CARD_DENY_X + CARD_BTN_W &&
                y >= CARD_DENY_Y && y < CARD_DENY_Y + CARD_BTN_H) zone = 0;
            else if (x >= CARD_ALLOW_X && x < CARD_ALLOW_X + CARD_BTN_W &&
                     y >= CARD_ALLOW_Y && y < CARD_ALLOW_Y + CARD_BTN_H) zone = 1;
        } else if (s_card.kind == PC_CARD_QUESTION) {
            if (x >= CARD_OPT_X && x < CARD_OPT_X + CARD_OPT_W &&
                y >= CARD_OPT_Y && y < CARD_OPT_Y + CARD_OPT_H) zone = 2;
            else if (x >= CARD_NEXT_X && x < CARD_NEXT_X + CARD_QBTN_W &&
                     y >= CARD_NEXT_Y && y < CARD_NEXT_Y + CARD_QBTN_H) zone = 0;
            else if (x >= CARD_SUBMIT_X && x < CARD_SUBMIT_X + CARD_QBTN_W &&
                     y >= CARD_SUBMIT_Y && y < CARD_SUBMIT_Y + CARD_QBTN_H) zone = 1;
        }
    }
    if (zone != s_card_press) {
        s_card_press = zone;
        card_style_refresh();
    }
}

static void card_apply(void)
{
    char text[PC_CARD_TEXT_MAX + 8];
    char clean[PC_CARD_TEXT_MAX + 8];
    int show_btns = 0;

    if (!card_head) return;
    if (s_card_rev == s_card_applied) return;
    s_card_applied = s_card_rev;
    card_hide_all();
    s_btn_text[0][0] = s_btn_text[1][0] = 0;
    s_btn_hint[0][0] = s_btn_hint[1][0] = 0;
    s_btn_icon[0][0] = s_btn_icon[1][0] = 0;
    s_opt_selected = 0;
    s_opt_enabled = 0;
    if (s_card.kind == PC_CARD_OFF) {
        s_card_press = -1;
        return;
    }

    if (s_card.kind == PC_CARD_SESSION) {
        int empty = (s_card.state == PM_STATE_IDLE) && !s_card.title[0] && !s_bmp[0].valid;
        if (empty) {
            /* v4 空态：插件把「鲸鱼 + 探索未至之境」渲成一张 2-bit 位图塞 slot 0，
             * 固件只负责摆位置；位图没到时退回文字（研究/66 §4.5）。 */
            if (!card_show_bmp(0, 274, CARD_PAPER)) {
                label(card_line, "探索未至之境", &lv_font_ui28, 240, 308, 1);
                lv_obj_set_style_text_color(card_line, lv_color_hex(CARD_PAPER), 0);
                lv_obj_remove_flag(card_line, LV_OBJ_FLAG_HIDDEN);
            }
            label(card_line2, "DSH 等待任务", &lv_font_ui20, 240, 363, 1);
            lv_obj_set_style_text_color(card_line2, lv_color_hex(CARD_MUTED), 0);
            lv_obj_remove_flag(card_line2, LV_OBJ_FLAG_HIDDEN);
        } else {
            label(card_head, state_text(s_card.state), &lv_font_ui20, 240, 276, 1);
            lv_obj_set_style_text_color(card_head, lv_color_hex(state_color(s_card.state)), 0);
            lv_obj_remove_flag(card_head, LV_OBJ_FLAG_HIDDEN);
            if (!card_show_bmp(0, 286, CARD_PAPER) && s_card.title[0]) {
                sanitize_text(s_card.title, clean, sizeof(clean), &lv_font_ui20);
                label_box(card_line, clean, &lv_font_ui20, 240, 310, 1, 316);
                lv_obj_remove_flag(card_line, LV_OBJ_FLAG_HIDDEN);
            }
            if (s_card.tool[0]) snprintf(text, sizeof(text), "%s · %d 步", s_card.tool, s_card.steps);
            else snprintf(text, sizeof(text), "%d 步", s_card.steps);
            label(card_info, text, &lv_font_ui16, 94, 338, 0);
            lv_obj_remove_flag(card_info, LV_OBJ_FLAG_HIDDEN);
            if (s_card.ctx < 0) snprintf(text, sizeof(text), "CTX --");
            else snprintf(text, sizeof(text), "CTX %d%%", s_card.ctx);
            label(card_ctx, text, &lv_font_ui16, 94, 364, 0);
            lv_obj_remove_flag(card_ctx, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(card_ctxbar,
                             s_card.ctx < 0 ? 0 : (s_card.ctx > 100 ? 100 : s_card.ctx),
                             LV_ANIM_OFF);
            lv_obj_set_style_bg_color(card_ctxbar,
                                      lv_color_hex(s_card.ctx >= 80 ? CARD_GOLD : CARD_BLUE),
                                      LV_PART_INDICATOR);
            lv_obj_remove_flag(card_ctxbar, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_card.footer[0]) {
            sanitize_text(s_card.footer, clean, sizeof(clean), &lv_font_ui16);
            label_box(card_footer, clean, &lv_font_ui16, 240, 416, 1, 420);
            lv_obj_remove_flag(card_footer, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_card.kind == PC_CARD_APPROVAL) {
        label(card_head, s_card.computer ? "电脑待确认" : "DSH 审批待确认",
              &lv_font_ui25, 240, 192, 1);
        lv_obj_set_style_text_color(card_head,
                                    lv_color_hex(s_card.computer ? CARD_MUTED : CARD_GOLD), 0);
        lv_obj_remove_flag(card_head, LV_OBJ_FLAG_HIDDEN);

        if (s_card.tool[0]) snprintf(text, sizeof(text), "%s 请求执行", s_card.tool);
        else snprintf(text, sizeof(text), "请求执行");
        sanitize_text(text, clean, sizeof(clean), &lv_font_ui20);
        label_box(card_sub, clean, &lv_font_ui20, 240, 229, 1, 380);
        lv_obj_remove_flag(card_sub, LV_OBJ_FLAG_HIDDEN);

        if (!card_show_bmp(0, 239, CARD_MUTED)) {
            sanitize_text(s_card.cmd[0] ? s_card.cmd : "命令详情请在电脑查看",
                          clean, sizeof(clean), &lv_font_ui16);
            label_box(card_line, clean, &lv_font_ui16, 240, 263, 1, 336);
            lv_obj_remove_flag(card_line, LV_OBJ_FLAG_HIDDEN);
        }
        snprintf(s_btn_text[0], sizeof(s_btn_text[0]), "拒绝");
        snprintf(s_btn_text[1], sizeof(s_btn_text[1]), "允许一次");
        snprintf(s_btn_icon[0], sizeof(s_btn_icon[0]), "×");
        snprintf(s_btn_icon[1], sizeof(s_btn_icon[1]), "→");
        s_btn_enabled[0] = s_btn_enabled[1] = s_card.can_allow ? 1 : 0;
        s_btn_emphasis[0] = 0;
        s_btn_emphasis[1] = 1;
        show_btns = 1;

        if (s_card.computer) snprintf(text, sizeof(text), "请到电脑查看");
        else if (s_card.countdown > 0) snprintf(text, sizeof(text), "%d 秒后转到电脑", s_card.countdown);
        else snprintf(text, sizeof(text), "仅允许本次执行");
        label(card_note, text, &lv_font_ui16, 240, 419, 1);
        lv_obj_remove_flag(card_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* 问答卡：标题行只有 "DSH 问答 · 单选/多选"；页码放在**选项提示**里
         * （设计稿 design/dsh_panel_v4/index.html:236 与 :156 的口径）。 */
        if (s_card.computer) {
            snprintf(text, sizeof(text), "DSH 问答 · 已移交电脑");
        } else {
            snprintf(text, sizeof(text), "DSH 问答 · %s", s_card.q_multi ? "多选" : "单选");
        }
        /* 标题上移 7 px：题干位图槽从 183 起、可占 34 px（2 行），
         * 原来的 177 会和位图首行压在一起（2026-11-21）。 */
        label(card_head, text, &lv_font_ui16, 240, 170, 1);
        lv_obj_set_style_text_color(card_head, lv_color_hex(CARD_GOLD), 0);
        lv_obj_remove_flag(card_head, LV_OBJ_FLAG_HIDDEN);

        if (!card_show_bmp(0, 183, CARD_PAPER)) {
            sanitize_text(s_card.cmd[0] ? s_card.cmd : "请选择一项",
                          clean, sizeof(clean), &lv_font_ui20);
            label_box(card_line, clean, &lv_font_ui20, 240, 207, 1, 400);
            lv_obj_remove_flag(card_line, LV_OBJ_FLAG_HIDDEN);
        }
        s_opt_enabled = s_card.computer ? 0 : 1;
        s_opt_selected = (s_card.q_sel & (1u << (s_card.q_page & 15))) != 0;

        snprintf(s_btn_text[0], sizeof(s_btn_text[0]), "下一项");
        snprintf(s_btn_text[1], sizeof(s_btn_text[1]), "提交");
        snprintf(s_btn_icon[0], sizeof(s_btn_icon[0]), ">>");
        snprintf(s_btn_icon[1], sizeof(s_btn_icon[1]), "✓");
        s_btn_hint[0][0] = 0;   /* 设计稿里"下一项"没有提示行 */
        if (s_card.computer) {
            /* 超时已移交电脑：按钮/选项全部禁用，底部给明确提示，别让人以为卡死 */
            snprintf(s_btn_hint[1], sizeof(s_btn_hint[1]), "已移交电脑");
            s_btn_enabled[0] = 0;
            s_btn_enabled[1] = 0;
            s_btn_emphasis[0] = 0;
            s_btn_emphasis[1] = 0;
            label(card_note, "已移交电脑 · 请在电脑上回答", &lv_font_ui16, 240, 419, 1);
            lv_obj_remove_flag(card_note, LV_OBJ_FLAG_HIDDEN);
        } else {
            int selected = 0;
            for (int b = 0; b < 16; b++) if (s_card.q_sel & (1u << b)) selected++;
            snprintf(s_btn_hint[1], sizeof(s_btn_hint[1]), "已选 %d", selected);
            s_btn_enabled[1] = selected > 0 ? 1 : 0;
            s_btn_enabled[0] = 1;
            s_btn_emphasis[0] = 0;
            s_btn_emphasis[1] = 1;
        }
        show_btns = 1;
    }

    if (show_btns) {
        if (s_card.kind == PC_CARD_QUESTION) {
            card_opt_apply();
            card_btn_apply(0, CARD_NEXT_X, CARD_NEXT_Y, CARD_QBTN_W, CARD_QBTN_H, s_card_press == 0);
            card_btn_apply(1, CARD_SUBMIT_X, CARD_SUBMIT_Y, CARD_QBTN_W, CARD_QBTN_H, s_card_press == 1);
        } else {
            card_btn_apply(0, CARD_DENY_X, CARD_DENY_Y, CARD_BTN_W, CARD_BTN_H, s_card_press == 0);
            card_btn_apply(1, CARD_ALLOW_X, CARD_ALLOW_Y, CARD_BTN_W, CARD_BTN_H, s_card_press == 1);
        }
    }
}

/* ------------------------------------------------------- 卡片层：对外 API */

void pc_monitor_note_link(void)
{
    s_card_live = 1;
    s_card_ms = lv_tick_get();
}

void pc_monitor_card_clear(void)
{
    memset(&s_card, 0, sizeof(s_card));
    s_card.kind = PC_CARD_OFF;
    s_card.ctx = -1;
    s_card_press = -1;
    s_card_rev++;
}

void pc_monitor_set_card(const pm_card_t *card)
{
    if (!card) {
        pc_monitor_card_clear();
        return;
    }
    /* 2026-11-21：换卡种时**不能**清位图。
     *
     * 插件（0.4.18）现在先发 slot0/1 位图、再发卡片帧；如果这里清槽，
     * 刚到的新位图会被擦掉，卡片退回固件字库文本 —— 题目缺字就显示 '?'。
     * 各卡种只显示自己用到的 slot（会话/审批用 slot0，问答用 slot0+slot1），
     * 插件对空文本也会显式发 0x57 清槽，所以旧图不会残留。 */
    s_card = *card;
    s_card.title[PC_CARD_TEXT_MAX - 1] = 0;
    s_card.cmd[PC_CARD_TEXT_MAX - 1] = 0;
    s_card.choice[PC_CARD_TEXT_MAX - 1] = 0;
    s_card.footer[PC_CARD_TEXT_MAX - 1] = 0;
    s_card.tool[sizeof(s_card.tool) - 1] = 0;
    if (s_card.kind < PC_CARD_OFF || s_card.kind > PC_CARD_QUESTION) s_card.kind = PC_CARD_OFF;
    s_card_rev++;
    s_card_press = -1;
}

/* 返回：1 = 整张图收齐生效；0 = 已收一片，等下一片；-1 = 丢弃（PC 需重发） */
int pc_monitor_card_bitmap(int slot, int w, int h, int index, int total, int bpp,
                           const uint8_t *data, int len)
{
    card_bmp_t *b;
    uint16_t expect;

    if (slot < 0 || slot >= PC_CARD_BMP_SLOTS) return -1;
    /* 清槽：w=h=total=0、无数据（标题/题目变空时插件用它擦掉旧图） */
    if (w == 0 && h == 0 && total == 0) {   /* 清槽 */
        s_bmp[slot].valid = 0;
        s_bmp[slot].open = 0;
        s_a8_valid[slot] = 0;
        s_card_rev++;
        return 1;
    }
    if (data == NULL || len <= 0) return -1;
    if (w <= 0 || (w & 7) != 0 || w > PC_CARD_BMP_W) return -1;
    if (h <= 0 || h > PC_CARD_BMP_H) return -1;
    if (bpp != 1 && bpp != 2) return -1;
    if (total <= 0 || total > 32 || index < 0 || index >= total) return -1;
    expect = (uint16_t)(((uint32_t)w * bpp / 8u) * h);
    b = &s_bmp[slot];

    if (index == 0) {
        memset(b->bits, 0, CARD_BMP_BYTES);
        b->w = (uint16_t)w;
        b->h = (uint16_t)h;
        b->total = (uint16_t)total;
        b->index = 0;
        b->expect = expect;
        b->bpp = (uint8_t)bpp;
        b->filled = 0;
        b->open = 1;
        b->valid = 0;
    }
    if (!b->open) return -1;
    if (w != b->w || h != b->h || total != b->total || (uint16_t)index != b->index ||
        (uint8_t)bpp != b->bpp) {
        b->open = 0;                /* 丢包/乱序：整张作废，PC 从 0 重发 */
        return -1;
    }
    if ((uint32_t)b->filled + (uint32_t)len > b->expect) {
        b->open = 0;
        return -1;
    }
    memcpy(&b->bits[b->filled], data, (size_t)len);
    b->filled = (uint16_t)(b->filled + len);
    b->index++;
    if (b->index < b->total) return 0;

    b->open = 0;
    if (b->filled != b->expect) {   /* 字节数对不上：不给画半张图 */
        b->valid = 0;
        return -1;
    }
    b->valid = 1;
    b->ver++;
    s_a8_valid[slot] = 0;
    s_card_rev++;
    return 1;
}

/* R3c C：监控页 App 的 load/close 负责会话可见性（替代 service 里的 page==4 判断） */
static int s_page_active;

void pc_monitor_page_enter(void) { s_page_active = 1; }
void pc_monitor_page_exit(void)  { s_page_active = 0; }
int  pc_monitor_page_active(void) { return s_page_active; }

/* P0-4：USB 是否被主机配置（= 表盘右滑能不能进监控页的闸门） */
static int s_usb_ready;

void pc_monitor_set_usb_ready(int on) { s_usb_ready = on ? 1 : 0; }
int  pc_monitor_usb_ready(void) { return s_usb_ready; }

lv_obj_t *pc_monitor_create(lv_obj_t *parent)
{
    page=lv_obj_create(parent);lv_obj_remove_style_all(page);
    lv_obj_set_size(page,480,480);
    lv_obj_set_style_bg_color(page,lv_color_black(),0);
    lv_obj_set_style_bg_opa(page,LV_OPA_COVER,0);
    lv_obj_remove_flag(page,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    /* 掉线/等待态用 v4 的两个词：连接已中断 / 等待连接 DSH 面板 */
    offline=new_label(page,0xF0F0F0);
    label(offline,"连接已中断",&lv_font_ui20,240,232,1);
    offline2=new_label(page,0xA8A8A8);
    label(offline2,"等待连接 DSH 面板",&lv_font_ui16,240,258,1);
    content=lv_obj_create(page);lv_obj_remove_style_all(content);
    lv_obj_set_size(content,480,480);
    lv_obj_remove_flag(content,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(content,tracks,LV_EVENT_DRAW_MAIN,NULL);
    /* 三环与数值坐标对齐 v4 主机面板（render-monitor-canvas.mjs）：
     * CPU 48px @(157,206)、GPU 28px @(322,169)、RAM 28px @(322,230)、
     * VRAM @(322,186)、RX/TX @(157/322, 248)。 */
    for(int k=0;k<3;k++) {
        arcs[k]=lv_arc_create(content);lv_obj_remove_style_all(arcs[k]);
        int diameter=2*radii[k]+6;
        lv_obj_set_size(arcs[k],diameter,diameter);
        lv_obj_set_pos(arcs[k],240-diameter/2,240-diameter/2);
        lv_obj_remove_flag(arcs[k],LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_arc_width(arcs[k],6,LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arcs[k],lv_color_hex(colors[k]),LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arcs[k],LV_OPA_COVER,LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arcs[k],false,LV_PART_INDICATOR);
        lv_arc_set_range(arcs[k],0,1000);lv_arc_set_bg_angles(arcs[k],120,60);
        values[k]=new_label(content,colors[k]);units[k]=new_label(content,colors[k]);
    }
    arc_captions[0]=new_label(content,colors[0]);
    label(arc_captions[0],"CPU",&lv_font_ui16,157,156,1);
    arc_captions[1]=new_label(content,colors[1]);gpu_caption=arc_captions[1];
    label(arc_captions[1],"GPU",&lv_font_ui14,322,144,1);
    arc_captions[2]=new_label(content,colors[2]);
    label(arc_captions[2],"RAM",&lv_font_ui14,322,201,1);
    net[0]=new_label(content,0xAAAAAA);net[1]=new_label(content,0xAAAAAA);

    /* 顶部 48px 时钟（v4：5 个数字槽位 x=136/166/192/218/248，基线 112） */
    {
        static const int slots[5]={136,166,192,218,248};
        for (int k=0;k<5;k++) {
            clock_digits[k]=new_label(content,0xF0F0F0);
            label(clock_digits[k],k==2?":":"0",&lv_font_ui48,slots[k],112,1);
        }
        clock_date=new_label(content,0xF0F0F0);
        clock_week=new_label(content,0xA8A8A8);
    }
    /* 卡片态：数值切成 compact 一行（与 v4 一致，给卡片让出中间区域） */
    compact[0]=new_label(content,colors[0]);
    compact[1]=new_label(content,colors[1]);
    compact[2]=new_label(content,colors[2]);
    label(compact[0],"CPU --",&lv_font_ui16,125,149,1);
    label(compact[1],"GPU --",&lv_font_ui16,240,149,1);
    label(compact[2],"RAM --",&lv_font_ui16,355,149,1);
    lv_obj_add_flag(compact[0],LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(compact[1],LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(compact[2],LV_OBJ_FLAG_HIDDEN);
    /* GPU's secondary reading shares its column; lower half stays available to plugins. */
    vram_label=new_label(content,0xAAAAAA);
    lower_create(content);
    card_create(content);       /* BLE 卡片层（0x53/0x54/0x55/0x57） */
    lv_obj_add_flag(content,LV_OBJ_FLAG_HIDDEN);
    return page;
}
static void rate(char *out,size_t size,const char *prefix,int64_t bytes)
{
    static const char *u[]={"B/s","KB/s","MB/s","GB/s"};
    if(bytes<0) { snprintf(out,size,"%s  --",prefix);return; }
    double n=(double)bytes;int i=0;
    while(n>=1024 && i<3) { n/=1024;i++; }
    snprintf(out,size,n>=100?"%s  %.0f %s":"%s  %.1f %s",prefix,n,u[i]);
}
static void ui_reset_cache(void)
{
    shown_state[0]=0;
    shown_info[0]=0;
    shown_ctx[0]=0;
    shown_metric[0]=shown_metric[1]=shown_metric[2]=-2;
    shown_vram=-2;
    shown_gpu_brand=-1;
    shown_rx=-2;shown_tx=-2;
    shown_ctx_value=-2;shown_ctx_hot=false;
    focus_img_set=false;shown_bmp_w=0;shown_bmp_hash=0;
    lv_obj_add_flag(focus_img,LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(focus_ph,LV_OBJ_FLAG_HIDDEN);
    lower_shown=false;
    lv_obj_add_flag(lower,LV_OBJ_FLAG_HIDDEN);
}
static void top_apply(const pm_link_t *s)
{
    int brand=s->gpu_brand>=0 && s->gpu_brand<=3?s->gpu_brand:0;
    if(brand!=shown_gpu_brand) {
        shown_gpu_brand=brand;
        lv_color_t color=lv_color_hex(gpu_colors[brand]);
        lv_obj_set_style_arc_color(arcs[1],color,LV_PART_INDICATOR);
        lv_obj_set_style_text_color(gpu_caption,color,0);
        lv_obj_set_style_text_color(values[1],color,0);
        lv_obj_set_style_text_color(units[1],color,0);
        lv_obj_set_style_text_color(vram_label,color,0);
        lv_obj_set_style_text_color(compact[1],color,0);
    }
    /* 坐标照抄 v4 主机面板：CPU 48@(157,206)、GPU 28@(322,169)、RAM 28@(322,230) */
    const int v[]={s->cpu,s->gpu,s->ram};
    const int cx[]={157,322,322};
    const int baseline[]={206,169,230};
    for(int k=0;k<3;k++) {
        char value[12];
        char ctext[24];
        const lv_font_t *font=k?&lv_font_ui28:&lv_font_ui48;
        if(v[k]<0) strcpy(value,"--");else snprintf(value,sizeof(value),"%d.%d",v[k]/10,v[k]%10);
        snprintf(ctext,sizeof(ctext),"%s %s%%",k==0?"CPU":k==1?"GPU":"RAM",value[0]?value:"--");
        if(v[k]!=shown_metric[k]) {
            shown_metric[k]=v[k];
            int w=text_width(font,value);
            label(values[k],value,font,cx[k]-w/2,baseline[k],0);
            label(units[k],"%",k?&lv_font_ui16:&lv_font_ui20,cx[k]+w/2+4,baseline[k],0);
            lv_arc_set_value(arcs[k],v[k]<0?0:v[k]);
            label_box(compact[k],ctext,&lv_font_ui16,125+k*115,149,1,120);
        }
    }
    char line[40];
    if(s->rx!=shown_rx) {
        shown_rx=s->rx;rate(line,sizeof(line),"RX",s->rx);
        label(net[0],line,&lv_font_ui16,157,248,1);
    }
    if(s->tx!=shown_tx) {
        shown_tx=s->tx;rate(line,sizeof(line),"TX",s->tx);
        label(net[1],line,&lv_font_ui16,322,248,1);
    }
    if(s->vram!=shown_vram) {
        shown_vram=s->vram;
        if(s->vram<0) snprintf(line,sizeof(line),"VRAM --");
        else snprintf(line,sizeof(line),"VRAM %d.%d%%",s->vram/10,s->vram%10);
        label(vram_label,line,&lv_font_ui14,322,186,1);
    }
}
static void lower_apply(const pm_link_t *s)
{
    if(!s->tick) {
        if(lower_shown) { lower_shown=false; lv_obj_add_flag(lower,LV_OBJ_FLAG_HIDDEN); }
        return;
    }
    if(!lower_shown) {
        lower_shown=true;
        lv_obj_remove_flag(lower,LV_OBJ_FLAG_HIDDEN);
        /* force a full lower-half refresh after being hidden */
        shown_state[0]=0;shown_info[0]=0;
        shown_ctx[0]=0;shown_ctx_value=-2;shown_ctx_hot=false;
        focus_img_set=false;shown_bmp_w=0;shown_bmp_hash=0;
    }
    char text[32];
    int state=s->lane_state[0];
    const char *name=state_name(state);
    if(strcmp(shown_state,name)) {
        lv_obj_set_style_text_color(focus_state,lv_color_hex(state_color(state)),0);
        label(focus_state,name,&lv_font_ui20,240,272,1);
        snprintf(shown_state,sizeof(shown_state),"%s",name);
    }

    /* big title bitmap (slot 0 only, horizontally centred at the widest 300px) */
    if((s->bmp_valid & 1u) != 0u) {
        uint16_t w=0,h=0;
        const uint8_t *a8=pm_bitmap_get(0,&w,&h);
        if(a8 && w && h &&
           (!focus_img_set || w!=shown_bmp_w || s->bmp_hash[0]!=shown_bmp_hash)) {
            lv_image_dsc_t *d=&focus_dsc;
            d->header.magic=LV_IMAGE_HEADER_MAGIC;
            d->header.cf=LV_COLOR_FORMAT_A8;
            d->header.flags=0;
            d->header.w=w;d->header.h=h;d->header.stride=w;
            d->data_size=(uint32_t)w*(uint32_t)h;
            d->data=a8;
            lv_image_set_src(focus_img,d);
            lv_obj_set_size(focus_img,w,h);
            lv_obj_set_pos(focus_img,(480-w)/2,286);
            lv_obj_add_flag(focus_ph,LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(focus_img,LV_OBJ_FLAG_HIDDEN);
            focus_img_set=true;shown_bmp_w=w;shown_bmp_hash=s->bmp_hash[0];
        }
    } else if(focus_img_set) {
        lv_obj_add_flag(focus_img,LV_OBJ_FLAG_HIDDEN);
        focus_img_set=false;shown_bmp_w=0;shown_bmp_hash=0;
    }
    if(!focus_img_set) {
        lv_obj_add_flag(focus_img,LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(focus_ph,LV_OBJ_FLAG_HIDDEN);
    }

    snprintf(text,sizeof(text),"%s · %d STEP",s->lane_tool[0],s->lane_step[0]);
    put(focus_info,shown_info,sizeof(shown_info),text,&lv_font_ui16,90,334,0);

    int ctx=s->ctx;
    if(ctx<0) snprintf(text,sizeof(text),"CTX --");
    else snprintf(text,sizeof(text),"CTX %d%%",ctx);
    put(ctx_label,shown_ctx,sizeof(shown_ctx),text,&lv_font_ui16,90,360,0);
    int value=ctx<0?0:ctx>100?100:ctx;
    if(value!=shown_ctx_value) {
        shown_ctx_value=value;
        lv_bar_set_value(ctx_bar,value,LV_ANIM_OFF);
    }
    bool hot=value>=80;
    if(hot!=shown_ctx_hot) {
        shown_ctx_hot=hot;
        lv_obj_set_style_bg_color(ctx_bar,lv_color_hex(hot?0xFFFA00:0x8FB8FF),LV_PART_INDICATOR);
    }
}
/* ---------------------------------------------------------------- BLE 数字通道 */

#define PC_EXT_TTL_MS 5000u
/* 卡片帧的最长静默：超过就认为卡片链路也断了（页面回离线提示，卡片不残留） */
#define PC_CARD_LIVE_MS 8000u
static pm_link_t s_ext;
static uint32_t s_ext_ms;
static int s_ext_valid;

void pc_monitor_set_metrics(int cpu, int gpu, int ram, int vram, int brand,
                            uint32_t rx, uint32_t tx)
{
    s_ext.cpu = cpu < 0 ? 0 : cpu > 1000 ? 1000 : cpu;
    s_ext.gpu = gpu < 0 ? 0 : gpu > 1000 ? 1000 : gpu;
    s_ext.ram = ram < 0 ? 0 : ram > 1000 ? 1000 : ram;
    s_ext.vram = (vram == 0xFFFF) ? -1 : vram;
    s_ext.gpu_brand = (brand >= 0 && brand <= 3) ? brand : 0;
    s_ext.rx = rx;
    s_ext.tx = tx;
    s_ext.session = true;
    s_ext.frame = true;
    s_ext_ms = lv_tick_get();
    s_ext_valid = 1;
}

/* 卡片态：大数值让位给卡片，切成 v4 的 compact 一行（卡片没了再切回来） */
static void metrics_mode(int compact_on)
{
    static int s_compact = -1;
    if (compact_on == s_compact) return;
    s_compact = compact_on;
    for (int k = 0; k < 3; k++) {
        if (compact_on) {
            lv_obj_add_flag(values[k], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(units[k], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(arc_captions[k], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(compact[k], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(values[k], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(units[k], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(arc_captions[k], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(compact[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (compact_on) {
        lv_obj_add_flag(net[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(net[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(vram_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(net[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(net[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(vram_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 顶部时钟：hh mm 五个数字槽位 + MM/DD + 星期X（v4 同位置） */
static void clock_apply(void)
{
    sd3010_time_t tm;
    static const int slots[5] = { 136, 166, 192, 218, 248 };
    char buf[16];
    int hour, min;

    if (!clock_digits[0]) return;
    if (svc_time_get(&tm) != 0) return;
    if (tm.hour == shown_clock_hour && tm.min == shown_clock_min && tm.day == shown_clock_day) return;
    shown_clock_hour = tm.hour;
    shown_clock_min = tm.min;
    shown_clock_day = tm.day;
    hour = tm.hour > 23 ? 23 : tm.hour;
    min = tm.min > 59 ? 59 : tm.min;
    {
        char digits[6];
        static const char *week[7] = { "日", "一", "二", "三", "四", "五", "六" };
        snprintf(digits, sizeof(digits), "%02d:%02d", hour, min);
        for (int k = 0; k < 5; k++) label(clock_digits[k], (char[]){digits[k], 0}, &lv_font_ui48, slots[k], 112, 1);
        snprintf(buf, sizeof(buf), "%02d/%02d", tm.month, tm.day);
        label(clock_date, buf, &lv_font_ui16, 310, 84, 1);
        snprintf(buf, sizeof(buf), "星期%s", week[tm.week <= 6 ? tm.week : 0]);
        label(clock_week, buf, &lv_font_ui16, 310, 111, 1);
    }
}

void pc_monitor_update(const pm_link_t *s,bool visible)
{
    /* BLE 数字通道优先：外部数值新鲜时用一份合并副本，不改调用方的 state */
    pm_link_t merged;
    if (s_ext_valid && (uint32_t)(lv_tick_get() - s_ext_ms) < PC_EXT_TTL_MS) {
        merged = *s;
        merged.session = true;
        merged.frame = true;
        merged.cpu = s_ext.cpu;
        merged.gpu = s_ext.gpu;
        merged.ram = s_ext.ram;
        merged.vram = s_ext.vram;
        merged.gpu_brand = s_ext.gpu_brand;
        merged.rx = s_ext.rx;
        merged.tx = s_ext.tx;
        s = &merged;
    }
    /* 卡片帧（0x53..0x57）也算链路活着：BLE 下 0x52 偶尔停 3 s 不能把页面打回离线。
     * 数值本身仍只认 0x52 的快照，过期就回退 pcmon 数据，不显示假数值。 */
    if (s_ext_valid && (uint32_t)(lv_tick_get() - s_ext_ms) >= PC_EXT_TTL_MS) {
        s_ext_valid = 0;
    }
    bool connected=(s->session && s->frame) ||
                   (s_card_live && (uint32_t)(lv_tick_get() - s_card_ms) < PC_CARD_LIVE_MS);
    if(!page) return;
    if(connected!=was_connected) {
        if(connected) {
            lv_obj_remove_flag(content,LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(offline,LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(offline2,LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(content,LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(offline,LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(offline2,LV_OBJ_FLAG_HIDDEN);
            ui_reset_cache();
        }
        was_connected=connected;
    }
    if(!connected) {
        metrics_mode(0);
        return;
    }
    clock_apply();
    /* v4 规则：只有审批/问答把数值收成 compact 一行；会话卡和空闲都保持大数值 */
    metrics_mode(s_card.kind == PC_CARD_APPROVAL || s_card.kind == PC_CARD_QUESTION);
    top_apply(s);
    lower_apply(s);
    card_apply();
}
