#!/usr/bin/env python3
"""PROTORIG ES PC-side BLE test client (bleak / Windows WinRT).

用法:
    python ble_pc_test.py --scan-only
    python ble_pc_test.py --test
    python ble_pc_test.py --test --no-set-time
    python ble_pc_test.py --test --name PROTORIG

前提:
    - 手表 R4 固件已启动且正在广播（若已连手机，先在手机上断开/关蓝牙）。
    - Windows 蓝牙已打开，手表离电脑 1 m 内。
"""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import sys
from typing import Any

from bleak import BleakClient, BleakScanner

TARGET_HINT = "PROTORIG"

UUID_DIS = "0000180a-0000-1000-8000-00805f9b34fb"
UUID_BAS = "0000180f-0000-1000-8000-00805f9b34fb"
UUID_CTS = "00001805-0000-1000-8000-00805f9b34fb"
UUID_CUSTOM = "50524f54-4f52-4947-4553-000000000001"

CHR_MANUFACTURER = "00002a29-0000-1000-8000-00805f9b34fb"
CHR_MODEL = "00002a24-0000-1000-8000-00805f9b34fb"
CHR_FW = "00002a26-0000-1000-8000-00805f9b34fb"
CHR_BATT = "00002a19-0000-1000-8000-00805f9b34fb"
CHR_CTS = "00002a2b-0000-1000-8000-00805f9b34fb"

CHR_CMD = "50524f54-4f52-4947-4553-000000000002"
CHR_EVT = "50524f54-4f52-4947-4553-000000000003"

FRAME_A5 = 0xA5
FRAME_5A = 0x5A
CMD_PING = 0x01
CMD_GET_INFO = 0x02
CMD_SET_TIME = 0x03
CMD_NOTIFY = 0x20
CMD_FIND = 0x22


def hexs(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def frame(cmd: int, payload: bytes = b"") -> bytes:
    return bytes([FRAME_A5, FRAME_5A, cmd, len(payload)]) + payload


def cts_now() -> bytes:
    now = dt.datetime.now()
    # CTS: year(uint16 LE), month, day, hours, minutes, seconds,
    #      day_of_week(1=Monday), fractions256, reason
    return bytes(
        [
            now.year & 0xFF,
            (now.year >> 8) & 0xFF,
            now.month,
            now.day,
            now.hour,
            now.minute,
            now.second,
            now.isoweekday(),  # 1=Monday .. 7=Sunday
            0,
            0,
        ]
    )


def parse_cts(data: bytes) -> str:
    if len(data) < 10:
        return f"invalid CTS len={len(data)}: {hexs(data)}"
    year = data[0] | (data[1] << 8)
    return (
        f"{year:04d}-{data[2]:02d}-{data[3]:02d} "
        f"{data[4]:02d}:{data[5]:02d}:{data[6]:02d} "
        f"weekday={data[7]} fractions={data[8]} reason={data[9]}"
    )


async def scan(timeout: float = 10.0) -> Any:
    print(f"[scan] {timeout:.0f}s ...")
    found = await BleakScanner.discover(timeout=timeout, return_adv=True)
    candidates = []
    for addr, (dev, adv) in found.items():
        name = adv.local_name or dev.name or ""
        if TARGET_HINT.lower() in name.lower():
            candidates.append((dev, adv, name))
            print(
                f"  FOUND {name!r}  addr={dev.address}  rssi={adv.rssi} "
                f"services={list(adv.service_uuids or [])}"
            )
    if not candidates:
        print(f"  no device containing {TARGET_HINT!r}; total seen={len(found)}")
        for addr, (dev, adv) in list(found.items())[:10]:
            print(f"    - {dev.name!r} {addr} rssi={adv.rssi}")
        return None
    # 名字完全匹配优先
    candidates.sort(key=lambda x: (x[2].lower() != "protorig es", -x[1].rssi))
    return candidates[0][0]


async def dump_services(client: BleakClient) -> None:
    print("\n[services]")
    for service in client.services:
        print(f"  service {service.uuid}  {service.description}")
        for char in service.characteristics:
            print(f"    char {char.uuid}  props={char.properties}")


async def read_text(client: BleakClient, uuid: str, label: str) -> str | None:
    try:
        val = await client.read_gatt_char(uuid)
        try:
            text = val.decode("utf-8", errors="replace").strip("\x00")
        except Exception:
            text = hexs(val)
        print(f"  {label}: {text!r}  ({hexs(val)})")
        return text
    except Exception as exc:
        print(f"  {label}: FAILED {exc}")
        return None


async def wait_notification(evt: asyncio.Event, buf: list[tuple[str, bytes]], timeout: float = 3.0) -> tuple[str, bytes] | None:
    try:
        await asyncio.wait_for(evt.wait(), timeout=timeout)
    except asyncio.TimeoutError:
        return None
    evt.clear()
    return buf[-1] if buf else None


async def run_test(dev: Any, set_time: bool) -> int:
    print(f"\n[connect] {dev.name!r} {dev.address}")
    ok = True
    async with BleakClient(dev, timeout=25) as client:
        print(f"[connect] connected={client.is_connected}")
        await dump_services(client)

        print("\n[read DIS/BAS/CTS]")
        await read_text(client, CHR_MANUFACTURER, "manufacturer")
        await read_text(client, CHR_MODEL, "model")
        await read_text(client, CHR_FW, "firmware")
        try:
            batt = await client.read_gatt_char(CHR_BATT)
            print(f"  battery: {batt[0]}%  ({hexs(batt)})")
        except Exception as exc:
            print(f"  battery: FAILED {exc}")
            ok = False
        try:
            cts = await client.read_gatt_char(CHR_CTS)
            print(f"  CTS: {parse_cts(cts)}")
        except Exception as exc:
            print(f"  CTS: FAILED {exc}")
            ok = False

        # 通知
        evt = asyncio.Event()
        notifications: list[tuple[str, bytes]] = []

        def on_notify(sender: Any, data: bytearray) -> None:
            b = bytes(data)
            notifications.append((str(sender), b))
            print(f"  [notify] {b.hex(' ')}")
            evt.set()

        try:
            await client.start_notify(CHR_EVT, on_notify)
            print("\n[notify] enabled on custom evt char")
        except Exception as exc:
            print(f"\n[notify] enable FAILED {exc}")
            ok = False

        async def send(cmd: int, payload: bytes, label: str, timeout: float = 3.0) -> bytes | None:
            nonlocal ok
            data = frame(cmd, payload)
            print(f"\n[{label}] write {hexs(data)}")
            try:
                await client.write_gatt_char(CHR_CMD, data, response=True)
            except Exception as exc:
                print(f"  write FAILED {exc}")
                ok = False
                return None
            got = await wait_notification(evt, notifications, timeout)
            if got:
                print(f"  response: {got[1].hex(' ')}")
                return got[1]
            print("  response: TIMEOUT")
            ok = False
            return None

        await send(CMD_PING, b"", "PING", timeout=2.0)
        await send(CMD_GET_INFO, b"", "GET_INFO", timeout=3.0)

        if set_time:
            cts = cts_now()
            print(f"\n[SET_TIME] {parse_cts(cts)}")
            await send(CMD_SET_TIME, cts, "SET_TIME", timeout=3.0)

        # FIND：手表应连续震 3 次
        await send(CMD_FIND, b"", "FIND", timeout=2.0)

        # NOTIFY：cat=1, tlen=len(title), title + 0x00 + body
        title = "PC测试".encode("utf-8")
        body = "来自电脑的 BLE 通知".encode("utf-8")
        payload = bytes([0x01, len(title)]) + title + b"\x00" + body
        await send(CMD_NOTIFY, payload, "NOTIFY", timeout=2.0)

        try:
            await client.stop_notify(CHR_EVT)
        except Exception:
            pass

    print("\n[result]", "PASS" if ok else "FAIL")
    return 0 if ok else 1


async def main_async(args: argparse.Namespace) -> int:
    dev = await scan(args.scan_timeout)
    if dev is None:
        return 2
    if args.scan_only:
        return 0
    return await run_test(dev, set_time=not args.no_set_time)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scan-only", action="store_true")
    ap.add_argument("--test", action="store_true")
    ap.add_argument("--no-set-time", action="store_true")
    ap.add_argument("--scan-timeout", type=float, default=10.0)
    args = ap.parse_args()
    if not args.scan_only:
        args.test = True
    try:
        return asyncio.run(main_async(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
