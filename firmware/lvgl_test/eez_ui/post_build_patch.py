#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
post_build_patch.py —— EEZ Build 之后自动做的三件事

**每改完版面、在 EEZ 里 Build 一次之后，就跑一遍这个脚本。**

1) 修 include
   生成的代码写的是 `#include <lvgl/lvgl.h>`，但我们工程里 lvgl.h 直接在搜索路径根上
   （SDK 的目录叫 lvgl_v9，不是 lvgl），所以改成 `#include "lvgl.h"`。

2) 把字体引用换成我们自己的
   EEZ 想自己生成字体（走内置 lv_font_conv 子进程），但在打包版里这一步会静默失败，
   生成不出 `.c`。而我们本来就有同款字体（同一份 HarmonyOS Sans TTF、同字号、
   同 198 个中文字符，见 lvgl_test/fonts_gen/，由 tools/gen_lvgl_fonts.py 生成）：
       ui_font_ui14/16/20/28  ->  lv_font_ui14/16/20/28
   顺带省掉一份重复的字库 flash。

3) 填充样式函数
   EEZ 生成的 styles.c 里，每个样式都是一个**空函数**：
       void add_style_page_title(lv_obj_t *obj) { (void)obj; };
   而 screens.c 已经在正确的地方调用它们（add_style_row_dim(...) 等）。
   这里把这些空函数填成我们的配色/字体 —— 也就是把「外观」这一层接管过来，
   版面仍然完全由 EEZ 的工程文件决定。
"""
import os
import re
import sys
import json

HERE = os.path.dirname(os.path.abspath(__file__))
UI = os.path.join(HERE, "src", "ui")

# ------------------------------------------------------------------ 我们的配色
GOLD = "0xF2ED65"
GOLD_HI = "0xFFE27A"
GOLD_DIM = "0x90958D"
WARM = "0xECEEE5"
WARN = "0xFF6C30"
BLACK = "0x000000"
CARD_BG = "0x141713"

# 样式名 -> C 代码（对应 styles.c 里的 add_style_<name>）
STYLE_IMPL = {
    "screen_bg": """
    lv_obj_set_style_bg_color(obj, lv_color_hex(%s), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);""" % BLACK,

    "page_title": """
    lv_obj_set_style_text_color(obj, lv_color_hex(%s), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui28, 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);""" % GOLD,

    "page_sub": """
    lv_obj_set_style_text_color(obj, lv_color_hex(%s), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui14, 0);""" % GOLD_DIM,

    "row_main": """
    lv_obj_set_style_text_color(obj, lv_color_hex(%s), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui20, 0);""" % WARM,

    "row_dim": """
    lv_obj_set_style_text_color(obj, lv_color_hex(%s), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui14, 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);""" % GOLD_DIM,

    "row_gold": """
    lv_obj_set_style_text_color(obj, lv_color_hex(%s), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui20, 0);""" % GOLD,

    "big_num": """
    lv_obj_set_style_text_color(obj, lv_color_hex(%s), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui48, 0);""" % WARM,

    "warn_text": """
    lv_obj_set_style_text_color(obj, lv_color_hex(%s), 0);
    lv_obj_set_style_text_font(obj, &lv_font_ui14, 0);""" % WARN,

    "card": """
    lv_obj_set_style_bg_color(obj, lv_color_hex(%s), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(%s), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);""" % (CARD_BG, GOLD_DIM),

    "accent": """
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xF2ED65), 0);
    lv_obj_set_style_bg_opa(obj, 255, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);""",

    "hline": """
    lv_obj_set_style_bg_color(obj, lv_color_hex(%s), 0);
    lv_obj_set_style_bg_opa(obj, 120, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);""" % GOLD_DIM,
}


def patch_includes(path):
    txt = open(path, encoding="utf-8").read()
    n1 = txt.count("#include <lvgl/lvgl.h>")
    txt = txt.replace("#include <lvgl/lvgl.h>", '#include "lvgl.h"')
    n2 = txt.count("ui_font_")
    txt = re.sub(r"\bui_font_(ui\d+)", r"lv_font_\1", txt)
    if n1 or n2:
        open(path, "w", encoding="utf-8", newline="\n").write(txt)
    return n1, n2


def patch_styles(path):
    txt = open(path, encoding="utf-8").read()
    filled = []
    for name, body in STYLE_IMPL.items():
        # 匹配 EEZ 生成的空函数体
        pat = re.compile(
            r"(void add_style_%s\(lv_obj_t \*obj\) \{).*?(\};)" % re.escape(name), re.S
        )
        m = pat.search(txt)
        if m:
            txt = pat.sub(lambda mm: mm.group(1) + body + "\n" + mm.group(2), txt, count=1)
            filled.append(name)
    open(path, "w", encoding="utf-8", newline="\n").write(txt)
    return filled


# ---------------------------------------------------------------- 标识符修复
# EEZ 用「页面名」派生 C 标识符，而我们的页面名是中文 → 被剥光，于是生成
#     SCREEN_ID_____ = 1,  SCREEN_ID_____ = 2,  SCREEN_ID___ = 3
# 这种**重名**的东西，直接编不过（enum 重名、struct 成员重名、函数重定义）。
#
# 页面顺序就是 EEZ 里 userPages 的顺序，所以这里按**出现顺序**做重命名：
# 第 1/2/3 个 create_screenXXX 依次变成 network/meets/settings，其余同理。
SCREEN_NAMES = ["network", "meets", "settings"]


def seq_sub(text, pattern, fmt):
    """按出现顺序，把第 k 次匹配套用 SCREEN_NAMES[k % 3]"""
    idx = [0]

    def rep(m):
        name = SCREEN_NAMES[idx[0] % len(SCREEN_NAMES)]
        idx[0] += 1
        return fmt.format(name)

    new = re.sub(pattern, rep, text)
    return new, idx[0]


def fix_tick_names(text):
    """tick_screen 的三类出现位置，映射规则各不相同：

       1) 函数定义   void tick_screen_XXX() { ... }
                     -> 按 create_screen 定义的**出现顺序**取名字
       2) 函数调用       tick_screen_XXX();          （在 create_screen_XXX 体内）
                     -> 取**所在 create_screen 函数**的名字
       3) 函数指针数组    tick_screen_XXX,           （tick_screen_funcs[]）
                     -> 按顺序取 network/meets/settings

    之前按"出现顺序 % 3"一把梭是错的 —— 因为调用出现在定义之前，
    顺序实际是 [network, network, meets, meets, settings, settings, ...]。
    """
    names = ["network", "meets", "settings"]
    create_order = re.findall(r"void create_screen_(\w+)\(\)", text)
    out = []
    cur = None
    di = ai = 0

    for line in text.split("\n"):
        m = re.search(r"void create_screen_(\w+)\(\)", line)
        if m:
            cur = m.group(1)

        if re.search(r"void tick_screen_+(?=\()", line):          # 定义
            name = create_order[di] if di < len(create_order) else "screen%d" % di
            di += 1
            line = re.sub(r"tick_screen_+", "tick_screen_" + name, line)
        elif re.search(r"tick_screen_+\(\s*\);", line):           # create 里的调用
            line = re.sub(r"tick_screen_+", "tick_screen_" + (cur or "screen0"), line)
        elif re.search(r"tick_screen_+\s*,", line):               # 指针数组
            line = re.sub(r"tick_screen_+", "tick_screen_" + names[ai % 3], line)
            ai += 1
        out.append(line)

    return "\n".join(out)


def fix_identifiers():
    total = {}

    # screens.c：函数定义/引用、objects 成员赋值
    p = os.path.join(UI, "screens.c")
    t = open(p, encoding="utf-8").read()
    t, n1 = seq_sub(t, r"create_screen_+(?=\()", "create_screen_{0}")
    t = fix_tick_names(t)
    t, n3 = seq_sub(t, r"objects\._+(?= =)", "objects.{0}")
    open(p, "w", encoding="utf-8", newline="\n").write(t)
    total["screens.c"] = (n1, "tick", n3)

    # screens.h：枚举、结构体成员、函数原型
    p = os.path.join(UI, "screens.h")
    t = open(p, encoding="utf-8").read()
    t, h1 = seq_sub(t, r"SCREEN_ID_+(?= =)", "SCREEN_ID_{0}".replace("{0}", "{0}"))
    t, h2 = seq_sub(t, r"lv_obj_t \*_+(?=;)", "lv_obj_t *{0}")
    t, h3 = seq_sub(t, r"create_screen_+(?=\()", "create_screen_{0}")
    t, h4 = seq_sub(t, r"tick_screen_+(?![A-Za-z0-9])", "tick_screen_{0}")
    open(p, "w", encoding="utf-8", newline="\n").write(t)
    total["screens.h"] = (h1, h2, h3, h4)

    # ui.c：loadScreen(SCREEN_ID_xxx) —— 只有一个（开机默认页）
    p = os.path.join(UI, "ui.c")
    t = open(p, encoding="utf-8").read()
    t, u1 = seq_sub(t, r"SCREEN_ID_+(?=\))", "SCREEN_ID_{0}")
    open(p, "w", encoding="utf-8", newline="\n").write(t)
    total["ui.c"] = (u1,)

    # 枚举名要全大写
    def upper_enum(m):
        return "SCREEN_ID_" + m.group(1).upper()

    p = os.path.join(UI, "screens.h")
    t = open(p, encoding="utf-8").read()
    t = re.sub(r"SCREEN_ID_(network|meets|settings)", upper_enum, t)
    open(p, "w", encoding="utf-8", newline="\n").write(t)

    p = os.path.join(UI, "ui.c")
    t = open(p, encoding="utf-8").read()
    t = re.sub(r"SCREEN_ID_(network|meets|settings)", upper_enum, t)
    open(p, "w", encoding="utf-8", newline="\n").write(t)

    return total


def normalize_screen_members():
    """把三个 screen 对象在 objects 结构体里的成员名统一成 scr_<page>

    为什么需要：胶水代码（eez_ui_glue.c）要往三个 screen 上挂手势层，
    需要一个**稳定**的名字。而 EEZ 用页面/控件名派生 C 标识符，名字会随
    工程变化（中文被剥光时是 `____`，填了 identifier 之后又变成
    `network_scr00`…），胶水就会编译不过。

    识别方式：screens.c 里每个 create_screen_<name>() 函数体里的
    第一条 `objects.<member> = obj;` 就是这个 screen 自己。
    （loadScreen / tick_screen 走的是数组下标，不依赖成员名，所以只需改
     screens.h 的结构体声明 + screens.c 的赋值这两处。）
    """
    p = os.path.join(UI, "screens.c")
    ph = os.path.join(UI, "screens.h")
    t = open(p, encoding="utf-8").read()
    th = open(ph, encoding="utf-8").read()

    pairs = []
    for m in re.finditer(r"void create_screen_(\w+)\(\)\s*\{", t):
        name = m.group(1)
        seg = t[m.end():m.end() + 3000]
        mm = re.search(r"objects\.(\w+) = obj;", seg)
        if mm:
            pairs.append((name, mm.group(1)))

    renamed = 0
    names = []
    for name, old in pairs:
        new = "scr_" + name
        names.append(new)
        if old == new:
            continue
        t = t.replace("objects.%s = obj;" % old, "objects.%s = obj;" % new)
        th = th.replace("lv_obj_t *%s;" % old, "lv_obj_t *%s;" % new)
        renamed += 1

    open(p, "w", encoding="utf-8", newline="\n").write(t)
    open(ph, "w", encoding="utf-8", newline="\n").write(th)
    return renamed, names


def patch_style_bindings():
    """EEZ can fall back to the widget default, ignoring serialized useStyle.
    Restore the project's explicit bindings by stable identifier, not position.
    """
    project = json.load(open(os.path.join(HERE, "protorig_ui.eez-project"), encoding="utf-8"))
    bindings = {}
    centers = {}
    def walk(node, screen_name=None):
        ident = "scr_" + screen_name if screen_name else node["identifier"]
        style = node["style"]["useStyle"]
        if "terminalCenterX" in node:
            centers[ident] = node["terminalCenterX"]
        bindings[ident] = re.sub(r"(?<!^)(?=[A-Z])", "_", style).lower()
        if style == "HLine":
            bindings[ident] = "hline"
        for child in node.get("children", []):
            walk(child)
    for page in project["userPages"]:
        walk(page["components"][0], page["codeIdentifier"])
    path = os.path.join(UI, "screens.c")
    text = open(path, encoding="utf-8").read()
    for ident, style in bindings.items():
        pattern = r"(objects\." + re.escape(ident) + r" = obj;(?:(?!objects\.).)*?)add_style_\w+\(obj\);"
        text, n = re.subn(pattern, lambda m: m[1] + "add_style_" + style + "(obj);", text, count=1, flags=re.S)
        if n != 1:
            raise RuntimeError("Style binding missing: " + ident)
    for ident, cx in centers.items():
        pattern = r"(objects\." + re.escape(ident) + r" = obj;\s*)lv_obj_set_pos\(obj, [^;]+;\s*lv_obj_set_size\(obj, [^;]+;"
        def center(m):
            old = m[0]
            y = int(re.search(r"lv_obj_set_pos\(obj, -?\d+, (-?\d+)\)", old)[1])
            return m[1] + f"lv_obj_set_pos(obj, {cx-160}, {y});\n            lv_obj_set_size(obj, 320, LV_SIZE_CONTENT);\n            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);"
        # Remove previous alignment line to keep the patch idempotent.
        text = re.sub(pattern, center, text)
    text = re.sub(r"(lv_obj_set_style_text_align\(obj, LV_TEXT_ALIGN_CENTER, 0\);\s*){2,}",
                  "lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);\n            ", text)
    open(path, "w", encoding="utf-8", newline="\n").write(text)


def fix_copy():
    """R3b：对外统一「协议网络记录」文案（相遇记录只作内部别名）。
    EEZ 工程源已同步改；这里再兜底一次，防止旧工程重新 Build 把文案覆盖回去。"""
    p = os.path.join(UI, "screens.c")
    if not os.path.isfile(p):
        return 0
    txt = open(p, encoding="utf-8").read()
    n = txt.count("相遇记录")
    if n:
        txt = txt.replace("相遇记录", "协议网络记录")
        open(p, "w", encoding="utf-8", newline="\n").write(txt)
    return n

def main():
    if not os.path.isdir(UI):
        sys.exit("找不到 %s —— 先在 EEZ Studio 里 Build 一次" % UI)

    total_i = total_f = 0
    for fn in sorted(os.listdir(UI)):
        if not fn.endswith((".c", ".h")):
            continue
        p = os.path.join(UI, fn)
        a, b = patch_includes(p)
        total_i += a
        total_f += b
        if a or b:
            print("  %-12s include修复 %d  字体引用替换 %d" % (fn, a, b))

    ids = fix_identifiers()
    n_scr, scr_names = normalize_screen_members()
    filled = patch_styles(os.path.join(UI, "styles.c"))
    patch_style_bindings()

    print()
    print("include 修复      : %d 处" % total_i)
    print("字体引用替换      : %d 处  (ui_font_* -> lv_font_*)" % total_f)
    print("标识符修复        : %s" % ", ".join(
        "%s(%s)" % (k, "/".join(str(x) for x in v)) for k, v in ids.items()))
    print("screen 成员名统一 : %d 个改名 -> %s" % (n_scr, ", ".join(scr_names)))
    print("填充样式函数      : %d/%d  %s" % (len(filled), len(STYLE_IMPL), ", ".join(filled)))
    print("文案统一          : %d 处  (相遇记录 -> 协议网络记录)" % n_copy)
    missing = set(STYLE_IMPL) - set(filled)
    if missing:
        print("!! 没匹配到的样式  :", ", ".join(sorted(missing)))


if __name__ == "__main__":
    main()
