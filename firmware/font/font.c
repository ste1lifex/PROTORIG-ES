/*
 * font.c —— 点阵字库的查找 / 解码 / 绘制
 *
 * 只依赖 stdint，不依赖 SDK，可以单独拿去用。
 */
#include "font.h"

/* ------------------------------------------------------------------ */
/* 查找                                                                */
/* ------------------------------------------------------------------ */

const font_glyph_t *font_lookup(const font_t *f, uint32_t cp)
{
    uint32_t lo, hi;

    if (f == 0 || f->count == 0) {
        return 0;
    }

    if (f->is_ascii) {
        if (cp < f->first || cp >= (uint32_t)f->first + f->count) {
            return 0;
        }
        return &f->glyphs[cp - f->first];
    }

    /* 二分查找升序的 codepoint 表 */
    lo = 0;
    hi = f->count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        uint32_t c = f->codes[mid];
        if (c == cp) {
            return &f->glyphs[mid];
        }
        if (c < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 取像素                                                              */
/* ------------------------------------------------------------------ */

uint8_t font_pixel(const font_t *f, const font_glyph_t *g, int x, int y)
{
    uint32_t stride;
    uint32_t bitpos;
    uint32_t byteoff;
    uint8_t byte;
    uint8_t v;
    uint8_t levels;

    if (x < 0 || y < 0 || x >= g->bw || y >= g->bh) {
        return 0;
    }

    stride = ((uint32_t)g->bw * f->bpp + 7) / 8;
    bitpos = (uint32_t)y * stride * 8 + (uint32_t)x * f->bpp;
    byteoff = bitpos >> 3;
    if (byteoff >= g->len) {
        return 0;
    }
    byte = f->bitmap[g->off + byteoff];

    levels = (uint8_t)((1u << f->bpp) - 1u);
    /* 从 MSB 侧取 bpp 位 */
    v = (uint8_t)((byte >> (8 - f->bpp - (bitpos & 7))) & levels);

    /* 归一化到 0..255 */
    return (uint8_t)((v * 255u + levels / 2u) / levels);
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

uint32_t font_utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int extra;
    int i;

    if (p == 0 || *p == 0) {
        return 0;
    }

    if (p[0] < 0x80) {
        *s += 1;
        return p[0];
    }

    if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F;
        extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07;
        extra = 3;
    } else {
        *s += 1;
        return 0xFFFD; /* 非法首字节 */
    }

    for (i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += 1;
            return 0xFFFD; /* 续字节不合法 */
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }

    *s += extra + 1;
    return cp;
}

/* ------------------------------------------------------------------ */
/* 排版                                                                */
/* ------------------------------------------------------------------ */

static int text_width_ctx(const font_t *f, const char *utf8)
{
    int w = 0;

    while (utf8 && *utf8) {
        uint32_t cp = font_utf8_next(&utf8);
        const font_glyph_t *g = font_lookup(f, cp);
        if (g) {
            w += g->adv;
        } else {
            w += f->px / 2; /* 缺字：给半个字宽，视觉上不至于挤在一起 */
        }
    }
    return w;
}

int font_text_width(const font_t *f, const char *utf8)
{
    return text_width_ctx(f, utf8);
}

int font_draw(const font_t *f, int pen_x, int base_y, const char *utf8,
              font_plot_fn plot, void *ctx)
{
    int pen = pen_x;

    while (utf8 && *utf8) {
        uint32_t cp = font_utf8_next(&utf8);
        const font_glyph_t *g = font_lookup(f, cp);
        int y, x;

        if (g == 0) {
            pen += f->px / 2;
            continue;
        }

        for (y = 0; y < g->bh; y++) {
            int py = base_y + g->by + y;
            for (x = 0; x < g->bw; x++) {
                uint8_t cov = font_pixel(f, g, x, y);
                if (cov) {
                    plot(ctx, pen + g->bx + x, py, cov);
                }
            }
        }
        pen += g->adv;
    }
    return pen;
}

int font_draw_center(const font_t *f, int cx, int base_y, const char *utf8,
                     font_plot_fn plot, void *ctx)
{
    int w = text_width_ctx(f, utf8);
    return font_draw(f, cx - w / 2, base_y, utf8, plot, ctx);
}

int font_draw_right(const font_t *f, int rx, int base_y, const char *utf8,
                    font_plot_fn plot, void *ctx)
{
    int w = text_width_ctx(f, utf8);
    return font_draw(f, rx - w, base_y, utf8, plot, ctx);
}
