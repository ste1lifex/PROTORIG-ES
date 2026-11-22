# 更新日志

## 2026-11-22 · 插件 0.4.20

- **连接开关语义修正**：关闭「USB 连接」/「蓝牙连接」会立刻停掉该通道——包括已经在跑的 BLE 扫描。此前只做到"下次不再开始扫描"，残留的扫描会继续收广播、扫到手表还会自动连上；「断开」也补上了同样的漏洞。
- **设置页**：两个连接开关与「设备探测」从最底部移到「概览」，分组由 7 个收敛为 6 个；概览新增「已暂停（手动断开）」「两个通道都已关闭」两种状态。
- **包内脚本**：`tools/pnpm-dsh.sh`、`build-mac.sh`、`setup-mac-build.sh` 恢复可执行位，README 里的 `./tools/pnpm-dsh.sh` 不再 Permission denied。
- 手表固件仍为 1.6.0，本版只有插件更新。

## 2026-11-21 · 首个公开版本

- **手表固件 1.6.0**提供默认表盘、图片表盘、下半区换图、设置、秒表、计时器、闹钟与通知卡。电脑模式支持 USB 像素透传、触摸回传和 BLE 卡片，息屏时保持宿主会话。版本号与构建时间可通过 `adminid` 查看。详见[固件文档](firmware/README.md)。
- **DSH 插件 0.4.19**提供会话与电脑指标面板、手表端审批和问答、设备绑定、表盘安装，以及 Windows/macOS 烧录入口。BLE 问答超时后，手表提示在电脑上继续操作。详见[插件文档](plugin/dsh-protorig-panel/README.md)。
- **npm 包**：`dsh-protorig-panel@0.4.19` 已发布到[公网 npm](https://www.npmjs.com/package/dsh-protorig-panel)，可用 `npm i -g dsh-protorig-panel@0.4.19` 安装。
- **硬件资料**包括 V1.6 原理图 PNG、有底盖与一体式表壳，以及电脑模式底座 V9.1。详见[原理图](schematic/README.md)与[结构设计](design/README.md)。
