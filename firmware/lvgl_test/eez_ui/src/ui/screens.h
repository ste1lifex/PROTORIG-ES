#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_NETWORK = 1,
    SCREEN_ID_MEETS = 2,
    SCREEN_ID_SETTINGS = 3,
    _SCREEN_ID_LAST = 3
};

typedef struct _objects_t {
    lv_obj_t *scr_network;
    lv_obj_t *scr_meets;
    lv_obj_t *scr_settings;
    lv_obj_t *network_lbl01;
    lv_obj_t *network_lbl02;
    lv_obj_t *network_box03;
    lv_obj_t *network_box04;
    lv_obj_t *network_box05;
    lv_obj_t *network_box06;
    lv_obj_t *network_lbl07;
    lv_obj_t *network_lbl08;
    lv_obj_t *network_lbl09;
    lv_obj_t *network_lbl10;
    lv_obj_t *network_lbl11;
    lv_obj_t *meets_lbl01;
    lv_obj_t *meets_lbl02;
    lv_obj_t *meets_box03;
    lv_obj_t *meets_box04;
    lv_obj_t *meets_lbl05;
    lv_obj_t *meets_lbl06;
    lv_obj_t *meets_box07;
    lv_obj_t *meets_lbl08;
    lv_obj_t *meets_lbl09;
    lv_obj_t *settings_lbl01;
    lv_obj_t *settings_lbl02;
    lv_obj_t *settings_box03;
    lv_obj_t *settings_box04;
    lv_obj_t *settings_lbl05;
    lv_obj_t *brightness_0;
    lv_obj_t *settings_lbl07;
    lv_obj_t *brightness_1;
    lv_obj_t *settings_lbl09;
    lv_obj_t *brightness_2;
    lv_obj_t *settings_lbl11;
    lv_obj_t *settings_lbl12;
    lv_obj_t *timeout_0;
    lv_obj_t *settings_lbl14;
    lv_obj_t *timeout_1;
    lv_obj_t *settings_lbl16;
    lv_obj_t *timeout_2;
    lv_obj_t *settings_lbl18;
} objects_t;

extern objects_t objects;

void create_screen_network();
void tick_screen_network();

void create_screen_meets();
void tick_screen_meets();

void create_screen_settings();
void tick_screen_settings();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/