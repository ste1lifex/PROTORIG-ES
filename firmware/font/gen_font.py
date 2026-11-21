#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font.py —— 把 TTF/OTF 转成嵌入式点阵字库（C 源码）

字体：HarmonyOS Sans SC（鸿蒙字体）—— 华为官方免费商用字体，
      字形风格脱胎于思源黑体 / Noto Sans CJK，覆盖简体中文 + 拉丁 + 常用符号。

用法：
    # 生成工程里的整套字库到 out/
    python gen_font.py --out out

    # 只生成某一个
    python gen_font.py --out out --only cjk_24

    # 把某个字库渲染成 PNG 预览（检查字形/度量是否正确）
    python gen_font.py --preview font_hz20 "中文测试 ABCabc 123"

输出格式（见 font.h）：
    - 每像素 bpp 位，1=二值 / 2=4 级灰 / 4=16 级灰
    - 位图按行打包，MSB first，每行字节对齐
    - 字形表 font_glyph_t[]，偏移/长度指向位图池
    - codepoint 表按升序存放，二分查找；纯 ASCII 字库用 is_ascii 直接算下标
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# --------------------------------------------------------------------------
# 字符集
# --------------------------------------------------------------------------


def gb2312_zones(first_zone, last_zone):
    """GB2312 区号 1..94 -> 字节 0xA0+zone。只保留能解码的。"""
    out = []
    for zone in range(first_zone, last_zone + 1):
        hi = 0xA0 + zone
        for pos in range(1, 95):
            lo = 0xA0 + pos
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return out


ASCII = [chr(c) for c in range(0x20, 0x7F)]

# 常用标点 / 全角符号（GB2312 区 1~9）
GB_SYMBOLS = gb2312_zones(1, 9)
# 一级汉字（区 16~55，按拼音排序，3755 字）—— 就是平时说的「常用汉字」
GB_L1 = gb2312_zones(16, 55)
# 二级汉字（区 56~87，3008 字）
GB_L2 = gb2312_zones(56, 87)

# 补几个 GB2312 里没有、但中文排版常用的符号
EXTRA = list("·—…‘’“”→←↑↓℃±×÷∞≈≤≥√°①②③④⑤⑥⑦⑧⑨⑩")

DIGITS = list("0123456789")
NUMERIC = list("0123456789:./-+%°")

# --------------------------------------------------------------------------
# 表盘 UI 用字（人工精选）
# --------------------------------------------------------------------------
# 全量一级汉字 16px 就已经 1 MB，而表盘界面来回就用这些字。
# 所以表盘的字库**只收 UI 真正会出现的中文**，体积从 1 MB 降到十几 KB。
# 规则：加新界面文案时，把新字补进这个字符串，然后重跑 gen_font.py。
UI_HANZI = (
    # 界面标题 / 标签
    "协议源石容器管理员相遇同调感知设置信号电量连接未已中扫描等待离线"
    # 时间日期
    "年月日时分秒周星期上午下午今天昨天明天"
    # 状态
    "弱强满充正在同步失败成功完成剩余高温低温正常警告错误无有可不"
    # 发现 / 记录页
    "附近发现记录最近编号距离强度首次最后名称类型未知个"
    # 设置页
    "版本关于返回保存校准亮度自动手动关闭打开开启语言中文英文重置恢复"
    # 数字与量词
    "一二三四五六七八九十零百千万亿次数条张枚"
    # 通用虚词 / 提示
    "的了在与和或为请稍候按住项选择确认取消更多详情刷新清空全部"
)
UI_PUNCT = "·—…‘’“”→←↑↓℃±×÷≈≤≥√°"
UI_CHARS = list(dict.fromkeys(UI_HANZI + UI_PUNCT + " ，。！？：；、（）《》【】"))

CHAR_SETS = {
    # 表盘 UI：ASCII + 精选中文
    "ui": ASCII + UI_CHARS,
    "digits": DIGITS,
    "numeric": NUMERIC,
    "ascii": ASCII,
    "latin": ASCII + [chr(c) for c in range(0xA0, 0x100)],
    "symbols": ASCII + GB_SYMBOLS + EXTRA,
    # 常用中文：ASCII + GB2312 全部符号区 + 一级汉字
    "cjk-common": ASCII + GB_SYMBOLS + EXTRA + GB_L1,
    # 全中文：再加二级汉字
    "cjk-full": ASCII + GB_SYMBOLS + EXTRA + GB_L1 + GB_L2,
}


def resolve_charset(spec):
    if spec in CHAR_SETS:
        return CHAR_SETS[spec]
    # 支持直接写字符：chars:汉字ABC
    if spec.startswith("chars:"):
        return list(spec[len("chars:"):])
    if os.path.isfile(spec):
        with open(spec, encoding="utf-8") as fp:
            return list(fp.read())
    raise SystemExit("未知字符集: %s" % spec)


# --------------------------------------------------------------------------
# 度量 / 位图
# --------------------------------------------------------------------------


def glyph_bitmap(font, ch, bpp):
    """渲染单个字形，返回 (bytes, bw, bh, bx, by, adv)。

    bx/by 相对基线：by 为负表示位图顶部在基线之上。
    """
    ascent, descent = font.getmetrics()
    adv = int(round(font.getlength(ch)))

    pad = 16
    W = max(adv, 1) + pad * 2
    H = ascent + descent + pad * 2
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    # PIL 默认 anchor "la"：x 是笔位置，y 是 ascender 线
    d.text((pad, pad), ch, font=font, fill=255)

    box = img.getbbox()
    if box is None:
        return b"", 0, 0, 0, 0, adv  # 空白字形（比如空格）

    x0, y0, x1, y1 = box
    bw, bh = x1 - x0, y1 - y0
    bx = x0 - pad
    by = (y0 - pad) - ascent

    glyph = img.crop(box)

    levels = (1 << bpp) - 1
    px = glyph.load()
    stride = (bw * bpp + 7) // 8
    buf = bytearray(stride * bh)
    for y in range(bh):
        acc = 0
        nbits = 0
        pos = 0
        base = y * stride          # ← 每行都要按 stride 偏移，否则行行重叠
        for x in range(bw):
            v = px[x, y]
            q = (v * levels + 127) // 255
            acc = (acc << bpp) | q
            nbits += bpp
            if nbits == 8:
                buf[base + pos] = acc
                pos += 1
                acc = 0
                nbits = 0
        if nbits:
            buf[base + pos] = acc << (8 - nbits)
    return bytes(buf), bw, bh, bx, by, adv


def build_font(ttf_path, px, bpp, charset, name):
    font = ImageFont.truetype(ttf_path, px)
    ascent, descent = font.getmetrics()

    # 只保留字体真的有的字符
    cov = coverage(ttf_path)
    chars = []
    seen = set()
    for ch in charset:
        if ch in seen:
            continue
        seen.add(ch)
        cp = ord(ch)
        if cov is not None and cp not in cov:
            continue
        chars.append(ch)
    chars.sort(key=ord)

    bitmap = bytearray()
    glyphs = []
    for ch in chars:
        data, bw, bh, bx, by, adv = glyph_bitmap(font, ch, bpp)
        off = len(bitmap)
        bitmap += data
        if adv > 255:
            adv = 255
        glyphs.append((off, len(data), bw, bh, bx, by, adv))

    is_ascii = (chars and ord(chars[0]) == 0x20 and
                [ord(c) for c in chars] == list(range(0x20, 0x20 + len(chars))))

    return {
        "name": name,
        "chars": chars,
        "glyphs": glyphs,
        "bitmap": bytes(bitmap),
        "px": px,
        "bpp": bpp,
        "ascent": ascent,
        "descent": descent,
        "line_height": ascent + descent,
        "is_ascii": is_ascii,
    }


_COV_CACHE = {}


def coverage(ttf_path):
    key = os.path.abspath(ttf_path)
    if key in _COV_CACHE:
        return _COV_CACHE[key]
    try:
        from fontTools.ttLib import TTFont
        t = TTFont(ttf_path, lazy=True)
        cov = set()
        for table in t["cmap"].tables:
            cov.update(table.cmap.keys())
        t.close()
    except Exception as exc:  # pragma: no cover
        print("  (cmap 读取失败，跳过覆盖检查: %s)" % exc, file=sys.stderr)
        cov = None
    _COV_CACHE[key] = cov
    return cov


# --------------------------------------------------------------------------
# C 源码输出
# --------------------------------------------------------------------------


def emit_bytes(fp, arr, per_line=16):
    for i in range(0, len(arr), per_line):
        chunk = arr[i:i + per_line]
        fp.write("    " + " ".join("0x%02X," % b for b in chunk) + "\n")


def write_font_c(out_dir, f, header_name):
    ident = "font_" + f["name"]
    path = os.path.join(out_dir, ident + ".c")
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("/* 自动生成，不要手改 —— 改 gen_font.py 后重新生成。\n")
        fp.write(" * %s : HarmonyOS Sans SC %dpx, %d bpp, %d 个字形\n */\n"
                 % (ident, f["px"], f["bpp"], len(f["chars"])))
        fp.write('#include "%s"\n\n' % header_name)

        if not f["is_ascii"]:
            fp.write("static const uint32_t %s_codes[%d] = {\n" % (ident, len(f["chars"])))
            for i in range(0, len(f["chars"]), 12):
                chunk = f["chars"][i:i + 12]
                fp.write("    " + " ".join("0x%04X," % ord(c) for c in chunk) + "\n")
            fp.write("};\n\n")

        fp.write("static const font_glyph_t %s_glyphs[%d] = {\n" % (ident, len(f["glyphs"])))
        for (off, ln, bw, bh, bx, by, adv) in f["glyphs"]:
            fp.write("    { %u, %u, %u, %u, %d, %d, %u, 0 },\n"
                     % (off, ln, bw, bh, bx, by, adv))
        fp.write("};\n\n")

        fp.write("static const uint8_t %s_bitmap[%d] = {\n" % (ident, max(len(f["bitmap"]), 1)))
        if f["bitmap"]:
            emit_bytes(fp, f["bitmap"])
        else:
            fp.write("    0x00,\n")
        fp.write("};\n\n")

        fp.write("const font_t %s = {\n" % ident)
        fp.write('    .name        = "HarmonyOS Sans SC %d",\n' % f["px"])
        fp.write("    .px          = %d,\n" % f["px"])
        fp.write("    .ascent      = %d,\n" % f["ascent"])
        fp.write("    .descent     = %d,\n" % f["descent"])
        fp.write("    .line_height = %d,\n" % f["line_height"])
        fp.write("    .bpp         = %d,\n" % f["bpp"])
        fp.write("    .is_ascii    = %d,\n" % (1 if f["is_ascii"] else 0))
        fp.write("    .first       = 0x%04X,\n" % (ord(f["chars"][0]) if f["chars"] else 0))
        fp.write("    .count       = %d,\n" % len(f["chars"]))
        fp.write("    .codes       = %s,\n" % (ident + "_codes" if not f["is_ascii"] else "NULL"))
        fp.write("    .glyphs      = %s_glyphs,\n" % ident)
        fp.write("    .bitmap      = %s_bitmap,\n" % ident)
        fp.write("};\n")
    return path, len(f["bitmap"]), len(f["glyphs"])


def write_header(out_dir, fonts, header_name):
    path = os.path.join(out_dir, header_name)
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("/* 自动生成，不要手改 —— 见 gen_font.py */\n")
        fp.write("#ifndef _FONTS_H_\n#define _FONTS_H_\n\n")
        fp.write('#include "font.h"\n\n')
        for f in fonts:
            fp.write("extern const font_t font_%s;   /* %dpx %dbpp %d glyphs */\n"
                     % (f["name"], f["px"], f["bpp"], len(f["chars"])))
        fp.write("\n#endif /* _FONTS_H_ */\n")
    return path


# --------------------------------------------------------------------------
# 字库清单
# --------------------------------------------------------------------------
#
# 说明：
#   bpp=4 的 ASCII 字库是抗锯齿的，屏上好看；体积也不大（95 个字）。
#   中日韩汉字用 bpp=1（二值），否则 24px × 3700 字就 1MB 起步，Flash 放不下。

TTF_REGULAR = "HarmonyOS_Sans_SC_Regular.ttf"
TTF_MEDIUM = "HarmonyOS_Sans_SC_Medium.ttf"
TTF_BOLD = "HarmonyOS_Sans_SC_Bold.ttf"

FONT_PLAN = [
    # (名字,          ttf,          px, bpp, 字符集)
    ("num48",        TTF_MEDIUM,   48, 4, "numeric"),
    # 表盘主角：104px 大时间。数字 + 冒号而已，但必须锐利（4bpp 抗锯齿）
    ("num104",       TTF_BOLD,    104, 4, "numeric"),
    ("ascii16",      TTF_REGULAR,  16, 4, "ascii"),
    ("ascii20",      TTF_REGULAR,  20, 4, "ascii"),
    ("ascii20b",     TTF_BOLD,     20, 4, "ascii"),
    ("ascii28",      TTF_MEDIUM,   28, 4, "ascii"),
    ("ascii40",      TTF_MEDIUM,   40, 4, "numeric"),
    # UI 中文字库（精简字符集，见 UI_HANZI）
    ("ui12",         TTF_REGULAR,  12, 4, "ui"),
    ("ui14",         TTF_REGULAR,  14, 4, "ui"),
    ("ui16",         TTF_MEDIUM,   16, 4, "ui"),
    ("ui20",         TTF_MEDIUM,   20, 4, "ui"),
    ("ui28",         TTF_MEDIUM,   28, 4, "ui"),
]

# 全量中文字库：只有需要「任意文本」（比如 BLE 名片里的自由昵称）时才需要，
# 代价是 16px 1bpp 就 1 MB、24px 接近 2 MB。默认不生成，省编译时间也省 Flash。
#   set GEN_FULL_CJK=1 && python gen_font.py
if os.environ.get("GEN_FULL_CJK") == "1":
    FONT_PLAN += [
        ("cjk_16",   TTF_REGULAR,  16, 1, "cjk-common"),
        ("cjk_24",   TTF_REGULAR,  24, 1, "cjk-common"),
    ]


# --------------------------------------------------------------------------
# 预览
# --------------------------------------------------------------------------


def do_preview(ttf_dir, name, text, out_png):
    plan = {n: (t, px, bpp, cs) for (n, t, px, bpp, cs) in FONT_PLAN}
    if name == "font_hz20":
        name = "cjk_24"
    if name not in plan:
        raise SystemExit("未知字库 %s；可选：%s" % (name, ", ".join(plan)))
    ttf, px, bpp, cs = plan[name]
    font = ImageFont.truetype(os.path.join(ttf_dir, ttf), px)
    ascent, descent = font.getmetrics()
    W = int(font.getlength(text)) + 20
    H = ascent + descent + 20
    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    d.text((10, 10), text, font=font, fill=(255, 255, 255))
    img = img.resize((W * 2, H * 2), Image.NEAREST)
    img.save(out_png)
    print("preview ->", out_png, img.size)


# --------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf-dir", default=os.path.join(os.path.dirname(__file__), "ttf"))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "out"))
    ap.add_argument("--only", action="append", default=None)
    ap.add_argument("--header-name", default="fonts.h")
    ap.add_argument("--preview", nargs=2, metavar=("FONT", "TEXT"))
    ap.add_argument("--preview-out", default="preview.png")
    args = ap.parse_args()

    if args.preview:
        do_preview(args.ttf_dir, args.preview[0], args.preview[1], args.preview_out)
        return

    os.makedirs(args.out, exist_ok=True)
    print("输出目录:", os.path.abspath(args.out))

    built = []
    total_bitmap = 0
    for (name, ttf, px, bpp, cs) in FONT_PLAN:
        if args.only and name not in args.only:
            continue
        ttf_path = os.path.join(args.ttf_dir, ttf)
        if not os.path.isfile(ttf_path):
            print("!! 缺字体文件:", ttf_path)
            continue
        chars = resolve_charset(cs)
        print("  %-10s %2dpx %dbpp  charset=%-11s (%d 字符) ..."
              % (name, px, bpp, cs, len(chars)), end="", flush=True)
        f = build_font(ttf_path, px, bpp, chars, name)
        path, nbytes, nglyphs = write_font_c(args.out, f, args.header_name)
        total_bitmap += nbytes
        built.append(f)
        print(" %6.1f KB 位图 / %d 字形" % (nbytes / 1024.0, nglyphs))

    if built:
        # fonts.h 必须覆盖「计划里所有已经生成过的」字库，而不只是这一次生成的。
        # 否则 --only 增量生成会把别的字库从 fonts.h 里抹掉，一编译就是一堆未声明。
        listed = []
        for (name, ttf, px, bpp, cs) in FONT_PLAN:
            if not os.path.isfile(os.path.join(args.out, "font_%s.c" % name)):
                continue
            f = next((b for b in built if b["name"] == name), None)
            if f is None:
                f = {"name": name, "px": px, "bpp": bpp,
                     "chars": resolve_charset(cs)}
            listed.append(f)
        write_header(args.out, listed, args.header_name)
        print("合计位图 %.1f KB，本次生成 %d 个，fonts.h 收录 %d 个"
              % (total_bitmap / 1024.0, len(built), len(listed)))


if __name__ == "__main__":
    main()
