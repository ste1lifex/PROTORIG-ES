#!/bin/bash
# setup-mac-build.sh —— 一次性在 macOS 上装好 PROTORIG ES 固件构建环境。
#
# 装什么（全部落在 $PROTORIG_TOOLS_DIR，默认 ~/Developer/.tools，不碰系统目录）：
#   1. Bouffalo 官方 RISC-V 工具链  gcc-riscv-14.3.0-macos-arm64（GCC 14.3 + Picolibc）
#      —— 并跑 fix_macos_toolchain.py 修掉它对 Homebrew dylib 的硬编码依赖；
#   2. CMake（官方 macOS universal 包，SDK 只要求 >= 3.15）；
#   3. GNU Make 4.x（从源码编译；macOS 自带 3.81 不支持 SDK 用的 $(file ...)）；
#   4. bouffalo_sdk（pin 到固定 commit；仓库 1.5 GB，注意磁盘）。
#
# 前置条件：Xcode Command Line Tools（clang）、python3、git、curl、网络。
# 预计耗时 10–20 分钟（取决于网速），磁盘占用约 4 GB。
#
# 用法：
#   bash firmware/tools/setup-mac-build.sh                # 幂等安装，已装好的步骤跳过
#   bash firmware/tools/setup-mac-build.sh --force        # 重装全部
#   PROTORIG_TOOLS_DIR=/opt/x bash firmware/tools/setup-mac-build.sh
#
# 装完后编译：
#   cd firmware/lvgl_test && ./build-mac.sh --clean

set -euo pipefail

# ---- 固定版本（换版本时一起改这里的 URL / sha256）----
TOOLCHAIN_URL="https://github.com/bouffalolab/toolchain/releases/download/default/gcc-riscv-14.3.0-macos-arm64.tar.xz"
TOOLCHAIN_SHA256="460c57933454dafc78592d4b7fc160bfdfe406560d06bb72a976e319aec1d7d7"
CMAKE_URL="https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-macos-universal.tar.gz"
CMAKE_SHA256="679e77a96b9ef9484cc6c1445255fc4f84206b48a65420efbedf16bdab55fbff"
MAKE_URL="https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz"
MAKE_SHA256="dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3"
# bouffalo_sdk master，2026-11-06；v2.3.38 tag 本身 components/Kconfig 引用缺失组件，
# master 上有 fix(build): remove deprecated components，所以 pin 这个 commit。
SDK_COMMIT="eba87edced5a03ce8bcad8cf6002494982f83a5f"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="${PROTORIG_TOOLS_DIR:-$HOME/Developer/.tools}"
FORCE=0
for arg in "$@"; do
  case "$arg" in
    --force) FORCE=1 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "未知参数: $arg" >&2; exit 2 ;;
  esac
done

DL_DIR="$TOOLS_DIR/dl"
mkdir -p "$DL_DIR"

step() { printf '\n\033[1;36m== %s ==\033[0m\n' "$*"; }
ok()   { printf '   \033[32m✓\033[0m %s\n' "$*"; }
warn() { printf '   \033[33m!\033[0m %s\n' "$*"; }

sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }

fetch() { # url dest expected_sha
  local url="$1" dest="$2" expected="$3"
  if [ -f "$dest" ] && [ "$(sha256_of "$dest")" = "$expected" ] && [ "$FORCE" = 0 ]; then
    ok "$(basename "$dest") 已在缓存中"
    return 0
  fi
  echo "   下载 $(basename "$dest") ..."
  curl -fSL --retry 3 --progress-bar -o "$dest.part" "$url"
  local actual
  actual="$(sha256_of "$dest.part")"
  if [ "$actual" != "$expected" ]; then
    rm -f "$dest.part"
    echo "   sha256 不匹配: $actual != $expected" >&2
    return 1
  fi
  mv "$dest.part" "$dest"
  ok "$(basename "$dest") sha256 校验通过"
}

# ---------------------------------------------------------------- 0. 前置检查
step "0/6 检查系统前置条件"
[ "$(uname -s)" = "Darwin" ] || { echo "本脚本只用于 macOS" >&2; exit 1; }
[ "$(uname -m)" = "arm64" ] || warn "当前不是 Apple Silicon (arm64)；官方 macOS 工具链是 arm64 版，Intel Mac 请改用 xpa/自编译工具链"
xcode-select -p >/dev/null 2>&1 || { echo "缺少 Xcode Command Line Tools：xcode-select --install" >&2; exit 1; }
for c in git curl python3 tar shasum; do command -v "$c" >/dev/null || { echo "缺少命令: $c" >&2; exit 1; }; done
ok "Xcode CLT: $(xcode-select -p)"
ok "python3: $(python3 --version 2>&1)"
ok "目标目录: $TOOLS_DIR"

# ---------------------------------------------------------------- 1. 工具链
step "1/6 Bouffalo RISC-V 工具链（GCC 14.3 + Picolibc）"
TC_ROOT="$TOOLS_DIR/toolchain"
TC_DIR="$(ls -d "$TC_ROOT"/riscv64-unknown-elf-* 2>/dev/null | head -1 || true)"
if [ -n "$TC_DIR" ] && [ "$FORCE" = 0 ] && "$TC_DIR/bin/riscv64-unknown-elf-gcc" --version >/dev/null 2>&1; then
  ok "已安装: ${TC_DIR}（$("$TC_DIR/bin/riscv64-unknown-elf-gcc" --version | head -1)）"
else
  [ "$FORCE" = 1 ] && rm -rf "$TC_ROOT"
  mkdir -p "$TC_ROOT"
  fetch "$TOOLCHAIN_URL" "$DL_DIR/toolchain.tar.xz" "$TOOLCHAIN_SHA256"
  echo "   解压工具链 ..."
  tar xf "$DL_DIR/toolchain.tar.xz" -C "$TC_ROOT"
  TC_DIR="$(ls -d "$TC_ROOT"/riscv64-unknown-elf-* | head -1)"
  ok "工具链: $TC_DIR"
fi

step "1b/6 修工具链的 Homebrew dylib 依赖（官方包在 Homebrew 机器上打包，换机必挂）"
python3 "$SCRIPT_DIR/fix_macos_toolchain.py" --toolchain "$TC_DIR"
"$TC_DIR/bin/riscv64-unknown-elf-gcc" --version | head -1
echo 'int main(void){return 0;}' > /tmp/protorig_tc_smoke.c
"$TC_DIR/bin/riscv64-unknown-elf-gcc" -march=rv32imafc_zicsr_zifencei -mabi=ilp32f \
  -c /tmp/protorig_tc_smoke.c -o /tmp/protorig_tc_smoke.o
ok "工具链冒烟测试通过（-march=rv32imafc 编译成功）"

# ---------------------------------------------------------------- 2. CMake
step "2/6 CMake"
CMAKE_BIN="$(ls -d "$TOOLS_DIR"/cmake/*/CMake.app/Contents/bin/cmake 2>/dev/null | head -1 || true)"
if [ -n "$CMAKE_BIN" ] && [ "$FORCE" = 0 ]; then
  ok "已安装: ${CMAKE_BIN}（$("$CMAKE_BIN" --version | head -1)）"
else
  [ "$FORCE" = 1 ] && rm -rf "$TOOLS_DIR/cmake"
  mkdir -p "$TOOLS_DIR/cmake"
  fetch "$CMAKE_URL" "$DL_DIR/cmake.tar.gz" "$CMAKE_SHA256"
  tar xf "$DL_DIR/cmake.tar.gz" -C "$TOOLS_DIR/cmake"
  CMAKE_BIN="$(ls -d "$TOOLS_DIR"/cmake/*/CMake.app/Contents/bin/cmake | head -1)"
  ok "CMake: $CMAKE_BIN"
fi

# ---------------------------------------------------------------- 3. GNU Make
step "3/6 GNU Make 4.x（macOS 自带 3.81 不够用）"
MAKE_BIN="$TOOLS_DIR/make/bin/make"
if [ -x "$MAKE_BIN" ] && [ "$FORCE" = 0 ]; then
  ok "已安装: $("$MAKE_BIN" --version | head -1)"
else
  [ "$FORCE" = 1 ] && rm -rf "$TOOLS_DIR/make" "$TOOLS_DIR/src"
  mkdir -p "$TOOLS_DIR/src"
  fetch "$MAKE_URL" "$DL_DIR/make.tar.gz" "$MAKE_SHA256"
  tar xf "$DL_DIR/make.tar.gz" -C "$TOOLS_DIR/src"
  MAKE_SRC="$(ls -d "$TOOLS_DIR"/src/make-* | head -1)"
  echo "   编译 GNU Make（约 1 分钟）..."
  ( cd "$MAKE_SRC" && ./configure --prefix="$TOOLS_DIR/make" >/dev/null && make -j"$(sysctl -n hw.ncpu)" >/dev/null && make install >/dev/null )
  ok "Make: $("$MAKE_BIN" --version | head -1)"
fi

# ---------------------------------------------------------------- 4. SDK
step "4/6 bouffalo_sdk（pin ${SDK_COMMIT}）"
SDK_DIR="$TOOLS_DIR/bouffalo_sdk"
if [ -d "$SDK_DIR/.git" ] && [ "$(git -C "$SDK_DIR" rev-parse HEAD 2>/dev/null)" = "$SDK_COMMIT" ] && [ "$FORCE" = 0 ]; then
  ok "已安装: $SDK_DIR @ ${SDK_COMMIT:0:10}"
else
  [ "$FORCE" = 1 ] && rm -rf "$SDK_DIR"
  echo "   clone bouffalo_sdk（约 1.5 GB，第一次要几分钟）..."
  rm -rf "$SDK_DIR"
  git clone --depth 1 https://github.com/bouffalolab/bouffalo_sdk.git "$SDK_DIR" >/dev/null 2>&1
  if [ "$(git -C "$SDK_DIR" rev-parse HEAD)" != "$SDK_COMMIT" ]; then
    echo "   master 已前进，切到 pin 的 commit ..."
    if git -C "$SDK_DIR" fetch --depth 1 origin "$SDK_COMMIT" >/dev/null 2>&1; then
      git -C "$SDK_DIR" checkout -q FETCH_HEAD
    else
      warn "拉不到 pin 的 commit，改用当前 master HEAD（$(git -C "$SDK_DIR" rev-parse --short HEAD)）"
    fi
  fi
  ok "SDK: $SDK_DIR @ $(git -C "$SDK_DIR" rev-parse --short HEAD)"
fi
[ -f "$SDK_DIR/components/Kconfig" ] || { echo "SDK 结构不完整: $SDK_DIR" >&2; exit 1; }
if grep -q "bflb_gmf" "$SDK_DIR/components/Kconfig"; then
  warn "components/Kconfig 引用了缺失组件 bflb_gmf —— 这是 v2.3.38 tag 的已知问题，请用 master commit"
fi

# ---------------------------------------------------------------- 5. 自检
step "5/6 构建环境自检"
export BL_SDK_BASE="$SDK_DIR"
export BL_TOOLCHAIN_DIR="$TC_DIR"
export PATH="$TC_DIR/bin:$(dirname "$CMAKE_BIN"):$(dirname "$MAKE_BIN"):$PATH"
echo "   SDK       : $BL_SDK_BASE"
echo "   Toolchain : $BL_TOOLCHAIN_DIR"
echo "   CMake     : $($CMAKE_BIN --version | head -1)"
echo "   Make      : $($MAKE_BIN --version | head -1)"
echo "   Picolibc  : $([ -f "$TC_DIR/riscv64-unknown-elf/lib/picolibc.specs" ] && echo 有 || echo 缺失)"
ok "自检完成"

step "6/6 完成"
cat <<EOF

下一步：
  cd firmware/lvgl_test
  ./build-mac.sh --clean          # 编译 → build/build_out/protorig_m2_bl616.bin

说明文档：docs/MACOS_FIRMWARE.md
EOF
