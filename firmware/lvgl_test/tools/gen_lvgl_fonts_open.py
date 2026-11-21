#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Regenerate LVGL fonts from Source Han Sans SC (SIL OFL).

Sources: fonts_src/SourceHanSansSC-{Regular,Medium,Bold}.otf
Output:  fonts_gen/lv_font_*.c, same names/sizes/bpp=4 as before.

Char set:
  - all non-ASCII chars found in eez_ui/src, apps/, ui/, main.c
  - the existing per-font --symbols set (parsed from the old generated header)
  - ASCII 0x20..0x7F + the CJK punctuation list required by the design
Large numeric fonts (ui48/ascii28/num104) intentionally keep only their
existing numeric/ASCII set; UI text sizes get the full CJK set.
"""
import os, re, subprocess, sys, glob, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
GEN = os.path.join(PROJ, 'fonts_gen')
SRC = os.environ.get('PROTORIG_FONT_DIR', os.path.join(PROJ, 'fonts_src'))

PUNCT = '，。！？：；、（）《》【】·—…“”‘’→←↑↓℃±×÷≈≤≥√°¥$€'

# (output name, weight OTF, px, full_cjk)
PLAN = [
    ('lv_font_ui14',      'Regular', 14,  True),
    ('lv_font_ui16',      'Medium',  16,  True),
    ('lv_font_ui20',      'Medium',  20,  True),
    ('lv_font_ui28',      'Medium',  28,  True),
    # 卡片固定文案专用 25px 字库（v4 的标题/按钮是 24~25px，20/28 都不合适）。
    # 只收 BLE 卡片里**固定**的字符串（任意标题/题目仍走 1-bit 位图），
    # 所以只有 ~120 个字，flash 代价 ~50 KB。
    ('lv_font_ui25',      'Medium',  25,  'card'),
    ('lv_font_ui48',      'Bold',    48,  False),
    ('lv_font_ascii28',   'Medium',  28,  False),
    ('lv_font_monitor20', 'Medium',  20,  True),
    ('lv_font_num104',    'Bold',   104,  False),
]

# BLE 卡片（pc_monitor.c）里所有**固定**文案用到的字符。新增文案时要同步这里，
# 否则屏上会出现 '?'（tools/card_test.py 的 fallback 用例能看出来）。
CARD_CHARS = (
    'DSH运行中等待操作本轮完成失败任务审批确认电脑请求执行秒转到仅允许本次拒绝一次下一项提交'
    '已选还有个回到第页问答单多题目命令详情请在查看连接中断探索未至之境电量星期一二三四五六日'
    '0123456789%·:/.()+-'
)

def source_files():
    out = []
    skip = {'fonts_gen', 'build', 'build_out', '.git', 'sdk', 'sdk_patches'}
    for root, dirs, files in os.walk(PROJ):
        dirs[:] = [d for d in dirs if d not in skip]
        for fn in files:
            if fn.endswith(('.c', '.h')):
                out.append(os.path.join(root, fn))
    return out

def source_chars():
    chars = set()
    for p in source_files():
        try:
            text = open(p, encoding='utf-8', errors='ignore').read()
        except OSError:
            continue
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//.*", "", text)
        for lit in re.findall(r'"([^"]*)"', text):
            chars.update(ch for ch in lit if ord(ch) > 0x7F and ord(ch) not in (0xFE0E, 0xFE0F, 0x200D))
    chars.update(PUNCT)
    chars.update(chr(c) for c in range(0x20, 0x80))
    return chars

def old_symbols(name):
    p = os.path.join(GEN, name + '.c')
    try:
        first = open(p, encoding='utf-8', errors='ignore').readline()
    except OSError:
        return set()
    m = re.search(r'--symbols\s+(.*?)\s+-o\s', first)
    return set(m.group(1)) if m else set()

def run(name, weight, px, full):
    otf = os.path.join(SRC, 'SourceHanSansSC-%s.otf' % weight)
    out = os.path.join(GEN, name + '.c')
    if full == 'card':
        chars = set(CARD_CHARS)
    else:
        chars = old_symbols(name)
        if full:
            chars |= source_chars()
        else:
            chars |= set('0123456789:./-+%° ')
    if full == 'card':
        pass
    else:
        pass
    symbols = ''.join(sorted(chars))
    cmd = ['npx', '--yes', 'lv_font_conv@1.5.3',
           '--font', otf, '--size', str(px), '--bpp', '4',
           '--format', 'lvgl', '--lv-include', 'lvgl.h',
           '--lv-font-name', name, '--no-compress',
           '-r', '0x20-0x7F', '--symbols', symbols, '-o', out]
    print('[gen] %-16s %2dpx %-6s chars=%d' % (name, px, weight, len(symbols)))
    cand = glob.glob(os.path.expandvars('%LOCALAPPDATA%/npm-cache/_npx/*/node_modules/lv_font_conv/lv_font_conv.js'))
    if cand:
        cmd = [shutil.which('node') or 'node', cand[0]] + cmd[3:]
        r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8')
    else:
        r = subprocess.run(subprocess.list2cmdline(cmd), shell=True, capture_output=True, text=True, encoding='utf-8')
    if r.returncode != 0:
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        raise SystemExit('lv_font_conv failed: ' + name)
    print('      %s %.1f KB' % (os.path.basename(out), os.path.getsize(out) / 1024.0))

def main():
    if not all(os.path.exists(os.path.join(SRC, 'SourceHanSansSC-%s.otf' % w))
               for w in ('Regular', 'Medium', 'Bold')):
        raise SystemExit('missing OTF under fonts_src/')
    for name, weight, px, full in PLAN:
        run(name, weight, px, full)

if __name__ == '__main__':
    main()
