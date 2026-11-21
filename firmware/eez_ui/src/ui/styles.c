#include "styles.h"
#include "images.h"
#include "fonts.h"

#include "ui.h"
#include "screens.h"

//
// Style: ScreenBg
//

void add_style_screen_bg(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
};

void remove_style_screen_bg(lv_obj_t *obj) {
    (void)obj;
};

//
// Style: PageTitle
//

void add_style_page_title(lv_obj_t *obj) {
    lv_obj_set_style_text_color(obj, lv_color_hex(0xF2ED65), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui28, 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
};

void remove_style_page_title(lv_obj_t *obj) {
    (void)obj;
};

//
// Style: PageSub
//

void add_style_page_sub(lv_obj_t *obj) {
    lv_obj_set_style_text_color(obj, lv_color_hex(0x90958D), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui14, 0);
};

void remove_style_page_sub(lv_obj_t *obj) {
    (void)obj;
};

//
// Style: RowMain
//

void add_style_row_main(lv_obj_t *obj) {
    lv_obj_set_style_text_color(obj, lv_color_hex(0xECEEE5), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui20, 0);
};

void remove_style_row_main(lv_obj_t *obj) {
    (void)obj;
};

//
// Style: RowDim
//

void add_style_row_dim(lv_obj_t *obj) {
    lv_obj_set_style_text_color(obj, lv_color_hex(0x90958D), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui14, 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
};

void remove_style_row_dim(lv_obj_t *obj) {
    (void)obj;
};

//
// Style: RowGold
//

void add_style_row_gold(lv_obj_t *obj) {
    lv_obj_set_style_text_color(obj, lv_color_hex(0xF2ED65), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui20, 0);
};

void remove_style_row_gold(lv_obj_t *obj) {
    (void)obj;
};

//
// Style: BigNum
//

void add_style_big_num(lv_obj_t *obj) {
    lv_obj_set_style_text_color(obj, lv_color_hex(0xECEEE5), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui48, 0);
};

void remove_style_big_num(lv_obj_t *obj) {
    (void)obj;
};

//
// Style: WarnText
//

void add_style_warn_text(lv_obj_t *obj) {
    lv_obj_set_style_text_color(obj, lv_color_hex(0xFF6C30), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui14, 0);
};

void remove_style_warn_text(lv_obj_t *obj) {
    (void)obj;
};

//
// Style: Card
//

void add_style_card(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x141713), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x90958D), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
};

void remove_style_card(lv_obj_t *obj) {
    (void)obj;
};

//
// Style: Accent
//

void add_style_accent(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xF2ED65), 0);
    lv_obj_set_style_bg_opa(obj, 255, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
};

void remove_style_accent(lv_obj_t *obj) {
    (void)obj;
};

//
// Style: HLine
//

void add_style_hline(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x90958D), 0);
    lv_obj_set_style_bg_opa(obj, 120, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
};

void remove_style_hline(lv_obj_t *obj) {
    (void)obj;
};

//
//
//

void add_style(lv_obj_t *obj, int32_t styleIndex) {
    typedef void (*AddStyleFunc)(lv_obj_t *obj);
    static const AddStyleFunc add_style_funcs[] = {
        add_style_screen_bg,
        add_style_page_title,
        add_style_page_sub,
        add_style_row_main,
        add_style_row_dim,
        add_style_row_gold,
        add_style_big_num,
        add_style_warn_text,
        add_style_card,
        add_style_accent,
        add_style_hline,
    };
    add_style_funcs[styleIndex](obj);
}

void remove_style(lv_obj_t *obj, int32_t styleIndex) {
    typedef void (*RemoveStyleFunc)(lv_obj_t *obj);
    static const RemoveStyleFunc remove_style_funcs[] = {
        remove_style_screen_bg,
        remove_style_page_title,
        remove_style_page_sub,
        remove_style_row_main,
        remove_style_row_dim,
        remove_style_row_gold,
        remove_style_big_num,
        remove_style_warn_text,
        remove_style_card,
        remove_style_accent,
        remove_style_hline,
    };
    remove_style_funcs[styleIndex](obj);
}