#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Per-font CJK coverage for the Source Han generated LVGL fonts.

Scans every firmware_m2 .c/.h, strips comments, keeps string literals, and
checks each UI font. Reports missing font + source file + char. Exit 0 only
when missing = 0.
"""
import os, re, sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
GEN = os.path.join(PROJ, 'fonts_gen')

# 2026-11-08（APP 第一批）：去掉过期的“需”（它只在注释里出现过，字库生成器按
# 字符串字面量取字，注释里的字不会进字库）；补上秒表/计时器/闹钟页的固定文案。
REQUIRED_BY_FONT = {
    'lv_font_ui14': '管理员周一周二周三周四周五周六日',
    'lv_font_monitor20': '监控界面连接电脑USB',
    'lv_font_ui16': '设置显示连接提醒关于秒表计时器协议网络网络记录USB监控屏幕亮度自动息屏'
                    '闹钟暂停继续复位取消分钟小时每天重复开始时间到',
    'lv_font_ui20': '设置显示连接提醒关于秒表计时器协议网络网络记录USB监控屏幕亮度自动息屏'
                    '闹钟暂停继续复位取消分钟小时每天重复开始时间到',
    'lv_font_ui28': '设置显示连接提醒关于秒表计时器协议网络网络记录USB监控屏幕亮度自动息屏'
                    '闹钟暂停继续复位取消分钟小时每天重复开始时间到',
}
PAGE_FONT = {
    'face_lvgl.c': 'lv_font_ui14',
    'pc_monitor.c': 'lv_font_monitor20',
}
UI_FONTS = ['lv_font_ui14', 'lv_font_ui16', 'lv_font_ui20', 'lv_font_ui28', 'lv_font_monitor20']

def source_entries():
    skip = {'fonts_gen', 'build', 'build_out', '.git', 'sdk', 'sdk_patches'}
    entries = {}
    for root, dirs, files in os.walk(PROJ):
        dirs[:] = [d for d in dirs if d not in skip]
        for fn in files:
            if not fn.endswith(('.c', '.h')):
                continue
            p = os.path.join(root, fn)
            rel = os.path.relpath(p, PROJ)
            try:
                text = open(p, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
            text = re.sub(r'//[^\n]*', '', text)
            chars = set()
            for lit in re.findall(r'"([^"]*)"', text):
                chars.update(ch for ch in lit if (ord(ch) > 0x7F and ord(ch) not in (0xFE0E, 0xFE0F, 0x200D)) or ch in 'USB')
            if chars:
                entries[rel] = chars
    return entries

def font_codepoints(name):
    p = os.path.join(GEN, name + '.c')
    text = open(p, encoding='utf-8', errors='ignore').read()
    points = set()
    arrays = {}
    for m in re.finditer(r'static const uint16_t (unicode_list_\d+)\[\]\s*=\s*\{(.*?)\};', text, re.S):
        arrays[m.group(1)] = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]+)', m.group(2))]
    starts = [m.start() for m in re.finditer(r'\.range_start\s*=', text)]
    for idx, pos in enumerate(starts):
        block = text[pos:starts[idx + 1] if idx + 1 < len(starts) else len(text)]
        m = re.search(r'\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+)', block)
        if not m:
            continue
        start, length = int(m.group(1)), int(m.group(2))
        if '.unicode_list = NULL' in block:
            points.update(range(start, start + length))
            continue
        um = re.search(r'\.unicode_list\s*=\s*(unicode_list_\d+)', block)
        if um:
            points.update(start + off for off in arrays.get(um.group(1), []))
    return points

def main():
    entries = source_entries()
    need = set()
    for chars in entries.values():
        need |= chars
    for req in REQUIRED_BY_FONT.values():
        need |= set(req)
    bad = 0
    for f in UI_FONTS:
        covered = font_codepoints(f)
        missing = sorted(c for c in need if ord(c) not in covered)
        if f in REQUIRED_BY_FONT:
            missing += [c for c in REQUIRED_BY_FONT[f] if ord(c) not in covered]
            missing = sorted(set(missing))
        print('%-18s glyphs=%d missing=%d' % (f, len(covered), len(missing)))
        if not missing:
            continue
        bad += 1
        for c in missing:
            files = [rel for rel, chars in entries.items() if c in chars]
            print('  MISSING %-8s U+%04X %s  files=%s' % (f, ord(c), c, ','.join(files[:5])))
    # explicit page/font owner checks
    for fn, f in PAGE_FONT.items():
        chars = entries.get(fn, set())
        covered = font_codepoints(f)
        miss = sorted(c for c in chars if ord(c) not in covered)
        print('%-18s page=%s chars=%d missing=%d' % (f, fn, len(chars), len(miss)))
        if miss:
            bad += 1
            print('  PAGE MISSING %s: %s' % (fn, ''.join(miss)))
    if bad:
        return 1
    print('FONT COVERAGE OK: all %d source chars covered; per-page checks pass' % len(need))
    return 0

if __name__ == '__main__':
    sys.exit(main())
