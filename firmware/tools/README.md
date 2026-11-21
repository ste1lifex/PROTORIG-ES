# 固件工具

- Windows 构建用 `BL_SDK_BASE` 下的 CMake，并从 `firmware/lvgl_test/tools/make/` 载入 `mingw32-make.exe`。这份 Make 工具需从 SDK 的 `tools/make/` 复制，步骤见[固件指南](../README.md)。
- Windows 烧录通过 `firmware/lvgl_test/flash.ps1` 调用 SDK 中的 `BLFlashCommand.exe`，使用构建生成的 `flash_prog_cfg.ini`。
- macOS 环境由 [setup-mac-build.sh](setup-mac-build.sh) 配置。构建用 [build-mac.sh](../lvgl_test/build-mac.sh)，烧录用 SDK 的 `BLFlashCommand-macos`。
- `serial_cmd.ps1` 和 `serial_capture.ps1` 用于发送命令和记录串口输出。实机检查入口见[测试与验收](../lvgl_test/docs/TESTING.md)。

`chips/` 是 BLFlashCommand 使用的芯片配置，工具版本说明见 [BLFlashCommand_ReleaseNote.txt](BLFlashCommand_ReleaseNote.txt)。
