#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
fork_terrain565.py —— 把 watchface/terrain.c 机械地转成 RGB565 直出版本

为什么这么做：
  terrain.c 直接写 gfx_t->px[]，而 gfx 是 RGB888（uint32）。
  要把它放进 LVGL，最省内存的办法是让它直接写 RGB565 —— 否则要么
  多一块 480x200x4 = 375 KB 的中间缓冲（RAM 放不下），要么每帧转换。
  RGB565 画布 480x200 只要 192 KB。

改动点（全部是机械替换，不重写逻辑）：
  1. 去掉 gfx.h 依赖，换成自己的 tf565_t（只有 px/band_y/band_h）
  2. s_trace / s_hiadd 两张 256 级查找表从 RGB888 改成 RGB565
  3. px_max / px_add -> 565 分量版本
  4. 3 处 g->px[] 写入点换成 565 辅助函数
  5. 公开函数改名加 565 后缀，避免和原工程混淆

输入 : ../../watchface/terrain.c
输出 : ./terrain565.c
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.dirname(HERE)                       # .../firmware/lvgl_test
SRC = os.path.join(os.path.dirname(OUT), "watchface", "terrain.c")
DST = os.path.join(OUT, "terrain565.c")

if "--legacy-output" not in sys.argv:
    sys.exit("V1.5 uses hand-maintained 2-D terrain565.c. Use --legacy-output to export the old renderer separately.")
DST = os.path.join(HERE, "terrain565_legacy.c")

src = open(SRC, "r", encoding="utf-8").read()

# ---------------------------------------------------------------- 替换表
PAIRS = [
    # 1) include 与类型
    ('#include "terrain.h"', '#include "terrain565.h"'),
    ('void terrain_render_band(gfx_t *g)', 'void terrain565_render_band(tf565_t *g)'),
    ('void terrain_init(uint32_t seed)', 'void terrain565_init(uint32_t seed)'),
    ('void terrain_set_growth(uint8_t g)', 'void terrain565_set_growth(uint8_t g)'),
    ('void terrain_begin_frame(uint32_t t_ms)', 'void terrain565_begin_frame(uint32_t t_ms)'),
    ('uint32_t terrain_seed(void)', 'uint32_t terrain565_seed(void)'),
    ('uint8_t terrain_get_growth(void)', 'uint8_t terrain565_get_growth(void)'),

    # 2) 查找表改 565
    ('static uint32_t s_trace[256];        /* GOLD_TRACE 的 256 级亮度 ramp  */',
     'static uint16_t s_trace[256];        /* GOLD_TRACE 的 256 级亮度 ramp (RGB565) */'),
    ('static uint32_t s_hiadd[256];        /* GOLD_HI 的加色增量 ramp        */',
     'static uint16_t s_hiadd[256];        /* GOLD_HI 的加色增量 ramp (RGB565) */'),
    ('s_trace[i] = (r << 16) | (g << 8) | b;',
     's_trace[i] = TF565_PACK(r, g, b);'),
    ('s_hiadd[i] = (r << 16) | (g << 8) | b;',
     's_hiadd[i] = TF565_PACK(r, g, b);'),

    # 3) 条带高（原来用 gfx 的 GFX_BAND_H）
    ('uint8_t  s_scan[GFX_BAND_H];  /* 每行的扫描线系数（带内下标）   */',
     'uint8_t  s_scan[TF565_BAND_H];  /* 每行的扫描线系数（带内下标）   */'),
    ('uint8_t  s_vmask[GFX_BAND_H]; /* 每行的扫描亮线垂直权重         */',
     'uint8_t  s_vmask[TF565_BAND_H]; /* 每行的扫描亮线垂直权重         */'),

    # 4) 写入点
    ('px_max(&g->px[(y - by) * TF_W + x], s_trace[cov]);',
     'tf565_max(&g->px[(y - by) * TF_W + x], s_trace[cov]);'),
    ('px_max(&row[x], s_trace[cov]);',
     'tf565_max(&row[x], s_trace[cov]);'),
    ('px_add(p, a2);', 'tf565_add(p, a2);'),
    ('            uint32_t *row = &g->px[(y - by) * TF_W];',
     '            uint16_t *row = &g->px[(y - by) * TF_W];'),
    ('            uint32_t *p;', '            uint16_t *p;'),

    # 5) px_max / px_add 定义换 565 版
    ('''static inline void px_max(uint32_t *p, uint32_t c)
{
    uint32_t o = *p;
    uint32_t r = o & 0x00FF0000u, g = o & 0x0000FF00u, b = o & 0x000000FFu;
    uint32_t cr = c & 0x00FF0000u, cg = c & 0x0000FF00u, cb = c & 0x000000FFu;
    if (cr > r) r = cr;
    if (cg > g) g = cg;
    if (cb > b) b = cb;
    *p = r | g | b;
}''',
     '''/* 分量取最大（RGB565 空间）。等价于原来的 px_max，只是位宽不同。 */
static inline void tf565_max(uint16_t *p, uint16_t c)
{
    uint16_t o = *p;
    uint16_t r = (o & 0xF800u) > (c & 0xF800u) ? (o & 0xF800u) : (c & 0xF800u);
    uint16_t g = (o & 0x07E0u) > (c & 0x07E0u) ? (o & 0x07E0u) : (c & 0x07E0u);
    uint16_t b = (o & 0x001Fu) > (c & 0x001Fu) ? (o & 0x001Fu) : (c & 0x001Fu);
    *p = (uint16_t)(r | g | b);
}'''),
    ('''static inline void px_add(uint32_t *p, uint32_t a)
{
    uint32_t o = *p;
    uint32_t c = s_hiadd[a & 0xFFu];
    uint32_t r = ((o >> 16) & 0xFFu) + ((c >> 16) & 0xFFu);
    uint32_t g = ((o >> 8) & 0xFFu) + ((c >> 8) & 0xFFu);
    uint32_t b = (o & 0xFFu) + (c & 0xFFu);
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    *p = (r << 16) | (g << 8) | b;
}''',
     '''/* 加色（RGB565 空间，分量各自饱和） */
static inline void tf565_add(uint16_t *p, uint32_t a)
{
    uint16_t o = *p;
    uint16_t c = s_hiadd[a & 0xFFu];
    uint32_t r = ((o >> 11) & 0x1Fu) + ((c >> 11) & 0x1Fu);
    uint32_t g = ((o >> 5) & 0x3Fu) + ((c >> 5) & 0x3Fu);
    uint32_t b = (o & 0x1Fu) + (c & 0x1Fu);
    if (r > 31u) r = 31u;
    if (g > 63u) g = 63u;
    if (b > 31u) b = 31u;
    *p = (uint16_t)((r << 11) | (g << 5) | b);
}'''),
]


def main():
    out = src
    problems = []
    for old, new in PAIRS:
        n = out.count(old)
        if n == 0:
            problems.append(old.splitlines()[0][:70])
            continue
        out = out.replace(old, new)
        print("  [%d] %s" % (n, old.splitlines()[0][:70]))
    if problems:
        print("\n!! 以下模式没找到（源码可能变了）：")
        for p in problems:
            print("   ", p)
        sys.exit(1)

    # 残留检查：不该再出现 gfx_ / gfx_t / px_max / px_add
    for bad in ("gfx_t", "gfx_", "px_max(", "px_add("):
        if bad in out:
            print("!! 仍然残留:", bad)
            for i, line in enumerate(out.splitlines(), 1):
                if bad in line:
                    print("   %d: %s" % (i, line.strip()[:90]))
            sys.exit(1)

    open(DST, "w", encoding="utf-8", newline="\n").write(out)
    print("\n写出 %s  (%.1f KB)" % (DST, os.path.getsize(DST) / 1024.0))


if __name__ == "__main__":
    main()
