# 构建期 SDK 补丁

`build.ps1`（Windows）与 `build-mac.sh`（macOS）在编译期间临时调整 Bouffalo SDK 的 USB 控制台，结束后恢复源文件。
两个补丁的职责见 [sdk_patches/](../sdk_patches/)。

## USB 控制台延迟初始化

BL616 的 USB 时钟来自 WIFIPLL。BLE 初始化会重新配置 RF/PLL，USB 控制台如果已启动，连接可能中断。
固件以 `protorig_usb_console_delay` 跳过 `board_init()` 里的早期 `usb_console_init()`，等 BLE 广播开始后再初始化 USB。
启动早期的日志不会出现在 USB 控制台，串口端口要等 USB 初始化后才会枚举。

## 电脑模式二进制接收

SDK 的 USB CDC 接收路径通过弱符号 `protorig_usb_pc_rx()` 将像素帧交给 [USB 链路实现](../bsp/usb_pc_link.c)，其余数据继续走 shell。
补丁也提供 USB 输入事件计数 `protorig_usb_console_in_events`，用于检测宿主掉线。

构建时请使用[固件指南](../../README.md)列出的入口。补丁失败时核对 `BL_SDK_BASE` 及 SDK 源文件，避免将改过的 SDK 作为构建基线。
