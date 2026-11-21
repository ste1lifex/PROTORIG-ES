/*
 * face_preview.c —— 在 PC 上跑真正的 face_lvgl.c，输出 480×480 PPM
 *
 * 为什么需要：插件设置页「默认表盘」的预览图（plugin/.../assets/faces/default.png）
 * 以前是手工拼的假图（只有标题/时间/日期/管理员），和真机差着表圈刻度、LOCAL·01、
 * 电量、分页点…… 2026-11-15 用户反馈"预览和表上完全不一样"。
 * 这里直接编译固件的表盘代码 + 同一份 watch_photo.c + 同一套字库，用 LVGL 的
 * 主机端口渲染一帧，再转成 PNG —— 布局、刻度、颜色、下半区图都和真机同源。
 *
 * 被替换掉的固件依赖（都只影响"表盘画什么"以外的行为）：
 *   power_is_on()             → 1（渲染时按亮屏算，分针才走动画路径）
 *   tp_lvgl_active_recently() → 0
 *   eez_ui_page()             → 0（表盘页）
 *   svc_device_id_admin()     → 0x2BEA4E（同真机）
 *   face_panel_available()    → 0（未装自定义下半区图 → 用内置图）
 *
 * 用法（由 tools/face_preview_host.py 调用）：
 *   face_preview.exe out.ppm [hh] [mm] [ss]
 */
#include "lvgl.h"
#include "face_lvgl.h"
#include "watch_photo.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCR_W 480
#define SCR_H 480

static uint16_t pixels[SCR_W * SCR_H];
static uint16_t drawbuf[SCR_W * 64];

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    uint16_t *src = (uint16_t *)data;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            pixels[y * SCR_W + x] = *src++;
        }
    }
    lv_display_flush_ready(display);
}

/* ---- 固件依赖的最小替身 ---- */
bool power_is_on(void) { return true; }
int tp_lvgl_active_recently(void) { return 0; }
int eez_ui_page(void) { return 0; }
uint32_t svc_device_id_admin(void) { return 0x2BEA4Eu; }
int svc_device_id_chip_ok(void) { return 1; }
int face_panel_available(void) { return 0; }
const uint8_t *face_panel_image_ptr(void) { return NULL; }

static void write_ppm(const char *path)
{
    FILE *out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    fprintf(out, "P6\n%d %d\n255\n", SCR_W, SCR_H);
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        uint16_t p = pixels[i];
        unsigned char rgb[3] = {
            (unsigned char)(((p >> 11) & 31) * 255 / 31),
            (unsigned char)(((p >> 5) & 63) * 255 / 63),
            (unsigned char)((p & 31) * 255 / 31),
        };
        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "face.ppm";
    int hh = argc > 2 ? atoi(argv[2]) : 22;
    int mm = argc > 3 ? atoi(argv[3]) : 47;
    int ss = argc > 4 ? atoi(argv[4]) : 36;

    lv_init();
    lv_display_t *display = lv_display_create(SCR_W, SCR_H);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, drawbuf, NULL, sizeof(drawbuf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_cb);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    face_lvgl_create();
    /* 展示值：和旧预览保持一致，方便对比；电量取一个非满电的日常读数。 */
    face_lvgl_set_battery(3920, 62);
    face_lvgl_set_time(2026, 11, 14, 6, hh, mm, ss);

    /* 表针有 180 ms 的缓动动画：走完再截图，否则指针停在中间角度。 */
    for (int i = 0; i < 60; i++) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
    lv_refr_now(NULL);

    write_ppm(out);
    printf("face_preview: %s (%dx%d)\n", out, SCR_W, SCR_H);
    return 0;
}
