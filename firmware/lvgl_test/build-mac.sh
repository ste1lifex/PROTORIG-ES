#!/bin/bash
# build-mac.sh —— 在 macOS 上编译 PROTORIG ES M2 固件（BL616 / bl616dk）。
#
# 与 Windows 的 build.ps1 等价：
#   1) 构建前给共享 SDK 临时打两个 PROTORIG 补丁（sdk_patches/apply_sdk_patches.py）；
#   2) make CHIP=bl616 BOARD=bl616dk PYTHON=python3；
#   3) 无论成功失败都把 SDK 恢复原样；
#   4) 打印 build/build_out/*.bin 的大小与 sha256。
#
# 依赖的三样东西（一次性装好，见 firmware/tools/setup-mac-build.sh 与
# docs/MACOS_FIRMWARE.md）：
#   BL_SDK_BASE       bouffalo_sdk（本机默认 ~/Developer/.tools/bouffalo_sdk）
#   BL_TOOLCHAIN_DIR  riscv64-unknown-elf GCC 14.3 macOS 版
#   cmake + GNU make 4.x（macOS 自带 make 3.81 不够：SDK 用了 $(file ...)）
#
# 用法：
#   ./build-mac.sh               # 增量编译
#   ./build-mac.sh --clean       # 先 make clean 再编
#   ./build-mac.sh --jobs 4      # 并行度（默认 CPU 核数）
#
# 环境变量覆盖（与 build.ps1 同名）：
#   BL_SDK_BASE / BL_TOOLCHAIN_DIR / BL_PYTHON_DIR
#   PROTORIG_TOOLS_DIR（默认 ~/Developer/.tools）
#   CMAKE_BIN / MAKE_BIN（显式指定 cmake / make）

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ="$SCRIPT_DIR"
CLEAN=0
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"

while [ $# -gt 0 ]; do
  case "$1" in
    --clean) CLEAN=1; shift ;;
    --jobs|-j) JOBS="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

TOOLS_DIR="${PROTORIG_TOOLS_DIR:-$HOME/Developer/.tools}"
BL_SDK_BASE="${BL_SDK_BASE:-$TOOLS_DIR/bouffalo_sdk}"
BL_PYTHON_DIR="${BL_PYTHON_DIR:-/usr/bin}"
PYTHON="${BL_PYTHON_DIR}/python3"

# ---- 定位工具链 / cmake / make（找不到就给可操作的报错）----
if [ -z "${BL_TOOLCHAIN_DIR:-}" ]; then
  BL_TOOLCHAIN_DIR="$(ls -d "$TOOLS_DIR"/toolchain/riscv64-unknown-elf-* 2>/dev/null | head -1 || true)"
fi
if [ -z "${CMAKE_BIN:-}" ]; then
  CMAKE_BIN="$(ls -d "$TOOLS_DIR"/cmake/*/CMake.app/Contents/bin/cmake 2>/dev/null | head -1 || true)"
  [ -z "$CMAKE_BIN" ] && CMAKE_BIN="$(command -v cmake || true)"
fi
if [ -z "${MAKE_BIN:-}" ]; then
  MAKE_BIN="$TOOLS_DIR/make/bin/make"
  [ -x "$MAKE_BIN" ] || MAKE_BIN="$(command -v make || true)"
fi

missing=()
[ -d "$BL_SDK_BASE" ]                                   || missing+=("SDK:            $BL_SDK_BASE")
[ -x "${BL_TOOLCHAIN_DIR:-/nonexistent}/bin/riscv64-unknown-elf-gcc" ] || missing+=("工具链:         ${BL_TOOLCHAIN_DIR:-未找到}")
[ -x "${CMAKE_BIN:-/nonexistent}" ]                     || missing+=("cmake:          ${CMAKE_BIN:-未找到}")
[ -x "${MAKE_BIN:-/nonexistent}" ]                      || missing+=("make:           ${MAKE_BIN:-未找到}")
[ -x "$PYTHON" ]                                        || missing+=("python3:        $PYTHON")
if [ ${#missing[@]} -gt 0 ]; then
  echo "缺少构建依赖：" >&2
  printf '  %s\n' "${missing[@]}" >&2
  echo "" >&2
  echo "一次性安装：bash $PROJ/../tools/setup-mac-build.sh" >&2
  echo "或按 docs/MACOS_FIRMWARE.md 手工指定 BL_SDK_BASE / BL_TOOLCHAIN_DIR 等。" >&2
  exit 1
fi

# macOS 自带 make 3.81 不支持 SDK 用到的 $(file ...)；GNU make <4 会直接报错
MAKE_VER="$("$MAKE_BIN" --version | head -1)"
case "$MAKE_VER" in
  *"GNU Make 4"*) ;;
  *) echo "需要 GNU Make >= 4.0（当前: ${MAKE_VER}）。运行 firmware/tools/setup-mac-build.sh 安装。" >&2; exit 1 ;;
esac

export BL_SDK_BASE BL_TOOLCHAIN_DIR
export PATH="$BL_TOOLCHAIN_DIR/bin:$(dirname "$CMAKE_BIN"):$(dirname "$MAKE_BIN"):$PATH"

echo "SDK      : $BL_SDK_BASE"
echo "Toolchain: $BL_TOOLCHAIN_DIR  ($("$BL_TOOLCHAIN_DIR/bin/riscv64-unknown-elf-gcc" --version | head -1))"
echo "CMake    : $CMAKE_BIN"
echo "Make     : $MAKE_VER"
echo "Python   : $PYTHON"
echo "Project  : $PROJ"
echo ""

PATCH_SCRIPT="$PROJ/sdk_patches/apply_sdk_patches.py"
restore_patches() {
  "$PYTHON" "$PATCH_SCRIPT" --sdk "$BL_SDK_BASE" --restore >/dev/null 2>&1 || true
}
trap restore_patches EXIT INT TERM

"$PYTHON" "$PATCH_SCRIPT" --sdk "$BL_SDK_BASE" --apply

cd "$PROJ"
if [ "$CLEAN" = 1 ]; then
  echo "[clean] make clean"
  "$MAKE_BIN" CHIP=bl616 BOARD=bl616dk PYTHON="$PYTHON" clean >/dev/null 2>&1 || true
fi

echo "[build] make CHIP=bl616 BOARD=bl616dk -j$JOBS"
"$MAKE_BIN" CHIP=bl616 BOARD=bl616dk PYTHON="$PYTHON" -j"$JOBS" 2>&1 | tee build_mac.log
RC=${PIPESTATUS[0]}
if [ "$RC" -ne 0 ]; then
  echo "" >&2
  echo "BUILD FAILED (exit=$RC)，完整日志: $PROJ/build_mac.log" >&2
  exit "$RC"
fi

echo ""
BIN="$(ls -t "$PROJ"/build/build_out/*.bin 2>/dev/null | grep -vE 'boot2|partition|efusedata|dwarfcfi' | head -1 || true)"
if [ -n "$BIN" ]; then
  echo "BUILD OK"
  echo "  bin : $BIN"
  echo "  size: $(stat -f%z "$BIN") bytes ($(echo "scale=1; $(stat -f%z "$BIN")/1024" | bc) KB)"
  echo "  sha256: $(shasum -a 256 "$BIN" | cut -d' ' -f1)"
  ELF="$(ls -t "$PROJ"/build/build_out/*.elf 2>/dev/null | head -1 || true)"
  if [ -n "$ELF" ]; then
    echo "  elf : $ELF"
    "$BL_TOOLCHAIN_DIR/bin/riscv64-unknown-elf-size" "$ELF" || true
  fi
  echo ""
  echo "下一步（烧录）：见 docs/MACOS_FIRMWARE.md 的「烧录」一节"
  echo "  （SDK 自带 BLFlashCommand-macos，x86_64，需要 Rosetta 2；或回 Windows 烧）"
else
  echo "BUILD OK 但没找到 build/build_out/*.bin" >&2
fi
