/*
 * font.h —— 点阵字库的运行时接口（和具体屏/驱动无关）
 *
 * 数据是 gen_font.py 生成的常量数组，全部放在 Flash 里（const，XIP，不占 RAM）。
 *
 * 字形寻址：
 *   is_ascii = 1  -> codes[] 为空，下标 = cp - first，O(1)
 *   is_ascii = 0  -> codes[] 是升序 codepoint 表，走二分查找
 *
 * 位图：
 *   每个字形 bpp 位/像素，按行打包，MSB first，每行字节对齐。
 *   stride = (bw * bpp + 7) / 8
 *   像素 (x,y) 位于第 y 行第 x 个字段。
 *
 * 度量：
 *   绘制文本时把笔放在基线 (pen_x, base_y)：
 *       像素原点 = (pen_x + bx, base_y + by)
 *       画完一个字形后 pen_x += adv
 *   by 是负数表示位图顶部在基线之上（绝大多数情况）。
 */
#ifndef _FONT_H_
#define _FONT_H_

#include <stdint.h>
#include <stddef.h>   /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/* 一个字形的元数据（12 字节，故意排好避免 padding） */
typedef struct {
    uint32_t off;  /* 在 font_t::bitmap 里的字节偏移 */
    uint16_t len;  /* 位图占用的字节数 */
    uint8_t bw;    /* 位图宽（像素） */
    uint8_t bh;    /* 位图高（像素） */
    int8_t bx;     /* 相对笔位置的 x 偏移 */
    int8_t by;     /* 相对基线的 y 偏移（负 = 基线之上） */
    uint8_t adv;   /* 步进宽度（像素） */
    uint8_t pad;
} font_glyph_t;

typedef struct {
    const char *name;
    uint8_t px;          /* 设计字号 */
    int8_t ascent;       /* 基线到 ascender 线 */
    int8_t descent;      /* 基线到 descender 线（正数） */
    uint8_t line_height; /* ascent + descent */
    uint8_t bpp;         /* 每像素位数：1 / 2 / 4 */
    uint8_t is_ascii;    /* 1 = codes 连续，可 O(1) 定位 */
    uint16_t first;      /* is_ascii 时的第一个 codepoint */
    uint16_t count;      /* 字形个数 */
    const uint32_t *codes;      /* 升序 codepoint（is_ascii 时为 NULL） */
    const font_glyph_t *glyphs;
    const uint8_t *bitmap;
} font_t;

/* 覆盖率回调：cov = 0..255（0 = 全透明，255 = 完全不透明） */
typedef void (*font_plot_fn)(void *ctx, int x, int y, uint8_t cov);

/* 查字形；没有返回 NULL */
const font_glyph_t *font_lookup(const font_t *f, uint32_t cp);

/* 取某个像素的覆盖率 0..255 */
uint8_t font_pixel(const font_t *f, const font_glyph_t *g, int x, int y);

/* UTF-8 解码：返回 codepoint，同时把 *s 往后移。非法字节返回 0xFFFD 并前进 1 字节 */
uint32_t font_utf8_next(const char **s);

/* 一段 UTF-8 文本按这个字库排版有多宽（像素） */
int font_text_width(const font_t *f, const char *utf8);

/* 把一段 UTF-8 文本画到 (pen_x, base_y)（base_y 是基线）。
 * 返回画完之后的笔位置。 */
int font_draw(const font_t *f, int pen_x, int base_y, const char *utf8,
              font_plot_fn plot, void *ctx);

/* 居中绘制：cx 是水平中心 */
int font_draw_center(const font_t *f, int cx, int base_y, const char *utf8,
                     font_plot_fn plot, void *ctx);

/* 右对齐绘制：rx 是右边界 */
int font_draw_right(const font_t *f, int rx, int base_y, const char *utf8,
                    font_plot_fn plot, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* _FONT_H_ */
