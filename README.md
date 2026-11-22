# PROTORIG ES

PROTORIG ES 是一块自制的圆屏智能手表，也可以作为 DeepSeek Harness（DSH）的可编程外设。
它使用 480×480 AMOLED、BL616 主控，配有触摸屏、RTC、振动马达和 500 mAh 电池。
外观主题是《明日方舟：终末地》的非官方二创。

本仓库提供固件 **1.6.0**、DSH 插件 **0.4.21**、表壳与底座模型，以及原理图 PNG。
插件已发布到公网 npm：[`dsh-protorig-panel`](https://www.npmjs.com/package/dsh-protorig-panel)（`npm i -g dsh-protorig-panel@0.4.21`）。
版本说明见 [CHANGELOG.md](CHANGELOG.md)。

## 功能

| 功能 | 说明 |
|---|---|
| 表盘与应用 | 默认表盘、可安装的图片表盘、设置、秒表、计时器、闹钟和通知卡。默认表盘的下半区可以独立换图。 |
| USB 电脑模式 | 作为 480×480 副屏显示 DSH 会话与电脑指标，并向电脑回传触摸。闹钟和计时器可在设备端弹窗。 |
| BLE 电脑模式 | 由手表渲染电脑指标、审批和问答卡片。USB 连接恢复后插件可切回有线链路。 |
| DSH 插件 | 显示会话、余额和电脑指标，在手表上处理审批与选择题，通过 USB 安装表盘和下半区图片，并在 Windows/macOS 上烧录固件。 |
| 设备选择 | 插件按手表的管理员编号绑定设备。USB 和 BLE 使用同一块表的同一编号。 |
| 电源 | 手表模式支持息屏与深睡，闹钟可经 SD3010 RTC 唤醒。电脑模式息屏时继续保持 USB/BLE 会话。 |

## 硬件

| 部件 | 型号 / 说明 |
|---|---|
| MCU | Ai-M62-CBS（BL616，RISC-V，Wi-Fi 6 + BLE 5.3） |
| 屏幕 | 1.6 英寸 480×480 AMOLED（ST7802，QSPI） |
| 触摸 | CST9220（I²C） |
| RTC | SD3010（AON 报警唤醒） |
| 电池 | 500 mAh |
| 其他 | 振动马达 |

## 快速开始

- [固件构建与烧录](firmware/README.md)提供 Windows 和 macOS 的步骤。烧录完成后，手动将 SW2 拨到 OFF 再拨回 ON。
- [DSH 插件](plugin/dsh-protorig-panel/README.md)已发布到公网 npm，也可从源码打包后安装到 DSH。文档包含连接、表盘和烧录操作。
- [表壳与底座](design/README.md)提供 STEP、STL、3MF 模型和装配参考。
- [原理图](schematic/README.md)提供 V1.6 板卡的 PNG。
- [开发指南](AGENTS.md)和[测试与验收](firmware/lvgl_test/docs/TESTING.md)介绍代码入口和验证步骤。
- [打包与发布](PUBLISHING.md)介绍源码归档和插件打包。

## 许可

- **代码、硬件原理图、表壳与底座设计文件**采用 MIT 许可。见 [LICENSE](LICENSE)。
- **文档与项目图片**采用 CC BY-SA 4.0。
- 第三方组件保留各自许可。固件思源黑体字库与插件 Noto Sans SC 子集采用 OFL，DSH 鲸鱼图形采用 MIT。

## 声明

本项目是《明日方舟：终末地》的非官方二创，与鹰角网络无关。游戏名称和标志的权利归原权利方所有。
