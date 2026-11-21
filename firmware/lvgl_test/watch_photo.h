#ifndef PROTORIG_WATCH_PHOTO_H
#define PROTORIG_WATCH_PHOTO_H
#include <stdint.h>

/* 默认表盘（face 0）下半区图片：480x232 @ y=248。像素在 watch_photo.c，
 * 由 tools/make_watch_photo.py 从 assets/watch_photo/source.png 生成。 */
#define WATCH_PHOTO_W 480
#define WATCH_PHOTO_H 232
#define WATCH_PHOTO_Y 248
#define WATCH_PHOTO_BYTES (WATCH_PHOTO_W * WATCH_PHOTO_H * 2)
/* Immutable, flash-resident, little-endian RGB565. */
extern const uint8_t watch_photo_pixels[WATCH_PHOTO_BYTES];
#endif
