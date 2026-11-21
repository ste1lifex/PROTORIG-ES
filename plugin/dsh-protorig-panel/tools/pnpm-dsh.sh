#!/bin/sh
# pnpm-dsh.sh —— 用 DSH 桌面版自带的 Node + pnpm 跑插件包的 pnpm 命令。
#
# 为什么需要它：DSH 桌面版（macOS）自带 Node 24.21.0 与 pnpm 11.28.5，但**没有 npm**。
# 插件开发机不需要另装 Node/npm；直接复用宿主的运行时，装出来的依赖和 DSH 插件管理器
# 装出来的完全同源（同一个 store、同一套平台预编译包）。
#
# 用法（在 plugin/dsh-protorig-panel 下）：
#   ./tools/pnpm-dsh.sh install --frozen-lockfile
#   ./tools/pnpm-dsh.sh test
#   ./tools/pnpm-dsh.sh pack            # 产物 dsh-protorig-panel-<version>.tgz
#
# 覆盖探测路径：
#   DSH_APP="/Applications/DeepSeek Harness.app"   # macOS 安装位置
#   DSH_NODE=...                                   # 直接指定 electron/node 可执行文件
#   DSH_PNPM=...                                   # 直接指定 pnpm.cjs
#
# 找不到 DSH 时退回到系统里的 pnpm（如果有）。

set -e

# 1) 显式覆盖优先
NODE_BIN="${DSH_NODE:-}"
PNPM_CJS="${DSH_PNPM:-}"
RUNTIME_BIN=""

# 2) macOS：DSH.app 自带的 primary-runtime
if [ -z "$PNPM_CJS" ]; then
  APP="${DSH_APP:-/Applications/DeepSeek Harness.app}"
  CAND_NODE="$APP/Contents/MacOS/DeepSeek Harness"
  CAND_PNPM="$APP/Contents/Resources/runtime/primary-runtime/dependencies/pnpm/bin/pnpm.cjs"
  CAND_BIN="$APP/Contents/Resources/runtime/bin"
  if [ -x "$CAND_NODE" ] && [ -f "$CAND_PNPM" ]; then
    NODE_BIN="$CAND_NODE"
    PNPM_CJS="$CAND_PNPM"
    RUNTIME_BIN="$CAND_BIN"
  fi
fi

# 3) 退回系统 pnpm
if [ -z "$PNPM_CJS" ]; then
  if command -v pnpm >/dev/null 2>&1; then
    exec pnpm "$@"
  fi
  echo "pnpm-dsh.sh: 找不到 DSH 自带运行时，也找不到系统 pnpm。" >&2
  echo "  可用 DSH_APP=... 指定安装目录，或 DSH_NODE=... DSH_PNPM=... 直接指定。" >&2
  exit 127
fi

# pnpm 跑 package.json scripts 时会再 spawn `node`：把宿主 runtime/bin 放到 PATH 最前，
# 保证脚本里写的是 `node` 也能解析到同一个运行时。
if [ -n "$RUNTIME_BIN" ]; then
  PATH="$RUNTIME_BIN:$PATH"
  export PATH
  DSH_DESKTOP_NODE_EXECUTABLE="$NODE_BIN"
  export DSH_DESKTOP_NODE_EXECUTABLE
fi

exec env ELECTRON_RUN_AS_NODE=1 "$NODE_BIN" "$PNPM_CJS" "$@"
