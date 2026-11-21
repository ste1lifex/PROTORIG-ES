"""Build and render the actual LVGL device pages and exercise their input paths."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import hashlib
import os
import subprocess

from PIL import Image, ImageDraw, ImageFont
from dev_env import get as dev_get

_SDK_BASE = dev_get("BL_SDK_BASE")
if not _SDK_BASE:
    raise SystemExit("缺少 BL_SDK_BASE：设置环境变量，或写入 firmware/dev-env.json（见 firmware/README.md）")
SDK = Path(_SDK_BASE) / "components/graphics/lvgl_v9"
CC = dev_get("HOST_CC", "gcc")

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tools/host"
OUT = ROOT / "tools/ui_host_output"
OUT.mkdir(exist_ok=True)
flags = ["-O1", "-std=c99", "-DLV_KCONFIG_IGNORE", '-DLV_CONF_PATH="' + (HOST / "lv_conf.h").as_posix() + '"']
for directory in (HOST, ROOT, ROOT / "apps", ROOT / "ui", ROOT / "drivers", ROOT / "bsp",
                  ROOT / "services", ROOT / "app", ROOT / "tests/host/stubs", SDK):
    flags += ["-I" + str(directory)]
sources = [p for p in SDK.rglob("*.c") if "port" not in p.relative_to(SDK).parts]
sources += [ROOT / "fonts_gen" / f"lv_font_ui{size}.c" for size in (14, 16, 20, 28, 48)]
sources += [ROOT / p for p in ("ui/ui_widgets.c", "ui/device_pages.c", "ui/app_pages.c",
                              "apps/app_list.c", "apps/app_notify.c",
                              "services/svc_stopwatch.c", "services/svc_timer.c",
                              "services/svc_alarm.c",
                              "eez_ui_glue.c", "tools/host/device_ui_test.c")]


def compile_one(source):
    obj = OUT / (hashlib.sha1(str(source).encode()).hexdigest()[:14] + ".o")
    if not source.is_relative_to(SDK) or not obj.exists() or obj.stat().st_mtime < max(
        source.stat().st_mtime, (HOST / "lv_conf.h").stat().st_mtime
    ):
        result = subprocess.run([CC, *flags, "-c", str(source), "-o", str(obj)],
                                capture_output=True, text=True, encoding="utf-8")
        if result.returncode:
            raise RuntimeError(str(source) + "\n" + result.stderr)
    return obj


with ThreadPoolExecutor(max_workers=8) as pool:
    objects = list(pool.map(compile_one, sources))
rsp = OUT / "link.rsp"
rsp.write_text("\n".join('"' + p.as_posix() + '"' for p in objects), encoding="utf-8")
exe = OUT / "device_ui_test.exe"
subprocess.run([CC, "@" + str(rsp), "-lm", "-o", str(exe)], check=True)
subprocess.run([str(exe)], cwd=OUT, check=True)

names = [
    ("apps", "应用"), ("settings", "终端设置"), ("brightness", "屏幕亮度"),
    ("timeout", "自动息屏"), ("connection", "连接模式"), ("network", "协议网络"),
    ("encounters", "相遇记录"), ("apps_page2", "第二批应用"),
    ("stopwatch", "秒表"), ("stopwatch_run", "秒表·计时中"),
    ("timer", "计时器"), ("timer_run", "计时器·倒计时"),
    ("alarm", "闹钟"), ("alarm_on", "闹钟·开启"),
    ("notify", "通知卡(圆屏)"), ("notify_alarm", "通知卡·第二条"),
]
font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 22)
sheet = Image.new("RGB", (1560, 3260), "#101411")
draw = ImageDraw.Draw(sheet)
for i, (name, title) in enumerate(names):
    image = Image.open(OUT / (name + ".ppm")).convert("RGB")
    mask = Image.new("L", image.size, 0)
    ImageDraw.Draw(mask).ellipse((0, 0, 479, 479), fill=255)
    cropped = Image.new("RGB", image.size, "#101411")
    cropped.paste(image, mask=mask)
    cropped.save(OUT / (name + ".png"))
    x = 20 + (i % 3) * 520
    y = 40 + (i // 3) * 540
    sheet.paste(cropped, (x, y))
    draw.text((x + 240, y - 30), title, font=font, fill="#F0F0F0", anchor="mt")
sheet.save(OUT / "overview.png")
print(OUT / "overview.png")
