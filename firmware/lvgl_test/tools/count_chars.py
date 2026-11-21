#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""统计「表盘 + EEZ 页面」真正用到的字符，看能比 UI_CHARS 省多少"""
import json
import os
import re
import sys

FW = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(FW, "font"))
import gen_font  # noqa

# 1) face_lvgl.c 里的字符串字面量
src = open(os.path.join(FW, "lvgl_test", "face_lvgl.c"), encoding="utf-8").read()
lits = re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', src)
face = set()
for s in lits:
    face.update(s)

print("face_lvgl.c 字符串 %d 条，用到 %d 个字符" % (len(lits), len(face)))
print("  非 ASCII:", "".join(sorted(c for c in face if ord(c) > 0x7F)))

# 2) EEZ 页面文字
d = json.load(open(os.path.join(FW, "eez_ui", "protorig_ui.eez-project"), encoding="utf-8"))
page = set()


def walk(n):
    if isinstance(n.get("text"), str):
        page.update(n["text"])
    for c in n.get("children") or []:
        walk(c)


for p in d["userPages"]:
    for r in p["components"]:
        walk(r)

UI = set(gen_font.UI_CHARS)
CJK = lambda s: set(c for c in s if ord(c) > 0x7F)

need = CJK("".join(face) + "".join(page))
print()
print("EEZ 页面非 ASCII : %d" % len(CJK("".join(page))))
print("表盘非 ASCII     : %d" % len(CJK("".join(face))))
print("两者并集         : %d" % len(need))
print("UI_CHARS         : %d  -> 可省 %d 个" % (len(UI), len(UI - need)))
print()
print("实际需要的非 ASCII 全集 (%d):" % len(need))
print(" ", "".join(sorted(need)))
print()
print("UI_CHARS 里有、但实际没用到的 (%d):" % len(UI - need))
print(" ", "".join(sorted(UI - need)))
