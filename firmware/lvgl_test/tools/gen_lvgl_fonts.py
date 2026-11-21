#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_lvgl_fonts.py —— 把原工程的 TTF + 字符集，用官方 lv_font_conv 生成 LVGL 字库

背景：
  V1.4 的表盘自己写了一套 TTF -> 点阵 C 数组 -> 运行时查表/UTF-8 解码/绘制
  （firmware/font/font.c）。那套东西能用，但本质是在重新实现一个字体引擎。
  LVGL 自带字体引擎（含抗锯齿、字距、中文），只需要把 TTF 转成它的格式。
  官方转换器就是 lv_font_conv（node 包，npx 直接跑）。

这里刻意【复用原工程的字符集定义】（gen_font.py 的 ASCII / NUMERIC / UI_CHARS），
所以出来的字库和原表盘是同一套字符覆盖，视觉效果可以对齐。

输出：firmware/lvgl_test/fonts_gen/lv_font_*.c
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # .../firmware/lvgl_test/tools
FW = os.path.dirname(os.path.dirname(HERE))                 # .../firmware
TTF_DIR = os.environ.get("PROTORIG_FONT_DIR", os.path.join(FW, "font", "ttf"))
OUT_DIR = os.path.join(os.path.dirname(HERE), "fonts_gen")

# 复用原工程的字符集
sys.path.insert(0, os.path.join(FW, "font"))
import gen_font  # noqa: E402

TTF_BOLD = "HarmonyOS_Sans_SC_Bold.ttf"
TTF_MEDIUM = "HarmonyOS_Sans_SC_Medium.ttf"
TTF_REGULAR = "HarmonyOS_Sans_SC_Regular.ttf"

# 表盘上真正用到的中文（原 UI_CHARS 是给整个 UI 用的，这里全带上）
UI_CHARS = "".join(gen_font.UI_CHARS)
NUMERIC = "".join(gen_font.NUMERIC)
DOT = "·"


def eez_page_chars():
    """把 EEZ 工程里所有标签文字用到的字符捞出来

    页面文字是在 EEZ Studio 里写的，很容易用到 UI_CHARS 里没有的字
    （比如「触摸屏幕返回表盘」的 触/摸/屏/幕/返/回/表/盘）。
    不并进来的话，屏上就是一个个方块。
    以后在 EEZ 里加页面/改文字，**重跑一次这个脚本**就自动覆盖了。
    """
    import json

    p = os.path.join(FW, "eez_ui", "protorig_ui.eez-project")
    if not os.path.exists(p):
        print("[warn] 找不到 %s，跳过 EEZ 页面字符" % p)
        return set()

    d = json.load(open(p, encoding="utf-8"))
    out = set()

    def walk(node):
        if isinstance(node.get("text"), str):
            out.update(node["text"])
        for ch in node.get("children") or []:
            walk(ch)

    for page in d.get("userPages", []):
        for root in page.get("components", []):
            walk(root)

    return out


_eez = eez_page_chars()
_extra = _eez - set(UI_CHARS)
PAGE_CHARS = "表盘网络相遇设置" + UI_CHARS + "".join(sorted(_extra))

# (LVGL 字库名, TTF, px, 额外符号)
PLAN = [
    ("lv_font_monitor20", TTF_MEDIUM, 20, "监控界面需连接电脑"),
    ("lv_font_ui28", TTF_MEDIUM, 28, DOT + PAGE_CHARS),
    ("lv_font_ui48", TTF_BOLD, 48, NUMERIC),
    ("lv_font_num104", TTF_BOLD,    104, NUMERIC),                    # 大时间（原 num104）
    ("lv_font_ascii28", TTF_MEDIUM,  28, NUMERIC),                    # 秒
    ("lv_font_ui20",   TTF_MEDIUM,   20, NUMERIC + DOT + PAGE_CHARS),  # 日期 + 页面标题
    ("lv_font_ui16",   TTF_MEDIUM,   16, DOT + PAGE_CHARS),           # 标题 / 管理员编号 / 页面正文
    ("lv_font_ui14",   TTF_REGULAR,  14, DOT + PAGE_CHARS),           # 标签 / 周几 / 页面小字
]


def run_one(name, ttf, px, extra):
    out = os.path.join(OUT_DIR, name + ".c")
    cmd = [
        "npx", "--yes", "lv_font_conv",
        "--font", os.path.join(TTF_DIR, ttf),
        "--size", str(px),
        "--bpp", "4",
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "--lv-font-name", name,
        "--no-compress",
        "-r", "0x20-0x7F",
    ]
    if extra:
        cmd += ["--symbols", extra]
    cmd += ["-o", out]

    print("[gen] %-16s px=%-4d <- %s" % (name, px, ttf))
    r = subprocess.run(cmd, capture_output=True, text=True, shell=True)
    if r.returncode != 0:
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        raise SystemExit("lv_font_conv 失败: " + name)
    size = os.path.getsize(out)
    print("      -> %s  (%.1f KB)" % (os.path.basename(out), size / 1024.0))
    return out


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    print("TTF   :", TTF_DIR)
    print("输出  :", OUT_DIR)
    print("中文字符数:", len(UI_CHARS), " + EEZ 页面新增", len(_extra), "=", len(PAGE_CHARS))
    if _extra:
        print("新增:", "".join(sorted(_extra)))
    print()
    only = sys.argv[sys.argv.index('--only')+1] if '--only' in sys.argv else None
    for name, ttf, px, extra in PLAN:
        if only and name != only:
            continue
        run_one(name, ttf, px, extra)
    print("\n全部完成。把 fonts_gen/*.c 加进 CMakeLists 即可。")


if __name__ == "__main__":
    main()
