# PROTORIG ES 开发指南

| 目录 | 用途 |
|---|---|
| [firmware/lvgl_test/](firmware/lvgl_test/) | BL616 固件，使用 LVGL 9.5、FreeRTOS 和 Bouffalo SDK |
| [plugin/dsh-protorig-panel/](plugin/dsh-protorig-panel/) | DSH 宿主插件、设置页与主机测试 |
| [design/](design/) | 表壳、底座和装配参考模型 |
| [schematic/](schematic/) | V1.6 原理图 PNG |

## 固件入口

- 构建输入和构建目标见 `firmware/lvgl_test/CMakeLists.txt`。原生页面在 `ui/`，应用与表盘注册在 `apps/`，分页与手势在 `eez_ui_glue.c`。
- 用户可见的 App 是设置、秒表、计时器和闹钟。协议网络与相遇记录是隐藏的占位页，`apps/app_list.c` 用 `hidden` 管理可见性。
- 表盘 ID 为 `0`（默认）和 `3`（图片）。默认表盘在 `face_lvgl.c`，内置下半区图由 `tools/make_watch_photo.py` 从 `assets/watch_photo/source.png` 生成。图片表盘与下半区面板的持久化实现分别在 `services/svc_face_store.*` 和 `services/svc_face_panel.*`。
- 电脑模式见 `services/svc_pc.c` 与 `bsp/usb_pc_link.c`，BLE 入口是 `ble_link.c`、`ble_svc.c` 和 `services/svc_ble.c`。电脑模式息屏时保留 USB/BLE 会话，并禁止自动进入 PDS。
- `ble_ui.c`、`terrain565.c` 与独立 EEZ Studio 示例 `firmware/eez_ui/` 属于参考源码，当前构建从 `CMakeLists.txt` 取源文件。

## 构建与验证

- 环境配置与烧录流程见[固件 README](firmware/README.md)。SDK、工具链、Python 和主机编译器的本机路径通过环境变量或 `firmware/dev-env.json` 配置。
- 使用工程的 `build.ps1` 或 `build-mac.sh`。它们在构建期间应用 USB 控制台相关 SDK 补丁。烧录后将 SW2 从 ON 拨到 OFF 再拨回 ON。电池接通时拔下 USB 不会重启手表。
- 图片表盘和下半区图保存在 media 分区，刷固件后仍可读取。恢复内置下半区图用 `facepanel clear` 或插件设置页「恢复内置」。
- 按改动选择[主机测试和实机验收](firmware/lvgl_test/docs/TESTING.md)。插件测试入口为 `npm --prefix plugin/dsh-protorig-panel test`。
- 修改表盘渲染后用 `firmware/lvgl_test/tools/face_preview_host.py` 生成插件预览。串口或 BLE 台架测试前，先暂停插件对目标设备的连接。

## 修改约定

- 保持原文件的行尾与 BOM。含中文的 PowerShell 脚本使用 UTF-8 BOM，以兼容 Windows PowerShell 5.1。
- 驱动和服务尽量独立于 LVGL，由 UI 任务操作 LVGL。新增 RTC 持久化字段前核对 `main.c` 的 SD3010 用户 RAM 地址表。
- 固件版本定义在 `firmware/lvgl_test/fw_version.h`，插件版本定义在 `plugin/dsh-protorig-panel/package.json`。两套版本独立维护。
- 公开文档中的功能、命令与路径应能在本仓库核对。验证记录应注明提交、固件构建时间和实机环境。
