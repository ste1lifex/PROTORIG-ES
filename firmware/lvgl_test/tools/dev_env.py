"""dev_env.py —— 读取本机开发路径（不入库）

背景：仓库里不能写死开发机的绝对路径（SDK / 工具链 / Python / 主机 gcc / 字体目录），
但本机脚本又需要它们。约定：

  1. 环境变量优先（BL_SDK_BASE / BL_TOOLCHAIN_DIR / BL_PYTHON_DIR / HOST_CC / PROTORIG_FONT_DIR）
  2. 否则读 `firmware/dev-env.json`（.gitignore 里排除；格式见 firmware/README.md）
  3. 都没有就返回调用方给的默认值（通常是 None，由调用方报错并提示）

用法：
    from dev_env import get as dev_get
    sdk = dev_get("BL_SDK_BASE")
"""
from __future__ import annotations

import json
import os
from pathlib import Path

_CACHE: dict | None = None


def _load_file() -> dict:
    global _CACHE
    if _CACHE is not None:
        return _CACHE
    # tools/ -> lvgl_test/ -> firmware/dev-env.json
    env_file = Path(__file__).resolve().parents[2] / "dev-env.json"
    try:
        _CACHE = json.loads(env_file.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        _CACHE = {}
    return _CACHE


def get(name: str, default=None):
    value = os.environ.get(name)
    if value:
        return value
    value = _load_file().get(name)
    if value:
        return value
    return default


def require(name: str, purpose: str) -> str:
    """取一个必需的开发路径；缺失时给出可执行的提示并退出。"""
    value = get(name)
    if value:
        return value
    raise SystemExit(
        f"缺少 {name}（{purpose}）。\n"
        f"  请设置环境变量 {name}，或把它写进 firmware/dev-env.json（见 firmware/README.md）。"
    )
