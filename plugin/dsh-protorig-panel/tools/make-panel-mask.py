#!/usr/bin/env python3
"""make-panel-mask.py —— 从内置 watch photo 反解「默认表盘下半区」的羽化 alpha

产物（仓库内）：
  plugin/dsh-protorig-panel/assets/faces/panel-mask.png   480×232 RGBA：A 通道 = 羽化 alpha
  （同时写出白底 RGB=255，方便预览/查看；服务端与客户端都只读 A 通道）

人工核对（local/ 不发布）：
  local/outputs/panel-mask-sample.png    内置照片裁切 × mask，应与内置 preview.png 轮廓一致
  local/outputs/panel-mask-compare.png   preview.png | panel-mask-sample.png 并排

为什么要反解，而不是写死 inner/outer 常数：
  内置 preview.png = source 的 480×232 cover 裁切 × 顶部渐变 × 圆边渐变 × 底部文字压暗。
  用户要的是「和默认图一样的羽化形状」——也就是内置图自己的顶部/圆边两段渐变，
  不是我们手调的半径。这里用 source 反解出内置图的逐像素暗角：
      preview / (source 裁切) ≈ 顶部smooth × 圆边smooth × 底部压暗
  底部压暗只作用于 y>102 的文字区，不是羽化，而且面板中心必须保持原亮度（=255），
  所以在 y<102 的干净区间取样拟合：
      · 顶部：y=0..26 行 → 拟合 smooth(y / topFade)；
      · 圆边：y=28..100 行 → 拟合 smooth((radius - r) / edgeFade)，
        圆心取屏幕圆心映射到面板局部坐标 (240, -8)。
  拟合出的 topFade / radius / edgeFade 只落在生成脚本里并立即烧进 PNG；
  JS 运行时不出现任何羽化常数（只加载这张图）。

注意：本脚本拟合的是**渐变量**（top_fade / radius / edge_fade），不是画面内容。
  2026-11-15 起内置图换成了纯黑底 logo 合成图（没有可采样的亮部），脚本已无法再从
  preview/source 反解 —— 也不需要：换图不动渐变量时 mask 几何完全不变。
  只有改了 config.json 的 top_fade / edge_fade 才需要重新拟合（届时要用带亮部的内容图）。
  本脚本是开发期工具，运行环境需要 Pillow；发行版运行时不依赖 Python。

用法：
  <python> plugin/dsh-protorig-panel/tools/make-panel-mask.py
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[3]
WATCH = ROOT / 'firmware/lvgl_test/assets/watch_photo'
OUT_DIR = ROOT / 'plugin/dsh-protorig-panel/assets/faces'
SAMPLE_DIR = ROOT / 'local/outputs'
W, H, Y = 480, 232, 248


def smooth(v):
    v = np.clip(v, 0.0, 1.0)
    return v * v * (3.0 - 2.0 * v)


def load_crop(config_path: Path) -> np.ndarray:
    """复刻 firmware/lvgl_test/tools/make_watch_photo.py 的 cover 裁切（未加暗角）。"""
    cfg = json.loads(config_path.read_text(encoding='utf-8'))
    with Image.open(WATCH / cfg['source']) as source:
        source = ImageOps.exif_transpose(source).convert('RGB')
    w, h = source.size
    scale = max(W / w, H / h)
    cw, ch = W / scale, H / scale
    x0 = (w - cw) * float(cfg.get('position_x', 0.5))
    y0 = (h - ch) * float(cfg.get('position_y', 0.5))
    crop = source.crop((x0, y0, x0 + cw, y0 + ch)).resize((W, H), Image.Resampling.LANCZOS)
    return np.asarray(crop).astype(np.float32)


def fit_fade(crop: np.ndarray, preview: np.ndarray) -> tuple[float, float, float]:
    """从 preview / crop 反解内置图自身的顶部 + 圆边羽化参数。"""
    # 绿色通道 6 bit（量化最细）；亮度权重压低暗部不可靠的比值。
    ratio = (preview[:, :, 1] + 0.5) / (crop[:, :, 1] + 0.5)
    bright = crop.max(axis=2)
    weight_threshold = 60.0

    # ---- 顶部渐变：只看 y<bottom_dim 起点(102) 之前，且避开圆边（x 140..341）----
    ys, xs = np.mgrid[0:27, 140:341]
    keep = bright[0:27, 140:341] > weight_threshold
    top_y = ys[keep].astype(np.float32)
    top_ratio = ratio[0:27, 140:341][keep]
    top_weight = bright[0:27, 140:341][keep]
    if top_y.size < 200:
        raise SystemExit('顶部羽化取样不足：内置 preview/source 对不上？')
    best = None
    for top_fade in np.arange(20.0, 40.001, 0.05):
        err = float(np.average((smooth(top_y / top_fade) - top_ratio) ** 2, weights=top_weight))
        if best is None or err < best[0]:
            best = (err, float(top_fade))
    _, top_fade = best

    # ---- 圆边渐变：y=28..100（bottom_dim 尚未开始），圆心 = 屏幕圆心 (240,240) ----
    ys, xs = np.mgrid[28:101, 0:480]
    keep = bright[28:101] > weight_threshold
    edge_y = ys[keep].astype(np.float32)
    edge_x = xs[keep].astype(np.float32)
    edge_ratio = ratio[28:101][keep]
    edge_weight = bright[28:101][keep]
    radius_px = np.hypot(edge_x - 240.0, edge_y - (240.0 - Y))

    def search(radius_values, edge_values):
        found = None
        for radius in radius_values:
            for edge_fade in edge_values:
                err = float(np.average(
                    (smooth((radius - radius_px) / edge_fade) - edge_ratio) ** 2,
                    weights=edge_weight,
                ))
                if found is None or err < found[0]:
                    found = (err, float(radius), float(edge_fade))
        return found

    coarse = search(np.arange(190.0, 226.01, 0.5), np.arange(18.0, 38.01, 0.5))
    _, r0, e0 = coarse
    _, radius, edge_fade = search(
        np.arange(r0 - 0.6, r0 + 0.601, 0.05),
        np.arange(e0 - 1.0, e0 + 1.001, 0.05),
    )
    return top_fade, radius, edge_fade


def build_alpha(top_fade: float, radius: float, edge_fade: float) -> np.ndarray:
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
    top = smooth(ys / top_fade)
    rr = np.hypot(xs - 240.0, ys - (240.0 - Y))
    edge = smooth((radius - rr) / edge_fade)
    return np.clip(np.rint(top * edge * 255.0), 0, 255).astype(np.uint8)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preview', type=Path, default=WATCH / 'preview.png')
    parser.add_argument('--config', type=Path, default=WATCH / 'config.json')
    parser.add_argument('--mask', type=Path, default=OUT_DIR / 'panel-mask.png')
    parser.add_argument('--sample-dir', type=Path, default=SAMPLE_DIR)
    args = parser.parse_args()

    preview = np.asarray(Image.open(args.preview).convert('RGB')).astype(np.float32)
    if preview.shape[:2] != (H, W):
        raise SystemExit(f'preview 尺寸不是 {W}×{H}：{preview.shape[:2]}')
    crop = load_crop(args.config)
    top_fade, radius, edge_fade = fit_fade(crop, preview)
    alpha = build_alpha(top_fade, radius, edge_fade)

    # 内核检查：中心必须满亮（否则说明拟合吸进了底部压暗）；四角/上边/下边必须黑。
    def at(x, y):
        return int(alpha[y, x])

    checks = {
        'center(240,116)': at(240, 116),
        'top(240,0)': at(240, 0),
        'top(240,10)': at(240, 10),
        'left(0,116)': at(0, 116),
        'right(479,116)': at(479, 116),
        'bottom(240,231)': at(240, 231),
        'corner(0,0)': at(0, 0),
    }
    if checks['center(240,116)'] != 255:
        raise SystemExit(f'中心不是 255：{checks}')
    for key in ('top(240,0)', 'left(0,116)', 'right(479,116)', 'bottom(240,231)', 'corner(0,0)'):
        if checks[key] != 0:
            raise SystemExit(f'{key} 不是 0：{checks}')

    # 白底 RGB + A=alpha：destination-in / 服务端都只吃 A 通道。
    rgba = np.empty((H, W, 4), dtype=np.uint8)
    rgba[:, :, :3] = 255
    rgba[:, :, 3] = alpha
    args.mask.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, 'RGBA').save(args.mask)

    # ---- 人工核对素材（local/outputs，不发布）----
    args.sample_dir.mkdir(parents=True, exist_ok=True)
    scale = (alpha.astype(np.float32) / 255.0)[:, :, None]
    sample = np.clip(np.rint(crop * scale), 0, 255).astype(np.uint8)
    Image.fromarray(sample, 'RGB').save(args.sample_dir / 'panel-mask-sample.png')
    sep = np.zeros((H, 8, 3), dtype=np.uint8)
    compare = np.concatenate([preview.astype(np.uint8), sep, sample], axis=1)
    Image.fromarray(compare, 'RGB').save(args.sample_dir / 'panel-mask-compare.png')

    print(f'fitted: top_fade={top_fade:.2f}px radius={radius:.2f}px edge_fade={edge_fade:.2f}px')
    print(f'alpha: {" ".join(f"{k}={v}" for k, v in checks.items())}')
    print(f'mask  -> {args.mask} ({args.mask.stat().st_size} B)')
    print(f'sample-> {args.sample_dir / "panel-mask-sample.png"}')
    print(f'compare-> {args.sample_dir / "panel-mask-compare.png"}')


if __name__ == '__main__':
    main()
