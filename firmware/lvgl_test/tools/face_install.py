#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PROTORIG ES 图片表盘安装工具（B：表盘可替换，2026-11-08）

契约：V1.6/research/70_表盘可替换_实现方案.md §4。
固件侧对应 services/svc_face_store.c + main.c 的 `faceimg` shell 命令。

子命令
------
  pack <png> -o <pfi> [--time-x --time-y --font --flags --color]
      PNG -> 480x480 cover 裁切 -> 256 色索引 + palette -> .pfi
  demo -o <pfi>
      生成一张测试图（渐变 + 暗色时间区）并打包
  upload <pfi> [--port COM8] [--x --y --font --flags --color]
      串口 `faceimg begin/erase/w/end/apply` 分片上传（48 B/行）
  info <pfi>
      打印 .pfi 的尺寸/CRC/布局

.pfi 布局（与固件 header 逐字节一致，小端）：
  0   u32 magic 'PFIM' = 0x4D494650      18  u32 time_color 0xRRGGBB
  4   u8  version = 1                    22  u32 image_len = 231,424
  5   u8  fmt = 1 (I8)                   26  u32 image_crc32 (zlib CRC32)
  6   u16 width  = 480                   30  u8[2] reserved = 0
  8   u16 height = 480                   32  u32 header_crc32 (zlib, over [0..31])
  10  u16 palette_entries = 256          36..255 0xFF
  12  i16 time_x
  14  i16 time_y
  16  u8  font_id (0=ui48 1=num104 2=ui28 3=ui20)
  17  u8  flags (bit0 日期, bit1 电量)
  [256..] image blob = 1024 B palette(B,G,R,A) + 480*480 索引

串口约定：shell 单行 <= 120 字符（CONFIG_SHELL_CMD_SIZE），
48 B = 96 个 hex 字符 + "faceimg w <off> " 前缀，总计 ~113 字符。

用法示例：
  python tools/face_install.py demo -o tmp/demo.pfi
  python tools/face_install.py upload tmp/demo.pfi --port COM8
  python tools/face_install.py upload tmp/demo.pfi --dry-run
"""

from __future__ import annotations

import argparse
import binascii
import os
import struct
import sys
import time

# ---------------------------------------------------------------------------
# 格式常量（必须与 services/svc_face_store.h 一致）
# ---------------------------------------------------------------------------
MAGIC = 0x4D494650          # 'PFIM'
VERSION = 1
FMT_I8 = 1
W = H = 480
PAL_N = 256
PAL_BYTES = PAL_N * 4       # lv_color32_t: B, G, R, A
IMAGE_LEN = PAL_BYTES + W * H               # 231,424
HEADER_LEN = 256
PFI_LEN = HEADER_LEN + IMAGE_LEN            # 231,680

SHELL_LINE_MAX = 120        # SDK CONFIG_SHELL_CMD_SIZE
CHUNK = 48                  # 每行字节数（hex 96 字符）

FONT_NAMES = {
    0: "ui48  (UI_FONT_HUGE)",
    1: "num104(UI_FONT_CLOCK)",
    2: "ui28  (UI_FONT_HEAD)",
    3: "ui20  (UI_FONT_TITLE)",
}

# 打包默认叠层参数：偏上、超大数字 + 日期 + 电量、信号黄
DEF_TIME_X = 110
DEF_TIME_Y = 170
DEF_FONT = 1
DEF_FLAGS = 0x03
DEF_COLOR = 0xFFFA00

BAUD = 115200


# ---------------------------------------------------------------------------
# header 编解码
# ---------------------------------------------------------------------------
def build_header(time_x: int, time_y: int, font: int, flags: int,
                 color: int, image_crc: int) -> bytes:
    """生成 256 B header（含 header_crc32）。"""
    if not -32768 <= time_x <= 32767 or not -32768 <= time_y <= 32767:
        raise ValueError("time_x/time_y 超出 int16")
    if not 0 <= font <= 3:
        raise ValueError("font 必须是 0..3")
    if not 0 <= flags <= 0x03:
        raise ValueError("flags 只能是 bit0/bit1")
    if not 0 <= color <= 0xFFFFFF:
        raise ValueError("颜色必须是 0xRRGGBB")

    hdr = bytearray(b"\xFF" * HEADER_LEN)
    struct.pack_into("<I", hdr, 0, MAGIC)
    hdr[4] = VERSION
    hdr[5] = FMT_I8
    struct.pack_into("<HHH", hdr, 6, W, H, PAL_N)
    struct.pack_into("<hh", hdr, 12, time_x, time_y)
    hdr[16] = font
    hdr[17] = flags
    struct.pack_into("<I", hdr, 18, color)
    struct.pack_into("<I", hdr, 22, IMAGE_LEN)
    struct.pack_into("<I", hdr, 26, image_crc & 0xFFFFFFFF)
    hdr[30] = 0
    hdr[31] = 0
    struct.pack_into("<I", hdr, 32, binascii.crc32(bytes(hdr[0:32])) & 0xFFFFFFFF)
    return bytes(hdr)


def parse_header(hdr: bytes) -> dict:
    """解析并校验 header；返回字段字典。失败抛 ValueError。"""
    if len(hdr) < HEADER_LEN:
        raise ValueError("header 太短")
    magic, = struct.unpack_from("<I", hdr, 0)
    if magic != MAGIC:
        raise ValueError("magic 不对：0x%08X（不是 .pfi？）" % magic)
    if hdr[4] != VERSION:
        raise ValueError("version=%u（期望 %u）" % (hdr[4], VERSION))
    if hdr[5] != FMT_I8:
        raise ValueError("fmt=%u（期望 1 = I8）" % hdr[5])
    width, height, pal = struct.unpack_from("<HHH", hdr, 6)
    if (width, height, pal) != (W, H, PAL_N):
        raise ValueError("尺寸/palette 不对：%ux%u/%u" % (width, height, pal))
    time_x, time_y = struct.unpack_from("<hh", hdr, 12)
    font = hdr[16]
    flags = hdr[17]
    color, = struct.unpack_from("<I", hdr, 18)
    image_len, image_crc = struct.unpack_from("<II", hdr, 22)
    header_crc, = struct.unpack_from("<I", hdr, 32)
    if image_len != IMAGE_LEN:
        raise ValueError("image_len=%u（期望 %u）" % (image_len, IMAGE_LEN))
    if font > 3:
        raise ValueError("font_id=%u 非法" % font)
    if flags & ~0x03:
        raise ValueError("flags=0x%02X 非法" % flags)
    got = binascii.crc32(bytes(hdr[0:32])) & 0xFFFFFFFF
    if header_crc != got:
        raise ValueError("header_crc32 不符：文件 0x%08X / 实算 0x%08X"
                         % (header_crc, got))
    return dict(width=width, height=height, palette_entries=pal,
                time_x=time_x, time_y=time_y, font=font, flags=flags,
                color=color & 0xFFFFFF, image_len=image_len,
                image_crc=image_crc, header_crc=header_crc)


def load_pfi(path: str, verify_crc: bool = True) -> tuple[dict, bytes]:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != PFI_LEN:
        raise ValueError("%s 长度 %u，期望 %u（.%s）"
                         % (path, len(data), PFI_LEN, "pfi"))
    hdr = data[:HEADER_LEN]
    blob = data[HEADER_LEN:]
    info = parse_header(hdr)
    crc = binascii.crc32(blob) & 0xFFFFFFFF
    if verify_crc and crc != info["image_crc"]:
        raise ValueError("image_crc32 不符：文件 0x%08X / 实算 0x%08X"
                         % (info["image_crc"], crc))
    return info, blob


# ---------------------------------------------------------------------------
# PNG -> 索引图
# ---------------------------------------------------------------------------
def quantize_indexed(img, dither: bool = True):
    """RGB 图 -> (palette_bytes 1024, index_bytes 230400)。

    palette 按 LVGL lv_color32_t 的内存序 B,G,R,A 输出；
    索引直取 tobytes()，与固件 I8 布局一致。
    """
    from PIL import Image

    method = Image.Quantize.MEDIANCUT
    d = Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE
    q = img.convert("RGB").quantize(colors=256, method=method, dither=d)
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


def pack_pil(img, out_path: str, time_x: int, time_y: int, font: int,
             flags: int, color: int, dither: bool) -> dict:
    """PIL 图 -> cover 裁切 -> 量化 -> 写 .pfi。返回 info 字典。"""
    from PIL import ImageOps

    fitted = ImageOps.fit(img.convert("RGB"), (W, H), method=3)  # 3 = LANCZOS
    palb, idx = quantize_indexed(fitted, dither=dither)
    blob = palb + idx
    crc = binascii.crc32(blob) & 0xFFFFFFFF
    hdr = build_header(time_x, time_y, font, flags, color, crc)

    out_dir = os.path.dirname(os.path.abspath(out_path))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(hdr)
        f.write(blob)
    info = parse_header(hdr)
    info["image_crc"] = crc
    return info


# ---------------------------------------------------------------------------
# 串口 shell 小封装
# ---------------------------------------------------------------------------
class ShellPort:
    """按行收发 shell。只认显式 token（如 'W-OK'），不解析 prompt。"""

    def __init__(self, port: str, baud: int = BAUD, timeout: float = 3.0):
        import serial

        # timeout 只用于兜底（正常用 in_waiting 非阻塞轮询，1 ms 粒度）
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.ser.dtr = True
        self.ser.rts = True
        self.timeout = timeout
        self._buf = b""
        self._lines: list[str] = []

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _read_once(self, wait: float = 0.001):
        """非阻塞收一次（轮询粒度 ms 级，别用 read(1) 的 50 ms 超时拖慢 4822 行上传）。"""
        if wait > 0:
            time.sleep(wait)
        n = self.ser.in_waiting
        if n:
            self._buf += self.ser.read(n)
        text = self._buf.decode("utf-8", "replace")
        if "\n" in text:
            parts = text.split("\n")
            self._buf = parts.pop().encode("utf-8", "replace")
            for p in parts:
                line = p.replace("\r", "").strip("\x00")
                if line:
                    self._lines.append(line)

    def drain(self, seconds: float = 0.15):
        end = time.time() + seconds
        while time.time() < end:
            self._read_once(0.002)
        self._buf = b""
        self._lines.clear()

    def send(self, line: str):
        if len(line) >= SHELL_LINE_MAX:
            raise RuntimeError("shell 命令行 %u 字符超过 %u 上限"
                               % (len(line), SHELL_LINE_MAX))
        self.ser.write(line.encode("ascii") + b"\r\n")
        self.ser.flush()

    def wait_for(self, tokens, timeout: float | None = None,
                 error_tokens=()) -> tuple[str | None, list[str]]:
        """读到出现任一 token 的行为止。返回 (命中 token, 期间的行)。"""
        if isinstance(tokens, str):
            tokens = (tokens,)
        deadline = time.time() + (self.timeout if timeout is None else timeout)
        seen = []
        while time.time() < deadline:
            while self._lines:
                line = self._lines.pop(0)
                seen.append(line)
                for tok in tokens:
                    if tok in line:
                        return tok, seen
                for tok in error_tokens:
                    if tok in line:
                        return tok, seen
            self._read_once(0.005)
        return None, seen


def find_port() -> str | None:
    """自动找 Bouffalo USB Console（VID 349B / PID 6160，与 flash.ps1 同一规则）。"""
    try:
        from serial.tools import list_ports
    except Exception:
        return None
    cands = []
    for p in list_ports.comports():
        if p.vid == 0x349B and p.pid == 0x6160:
            cands.append(p.device)
        elif p.description and ("Bouffalo" in p.description or
                                "USB" in p.description.upper()):
            cands.append(p.device)
    return cands[0] if cands else None


# ---------------------------------------------------------------------------
# 子命令
# ---------------------------------------------------------------------------
def cmd_pack(args) -> int:
    from PIL import Image

    img = Image.open(args.png)
    info = pack_pil(img, args.out, args.time_x, args.time_y, args.font,
                    args.flags, args.color, dither=not args.no_dither)
    print("pack OK: %s" % args.out)
    print("  size   = %u bytes (header %u + image %u)"
          % (PFI_LEN, HEADER_LEN, IMAGE_LEN))
    print("  crc32  = 0x%08X" % info["image_crc"])
    print("  layout = time=(%d,%d) font=%d(%s) flags=0x%02X color=0x%06X"
          % (info["time_x"], info["time_y"], info["font"],
             FONT_NAMES[info["font"]].split("(")[0].strip(),
             info["flags"], info["color"]))
    return 0


def cmd_demo(args) -> int:
    """生成测试图：竖向渐变 + 圆环 + 暗色时间区，便于上板看时间叠层。"""
    from PIL import Image, ImageDraw

    t0 = (28, 52, 104)      # 顶部钢蓝
    t1 = (118, 24, 58)      # 底部暗红
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        k = y / (H - 1)
        r = int(t0[0] + (t1[0] - t0[0]) * k)
        g = int(t0[1] + (t1[1] - t0[1]) * k)
        b = int(t0[2] + (t1[2] - t0[2]) * k)
        for x in range(W):
            # 轻微径向变暗，方便看 256 色过渡
            d = ((x - W / 2) ** 2 + (y - H / 2) ** 2) ** 0.5 / (W * 0.72)
            f = max(0.55, 1.0 - 0.35 * d)
            px[x, y] = (int(r * f), int(g * f), int(b * f))

    d = ImageDraw.Draw(img)
    # 外圈（圆屏安全半径 207 px 附近）与几道刻度
    for rr, col in ((207, (120, 120, 130)), (203, (70, 72, 84))):
        d.ellipse((W / 2 - rr, H / 2 - rr, W / 2 + rr, H / 2 + rr),
                  outline=col, width=2)
    for i in range(12):
        import math
        a = math.radians(i * 30 - 90)
        x0 = W / 2 + 190 * math.cos(a)
        y0 = H / 2 + 190 * math.sin(a)
        x1 = W / 2 + 200 * math.cos(a)
        y1 = H / 2 + 200 * math.sin(a)
        d.line((x0, y0, x1, y1), fill=(150, 150, 160), width=3)

    # 时间区暗块（放大数字后仍可读）
    d.rounded_rectangle((60, 150, 420, 330), radius=24, fill=(8, 8, 12))

    # 底部装饰条
    d.rectangle((40, 400, 440, 404), fill=(255, 250, 0))

    # 直接走和 pack 相同的管线（不落地中间 PNG）
    info = pack_pil(img, args.out, args.time_x, args.time_y, args.font,
                    args.flags, args.color, dither=not args.no_dither)
    print("demo OK: %s" % args.out)
    print("  size   = %u bytes" % PFI_LEN)
    print("  crc32  = 0x%08X" % info["image_crc"])
    print("  layout = time=(%d,%d) font=%d(%s) flags=0x%02X color=0x%06X"
          % (info["time_x"], info["time_y"], info["font"],
             FONT_NAMES[info["font"]].split("(")[0].strip(),
             info["flags"], info["color"]))
    return 0


def cmd_info(args) -> int:
    info, blob = load_pfi(args.pfi)
    palb, idx = blob[:PAL_BYTES], blob[PAL_BYTES:]
    print("%s" % args.pfi)
    print("  file   = %u bytes (header %u + image %u)"
          % (os.path.getsize(args.pfi), HEADER_LEN, IMAGE_LEN))
    print("  magic  = 0x%08X  version=%u fmt=%u" % (MAGIC, VERSION, FMT_I8))
    print("  image  = %ux%u palette=%u len=%u"
          % (info["width"], info["height"], info["palette_entries"],
             info["image_len"]))
    print("  crc32  = 0x%08X (recomputed from blob: 0x%08X) %s"
          % (info["image_crc"], binascii.crc32(blob) & 0xFFFFFFFF,
             "OK" if (binascii.crc32(blob) & 0xFFFFFFFF) == info["image_crc"]
             else "MISMATCH"))
    print("  header_crc32 = 0x%08X" % info["header_crc"])
    print("  layout = time=(%d,%d) font=%u(%s) flags=0x%02X(bit0 日期=%u bit1 电量=%u)"
          % (info["time_x"], info["time_y"], info["font"],
             FONT_NAMES.get(info["font"], "?"), info["flags"],
             1 if info["flags"] & 1 else 0,
             1 if info["flags"] & 2 else 0))
    print("  color  = 0x%06X" % info["color"])
    print("  blob   = palette %u B (B,G,R,A) + index %u B"
          % (len(palb), len(idx)))
    return 0


def cmd_upload(args) -> int:
    # --force：跳过本地 CRC 校验，专门用来验证固件侧的"坏 CRC 拒装"（上板 5.2.8）
    info, blob = load_pfi(args.pfi, verify_crc=not args.force)
    # 命令行给的叠层参数优先；没给就用 .pfi 里的
    time_x = info["time_x"] if args.time_x is None else args.time_x
    time_y = info["time_y"] if args.time_y is None else args.time_y
    font = info["font"] if args.font is None else args.font
    flags = info["flags"] if args.flags is None else args.flags
    color = info["color"] if args.color is None else args.color

    chunks = [blob[i:i + CHUNK] for i in range(0, len(blob), CHUNK)]
    # shell 单行上限（CONFIG_SHELL_CMD_SIZE=120）：48 B 的 hex 行最长 113 字符
    worst = len("faceimg w %u " % ((len(chunks) - 1) * CHUNK)) + 2 * CHUNK
    if worst >= SHELL_LINE_MAX:
        raise RuntimeError("shell 行宽 %u >= %u，请减小 CHUNK" % (worst, SHELL_LINE_MAX))
    print("upload: %s" % args.pfi)
    print("  image  = %u B -> %u line(s) x <=%u B" % (len(blob), len(chunks), CHUNK))
    print("  crc32  = 0x%08X" % info["image_crc"])
    print("  layout = time=(%d,%d) font=%d flags=0x%02X color=0x%06X"
          % (time_x, time_y, font, flags, color))

    if args.dry_run:
        print("  [dry-run] 不发串口。实际会执行：")
        print("    faceimg erase")
        print("    faceimg begin %u %08X" % (len(blob), info["image_crc"]))
        print("    faceimg w <off> <hex> x %u (off = 0, %u, %u, ...)"
              % (len(chunks), CHUNK, CHUNK * 2))
        print("    faceimg end %d %d %d %d %06X" % (time_x, time_y, font, flags, color))
        if not args.no_apply:
            print("    faceimg apply")
        return 0

    port = args.port or find_port()
    if not port:
        print("ERROR: 没有找到 Bouffalo USB Console 串口，用 --port COMx 指定")
        return 2
    print("  port   = %s @ %u" % (port, args.baud))

    try:
        sh = ShellPort(port, baud=args.baud, timeout=args.timeout)
    except Exception as e:
        print("ERROR: 打开 %s 失败：%s" % (port, e))
        return 2

    try:
        sh.drain(0.2)
        sh.send("")
        sh.drain(0.1)

        # 0) 探活：确认固件里有 faceimg 命令
        sh.send("faceimg")
        tok, lines = sh.wait_for("[faceimg]", timeout=3.0)
        if tok is None:
            print("ERROR: 没等到 faceimg 响应（旧固件？串口不对？）")
            for ln in lines[-8:]:
                print("    | %s" % ln)
            return 3
        print("  device : %s" % lines[-1])

        # 1) 擦除（58 个 4 KB 扇区，芯片手册 worst-case 可能 >3 s，单独放长超时）
        sh.send("faceimg erase")
        tok, lines = sh.wait_for("[faceimg] erase", timeout=max(args.timeout, 12.0))
        if tok is None or "erase ok" not in lines[-1]:
            print("ERROR: erase 失败/超时")
            for ln in lines[-6:]:
                print("    | %s" % ln)
            return 4

        # 2) begin
        sh.send("faceimg begin %u %08X" % (len(blob), info["image_crc"]))
        tok, lines = sh.wait_for("[faceimg] begin", timeout=args.timeout)
        if tok is None or " ok" not in lines[-1]:
            print("ERROR: begin 失败/超时")
            for ln in lines[-6:]:
                print("    | %s" % ln)
            return 4

        # 3) 48 B/行写 image blob
        t0 = time.time()
        for k, chunk in enumerate(chunks):
            off = k * CHUNK
            line = "faceimg w %u %s" % (off, binascii.hexlify(chunk).decode())
            ok = False
            for attempt in (0, 1):
                sh.send(line)
                tok, lines = sh.wait_for(("W-OK",), timeout=args.timeout,
                                         error_tokens=("W-ERR",))
                if tok == "W-OK":
                    ok = True
                    break
                if tok is not None:
                    break
            if not ok:
                print("\nERROR: 写 off=%u 失败（第 %u/%u 片）"
                      % (off, k + 1, len(chunks)))
                for ln in lines[-6:]:
                    print("    | %s" % ln)
                return 5
            if (k + 1) % 200 == 0 or k + 1 == len(chunks):
                pct = 100.0 * (k + 1) / len(chunks)
                print("\r  upload : %u/%u (%5.1f%%)  %.1f s"
                      % (k + 1, len(chunks), pct, time.time() - t0),
                      end="", flush=True)
        print("")

        # 4) end：固件侧重算 CRC -> 写 header（坏 CRC 会在这里被拒）
        sh.send("faceimg end %d %d %d %d %06X"
                % (time_x, time_y, font, flags, color))
        tok, lines = sh.wait_for(("install OK",), timeout=max(args.timeout, 8.0),
                                 error_tokens=("install ERR",))
        if tok != "install OK":
            print("ERROR: 固件拒绝安装")
            for ln in lines[-8:]:
                print("    | %s" % ln)
            return 6
        print("  end    : %s" % lines[-1])

        # 5) info：回读校验（设备侧把所有 231 KB 读回来重算 CRC）
        sh.send("faceimg info")
        tok, lines = sh.wait_for("[faceimg] valid=", timeout=max(args.timeout, 8.0))
        if tok is None:
            print("ERROR: faceimg info 无响应")
            return 7
        print("  verify : %s" % lines[-1])
        got = None
        for word in lines[-1].split():
            if word.startswith("crc="):
                got = int(word[4:], 16)
        if got is None or got != info["image_crc"]:
            print("ERROR: 回读 CRC 不一致（期望 0x%08X）" % info["image_crc"])
            return 7

        # 6) apply：切到 face 3（LVGL 任务执行 + face_id 持久化）
        if args.no_apply:
            print("  apply  : 跳过（--no-apply），上板后 shell 敲 face 3 即可")
        else:
            sh.send("faceimg apply")
            tok, lines = sh.wait_for("apply queued", timeout=args.timeout)
            if tok is None:
                print("ERROR: apply 无响应")
                return 8
            print("  apply  : %s" % lines[-1])

        print("UPLOAD OK")
        return 0
    finally:
        sh.close()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def _add_layout_args(p: argparse.ArgumentParser, defaults: bool):
    d = dict(time_x=DEF_TIME_X, time_y=DEF_TIME_Y, font=DEF_FONT,
             flags=DEF_FLAGS, color=DEF_COLOR) if defaults else \
        dict(time_x=None, time_y=None, font=None, flags=None, color=None)
    p.add_argument("--time-x", "--x", dest="time_x", type=int, default=d["time_x"],
                   help="时间文本左上角 x")
    p.add_argument("--time-y", "--y", dest="time_y", type=int, default=d["time_y"],
                   help="时间文本左上角 y")
    p.add_argument("--font", type=int, default=d["font"],
                   help="0=ui48 1=num104 2=ui28 3=ui20")
    p.add_argument("--flags", type=lambda s: int(s, 0), default=d["flags"],
                   help="bit0 日期, bit1 电量（默认 0x3）")
    p.add_argument("--color", type=lambda s: int(s, 16), default=d["color"],
                   help="时间颜色 0xRRGGBB")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="PROTORIG ES 图片表盘安装工具（PNG -> .pfi -> USB 串口上传）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack", help="PNG -> .pfi")
    p.add_argument("png")
    p.add_argument("-o", "--out", required=True, help="输出 .pfi")
    p.add_argument("--no-dither", action="store_true", help="关闭 Floyd-Steinberg 抖动")
    _add_layout_args(p, defaults=True)
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("demo", help="生成测试图并打包")
    p.add_argument("-o", "--out", default="tmp/demo.pfi", help="输出 .pfi")
    p.add_argument("--no-dither", action="store_true", help="关闭抖动")
    _add_layout_args(p, defaults=True)
    p.set_defaults(func=cmd_demo)

    p = sub.add_parser("info", help="打印 .pfi 信息")
    p.add_argument("pfi")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("upload", help="串口上传 .pfi")
    p.add_argument("pfi")
    p.add_argument("--port", default=None, help="COM8 之类；默认自动找 VID 349B")
    p.add_argument("--baud", type=int, default=BAUD)
    p.add_argument("--timeout", type=float, default=3.0, help="单条命令超时（秒）")
    p.add_argument("--dry-run", action="store_true", help="只打印计划，不开串口")
    p.add_argument("--no-apply", action="store_true", help="上传后不自动切到 face 3")
    p.add_argument("--force", action="store_true",
                   help="跳过本地 CRC 校验（只为验证固件侧坏 CRC 拒装，别日常用）")
    _add_layout_args(p, defaults=False)
    p.set_defaults(func=cmd_upload)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except (ValueError, OSError) as e:
        print("ERROR: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
