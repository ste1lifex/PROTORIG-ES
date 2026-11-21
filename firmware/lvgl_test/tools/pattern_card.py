"""生成 docs/bug_thin_stroke_smearing/pattern_expected.png —— 给现场判读用的一页纸。

把"真机上应该长什么样"连同每条带的判读要点画在一起，
用户拍照后在手机上就能对着比，不需要再去读文档或代码。

用法：
    python lvgl_test/tools/pattern_card.py
"""
import ctypes
import os
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / 'tools' / 'host'
OUT = ROOT / 'tools' / 'host_output'
DOCS = ROOT.parent / 'docs' / 'bug_thin_stroke_smearing'
OUT.mkdir(exist_ok=True)
DOCS.mkdir(exist_ok=True)

W = 480
PATTERN_H = 192
Y_WHITE, Y_CHECK_A, Y_CHECK_B = 12, 36, 60
Y_RG_ROWS, Y_LINES, Y_RAMP, Y_PAIRS = 84, 100, 148, 164


def render():
    dll = OUT / 'pattern_dll.dll'
    cc = os.environ.get('HOST_CC', 'C:/msys64/mingw64/bin/gcc.exe')
    if not dll.exists() or dll.stat().st_mtime < (ROOT / 'st7802_pattern.h').stat().st_mtime:
        subprocess.run([cc, '-O2', '-std=c99', '-shared', '-I', str(ROOT),
                        str(HOST / 'pattern_dll.c'), '-o', str(dll)], check=True)
    lib = ctypes.CDLL(str(dll))
    lib.st7802_pattern_row.restype = ctypes.c_int
    lib.st7802_pattern_row.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint16)]
    buf = (ctypes.c_uint16 * W)()
    rows = []
    for y in range(PATTERN_H):
        lib.st7802_pattern_row(y, buf)
        rows.append(list(buf))

    def to_rgb(v):
        return ((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)

    img = Image.new('RGB', (W, PATTERN_H))
    img.putdata([to_rgb(v) for row in rows for v in row])
    return img


def font(size):
    for p in (r'C:\Windows\Fonts\msyh.ttc', r'C:\Windows\Fonts\simhei.ttf',
              r'C:\Windows\Fonts\msyh.ttf'):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                continue
    return ImageFont.load_default()


def main():
    img = render()
    scale = 3                       # 图案放大 3 倍，1px 细节在手机上也能看清
    gw, gh = W * scale, PATTERN_H * scale

    pad = 20
    label_w = 660                   # 左侧文字栏
    head = 100
    grid_x = pad * 2 + label_w      # 图案左上角
    grid_y = head

    sw = grid_x + gw + pad
    sh = grid_y + gh + pad
    sheet = Image.new('RGB', (sw, sh), '#14161a')
    d = ImageDraw.Draw(sheet)
    d.text((pad, 20), '真机自证图案 —— 应该长这样', font=font(38), fill='#f2f4ef')
    d.text((pad, 66), '红白/绿白格子必须细到"一根一根数得清"；整屏静止、不闪、不抖',
           font=font(21), fill='#9aa3ad')
    sheet.paste(img.resize((gw, gh), Image.NEAREST), (grid_x, grid_y))

    bands = [
        (0, Y_WHITE, '① 纯白', '对照组。它也坏 => 整条链路崩了（先查时钟/供电）'),
        (Y_WHITE, Y_CHECK_A, '② 红白棋盘格', '该像细密织物；塌成 2px 粗条 => 成对置换'),
        (Y_CHECK_A, Y_CHECK_B, '③ 绿白棋盘格', '与②只差 1 像素相位；只坏一条 => 与字边界对齐有关'),
        (Y_CHECK_B, Y_RG_ROWS, '④ 红绿横条', '红绿位置互换 => 色序 / 半字序'),
        (Y_RG_ROWS, Y_LINES, '⑤ 单像素竖线', '缺 / 多 / 错位 => 丢像素、列偏移（分奇偶两组）'),
        (Y_LINES, Y_RAMP, '⑥ 满宽灰阶', '必须由黑到白单调；乱序或重复 => 像素内位序错'),
        (Y_RAMP, Y_PAIRS, '⑦ 红绿线对', '某一档开始串色 => 同字两像素之间出问题'),
    ]

    f_lab, f_note = font(27), font(20)
    # 文字栏内均匀分布，再用折线连到对应色带的竖直中心 —— 避免不同色带
    # 的行高不同导致文字互相重叠（第一版就是直接按行高排，全叠在一起了）。
    row_h = (gh - 40) // len(bands)
    for i, (y0, y1, name, note) in enumerate(bands):
        ly = grid_y + 24 + i * row_h
        by = grid_y + (y0 + y1) * scale // 2
        d.text((pad, ly - 30), name, font=f_lab, fill='#ffd54a')
        d.text((pad, ly + 4), note, font=f_note, fill='#cfd6de')
        # 折线：文字右侧 -> 图案左缘
        x1 = pad + label_w - 40
        d.line([(x1, ly - 4), (grid_x - 14, ly - 4), (grid_x - 14, by), (grid_x - 2, by)],
               fill='#5b6472', width=2)

    out = DOCS / 'pattern_expected.png'
    sheet.save(out)
    print('wrote', out)


if __name__ == '__main__':
    main()
