# sdk_patches — 构建期临时 SDK 补丁

这些补丁由 `build.ps1` 在构建前 apply、构建后 reverse，**不修改共享 SDK 的持久状态**。
遇到 apply 失败/版本不匹配会直接中止构建并报错。

| 补丁 | 作用 | 关联 |
|---|---|---|
| `bouffalo_sdk_usb_console_delay.patch` | `board.c` 延迟 `usb_console_init()`，等 BLE 广播后再初始化 USB，实现 BLE+USB 共存 | [SDK 补丁说明](../docs/BUILD_SDK_PATCH.md) |
| `bouffalo_sdk_usb_pc_rx.patch` | `usb_console.c` 在 `shell_exe_cmd()` 之前插弱钩子 `protorig_usb_pc_rx()`，让固件在电脑模式下直接收 `A5 5A len` 二进制帧（像素透传上行）；弱默认返回 0，其他 app 行为不变 | [USB 接收实现](../bsp/usb_pc_link.c)、[USB 台架脚本](../tools/usb_pc_test.ps1) |

## 说明
- SDK 路径来自 `$env:BL_SDK_BASE` 或 `firmware/dev-env.json`，配置见[固件指南](../../README.md)。
- 补丁分别针对 `bsp/board/bl616dk/board.c` 与 `bsp/common/usb_app/usb_console.c`；应用方式与 `build.ps1` 内的文本变换一致；
- 请使用工程构建脚本应用并恢复补丁。Windows 使用 `build.ps1`，macOS 使用 `build-mac.sh`。
- 两个补丁都**只动软件层**：USB 描述符、端点号、写路径都没改，所以不会重新枚举、COM 号不变。
