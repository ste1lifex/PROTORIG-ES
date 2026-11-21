#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""face_panel_install.py —— 默认表盘下半区面板（face 0，480x232 I8）安装/清除

契约：research/76 §2.1 / 77。
  panel blob = 1024 B palette (B,G,R,A) + 480*232 索引，共 112,384 B
  shell: facepanel [status|info] | erase | clear | begin <len> <crc32> |
         w <off> <hex> (<=48 B/行) | end

用法：
  python -X utf8 tools/face_panel_install.py demo  out.png
  python -X utf8 tools/face_panel_install.py pack  in.png out.pfp
  python -X utf8 tools/face_panel_install.py upload out.pfp --port COM8
  python -X utf8 tools/face_panel_install.py clear --port COM8
  python -X utf8 tools/face_panel_install.py info  --port COM8
"""
from __future__ import annotations

import argparse
import binascii
import os
import sys
import time

W, H = 480, 232
PAL_N, PAL_BYTES = 256, 1024
IMG_LEN = PAL_BYTES + W * H
CHUNK = 48
SHELL_LINE_MAX = 120
PFH_LEN = 24

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from face_install import ShellPort, find_port  # noqa: E402


def quantize(img):
    """RGB -> (palette 1024, indices 480*232)。palette 内存序 B,G,R,A。"""
    from PIL import Image, ImageOps

    fitted = ImageOps.fit(img.convert("RGB"), (W, H), method=3, centering=(0.5, 0.5))
    q = fitted.quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                        dither=Image.Dither.FLOYDSTEINBERG)
    if q.mode != "P":
        raise ValueError("quantize 没有返回 P 模式")
    pal = q.getpalette() or []
    if len(pal) < 768:
        pal = list(pal) + [0] * (768 - len(pal))
    palb = bytearray(PAL_BYTES)
    for i in range(PAL_N):
        r, g, b = pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]
        palb[4 * i + 0] = b
        palb[4 * i + 1] = g
        palb[4 * i + 2] = r
        palb[4 * i + 3] = 0xFF
    idx = q.tobytes()
    if len(idx) != W * H:
        raise ValueError("索引长度 %u != %u" % (len(idx), W * H))
    return bytes(palb), idx


def pack_panel(png_path, out_path):
    from PIL import Image

    img = Image.open(png_path)
    palb, idx = quantize(img)
    blob = palb + idx
    crc = binascii.crc32(blob) & 0xFFFFFFFF
    with open(out_path, "wb") as f:
        f.write(blob)
    print("pack OK: %s" % out_path)
    print("  size  = %u B (palette %u + pixels %u)" % (len(blob), PAL_BYTES, W * H))
    print("  crc32 = 0x%08X" % crc)
    return crc


def cmd_demo(args):
    from PIL import Image, ImageDraw, ImageFont

    img = Image.new("RGB", (W, H), (8, 10, 14))
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / max(1, H - 1)
        d.line([(0, y), (W, y)], fill=(int(10 + 40 * t), int(20 + 90 * t), int(30 + 140 * t)))
    for i in range(24):
        x = int(i * W / 24)
        d.line([(x, 0), (x, H)], fill=(20, 30, 40))
    # 顶部渐隐，和默认表盘的过渡接近
    for y in range(36):
        a = y / 35.0
        for x in range(W):
            r, g, b = img.getpixel((x, y))
            d.point((x, y), (int(r * a), int(g * a), int(b * a)))
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 34)
        small = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 16)
    except OSError:
        font = small = None
    d.text((24, 70), "PROTORIG ES", fill=(236, 238, 229), font=font)
    d.text((24, 118), "LOWER PANEL 480x232  I8", fill=(120, 220, 200), font=small)
    d.text((24, 146), "installed over USB / facepanel", fill=(150, 160, 170), font=small)
    d.text((24, 188), "time+hands stay live on top", fill=(150, 160, 170), font=small)
    img.save(args.png)
    print("demo -> %s" % args.png)


def open_shell(args):
    port = args.port or find_port()
    if not port:
        print("ERROR: 没有找到串口，用 --port COMx 指定")
        raise SystemExit(2)
    print("  port = %s @ %u" % (port, args.baud))
    return ShellPort(port, baud=args.baud, timeout=args.timeout)


def cmd_info(args):
    sh = open_shell(args)
    try:
        sh.drain(0.2)
        sh.send("facepanel info")
        tok, lines = sh.wait_for("[facepanel]", timeout=6.0)
        for ln in lines[-6:]:
            print("  | %s" % ln)
        return 0 if tok else 3
    finally:
        sh.close()


def cmd_clear(args):
    sh = open_shell(args)
    try:
        sh.drain(0.2)
        sh.send("facepanel clear")
        tok, lines = sh.wait_for("[facepanel] clear", timeout=12.0)
        for ln in lines[-3:]:
            print("  | %s" % ln)
        return 0 if tok else 3
    finally:
        sh.close()


def cmd_upload(args):
    blob = open(args.pfp, "rb").read()
    if len(blob) != IMG_LEN:
        raise SystemExit("blob 长度 %u != %u" % (len(blob), IMG_LEN))
    crc = (int(args.crc, 0) & 0xFFFFFFFF) if getattr(args, 'crc', '') else (binascii.crc32(blob) & 0xFFFFFFFF)
    chunks = [blob[i:i + CHUNK] for i in range(0, len(blob), CHUNK)]
    worst = len("facepanel w %u " % ((len(chunks) - 1) * CHUNK)) + 2 * CHUNK
    if worst >= SHELL_LINE_MAX:
        raise SystemExit("shell 行宽 %u >= %u" % (worst, SHELL_LINE_MAX))
    print("upload: %s" % args.pfp)
    print("  blob = %u B -> %u line(s) x <=%u B, crc=0x%08X"
          % (len(blob), len(chunks), CHUNK, crc))
    if args.dry_run:
        print("  [dry-run] facepanel erase / begin / w x %u / end" % len(chunks))
        return 0

    sh = open_shell(args)
    try:
        sh.drain(0.2)
        sh.send("facepanel")
        tok, lines = sh.wait_for("[facepanel]", timeout=3.0)
        if tok is None:
            print("ERROR: 没等到 facepanel 响应（旧固件/串口不对）")
            for ln in lines[-6:]:
                print("  | %s" % ln)
            return 3

        sh.send("facepanel erase")
        tok, lines = sh.wait_for("erase", timeout=max(args.timeout, 12.0))
        if tok is None:
            print("ERROR: erase 超时")
            return 4

        sh.send("facepanel begin %u %08X" % (len(blob), crc))
        tok, lines = sh.wait_for("begin", timeout=args.timeout)
        if tok is None or "ok" not in lines[-1].lower():
            print("ERROR: begin 失败")
            for ln in lines[-4:]:
                print("  | %s" % ln)
            return 4

        t0 = time.time()
        for k, chunk in enumerate(chunks):
            off = k * CHUNK
            line = "facepanel w %u %s" % (off, binascii.hexlify(chunk).decode())
            ok = False
            for _ in (0, 1):
                sh.send(line)
                tok, lines = sh.wait_for(("W-OK",), timeout=args.timeout,
                                         error_tokens=("W-ERR",))
                if tok == "W-OK":
                    ok = True
                    break
                if tok is not None:
                    break
            if not ok:
                print("\nERROR: 写 off=%u 失败（%u/%u）" % (off, k + 1, len(chunks)))
                for ln in lines[-4:]:
                    print("  | %s" % ln)
                return 5
            if (k + 1) % 200 == 0 or k + 1 == len(chunks):
                print("\r  upload: %u/%u (%5.1f%%) %.1f s"
                      % (k + 1, len(chunks), 100.0 * (k + 1) / len(chunks),
                         time.time() - t0), end="", flush=True)
        print("")

        sh.send("facepanel end")
        tok, lines = sh.wait_for(("install OK",), timeout=max(args.timeout, 8.0),
                                 error_tokens=("install ERR",))
        for ln in lines[-3:]:
            print("  | %s" % ln)
        if tok != "install OK":
            print("ERROR: end 失败（CRC 不对/写失败）")
            return 6
        print("panel install OK")
        return 0
    finally:
        sh.close()


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("demo")
    d.add_argument("png")
    d.set_defaults(fn=cmd_demo)
    p = sub.add_parser("pack")
    p.add_argument("png")
    p.add_argument("pfp")
    p.set_defaults(fn=lambda a: pack_panel(a.png, a.pfp) and 0)
    for name, fn in (("upload", cmd_upload), ("clear", cmd_clear), ("info", cmd_info)):
        q = sub.add_parser(name)
        q.add_argument("--port", default="")
        q.add_argument("--baud", type=int, default=115200)
        q.add_argument("--timeout", type=float, default=3.0)
        q.set_defaults(fn=fn)
    up = sub.choices["upload"]
    up.add_argument("pfp")
    up.add_argument("--dry-run", action="store_true")
    up.add_argument("--crc", default="", help="覆盖 CRC（诊断用，故意给错验证拒装）")
    args = ap.parse_args(argv)
    if getattr(args, "dry_run", False) and args.fn is cmd_upload:
        pass
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
