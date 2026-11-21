#!/usr/bin/env python3
"""fix_macos_toolchain.py —— 修好 Bouffalo macOS RISC-V 工具链的 Homebrew 依赖。

背景
====
官方 ``gcc-riscv-14.3.0-macos-arm64.tar.xz``（bouffalolab/toolchain release tag
``default``）是**在装了 Homebrew 的机器上打包**的：``cc1`` / ``cc1plus`` / ``lto1``
等可执行文件的 LC_LOAD_DYLIB 里写死了

    /opt/homebrew/opt/gmp/lib/libgmp.10.dylib
    /opt/homebrew/opt/isl/lib/libisl.23.dylib
    /opt/homebrew/opt/libmpc/lib/libmpc.3.dylib
    /opt/homebrew/opt/mpfr/lib/libmpfr.6.dylib
    /opt/homebrew/opt/zstd/lib/libzstd.1.dylib

没有 Homebrew 的 Mac 上，``cc1`` 一启动就 dyld: Library not loaded → gcc 内部
崩溃。**这不是项目代码的问题**，而是工具链发布包的问题。

本脚本做的事
=============
1. 从 Homebrew bottle（ghcr.io，公开、免安装 Homebrew）下载上面 5 个库的
   macOS arm64 版本，解出所需的 .dylib 到 ``<prefix>/lib``；
2. 把工具链里所有引用 ``/opt/homebrew`` / ``@@HOMEBREW_PREFIX@@`` 的 dylib 路径
   改成 ``@loader_path``（必要时把库复制到该二进制同目录，绕开 load command
   空间不足的问题 —— install_name_tool 不能把变长的命令塞进原空间）；
3. 对改动过的 Mach-O 全部 ad-hoc 重新签名（``codesign --force -s -``）；
4. 幂等：重复执行不会重复下载/重复改写。

用法
====
    python3 fix_macos_toolchain.py --toolchain ~/Developer/.tools/toolchain/riscv64-unknown-elf-gthread
    python3 fix_macos_toolchain.py --toolchain ... --prefix ~/Developer/.tools/hostlibs

成功后 ``riscv64-unknown-elf-gcc --version`` 和一次 ``-c`` 编译应该都正常。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

HOMEBREW_FORMULAS = ["gmp", "isl", "libmpc", "mpfr", "zstd"]
# 优先用较新的 macOS 瓶；找不到就依次退
BOTTLE_TAGS = ["arm64_golden_gate", "arm64_tahoe", "arm64_sequoia", "arm64_sonoma", "arm64_ventura"]
GHCR_TOKEN_URL = "https://ghcr.io/token?scope=repository:homebrew/core/{formula}:pull&service=ghcr.io"
FORMULA_API = "https://formulae.brew.sh/api/formula/{formula}.json"
BAK_SUFFIX = ".homebrew-orig"


def log(msg: str) -> None:
    print(f"[fix-toolchain] {msg}")


def run(*cmd: str, check: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=check)


def otool_deps(path: str) -> list[str]:
    out = run("otool", "-L", path).stdout
    deps: list[str] = []
    for line in out.splitlines()[1:]:
        line = line.strip()
        if not line:
            continue
        deps.append(line.split()[0])
    return deps


def is_macho(path: str) -> bool:
    try:
        out = run("file", "-b", path).stdout
    except OSError:
        return False
    return "Mach-O" in out


def has_bad_dep(path: str, prefix_lib: str) -> bool:
    for dep in otool_deps(path):
        if "/opt/homebrew" in dep or "@@HOMEBREW_PREFIX@@" in dep:
            return True
    return False


# ------------------------------------------------------------------ 下载 bottle
def http_get(url: str, headers: dict[str, str] | None = None) -> bytes:
    req = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(req, timeout=120) as resp:
        return resp.read()


def needed_lib_names(toolchain: str) -> set[str]:
    """从工具链现成的引用里收集需要的 dylib 文件名（不写死版本号）。"""
    names: set[str] = set()
    for root, _dirs, files in os.walk(toolchain):
        for name in files:
            path = os.path.join(root, name)
            if not is_macho(path):
                continue
            for dep in otool_deps(path):
                if "/opt/homebrew" in dep or "@@HOMEBREW_PREFIX@@" in dep:
                    names.add(os.path.basename(dep))
    return names


def fetch_formula(formula: str) -> tuple[str, str]:
    """返回 (下载 URL, sha256)。优先 arm64 瓶。"""
    meta = json.loads(http_get(FORMULA_API.format(formula=formula)).decode())
    files = meta.get("bottle", {}).get("stable", {}).get("files", {})
    for tag in BOTTLE_TAGS:
        if tag in files:
            entry = files[tag]
            return entry["url"], entry.get("sha256", "")
    raise RuntimeError(f"{formula}: 没有可用的 macOS arm64 bottle（{list(files)}）")


def download_bottle(formula: str, dest_dir: str) -> str:
    url, sha = fetch_formula(formula)
    token = json.loads(
        http_get(GHCR_TOKEN_URL.format(formula=formula)).decode()
    )["token"]
    data = http_get(url, {"Authorization": f"Bearer {token}"})
    if sha:
        actual = hashlib.sha256(data).hexdigest()
        if actual != sha:
            raise RuntimeError(f"{formula}: bottle sha256 不匹配（{actual} != {sha}）")
    os.makedirs(dest_dir, exist_ok=True)
    path = os.path.join(dest_dir, f"{formula}.tar.gz")
    with open(path, "wb") as fh:
        fh.write(data)
    log(f"downloaded {formula} bottle ({len(data) // 1024} KB)")
    return path


def extract_needed(bottle: str, wanted: set[str], lib_dir: str) -> dict[str, str]:
    """从 bottle 里抽出 wanted 里的 dylib，返回 {name: 目标路径}。"""
    os.makedirs(lib_dir, exist_ok=True)
    found: dict[str, str] = {}
    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(bottle, "r:gz") as tar:
            tar.extractall(tmp)
        for root, _dirs, files in os.walk(tmp):
            for name in files:
                if name not in wanted:
                    continue
                src = os.path.join(root, name)
                dst = os.path.join(lib_dir, name)
                if not os.path.exists(dst):
                    shutil.copy2(src, dst)
                    os.chmod(dst, 0o755)
                found[name] = dst
    return found


# --------------------------------------------------------------- 改写 + 签名
def rewrite_file(path: str, lib_dir: str, work_dir: str) -> bool:
    """把 path 里的 Homebrew 依赖改成 @loader_path，必要时复制库到同目录。"""
    changed = False
    for dep in otool_deps(path):
        if "/opt/homebrew" not in dep and "@@HOMEBREW_PREFIX@@" not in dep:
            continue
        base = os.path.basename(dep)
        lib_src = os.path.join(lib_dir, base)
        if not os.path.exists(lib_src):
            raise RuntimeError(f"{path}: 缺少 {base}，请确认 bottle 已解出")
        # 同一目录的库副本（所有消费者都能用 @loader_path 找到）
        lib_dst = os.path.join(work_dir, base)
        if not os.path.exists(lib_dst):
            shutil.copy2(lib_src, lib_dst)
            os.chmod(lib_dst, 0o755)
        r = run("install_name_tool", "-change", dep, "@loader_path/" + base, path)
        if r.returncode != 0:
            raise RuntimeError(f"install_name_tool 失败: {path} <- {dep}: {r.stderr.strip()}")
        changed = True
    return changed


def fix_lib_ids(lib_dir: str) -> None:
    for name in os.listdir(lib_dir):
        if not name.endswith(".dylib"):
            continue
        path = os.path.join(lib_dir, name)
        # 1) 自身 id 改成 @loader_path/xxx
        run("install_name_tool", "-id", "@loader_path/" + name, path)
        # 2) 内部依赖同样处理（同一目录，都用 @loader_path）
        for dep in otool_deps(path):
            if "/opt/homebrew" in dep or "@@HOMEBREW_PREFIX@@" in dep:
                base = os.path.basename(dep)
                r = run("install_name_tool", "-change", dep, "@loader_path/" + base, path)
                if r.returncode != 0:
                    raise RuntimeError(f"install_name_tool 失败: {path} <- {dep}: {r.stderr.strip()}")


def codesign(paths: list[str]) -> None:
    for path in paths:
        r = run("codesign", "--force", "--sign", "-", path)
        if r.returncode != 0:
            raise RuntimeError(f"codesign 失败: {path}: {r.stderr.strip()}")
        run("xattr", "-d", "com.apple.quarantine", path)


def main() -> int:
    ap = argparse.ArgumentParser(description="修补 Bouffalo macOS RISC-V 工具链的 Homebrew 依赖")
    ap.add_argument("--toolchain", required=True, help="riscv64-unknown-elf-* 工具链根目录")
    ap.add_argument("--prefix", default=None, help="存放 host dylib 的目录（默认工具链同级 hostlibs/）")
    ap.add_argument("--cache-dir", default=None, help="bottle 下载缓存目录")
    args = ap.parse_args()

    toolchain = os.path.abspath(os.path.expanduser(args.toolchain))
    prefix = os.path.abspath(os.path.expanduser(
        args.prefix or os.path.join(os.path.dirname(toolchain), "hostlibs")))
    lib_dir = os.path.join(prefix, "lib")
    cache_dir = os.path.abspath(os.path.expanduser(
        args.cache_dir or os.path.join(prefix, "bottles")))

    if not os.path.isdir(toolchain):
        raise SystemExit(f"工具链目录不存在: {toolchain}")

    wanted = needed_lib_names(toolchain)
    if not wanted:
        log("工具链没有 Homebrew 依赖引用 —— 已经是干净的，无需修补")
        return 0
    log(f"需要 host 库: {', '.join(sorted(wanted))}")

    # 1) bottle → prefix/lib
    for formula in HOMEBREW_FORMULAS:
        try:
            bottle = download_bottle(formula, cache_dir)
        except Exception as exc:  # 网络/上游变化时给出可读错误
            raise SystemExit(f"下载 {formula} bottle 失败: {exc}") from exc
        extract_needed(bottle, wanted, lib_dir)

    missing = [n for n in wanted if not os.path.exists(os.path.join(lib_dir, n))]
    if missing:
        raise SystemExit(f"bottle 里没有找到这些库: {missing}")

    # 2) 修 prefix/lib 里各库自身的 id / 依赖
    fix_lib_ids(lib_dir)
    signed: list[str] = [os.path.join(lib_dir, n) for n in os.listdir(lib_dir) if n.endswith(".dylib")]

    # 3) 修工具链里所有引用 Homebrew 的二进制
    fixed_binaries = 0
    for root, _dirs, files in os.walk(toolchain):
        for name in files:
            path = os.path.join(root, name)
            if not is_macho(path):
                continue
            if not has_bad_dep(path, lib_dir):
                continue
            if rewrite_file(path, lib_dir, root):
                fixed_binaries += 1
                signed.append(path)

    # 4) 统一重新签名
    seen = set()
    final = []
    for p in signed:
        if p not in seen:
            seen.add(p)
            final.append(p)
    codesign(final)

    log(f"完成：修补 {fixed_binaries} 个工具链二进制，{len(final)} 个 Mach-O 已重新签名")
    log(f"host 库目录: {lib_dir}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001 - CLI 友好报错
        print(f"[fix-toolchain] ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
