#!/usr/bin/env python3
"""apply_sdk_patches.py —— 构建前临时给 Bouffalo SDK 打 PROTORIG 两个补丁（跨平台）。

与 Windows 的 ``build.ps1`` 做的事**完全等价**，只是不依赖 PowerShell，
所以在 macOS / Linux 上也能用：

  1. ``bsp/board/bl616dk/board.c``
     在 ``extern void log_start(void);`` 前插入弱符号
     ``protorig_usb_console_delay``，并把 ``usb_console_init()`` 包成
     ``if (!protorig_usb_console_delay) { ... }``。
     原因：BL616 的 USB 时钟来自 WIFIPLL，BLE 的 rfparam_init() 会把它重配，
     导致已枚举的 USB 掉线；延迟到 BLE 广播后再初始化 USB 才能共存
     （见 docs/BUILD_SDK_PATCH.md、research/34）。

  2. ``bsp/common/usb_app/usb_console.c``
     在 ``shell_exe_cmd()`` 前插入弱钩子 ``protorig_usb_pc_rx()``，
     让固件在电脑模式下直接收 ``A5 5A len`` 二进制像素帧
     （见 research/56 §8、bsp/usb_pc_link.c）。

安全约定（与 build.ps1 一致）：
  * 打补丁前先备份到 ``<file>.protorig-bak``；restore 时原样写回并删备份；
  * 锚点找不到 / 补丁文件缺失 → 非零退出，绝不静默继续；
  * 已经打过补丁（源码里出现标记）→ 拒绝重复打补丁，直接报错；
  * 换行符自适应：SDK 在 Windows 上可能是 CRLF、macOS/Linux 上是 LF。

用法：
  python3 sdk_patches/apply_sdk_patches.py --sdk /path/to/bouffalo_sdk --apply
  python3 sdk_patches/apply_sdk_patches.py --sdk /path/to/bouffalo_sdk --restore
  python3 sdk_patches/apply_sdk_patches.py --sdk /path/to/bouffalo_sdk --check
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys

BOARD_REL = os.path.join("bsp", "board", "bl616dk", "board.c")
USB_REL = os.path.join("bsp", "common", "usb_app", "usb_console.c")
BAK_SUFFIX = ".protorig-bak"

# ---------------------------------------------------------------- 补丁 1：board.c
BOARD_ANCHOR = "extern void log_start(void);"
BOARD_MARK = "protorig_usb_console_delay"
BOARD_INSERT_LINES = [
    "/* R4-usbdelay: app may strongly define protorig_usb_console_delay=1 to delay",
    " * USB console init until after BLE/RF init. Weak default 0 keeps other apps",
    " * (probe etc.) unchanged. */",
    "__attribute__((weak)) int protorig_usb_console_delay = 0;",
    "",
]
BOARD_OLD = "#ifdef CONFIG_BSP_CONSOLE_USB_CDC\n    usb_console_init();\n#endif"
BOARD_NEW = (
    "#ifdef CONFIG_BSP_CONSOLE_USB_CDC\n"
    "    if (!protorig_usb_console_delay) {\n"
    "        usb_console_init();\n"
    "    }\n"
    "#endif"
)

# ------------------------------------------------------- 补丁 2：usb_console.c
USB_ANCHOR = "static const uint8_t usb_console_device_descriptor[] = {"
USB_MARK = "protorig_usb_pc_rx"
USB_INSERT_LINES = [
    "/* protorig-usbpc: raw CDC RX hook.",
    " * The PROTORIG ES app strongly defines protorig_usb_pc_rx() to take binary",
    " * pixel frames in PC mode (A5 5A len payload, see bsp/usb_pc_link.c); the",
    " * weak default keeps stock shell behaviour for every other app. Return",
    " * 1 = consumed, 0 = hand the bytes to the shell. */",
    "__attribute__((weak)) int protorig_usb_pc_rx(const uint8_t *buf, uint16_t len)",
    "{",
    "    (void)buf;",
    "    (void)len;",
    "    return 0;",
    "}",
    "",
    "/* protorig-usbpc: number of completed USB IN transfers. The app watches",
    " * this counter to detect a dead PC app while the cable is still plugged",
    " * in (closing the PC process stops the host from reading our IN data). */",
    "volatile uint32_t protorig_usb_console_in_events = 0;",
    "",
]
USB_OLD = (
    "            if ((rx_len != 0U) && (rx_len <= sizeof(usb_console_rx_buffer))) {\n"
    "                shell_exe_cmd(usb_console_rx_buffer, (uint16_t)rx_len);\n"
    "            }"
)
USB_NEW = (
    "            if ((rx_len != 0U) && (rx_len <= sizeof(usb_console_rx_buffer))) {\n"
    "                /* protorig-usbpc: 1 = binary frame consumed, 0 = shell */\n"
    "                if (!protorig_usb_pc_rx(usb_console_rx_buffer, (uint16_t)rx_len)) {\n"
    "                    shell_exe_cmd(usb_console_rx_buffer, (uint16_t)rx_len);\n"
    "                }\n"
    "            }"
)
# 主机存活计数：usb_console_bulk_in_cb 里每次 IN 完成 +1。PC 端进程被杀后没人再读 IN 端点，
# 这个计数停止增长 —— 固件（svc_pc.c 的 pc_host_watch）据此判定"主机消失"。
# 这一步 build.ps1 一直是有的，本脚本早先漏了 → macOS 编译必然
# `undefined reference to 'protorig_usb_console_in_events'`。
IN_OLD = "    usb_console.tx_done_pending = true;"
IN_NEW = (
    "    protorig_usb_console_in_events++;\n"
    "    usb_console.tx_done_pending = true;"
)


class PatchError(RuntimeError):
    pass


def read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8", newline="") as fh:
        return fh.read()


def write_text(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)


def eol_of(text: str) -> str:
    return "\r\n" if "\r\n" in text else "\n"


def normalize(text: str) -> str:
    return text.replace("\r\n", "\n")


def apply_patch(path: str, anchor: str, insert_lines: list[str], mark: str,
                replacements: list[tuple[str, str]]) -> None:
    text = read_text(path)
    if mark in text:
        raise PatchError(f"{path}: already patched (found '{mark}')，拒绝重复打补丁")
    eol = eol_of(text)
    norm = normalize(text)

    if anchor not in norm:
        raise PatchError(f"{path}: 锚点未找到: {anchor!r}（SDK 版本不匹配？）")
    insert = eol.join(insert_lines) + eol
    norm = norm.replace(anchor, insert + anchor, 1)

    for old, new in replacements:
        if old not in norm:
            raise PatchError(f"{path}: 待替换代码块未找到（SDK 版本不匹配？）:\n{old}")
        norm = norm.replace(old, new, 1)

    backup = path + BAK_SUFFIX
    if os.path.exists(backup):
        raise PatchError(f"{backup} 已存在（上次构建没有正常恢复？先手工检查）")
    shutil.copy2(path, backup)
    write_text(path, norm.replace("\n", eol))


def restore(path: str) -> bool:
    backup = path + BAK_SUFFIX
    if not os.path.exists(backup):
        return False
    shutil.copy2(backup, path)
    os.remove(backup)
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description="PROTORIG SDK 补丁（apply/restore/check）")
    ap.add_argument("--sdk", required=True, help="bouffalo_sdk 根目录（BL_SDK_BASE）")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--apply", action="store_true", help="构建前打补丁（幂等检查）")
    g.add_argument("--restore", action="store_true", help="构建后恢复原始 SDK")
    g.add_argument("--check", action="store_true", help="只检查是否已打补丁")
    args = ap.parse_args()

    sdk = os.path.abspath(args.sdk)
    board = os.path.join(sdk, BOARD_REL)
    usb = os.path.join(sdk, USB_REL)
    for p in (board, usb):
        if not os.path.exists(p):
            raise PatchError(f"SDK 文件不存在: {p}")

    if args.check:
        board_text = normalize(read_text(board))
        usb_text = normalize(read_text(usb))
        ok = BOARD_MARK in board_text and USB_MARK in usb_text
        print(f"[sdk-patch] {'已打补丁' if ok else '未打补丁'}（{sdk}）")
        return 0 if ok else 1

    if args.restore:
        restored = [p for p in (board, usb) if restore(p)]
        for p in restored:
            print(f"[sdk-patch] restored {os.path.relpath(p, sdk)}")
        if not restored:
            print("[sdk-patch] nothing to restore")
        return 0

    # --apply
    try:
        apply_patch(
            board,
            BOARD_ANCHOR,
            BOARD_INSERT_LINES,
            BOARD_MARK,
            [(BOARD_OLD, BOARD_NEW)],
        )
        print(f"[sdk-patch] applied board.c (usb console delay)")
        apply_patch(
            usb,
            USB_ANCHOR,
            USB_INSERT_LINES,
            USB_MARK,
            [(USB_OLD, USB_NEW), (IN_OLD, IN_NEW)],
        )
        print(f"[sdk-patch] applied usb_console.c (raw CDC RX hook)")
    except PatchError:
        # 失败时把已经打上的补丁回滚，避免 SDK 停在半打补丁状态
        restore(board)
        restore(usb)
        raise
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PatchError as exc:
        print(f"[sdk-patch] ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
