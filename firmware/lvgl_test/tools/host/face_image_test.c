/*
 * face_image_test.c —— 主机侧渲染验证（不给固件编译，见 tools/face_image_host_render.py）
 *
 * 目的：把 .pfi 里的 image blob（1024 B palette + 480x480 索引，和 flash 里的字节完全一样）
 * 原样交给 LVGL 当 LV_COLOR_FORMAT_I8 图，走和真机同一条
 *   bin_decoder（按行解码成 ARGB8888） -> lv_draw_sw_blend（ARGB8888 -> RGB565）
 * 路径渲染一帧，然后按 palette 逐像素对拍。
 *
 * 这一步专门用来在上板之前回答 70 号文档 §6 的头号风险：
 *   "XIP 读 media 区不生效/花屏" —— 排除 XIP 之后，I8+palette 的 LVGL 语义对不对。
 *
 * 用法： face_image_test <demo.pfi> [out.ppm]
 *   退出码 0 = 全像素匹配；非 0 = 有差异（打印差异比例/最大通道差）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lvgl.h"

#define IMG_W       480
#define IMG_H       480
#define PAL_BYTES   (256 * 4)
#define IMAGE_LEN   (PAL_BYTES + IMG_W * IMG_H)
#define HEADER_LEN  256

static uint16_t s_fb[IMG_W * IMG_H];
static uint8_t *s_blob;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* DIRECT 模式：LVGL 直接画进 s_fb，flush 只需要"放行" */
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static uint8_t *read_file(const char *path, size_t want, size_t *got)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;

    if (f == NULL) {
        fprintf(stderr, "open %s failed\n", path);
        return NULL;
    }
    buf = malloc(want + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    *got = fread(buf, 1, want + 1, f);
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    size_t n = 0;
    uint8_t *file;
    lv_image_dsc_t dsc;
    lv_display_t *disp;
    lv_obj_t *scr, *img;
    int bad = 0, maxdiff = 0;
    int x, y;

    if (argc < 2) {
        fprintf(stderr, "usage: face_image_test <x.pfi> [out.ppm]\n");
        return 2;
    }
    file = read_file(argv[1], HEADER_LEN + IMAGE_LEN, &n);
    if (file == NULL) {
        return 2;
    }
    if (n != (size_t)(HEADER_LEN + IMAGE_LEN)) {
        fprintf(stderr, "bad .pfi size %u (want %u)\n", (unsigned)n,
                (unsigned)(HEADER_LEN + IMAGE_LEN));
        return 2;
    }
    s_blob = file + HEADER_LEN;

    lv_init();
    disp = lv_display_create(IMG_W, IMG_H);
    if (disp == NULL) {
        fprintf(stderr, "lv_display_create failed\n");
        return 2;
    }
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb),
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    memset(&dsc, 0, sizeof(dsc));
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_I8;
    dsc.header.w = IMG_W;
    dsc.header.h = IMG_H;
    dsc.header.stride = IMG_W;          /* I8: 1 B/px */
    dsc.data_size = IMAGE_LEN;          /* palette + 像素（固件同款） */
    dsc.data = s_blob;

    img = lv_image_create(scr);
    lv_image_set_src(img, &dsc);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_invalidate(scr);
    lv_refr_now(disp);

    /* 逐像素对拍：期望值 = palette[idx] 直接转 RGB565（palette alpha = 0xFF，
     * lv_color_24_16_mix 在 opa=255 时就是 (r>>3,g>>2,b>>3)） */
    for (y = 0; y < IMG_H; y++) {
        for (x = 0; x < IMG_W; x++) {
            uint8_t idx = s_blob[PAL_BYTES + y * IMG_W + x];
            uint8_t b = s_blob[4 * idx + 0];
            uint8_t g = s_blob[4 * idx + 1];
            uint8_t r = s_blob[4 * idx + 2];
            uint16_t exp = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            uint16_t got = s_fb[y * IMG_W + x];
            int dr = abs((int)((got >> 11) & 0x1F) - (int)((exp >> 11) & 0x1F));
            int dg = abs((int)((got >> 5) & 0x3F) - (int)((exp >> 5) & 0x3F));
            int db = abs((int)(got & 0x1F) - (int)(exp & 0x1F));

            if (dr || dg || db) {
                bad++;
                if (dr > maxdiff) maxdiff = dr;
                if (dg > maxdiff) maxdiff = dg;
                if (db > maxdiff) maxdiff = db;
            }
        }
    }

    printf("[render] %s: %dx%d, mismatch=%d/%d (%.4f%%), max channel diff=%d\n",
           argv[1], IMG_W, IMG_H, bad, IMG_W * IMG_H,
           100.0 * bad / (IMG_W * IMG_H), maxdiff);
    printf("[render] pixel(0,0): idx=%u palette=(B%u G%u R%u) got=0x%04X\n",
           s_blob[PAL_BYTES], s_blob[0], s_blob[1], s_blob[2], s_fb[0]);
    printf("[render] pixel(240,240): idx=%u got=0x%04X\n",
           s_blob[PAL_BYTES + 240 * IMG_W + 240], s_fb[240 * IMG_W + 240]);

    if (argc >= 3) {
        FILE *f = fopen(argv[2], "wb");

        if (f != NULL) {
            fprintf(f, "P6\n%d %d\n255\n", IMG_W, IMG_H);
            for (y = 0; y < IMG_H; y++) {
                for (x = 0; x < IMG_W; x++) {
                    uint16_t p = s_fb[y * IMG_W + x];
                    uint8_t rgb[3];
                    rgb[0] = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
                    rgb[1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
                    rgb[2] = (uint8_t)((p & 0x1F) * 255 / 31);
                    fwrite(rgb, 1, 3, f);
                }
            }
            fclose(f);
            printf("[render] wrote %s\n", argv[2]);
        }
    }
    free(file);
    return bad == 0 ? 0 : 1;
}
