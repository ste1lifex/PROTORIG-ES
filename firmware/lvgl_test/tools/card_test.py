#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PROTORIG ES 卡片层台架验收（0x53..0x58）。

背景（research/65）：BLE 传不了像素，手表用本地 LVGL 监控页画三环；
卡片（会话/审批/问答）由电脑下发**小数据**（标题/命令/题目走 1-bit 位图），
触摸由手表回传，插件做 hit-test。

这个脚本只验证**固件 + 链路**这一半（插件那一半用 DSH 3100 验）：
  · USB 通道：进 pcmode 但不发 HELLO（= 不冻结本地页），卡片直接画在本地页上；
  · BLE 通道：0x51 进电脑模式 → 0x52 推三环数值 → 0x53/0x54/0x55/0x57 出卡；
  · 每条卡片帧都应收到固件回执 0x59，以及热区自报 0x58；
  · --hold 期间打印手表回传的 0x04 触摸帧（真人点屏幕）。

用法：
    python tools/card_test.py --transport usb --stage all
    python tools/card_test.py --transport ble --stage all
    python tools/card_test.py --transport ble --stage p3 --hold 20   # 停在审批卡等人点

注意：串口/蓝牙同一时刻只允许一个进程；跑之前先停插件和 CLI。
"""
from __future__ import annotations

import argparse
import asyncio
import re
import struct
import sys
import threading
import time

# ---------------------------------------------------------------- 协议常量

OP_SESSION = 0x53
OP_APPROVAL = 0x54
OP_QUESTION = 0x55
OP_CARD_CLEAR = 0x56
OP_CARD_BMP = 0x57

CMD_PC_FRAME = 0x50
CMD_PC_MODE = 0x51
CMD_METRICS = 0x52

EVT_ZONES = 0x58
EVT_ACK = 0x59

ACK_NAMES = {1: "card", 2: "bmp-ok", 3: "bmp-bad", 4: "card-bad"}
CARD_NAMES = {0: "off", 1: "session", 2: "approval", 3: "question"}

PM_STATE = {"IDLE": 0, "RUNNING": 1, "NEEDS": 2, "DONE": 3, "FAILED": 4}

BMP_MAX_W = 296
BMP_H = 30
BMP_CHUNK = 231
FRAME_MAX = 240


# ---------------------------------------------------------------- 载荷构造

def _str8(text: str, limit: int) -> bytes:
    """u8 长度前缀 + UTF-8，按字节截断且不切断半个字符。"""
    raw = (text or "").encode("utf-8")
    if len(raw) > limit:
        raw = raw[:limit]
        cut = len(raw)
        while cut > 0:
            start = cut - 1
            while start > 0 and (raw[start] & 0xC0) == 0x80:
                start -= 1
            lead = raw[start]
            need = 1 if lead < 0x80 else 2 if lead >> 5 == 6 else 3 if lead >> 4 == 14 else 4
            if start + need <= len(raw):
                break
            cut = start
        raw = raw[:cut]
    return bytes([len(raw)]) + raw


def card_session(state: int, steps: int, ctx: int, tool: str, title: str,
                 footer: str = "") -> bytes:
    return (bytes([OP_SESSION, state & 0xFF, min(255, steps), 0xFF if ctx < 0 else ctx])
            + _str8(tool, 11) + _str8(title, 72) + _str8(footer, 48))


def card_approval(can_allow: int, computer: int, countdown: int, tool: str, cmd: str) -> bytes:
    return (bytes([OP_APPROVAL, 1 if can_allow else 0, 1 if computer else 0, min(99, countdown)])
            + _str8(tool, 11) + _str8(cmd, 96))


def card_question(q_index: int, q_count: int, multi: int, sel_mask: int, page: int,
                  prompt: str, option: str, opt_count: int = 3) -> bytes:
    return (bytes([OP_QUESTION, q_index, q_count, 1 if multi else 0,
                   sel_mask & 0xFF, (sel_mask >> 8) & 0xFF, page, max(1, min(255, opt_count))])
            + _str8(prompt, 72) + _str8(option, 96))


def card_clear() -> bytes:
    return bytes([OP_CARD_CLEAR])


def card_bitmap_clear(slot: int) -> bytes:
    """清掉一个位图槽（w=h=total=bpp=0，无数据）。"""
    return bytes([OP_CARD_BMP, slot & 1, 0, 0, 0, 0, 0, 0, 0])


def card_bitmaps(slot: int, width: int, height: int, bits: bytes, bpp: int = 2):
    """0x57 分片：头 9 B（含 bpp）+ 数据 ≤231 B。"""
    width = max(8, min(BMP_MAX_W, width & ~7))
    stride = (width * bpp) >> 3
    need = stride * height
    total = max(1, (need + BMP_CHUNK - 1) // BMP_CHUNK)
    for index in range(total):
        chunk = bits[index * BMP_CHUNK:min((index + 1) * BMP_CHUNK, need)]
        yield (bytes([OP_CARD_BMP, slot & 1, width & 0xFF, (width >> 8) & 0xFF,
                      height & 0xFF, (height >> 8) & 0xFF, index, total, bpp]) + chunk)


def text_bitmap(text: str, max_width: int = BMP_MAX_W):
    """用系统雅黑把一行中文渲成 1-bit 位图（和插件 card-bitmap.mjs 同一种格式）。"""
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 25)
    probe = Image.new("L", (8, 8))
    width = int(ImageDraw.Draw(probe).textlength(text, font=font)) + 6
    width = max(8, min(max_width, (width + 7) // 8 * 8))
    image = Image.new("L", (width, BMP_H), 0)
    ImageDraw.Draw(image).text((2, -3), text, fill=255, font=font)  # 30px 行高，基线约 24
    pixels = image.load()
    # 2-bit（4 级灰）：和插件一样把覆盖率量化成 0/85/170/255
    stride = width >> 2
    bits = bytearray(stride * BMP_H)
    for y in range(BMP_H):
        for x in range(width):
            level = min(3, round(pixels[x, y] / 85))
            if level:
                bits[y * stride + (x >> 2)] |= level << (6 - ((x & 3) << 1))
    return width, BMP_H, bytes(bits)


# ---------------------------------------------------------------- 事件解析

class Event:
    def __init__(self, cmd: int, payload: bytes):
        self.cmd = cmd
        self.payload = payload

    def __repr__(self) -> str:
        if self.cmd == EVT_ACK and len(self.payload) >= 2:
            what = ACK_NAMES.get(self.payload[0], hex(self.payload[0]))
            return f"ACK {what} code={self.payload[1]}"
        if self.cmd == EVT_ZONES and len(self.payload) >= 2:
            kind = CARD_NAMES.get(self.payload[0], str(self.payload[0]))
            count = self.payload[1]
            zones = []
            for i in range(count):
                at = 2 + i * 9
                if at + 9 > len(self.payload):
                    break
                zid, x, y, w, h = struct.unpack_from("<BHHHH", self.payload, at)
                zones.append(f"id{zid}@({x},{y},{w}x{h})")
            return f"ZONES kind={kind} count={count} " + " ".join(zones)
        if self.cmd == CMD_PC_FRAME and len(self.payload) >= 6 and self.payload[0] == 0x04:
            pressed, x, y = struct.unpack_from("<BHH", self.payload, 1)
            return f"TOUCH pressed={pressed} x={x} y={y}"
        if self.cmd == 0x91 and len(self.payload) >= 4:
            return (f"STATUS link={self.payload[0]} batt={self.payload[1]} "
                    f"bright={self.payload[2]} page={self.payload[3]}")
        return f"cmd=0x{self.cmd:02X} len={len(self.payload)} {self.payload.hex()}"



def parse_zones(event: Event) -> list[dict]:
    out = []
    count = event.payload[1] if len(event.payload) > 1 else 0
    for i in range(count):
        at = 2 + i * 9
        if at + 9 > len(event.payload):
            break
        zid, x, y, w, h = struct.unpack_from("<BHHHH", event.payload, at)
        out.append({"id": zid, "x": x, "y": y, "w": w, "h": h})
    return out


def touch_points(events: list[Event]) -> list[tuple[int, int]]:
    """只取"按下"帧（抬起帧坐标会漂移，命中判定用按下点）。"""
    out = []
    for event in events:
        if event.cmd == CMD_PC_FRAME and len(event.payload) >= 6 and event.payload[0] == 0x04:
            pressed, x, y = struct.unpack_from("<BHH", event.payload, 1)
            if pressed:
                out.append((x, y))
    return out


def hit_zone(point: tuple[int, int], zone: dict) -> bool:
    x, y = point
    return (zone["x"] <= x < zone["x"] + zone["w"]
            and zone["y"] <= y < zone["y"] + zone["h"])


def decode_ble_frame(raw: bytes) -> Event | None:
    if len(raw) < 4 or raw[0] != 0xA5 or raw[1] != 0x5A:
        return None
    cmd, length = raw[2], raw[3]
    if len(raw) < 4 + length:
        return None
    return Event(cmd, raw[4:4 + length])


LINE_FRAME = re.compile(r"@PCM1 FRAME a55a50([0-9A-Fa-f]+)")


# ---------------------------------------------------------------- USB 通道

class UsbTransport:
    def __init__(self, port: str):
        import serial  # pyserial
        self.serial = serial.Serial(port, 115200, timeout=0.1, write_timeout=2)
        self.events: list[Event] = []
        self.lines: list[str] = []
        self._alive = True
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self) -> None:
        buffer = b""
        while self._alive:
            try:
                chunk = self.serial.read(4096)
            except Exception:
                break
            if not chunk:
                continue
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip()
                if not text:
                    continue
                self.lines.append(text)
                match = LINE_FRAME.search(text)
                if match:
                    raw = bytes.fromhex(match.group(1))
                    if raw:
                        self.events.append(Event(raw[0], raw[1:]))

    def command(self, text: str) -> None:
        self.serial.write((text + "\r\n").encode())

    async def send(self, payload: bytes) -> None:
        """USB 是同步写，但保持与 BLE 一样的 await 接口（run_stages 两条链路共用）。"""
        if len(payload) > FRAME_MAX:
            raise ValueError(f"payload 太长 {len(payload)}")
        self.serial.write(bytes([0xA5, 0x5A, len(payload)]) + payload)

    async def tick_metrics(self, now: float) -> None:
        """USB 没有 0x52（数值走 pcmon 协议），台架保持空实现。"""
        return

    def wait_line(self, needle: str, timeout: float = 3.0) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if any(needle in line for line in self.lines):
                return True
            time.sleep(0.05)
        return False

    def pop_events(self) -> list[Event]:
        out, self.events = self.events, []
        return out

    def close(self) -> None:
        self._alive = False
        time.sleep(0.2)
        try:
            self.serial.close()
        except Exception:
            pass


# ---------------------------------------------------------------- BLE 通道

class BleTransport:
    CHR_CMD = "50524f54-4f52-4947-4553-000000000002"
    CHR_EVT = "50524f54-4f52-4947-4553-000000000003"

    def __init__(self, address: str | None, name: str):
        self.address = address
        self.name = name
        self.client = None
        self.events: list[Event] = []
        self.lines: list[str] = []
        self.lost = False
        self.animate = False

    async def open(self, attempts: int = 6) -> None:
        from bleak import BleakClient, BleakScanner
        target = self.address
        if not target:
            for i in range(attempts):
                device = await BleakScanner.find_device_by_filter(
                    lambda d, adv: (d.name or adv.local_name or "").startswith(self.name), timeout=5)
                if device is not None:
                    target = device.address
                    break
                print(f"    扫描第 {i + 1}/{attempts} 次没找到广播，3 s 后重试"
                      f"（手表息屏会停广播：点一下屏幕）…")
                await asyncio.sleep(3)
            if not target:
                raise SystemExit(f"扫描不到广播名以 {self.name!r} 开头的手表")
        self.client = BleakClient(target, timeout=10)
        await self.client.connect()
        await self.client.start_notify(self.CHR_EVT, self._on_evt)

    def _on_evt(self, _sender, data: bytearray) -> None:
        event = decode_ble_frame(bytes(data))
        if event:
            self.events.append(event)

    async def tick_metrics(self, now: float) -> None:
        """台架每秒推一帧 0x52（正弦假数据）：用户看到的才像真实产品在工作。"""
        if not self.animate:
            return
        import math
        cpu = int(500 + 400 * math.sin(now * 1.2))
        gpu = int(300 + 600 * math.sin(now * 0.7 + 1.0))
        ram = int(550 + 100 * math.sin(now * 0.2))
        payload = struct.pack("<HHHHBII", max(0, min(1000, cpu)), max(0, min(1000, gpu)),
                              max(0, min(1000, ram)), 528, 1,
                              int(40000 + 30000 * abs(math.sin(now))),
                              int(4000 + 2000 * abs(math.cos(now))))
        await self.send_cmd(CMD_METRICS, payload)

    async def send_cmd(self, cmd: int, payload: bytes = b"") -> None:
        frame = bytes([0xA5, 0x5A, cmd, len(payload)]) + payload
        try:
            await self.client.write_gatt_char(self.CHR_CMD, frame, response=False)
        except Exception as exc:  # noqa: BLE001
            if not self.lost:
                self.lost = True
                print(f"    !! BLE 写入失败，链路已断：{exc}")
            raise

    async def send(self, payload: bytes) -> None:
        if len(payload) > FRAME_MAX:
            raise ValueError(f"payload 太长 {len(payload)}")
        await self.send_cmd(CMD_PC_FRAME, payload)

    def pop_events(self) -> list[Event]:
        out, self.events = self.events, []
        return out

    async def close(self) -> None:
        try:
            await self.client.disconnect()
        except Exception:
            pass


# ---------------------------------------------------------------- 用例

class Case:
    def __init__(self, name: str):
        self.name = name
        self.passed = 0
        self.failed = 0

    def expect(self, ok: bool, text: str) -> None:
        if ok:
            self.passed += 1
            print(f"  PASS {text}")
        else:
            self.failed += 1
            print(f"  FAIL {text}")


async def collect(transport, seconds: float, report: Case, label: str,
                  expect_kinds: set[int] | None = None,
                  keepalive: bytes | None = None) -> list[Event]:
    """收 seconds 秒的事件，打印，并按需断言"回执 + 热区"。

    keepalive：每 1 s 重发一次这张卡，让屏幕在 --hold 期间一直亮着这张卡
    （固件侧卡片租约 8 s、数值 5 s；1 Hz 重发让用户有足够时间对照，不怕错过）。
    """
    events: list[Event] = []
    started = time.time()
    deadline = started + seconds
    next_keep = started + 1.0
    next_metrics = started
    while time.time() < deadline:
        events.extend(transport.pop_events())
        if time.time() >= next_metrics:
            await transport.tick_metrics(time.time() - started)
            next_metrics = time.time() + 1.0
        if keepalive is not None and time.time() >= next_keep:
            await transport.send(keepalive)
            next_keep = time.time() + 1.0
        await asyncio.sleep(0.05)
    for event in events:
        print(f"    <- {label} {event}")
    if expect_kinds is not None:
        acks = [e for e in events if e.cmd == EVT_ACK and e.payload and e.payload[0] == 1]
        zones = [e for e in events if e.cmd == EVT_ZONES]
        report.expect(bool(acks), f"{label} 收到卡片回执 0x59")
        report.expect(bool(zones), f"{label} 收到热区自报 0x58")
        if zones:
            report.expect(zones[-1].payload[0] in expect_kinds,
                          f"{label} 热区 kind={CARD_NAMES.get(zones[-1].payload[0])}")
    return events


async def send_bitmap(transport, slot: int, text: str, report: Case | None = None) -> None:
    try:
        width, height, bits = text_bitmap(text)
    except Exception as exc:  # noqa: BLE001
        print(f"    !! 位图渲染跳过（{exc}）")
        return
    for payload in card_bitmaps(slot, width, height, bits):
        await transport.send(payload)
        await asyncio.sleep(0.02)
    print(f"    -> 位图 slot={slot} {width}x{height} {len(bits)} B 「{text}」")


async def run_stages(transport, args) -> Case:
    report = Case("card")
    stage = args.stage

    if args.transport == "ble":
        print("[1] BLE：进电脑模式 + 推三环数值")
        await transport.send_cmd(CMD_PC_MODE, bytes([1]))
        await asyncio.sleep(1.2)
        transport.animate = True
        await transport.tick_metrics(0.0)
        await asyncio.sleep(0.4)
        print("    -> 0x51 进屏 + 0x52 三环数值（123/456/789）")

    if stage in ("p1", "all"):
        print(f"[{stage}] P1 会话卡：RUNNING / 3 步 / CTX 42%")
        first = card_session(PM_STATE["RUNNING"], 3, 42, "bash", "BLE 对齐 USB",
                             "电量 97%  ·  ¥91.38")
        await transport.send(first)
        await send_bitmap(transport, 0, "BLE 对齐 USB")
        await collect(transport, 1.0, report, "P1", {1})
        if args.hold > 0:
            print(f"    会话卡停在表上 {args.hold:.0f} s（DSH 运行中 / 标题 / bash · 3 步 / CTX 42% / 电量·余额）",
                  flush=True)
            await collect(transport, args.hold, report, "P1-hold", None, keepalive=first)
        print("    -> 会话卡：DONE+失败 收尾")
        await transport.send(card_session(PM_STATE["DONE"], 5, 66, "edit", "收了"))
        await send_bitmap(transport, 0, "收了")
        await collect(transport, 0.8, report, "P1b", {1})

    if stage in ("p2", "all"):
        print(f"[{stage}] P2 触摸回传（会话卡态，无热区）")
        p2_card = card_session(PM_STATE["NEEDS"], 4, 51, "bash", "等我点一下")
        await transport.send(p2_card)
        await send_bitmap(transport, 0, "等我点一下")
        await collect(transport, 0.8, report, "P2", {1})
        print(f"    现在请在手表面上点/滑 {args.hold:.0f} s，我收 0x04 触摸帧…")
        events = await collect(transport, args.hold, report, "P2-touch", None, keepalive=p2_card)
        touches = [e for e in events if e.cmd == CMD_PC_FRAME and e.payload[:1] == b"\x04"]
        report.expect(bool(touches), "收到手表回传的触摸帧 0x04")

    if stage in ("p3", "all"):
        print(f"[{stage}] P3 审批卡：bash / rm -rf build")
        approval = card_approval(1, 0, 20, "bash", "rm -rf build")
        await transport.send(approval)
        await send_bitmap(transport, 0, "rm -rf build")
        await collect(transport, 1.0, report, "P3", {2})
        if args.hold > 0:
            print(f"    停在审批卡 {args.hold:.0f} s：请点 [拒绝] 或 [允许一次]")
            held = await collect(transport, args.hold, report, "P3-hold", {2},
                                 keepalive=approval)
            zones = [z for e in held if e.cmd == EVT_ZONES and e.payload
                     for z in parse_zones(e) if z["id"] in (1, 2)][-2:]
            hits = [t for t in touch_points(held)
                    if any(hit_zone(t, z) for z in zones)]
            report.expect(bool(hits), f"点到了按钮热区（{len(hits)} 次，坐标 {hits[:3]}）")
        print("    -> 超时态：canAllow=0 computer=1（到电脑确认）")
        await transport.send(card_approval(0, 1, 0, "bash", "rm -rf build"))
        await collect(transport, 0.8, report, "P3b", {2})

    if stage in ("p4", "all"):
        print(f"[{stage}] P4 问答卡：1/3 单选，选项页 0")
        q1 = card_question(0, 3, 0, 0x01, 0, "先做哪一项？", "固件卡片层")
        await transport.send(q1)
        await send_bitmap(transport, 0, "先做哪一项？")
        await send_bitmap(transport, 1, "固件卡片层")
        await collect(transport, 1.2, report, "P4", {3})
        print("    -> 翻到第 2 个选项（已选）")
        await transport.send(card_question(0, 3, 0, 0x02, 1, "先做哪一项？", "插件触摸路由"))
        await send_bitmap(transport, 1, "插件触摸路由")
        await collect(transport, 0.8, report, "P4b", {3})
        if args.hold > 0:
            print(f"    停在问答卡 {args.hold:.0f} s：请依次点 [选项] → [下一项] → [提交]")
            q2 = card_question(0, 3, 0, 0x02, 1, "先做哪一项？", "插件触摸路由")
            held = [e for e in await collect(transport, args.hold, report, "P4-hold", {3},
                                             keepalive=q2)
                    if e.cmd == EVT_ZONES or e.cmd == CMD_PC_FRAME]
            zones = [z for e in held if e.cmd == EVT_ZONES and e.payload
                     for z in parse_zones(e) if z["id"] in (3, 4, 5)][-3:]
            hits = [t for t in touch_points(held)
                    if any(hit_zone(t, z) for z in zones)]
            report.expect(bool(hits), f"点到了问答热区（{len(hits)} 次，坐标 {hits[:3]}）")
        print("    -> 多选态（v4 风格：金色描边 + ✓）：DSH 问答 · 多选 · 2/3 页，已选 2 项")
        multi = card_question(0, 1, 1, 0b011, 1, "这两项都要做", "插件触摸路由", 3)
        await transport.send(multi)
        await send_bitmap(transport, 0, "这两项都要做")
        await send_bitmap(transport, 1, "插件触摸路由")
        await collect(transport, 1.0, report, "P4-multi", {3})
        if args.hold > 0:
            print(f"    多选卡停 {min(args.hold, 12):.0f} s 给用户对照（不用点）")
            await collect(transport, min(args.hold, 12), report, "P4-multi-hold", None,
                          keepalive=multi)

    if stage == "empty":
        print("[empty] 空态：IDLE + 无标题 + 无位图（应显示 当前没有会话 / 在电脑上启动 DSH 任务）")
        await transport.send(card_bitmap_clear(0))
        await asyncio.sleep(0.1)
        await transport.send(card_session(PM_STATE["IDLE"], 0, -1, "", "", "电量 97%  ·  ¥91.38"))
        await collect(transport, 1.0, report, "EMPTY", {1})
        if args.hold > 0:
            print(f"    空态停 {args.hold:.0f} s（对照 v4 的空态）")
            await collect(transport, args.hold, report, "EMPTY-hold", None,
                          keepalive=card_session(PM_STATE["IDLE"], 0, -1, "", "",
                                                 "电量 97%  ·  ¥91.38"))

    if stage == "fallback":
        print("[fallback] 文本兜底：清掉位图槽后只发标题文本（缺字应显示 '?'，不应出方块/空白）")
        await transport.send(card_bitmap_clear(0))
        await asyncio.sleep(0.1)
        card = card_session(PM_STATE["RUNNING"], 7, 55, "bash",
                            "PROTORIG ES BLE 对齐 USB 开发任务", "电量 97%  ·  ¥91.38")
        await transport.send(card)
        await collect(transport, 1.0, report, "FALLBACK", {1})
        if args.hold > 0:
            print(f"    文本兜底停 {args.hold:.0f} s")
            await collect(transport, args.hold, report, "FALLBACK-hold", None, keepalive=card)

    if stage == "cycle":
        print("[cycle] 20 轮 会话→审批→问答→清卡（查残影/错位/坏帧）")
        for i in range(20):
            await transport.send(card_session(PM_STATE["RUNNING"], i, 30 + i, "bash", "轮次 %d" % i))
            await send_bitmap(transport, 0, "轮次 %d" % i)
            await transport.send(card_approval(1, 0, 9, "bash", "cmd-%d" % i))
            await send_bitmap(transport, 0, "cmd-%d" % i)
            await transport.send(card_question(0, 1, i % 2, (i % 3) + 1, i % 2,
                                               "题目 %d" % i, "选项 %d" % i, 3))
            await send_bitmap(transport, 0, "题目 %d" % i)
            await send_bitmap(transport, 1, "选项 %d" % i)
            await transport.send(card_clear())
            await asyncio.sleep(0.2)
        events = await collect(transport, 2.5, report, "CYCLE")
        bad = [e for e in events if e.cmd == EVT_ACK and e.payload and e.payload[0] in (3, 4)]
        cards = [e for e in events if e.cmd == EVT_ACK and e.payload and e.payload[0] == 1]
        bmps = [e for e in events if e.cmd == EVT_ACK and e.payload and e.payload[0] == 2]
        report.expect(not bad, f"20 轮无坏帧回执（bad={len(bad)}）")
        report.expect(len(cards) >= 40, f"卡片回执 {len(cards)} 条（4 卡 ×20 轮，允许合并）")
        report.expect(len(bmps) >= 40, f"位图收齐回执 {len(bmps)} 条（4 张 ×20 轮）")

    if stage in ("clear", "all"):
        print(f"[{stage}] 撤卡")
        try:
            await transport.send(card_clear())
            await collect(transport, 0.8, report, "CLEAR", {0})
        except Exception as exc:  # noqa: BLE001
            print(f"  !! 撤卡没发出去（链路已断）：{exc}")
    return report


def find_port() -> str:
    from serial.tools import list_ports
    for port in list_ports.comports():
        if port.vid == 0x349B and port.pid == 0x6160:
            return port.device
    raise SystemExit("找不到手表 USB 串口（VID 349B / PID 6160）")


async def main() -> int:
    ap = argparse.ArgumentParser(description="PROTORIG ES 卡片层台架验收")
    ap.add_argument("--transport", choices=["usb", "ble"], default="ble")
    ap.add_argument("--stage",
                    choices=["p1", "p2", "p3", "p4", "clear", "all",
                             "empty", "fallback", "cycle"], default="all")
    ap.add_argument("--port", default="", help="USB 应用口（默认自动找 VID 349B）")
    ap.add_argument("--address", default="", help="BLE MAC（默认按广播名扫描）")
    ap.add_argument("--name", default="PROTORIG ES", help="BLE 广播名前缀")
    ap.add_argument("--hold", type=float, default=0.0, help="每张卡停留秒数（等真人点屏幕）")
    ap.add_argument("--keep", action="store_true", help="结束后不退出电脑模式/不断开")
    args = ap.parse_args()

    transport = None
    try:
        if args.transport == "usb":
            port = args.port or find_port()
            print(f"[usb] 打开 {port}")
            transport = UsbTransport(port)
            print("[usb] pcmode 1（不发 HELLO：保持本地页可见）")
            transport.command("pcmode 1")
            if not transport.wait_line("@PCM1 PC 1", 3.0):
                print("  !! 没等到 @PCM1 PC 1，继续尝试（卡片帧可能被丢）")
            # shell 的 pcmode 不会切页面；卡片画在监控页（-1 屏）上，得把它叫出来
            transport.command("pcmon view monitor")
            await asyncio.sleep(0.5)
        else:
            transport = BleTransport(args.address or None, args.name)
            print("[ble] 连接手表…")
            await transport.open()
            print("[ble] 已连接")

        report = await run_stages(transport, args)
        print(f"\n=== {report.passed} PASS / {report.failed} FAIL ===")
        if not args.keep:
            if args.transport == "usb":
                transport.command("pcmode 0")
                await asyncio.sleep(0.3)
            else:
                await transport.send_cmd(CMD_PC_MODE, bytes([0]))
                await asyncio.sleep(0.3)
        return 0 if report.failed == 0 else 1
    finally:
        if transport is not None:
            if args.transport == "usb":
                transport.close()
            else:
                await transport.close()


if __name__ == "__main__":
    try:
        raise SystemExit(asyncio.run(main()))
    except KeyboardInterrupt:
        raise SystemExit(130)
