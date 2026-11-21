#!/usr/bin/env python3
"""build_panel_fonts.py —— 生成面板用的中文字体子集（macOS / Windows 完全一致）

背景（2026-11-09）：
  面板文字由电脑侧 Skia/resvg 渲染，再以像素推给手表。渲染器原来写死
  `SHSans`（Windows 开发机注册的别名），macOS 没这个字体 → 中文全变方块。
  发行版要求：**不能依赖系统字体**（两端字体不一致），也不能有 npm 之外的依赖。
  所以把一份 OFL 中文字体**子集化后放进 npm 包**，运行时注册同一个家族名。

字体来源：Google Fonts 的 Noto Sans SC 可变字体（OFL-1.1，无 Reserved Font Name
冲突：上游 OFL 是 Adobe + Reserved Font Name 'Source'，我们改名后分发，合规）。
  上游：https://github.com/google/fonts/tree/main/ofl/notosanssc
  本脚本在构建机上联网下载 VF，实例化 400/500/700 → 子集 → 改名 → 输出到 assets/fonts/。

用法：
  python3 tools/build_panel_fonts.py [--download-dir ~/Developer/.tools/noto-fonts]
  # 产物：assets/fonts/ProtorigPanelSans-{Regular,Medium,Bold}.ttf + OFL.txt + SOURCES.json

子集范围（够面板动态文本用，缺字由系统字体兜底）：
  ASCII / Latin-1 / 常用标点 / 箭头、几何符号 / CJK 标点 / 全角 / GB2312 汉字。
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT_DIR = HERE.parent / "assets" / "fonts"

VF_URL = "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
OFL_URL = "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/OFL.txt"
VF_NAME = "NotoSansSC-VF.ttf"
FAMILY = "PROTORIG Panel Sans"
WEIGHTS = {400: ("Regular", "ProtorigPanelSans-Regular"), 500: ("Medium", "ProtorigPanelSans-Medium"), 700: ("Bold", "ProtorigPanelSans-Bold")}


def gb2312_chars() -> set[str]:
    chars = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return chars


def unicode_ranges() -> list[tuple[int, int]]:
    ranges = [
        (0x0020, 0x007E),  # ASCII
        (0x00A0, 0x00FF),  # Latin-1
        (0x2000, 0x206F),  # General Punctuation（…、–、—、'' 等）
        (0x2190, 0x21FF),  # Arrows（→ 等）
        (0x25A0, 0x25FF),  # Geometric Shapes（● ▶ 等）
        (0x2600, 0x26FF),  # Misc Symbols
        (0x2700, 0x27BF),  # Dingbats（✓ ✗ 等）
        (0x3000, 0x303F),  # CJK 标点
        (0xFF00, 0xFFEF),  # 全角
    ]
    return ranges


def ensure_source(download_dir: Path) -> tuple[Path, Path]:
    download_dir.mkdir(parents=True, exist_ok=True)
    vf = download_dir / VF_NAME
    ofl = download_dir / "OFL.txt"
    if not vf.exists():
        print(f"[fonts] 下载可变字体 → {vf}")
        urllib.request.urlretrieve(VF_URL, vf)
    if not ofl.exists():
        print(f"[fonts] 下载 OFL → {ofl}")
        urllib.request.urlretrieve(OFL_URL, ofl)
    return vf, ofl


def build_weight(vf: Path, wght: int, out_path: Path, unicodes: set[int]) -> None:
    from fontTools import subset
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer

    font = TTFont(vf)
    instance = instancer.instantiateVariableFont(font, {"wght": wght}, updateFontNames=False, inplace=False)

    options = subset.Options()
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.notdef_outline = True
    options.recalc_bounds = True
    options.drop_tables += ["DSIG"]
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=sorted(unicodes))
    subsetter.subset(instance)

    # 改名：家族统一为 FAMILY，权重按 400/500/700（OFL 要求改名后才能分发子集）
    subfamily, ps_name = WEIGHTS[wght]
    name = instance["name"]
    full = f"{FAMILY} {subfamily}"
    records = {
        1: FAMILY,
        2: subfamily,
        3: f"{full}; {wght}; 2026",
        4: full,
        6: ps_name,
        16: FAMILY,
        17: subfamily,
    }
    for name_id, value in records.items():
        name.setName(value, name_id, 3, 1, 0x409)  # Windows Unicode English
        name.setName(value, name_id, 1, 0, 0)      # Mac Roman
    if "OS/2" in instance:
        instance["OS/2"].usWeightClass = wght
    instance.save(out_path)
    print(f"[fonts] {out_path.name}: {wght} → {out_path.stat().st_size / 1024:.0f} KB")


def main() -> int:
    ap = argparse.ArgumentParser(description="生成面板用的中文字体子集（OFL）")
    ap.add_argument("--download-dir", default=os.path.expanduser("~/Developer/.tools/noto-fonts"))
    ap.add_argument("--keep-vf", action="store_true", help="保留下载的可变字体")
    args = ap.parse_args()

    try:
        import fontTools  # noqa: F401
    except ImportError:
        print("[fonts] 需要 fonttools：python3 -m pip install --user fonttools", file=sys.stderr)
        return 1

    vf, ofl = ensure_source(Path(args.download_dir))
    unicodes = set()
    for lo, hi in unicode_ranges():
        unicodes.update(range(lo, hi + 1))
    gb = gb2312_chars()
    unicodes.update(ord(ch) for ch in gb)
    print(f"[fonts] 子集字符数: {len(unicodes)}（含 GB2312 {len(gb)}）")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for wght in WEIGHTS:
        _, ps = WEIGHTS[wght]
        build_weight(vf, wght, OUT_DIR / f"{ps}.ttf", unicodes)

    (OUT_DIR / "OFL.txt").write_text(ofl.read_text(encoding="utf-8"), encoding="utf-8")
    (OUT_DIR / "SOURCES.json").write_text(
        json.dumps(
            {
                "family": FAMILY,
                "upstream": "Noto Sans SC (Google Fonts, OFL-1.1)",
                "source": VF_URL,
                "license": "OFL-1.1",
                "generator": "tools/build_panel_fonts.py",
                "weights": {"Regular": 400, "Medium": 500, "Bold": 700},
                "subset": "ASCII+Latin-1+punct+arrows+geometric+misc symbols+dingbats+CJK punct+fullwidth+GB2312",
                "note": "子集化 + 改名为 PROTORIG Panel Sans；随 npm 包分发，保证 macOS/Windows 同一字体",
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print("[fonts] 完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
