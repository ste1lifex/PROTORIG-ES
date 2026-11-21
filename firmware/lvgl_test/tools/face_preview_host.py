"""face_preview_host.py —— 在 PC 上渲染真实的 0 表盘，产出插件预览图

用固件自己的 face_lvgl.c + watch_photo.c + fonts_gen/*.c 编一个主机版渲染器
（tools/host/face_preview.c，替身见该文件注释），跑一帧 480×480，然后：

  tools/face_host_output/face.ppm           原始帧
  tools/face_host_output/face.png           同上，PNG
  plugin/dsh-protorig-panel/assets/faces/default.png   插件设置页「默认表盘」预览

为什么要真渲染：手工拼的预览图和真机差着表圈刻度、LOCAL·01、电量、分页点，
2026-11-15 用户反馈"和表上完全不一样"。

用法（改完 watch_photo / face_lvgl 之后重跑）：
  python firmware/lvgl_test/tools/face_preview_host.py
  python firmware/lvgl_test/tools/face_preview_host.py --time 22:47:36
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image
from dev_env import get as dev_get

_SDK_BASE = dev_get("BL_SDK_BASE")
if not _SDK_BASE:
    raise SystemExit("缺少 BL_SDK_BASE：设置环境变量，或写入 firmware/dev-env.json（见 firmware/README.md）")
SDK = Path(_SDK_BASE) / "components/graphics/lvgl_v9"
CC = dev_get("HOST_CC", "gcc")

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tools/host"
OUT = ROOT / "tools/face_host_output"
PLUGIN_PREVIEW = ROOT.parents[1] / "plugin/dsh-protorig-panel/assets/faces/default.png"

FONTS = ("lv_font_num104", "lv_font_ascii28", "lv_font_ui20", "lv_font_ui16", "lv_font_ui14")

flags = ["-O1", "-std=c99", "-DLV_KCONFIG_IGNORE",
         '-DLV_CONF_PATH="' + (HOST / "lv_conf.h").as_posix() + '"']
for directory in (HOST, ROOT, ROOT / "apps", ROOT / "ui", ROOT / "drivers", ROOT / "bsp",
                  ROOT / "services", ROOT / "app", ROOT / "tests/host/stubs", SDK):
    flags += ["-I" + str(directory)]

sources = [p for p in SDK.rglob("*.c") if "port" not in p.relative_to(SDK).parts]
sources += [ROOT / "fonts_gen" / f"{name}.c" for name in FONTS]
sources += [ROOT / "face_lvgl.c", ROOT / "watch_photo.c", HOST / "face_preview.c"]


def compile_one(source: Path) -> Path:
    obj = OUT / (hashlib.sha1(str(source).encode()).hexdigest()[:14] + ".o")
    if (not str(source).startswith(str(SDK)) or not obj.exists()
            or obj.stat().st_mtime < max(source.stat().st_mtime,
                                         (HOST / "lv_conf.h").stat().st_mtime)):
        result = subprocess.run([CC, *flags, "-c", str(source), "-o", str(obj)],
                                capture_output=True, text=True, encoding="utf-8")
        if result.returncode:
            raise RuntimeError(str(source) + "\n" + result.stderr)
    return obj


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--time", default="22:47:36")
    parser.add_argument("--no-copy", action="store_true", help="只渲染，不覆盖插件预览")
    args = parser.parse_args()
    hh, mm, ss = (int(x) for x in args.time.split(":"))

    OUT.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        objects = list(pool.map(compile_one, sources))

    rsp = OUT / "link.rsp"
    rsp.write_text("\n".join('"' + p.as_posix() + '"' for p in objects), encoding="utf-8")
    exe = OUT / "face_preview.exe"
    subprocess.run([CC, "@" + str(rsp), "-lm", "-o", str(exe)], check=True)

    ppm = OUT / "face.ppm"
    subprocess.run([str(exe), str(ppm), str(hh), str(mm), str(ss)], cwd=OUT, check=True)
    png = OUT / "face.png"
    Image.open(ppm).save(png)
    print(f"rendered -> {png}")

    if not args.no_copy:
        PLUGIN_PREVIEW.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(png, PLUGIN_PREVIEW)
        print(f"plugin preview -> {PLUGIN_PREVIEW}")


if __name__ == "__main__":
    main()
