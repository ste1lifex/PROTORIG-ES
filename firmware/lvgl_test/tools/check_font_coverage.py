#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
check_font_coverage.py —— 验证「页面上每个字符都在字库里」

这是对「文字显示成方块」那个 bug 的离线验证：
  1. 从 EEZ 工程里把所有标签文字捞出来（页面上的字）
  2. 从 face_lvgl.c 的字符串字面量里捞表盘上的字
  3. 按 gen_lvgl_fonts.py 的同一套规则算出字符集
  4. 逐个核对 —— 有缺就直接报错退出

lv_font_conv 的契约是：--symbols 里给的字符一定进字库
（TTF 里没有的会报错）。所以「字符在集合里」==「字库里有」。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)                       # lvgl_test
FW = os.path.dirname(PROJ)                         # firmware

sys.path.insert(0, os.path.join(FW, "font"))
import gen_font  # noqa: E402


def chars_from_eez():
    import json

    p = os.path.join(FW, "eez_ui", "protorig_ui.eez-project")
    d = json.load(open(p, encoding="utf-8"))
    out = set()
    detail = []

    def walk(node, page):
        t = node.get("text")
        if isinstance(t, str) and t:
            out.update(t)
            detail.append((page, t))
        for c in node.get("children") or []:
            walk(c, page)

    for page in d.get("userPages", []):
        for root in page.get("components", []):
            walk(root, page["name"])
    return out, detail


def strip_comments(src):
    """先去 C 注释 —— 否则注释里带引号的中文（例如
    `给"用 LVGL 重画原来那个表盘"这件事一个答案`）会被当成字符串字面量误报"""
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def chars_from_face():
    src = open(os.path.join(PROJ, "face_lvgl.c"), encoding="utf-8").read()
    src = strip_comments(src)
    lits = re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', src)
    out = set()
    for s in lits:
        out.update(s)
    return out, lits


def staleness_check():
    """字库文件是不是比"会改到字符集的源"旧？

    ⚠️ 这个检查是真的踩过坑：改版式时把底部提示从「触摸屏幕返回表盘」改成
    「点两侧切换 · 点底部返回」，引入了 点/两/侧/切/换/底 六个新字。
    下面的覆盖检查是"按规则算"的（它认为生成器会包含这些字），
    所以**照样通过** —— 但磁盘上的字库还是旧的，屏上就是方块。
    只有比对时间戳才能发现"字库没重生成"。
    """
    import glob

    fonts = glob.glob(os.path.join(PROJ, "fonts_gen", "lv_font_*.c"))
    if not fonts:
        return None, "fonts_gen/lv_font_*.c 不存在"

    newest_font = max(os.path.getmtime(f) for f in fonts)
    sources = [
        os.path.join(FW, "eez_ui", "protorig_ui.eez-project"),
        os.path.join(PROJ, "face_lvgl.c"),
        os.path.join(FW, "font", "gen_font.py"),
    ]
    newer = [(p, os.path.getmtime(p)) for p in sources
             if os.path.exists(p) and os.path.getmtime(p) > newest_font]
    return newer, None


def main():
    page_chars, page_texts = chars_from_eez()
    face_chars, face_lits = chars_from_face()

    # gen_lvgl_fonts.py 用的是「UI_CHARS + EEZ 页面字符」，再加 ASCII(0x20-0x7F)
    supplied = set(gen_font.UI_CHARS) | page_chars
    supplied |= set(chr(i) for i in range(0x20, 0x80))
    supplied |= set("".join(gen_font.NUMERIC))
    supplied |= set("·")

    needed = page_chars | face_chars
    needed = set(c for c in needed if c not in "\r\n\t")

    missing = sorted(c for c in needed if c not in supplied)

    print("EEZ 页面标签   : %d 条，%d 个不同字符" % (len(page_texts), len(page_chars)))
    print("表盘字符串     : %d 条，%d 个不同字符" % (len(face_lits), len(face_chars)))
    print("需要覆盖       : %d 个字符" % len(needed))
    print("字库提供的字符 : %d 个（UI_CHARS %d + 页面 %d + ASCII）"
          % (len(supplied), len(gen_font.UI_CHARS), len(page_chars)))
    print()
    if missing:
        print("!! 缺 %d 个字符，屏上会是方块：" % len(missing))
        for ch in missing:
            where = [t for (pg, t) in page_texts if ch in t][:2]
            print("   %r U+%04X   <- %s" % (ch, ord(ch), " | ".join(where)))
        sys.exit(1)
    print("[OK] 全部覆盖 —— 三个页面 + 表盘的每个字符都在字库里，不会有方块")

    newer, err = staleness_check()
    if err:
        print("!! 字库过期检测失败: %s" % err)
        sys.exit(1)
    if newer:
        print()
        print("!! 字库比下面这些源文件旧 —— 说明改了字符集但**没重新生成字库**：")
        for p, _ in newer:
            print("     %s" % p)
        print("   跑： python lvgl_test/tools/gen_lvgl_fonts.py")
        sys.exit(1)
    print("[OK] 字库是最新的（不比任何会改字符集的源文件旧）")


if __name__ == "__main__":
    main()
