#ifndef EEZ_LVGL_UI_STYLES_H
#define EEZ_LVGL_UI_STYLES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Style: ScreenBg
void add_style_screen_bg(lv_obj_t *obj);
void remove_style_screen_bg(lv_obj_t *obj);

// Style: PageTitle
void add_style_page_title(lv_obj_t *obj);
void remove_style_page_title(lv_obj_t *obj);

// Style: PageSub
void add_style_page_sub(lv_obj_t *obj);
void remove_style_page_sub(lv_obj_t *obj);

// Style: RowMain
void add_style_row_main(lv_obj_t *obj);
void remove_style_row_main(lv_obj_t *obj);

// Style: RowDim
void add_style_row_dim(lv_obj_t *obj);
void remove_style_row_dim(lv_obj_t *obj);

// Style: RowGold
void add_style_row_gold(lv_obj_t *obj);
void remove_style_row_gold(lv_obj_t *obj);

// Style: BigNum
void add_style_big_num(lv_obj_t *obj);
void remove_style_big_num(lv_obj_t *obj);

// Style: WarnText
void add_style_warn_text(lv_obj_t *obj);
void remove_style_warn_text(lv_obj_t *obj);

// Style: Card
void add_style_card(lv_obj_t *obj);
void remove_style_card(lv_obj_t *obj);

// Style: Accent
void add_style_accent(lv_obj_t *obj);
void remove_style_accent(lv_obj_t *obj);

// Style: HLine
void add_style_hline(lv_obj_t *obj);
void remove_style_hline(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_STYLES_H*/