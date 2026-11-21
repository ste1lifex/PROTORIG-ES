# 通用点阵字库工具

把 TTF/OTF 转成嵌入式能直接用的点阵字库（C 常量数组，放 Flash，XIP，不占 RAM）。

此目录提供独立的字库生成、查找和绘制工具。手表的 LVGL 固件使用 [fonts_gen/](../lvgl_test/fonts_gen/) 的字库，重生成步骤见[固件字体说明](../lvgl_test/fonts_src/README.md)。

---

## 1. 目录

```
font/
├── README.md            你在这里
├── gen_font.py          生成器：TTF -> 点阵 C 数组
├── font.h / font.c      运行时：查找字形 / UTF-8 解码 / 绘制（不依赖 SDK，可以单独拿去用）
├── ttf/                 输入字体目录（需另行准备）
└── out/                 生成结果（fonts.h + font_*.c）
```

---

## 2. 生成

```powershell
cd <repo>/firmware/font
python gen_font.py --out out
```

依赖 `pillow`、`fonttools` 和 `numpy`。默认字体文件名见 `gen_font.py` 中的 `TTF_REGULAR`、`TTF_MEDIUM` 与 `TTF_BOLD`，可通过 `--ttf-dir` 指定字体目录。

只生成某几个：

```powershell
python gen_font.py --out out --only cjk_24 --only ascii20
```

渲染一张预览图看字形和度量对不对（**改完生成器一定要先看这个**）：

```powershell
python gen_font.py --preview cjk_24 "中文测试 ABCabc 123 ，。！"
```

---

## 3. 当前字库清单

| 名字 | 字号 | 灰阶 | 字符集 | 位图 | 用途 |
|------|------|------|--------|------|------|
| `font_num48` | 48px | 4bpp | 数字+`:.%-+` | 5.8 KB | 大号时间（备用） |
| `font_ascii16` | 16px | 4bpp | ASCII 0x20~0x7E | 4.6 KB | 电量槽、诊断浮层 |
| `font_ascii20` | 20px | 4bpp | ASCII | 6.1 KB | 常规正文 |
| `font_ascii20b` | 20px | 4bpp | ASCII（粗） | 6.9 KB | **表盘数字都用它** |
| `font_ascii28` | 28px | 4bpp | ASCII | 12.4 KB | 大号文字 |
| `font_ascii40` | 40px | 4bpp | 数字 | 3.8 KB | 特大数字 |
| `font_cjk_16` | 16px | 1bpp | cjk-common（4534 字） | 129.4 KB | 中文小字 |
| `font_cjk_24` | 24px | 1bpp | cjk-common（4534 字） | 280.9 KB | **中文主字号** |

位图合计约 **450 KB**；再加上字形索引（`codes[]` 4 B/字 + `glyph_t` 12 B/字），
两个中文字库合计约 595 KB。**集成前核对目标固件的 Flash 占用**（见第 6 节）。

### 字符集预设

| 名字 | 内容 | 字数 |
|------|------|------|
| `digits` | `0-9` | 10 |
| `numeric` | `0-9 : . / - + % °` | 17 |
| `ascii` | 0x20~0x7E | 95 |
| `latin` | ascii + 0xA0~0xFF | 191 |
| `symbols` | ascii + GB2312 区1~9 + 常用符号 | ~800 |
| **`cjk-common`** | ascii + GB2312 符号区 + **一级汉字 3755** | **4534** |
| `cjk-full` | cjk-common + 二级汉字 3008 | ~7540 |

`cjk-common` 就是平时说的「常用简体中文」——
GB2312 一级汉字按拼音排序的 3755 字，覆盖日常 99% 以上。
二级汉字是生僻字，要的话把 `FONT_PLAN` 里某个条目的字符集改成 `cjk-full` 即可（体积约 +60%）。

也可以直接写字符：`--charset chars:你好世界ABC`，或者给一个 UTF-8 文本文件路径。

---

## 4. 数据格式（`font.h`）

```c
typedef struct {
    uint32_t off;   /* 位图池里的字节偏移 */
    uint16_t len;
    uint8_t  bw, bh;      /* 位图宽 / 高（像素） */
    int8_t   bx, by;      /* 相对笔位置 / 基线的偏移，by 负数 = 基线之上 */
    uint8_t  adv;         /* 步进宽度 */
    uint8_t  pad;
} font_glyph_t;   /* 12 字节，故意排好避免 padding */

typedef struct {
    const char *name;
    uint8_t px, ascent, descent, line_height, bpp, is_ascii;
    uint16_t first, count;
    const uint32_t     *codes;   /* 升序 codepoint；is_ascii 时为 NULL */
    const font_glyph_t *glyphs;
    const uint8_t      *bitmap;
} font_t;
```

* **位图**：每像素 `bpp` 位，按行打包，**MSB first**，每行字节对齐。
  `stride = (bw * bpp + 7) / 8`。
* **寻址**：`is_ascii` 为 1 时下标 = `cp - first`，O(1)；
  否则在 `codes[]` 里二分查找。
* **度量**：把笔放在基线 `(pen_x, base_y)`，
  像素原点 = `(pen_x + bx, base_y + by)`，画完 `pen_x += adv`。

## 5. 怎么用

```c
#include "fonts.h"

/* 覆盖率回调：往哪画由调用方决定（可以是屏、可以是内存、可以是串口） */
static void my_plot(void *ctx, int x, int y, uint8_t cov) { /* 混合 cov/255 */ }

font_draw_center(&font_ascii20b, 208, 392, "10:08", my_plot, ctx);  /* 居中 */
font_draw(&font_cjk_24, 20, 60, "你好，世界", my_plot, ctx);         /* 左对齐 */
int w = font_text_width(&font_cjk_24, "预计 1013 hPa");             /* 量宽度 */
```

---

## 6. 集成与空间

在应用中编入需要的 `font/out/font_*.c`，并根据目标固件的链接映射核对 Flash 占用。
字形行以 `stride` 为单位连续存储，可用 `gen_font.py --preview` 检查生成效果。

---

## 7. 换字体 / 加字号

`gen_font.py` 底部的 `FONT_PLAN` 就是清单，加一行就行：

```python
("ascii32b", TTF_BOLD, 32, 4, "ascii"),
("cjk_32",   TTF_MEDIUM, 32, 1, "cjk-common"),
```

`bpp` 建议：

| 场景 | bpp | 说明 |
|------|-----|------|
| 中文（字多） | 1 | 24px × 4534 字 ≈ 281 KB；4bpp 会到 1.1 MB，放不下 |
| 西文（字少） | 4 | 抗锯齿，AMOLED 上看着明显好 |
| 超大数字 | 4 | 只有十几个字，随便用 |

预设选用 Regular、Medium 和 Bold。

想换别的字体，把 TTF 放在输入字体目录，并更新 `FONT_PLAN` 选用的文件名。
`--ttf-dir` 也能指到别的目录。
