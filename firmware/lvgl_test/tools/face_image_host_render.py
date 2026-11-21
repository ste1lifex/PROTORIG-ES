"""主机侧渲染验证：把 .pfi 的 I8 图交给真 LVGL 渲染一帧并逐像素对拍。

对应契约 70 §6 的头号风险「XIP 读 media 区不生效/花屏」的另一半：
先排除 XIP，只验证 LVGL 对「palette 在前、像素在后」的 I8 图语义是否理解正确。
（真机 XIP 那一半只能上板验。）

做法：
  1) 用 HOST_CC（环境变量或 firmware/dev-env.json，默认 PATH 里的 gcc）把 SDK 的 LVGL 源码编成 .o
     —— 和 tools/device_ui_host.py 用同一套 flags / 同一份 tools/host/lv_conf.h，
        所以能直接复用 tools/ui_host_output 里已有的对象缓存；
  2) 编 tools/host/face_image_test.c，链接成一个独立 exe（不掺任何固件源文件）；
  3) 跑 exe，读 .pfi、渲染、逐像素对拍；顺便把结果转成 PNG 方便肉眼看。

用法：
  python tools/face_image_host_render.py tmp/demo.pfi
  python tools/face_image_host_render.py tmp/demo.pfi --keep-ppm
"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from dev_env import get as dev_get

_SDK_BASE = dev_get("BL_SDK_BASE")
if not _SDK_BASE:
    raise SystemExit("缺少 BL_SDK_BASE：设置环境变量，或写入 firmware/dev-env.json（见 firmware/README.md）")
SDK = Path(_SDK_BASE) / "components/graphics/lvgl_v9"
CC = dev_get("HOST_CC", "gcc")

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tools" / "host"
CACHE_REUSE = ROOT / "tools" / "ui_host_output"      # device_ui_host.py 的对象缓存
OUT = ROOT / "tools" / "host_output_face"


def obj_name(source: Path) -> str:
    return hashlib.sha1(str(source).encode()).hexdigest()[:14] + ".o"


def flags() -> list[str]:
    f = ["-O1", "-std=c99", "-DLV_KCONFIG_IGNORE",
         '-DLV_CONF_PATH="' + (HOST / "lv_conf.h").as_posix() + '"']
    for d in (HOST, ROOT, ROOT / "apps", ROOT / "ui", ROOT / "drivers", ROOT / "bsp",
              ROOT / "services", ROOT / "app", ROOT / "tests/host/stubs", SDK):
        f += ["-I" + str(d)]
    return f


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pfi")
    ap.add_argument("--out", default=None, help="输出 PNG（默认 tmp/face_image_host.png）")
    ap.add_argument("--keep-ppm", action="store_true")
    args = ap.parse_args()

    if not Path(CC).exists():
        print("找不到 gcc: %s（用环境变量 HOST_CC 覆盖）" % CC)
        return 2
    if not Path(args.pfi).is_file():
        print("找不到 %s" % args.pfi)
        return 2

    OUT.mkdir(exist_ok=True)
    fl = flags()
    sdk_sources = [p for p in SDK.rglob("*.c") if "port" not in p.relative_to(SDK).parts]
    test_src = HOST / "face_image_test.c"

    # 选对象缓存目录：优先复用 device_ui_host.py 的（同 flags），缺的再自己编
    def cached_obj(src: Path) -> Path:
        for d in (CACHE_REUSE, OUT):
            p = d / obj_name(src)
            if p.exists():
                return p
        return OUT / obj_name(src)

    todo = [s for s in sdk_sources if not cached_obj(s).exists()]
    test_obj = cached_obj(test_src)
    if (not test_obj.exists()) or test_obj.stat().st_mtime < test_src.stat().st_mtime:
        todo.append(test_src)

    def compile_one(src: Path):
        out = OUT / obj_name(src)
        if out.exists() and out.stat().st_mtime >= src.stat().st_mtime:
            return out
        r = subprocess.run([CC, *fl, "-c", str(src), "-o", str(out)],
                           capture_output=True, text=True, encoding="utf-8")
        if r.returncode:
            raise RuntimeError("%s\n%s" % (src, r.stderr))
        return out

    if todo:
        print("编译 %d 个源文件（首次较慢）..." % len(todo))
        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(compile_one, todo))

    objs = [cached_obj(s) for s in sdk_sources]
    missing = [o for o in objs if not o.exists()]
    if missing:
        print("缺少 %d 个对象文件，重跑一次；示例：%s" % (len(missing), missing[0]))
        return 3
    objs.append(OUT / obj_name(test_src))

    exe = OUT / "face_image_test.exe"
    rsp = OUT / "link.rsp"
    rsp.write_text("\n".join('"' + p.as_posix() + '"' for p in objs), encoding="utf-8")
    subprocess.run([CC, "@" + str(rsp), "-lm", "-o", str(exe)], check=True)

    ppm = OUT / "face_image_host.ppm"
    rc = subprocess.run([str(exe), str(Path(args.pfi).resolve()), str(ppm)]).returncode

    png = Path(args.out) if args.out else ROOT / "tmp" / "face_image_host.png"
    try:
        from PIL import Image

        png.parent.mkdir(parents=True, exist_ok=True)
        Image.open(ppm).save(png)
        print("[png] %s" % png)
    except Exception as e:      # 没有 Pillow 也不影响对拍结论
        print("PNG 转换失败（不影响对拍）：%s" % e)
    if not args.keep_ppm:
        try:
            ppm.unlink()
        except OSError:
            pass
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
