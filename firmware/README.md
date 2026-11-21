# 固件工程（lvgl_test）

BL616（Ai-M62-CBS）+ LVGL 9.5 + FreeRTOS + Bouffalo SDK，480×480 AMOLED 表。

## 环境

| 项 | 环境变量 | 说明 |
|---|---|---|
| SDK | `BL_SDK_BASE` | Bouffalo SDK 根目录（仓库不含 SDK） |
| 工具链 | `BL_TOOLCHAIN_DIR` | RISC-V GCC 工具链目录 |
| Python | `BL_PYTHON_DIR` | 带 Pillow 的 Python（生成字库/图片用） |
| 主机 gcc | `HOST_CC` | 主机测试/预览用（默认 `gcc`） |

可用环境变量或本机配置文件提供工具路径，环境变量优先。

1. 环境变量（上表）；
2. `firmware/dev-env.json`（**不入库**，.gitignore 已排除）：
   ```json
   {
     "BL_SDK_BASE": "<SDK-dir>",
     "BL_TOOLCHAIN_DIR": "<toolchain-dir>",
     "BL_PYTHON_DIR": "<python-dir>",
     "HOST_CC": "<host-gcc>",
     "PROTORIG_FONT_DIR": "<ttf-dir>"
   }
   ```
   PowerShell 脚本与主机 Python 工具都会自动读取该文件。直接运行 `make` 时使用 `firmware/dev-env.mk`。

**Windows**：将 SDK 的 `tools/make/` 中的 `.exe` 和 `.dll` 放入 `firmware/lvgl_test/tools/make/`。
构建脚本从该目录载入 `mingw32-make.exe`，从 SDK 的 `tools/cmake/bin/` 载入 CMake。
以下命令从仓库根目录运行，需先设置 `BL_SDK_BASE` 环境变量。

```powershell
Copy-Item "$env:BL_SDK_BASE/tools/make/*.exe" firmware/lvgl_test/tools/make/
Copy-Item "$env:BL_SDK_BASE/tools/make/*.dll" firmware/lvgl_test/tools/make/
```

**macOS**：`bash firmware/tools/setup-mac-build.sh` 一次性装依赖，
然后 `cd firmware/lvgl_test && ./build-mac.sh --clean`。工具链是官方 `gcc-riscv-14.3.0-macos-arm64`；
烧录用 SDK 自带 `BLFlashCommand-macos`（x86_64，需要 Rosetta 2）。细节见 `firmware/tools/setup-mac-build.sh`
与 `firmware/lvgl_test/build-mac.sh`。

## 编译 / 烧录

```powershell
cd firmware/lvgl_test
.\build.ps1                                # 编（工程脚本会临时给 SDK 打 usb_pc_rx / usb_console_delay 补丁，结束自动还原）
.\flash.ps1 -AppPort COM8                  # 让应用跳回 ROM 下载模式并烧（自动找 ROM 口）
.\flash.ps1 -Port COM7                     # 已经在 ROM 下载模式时直接烧
```

> - 也可从 `firmware/` 运行 `.\build.ps1 lvgl_test`，该入口会调用工程自己的构建脚本。
> - 从 agent 的 bash / Git Bash（MSYS）里编译，用 `firmware/build-agent.ps1 lvgl_test`
>   （它会清掉 MSYS 的 `SHELL=/usr/bin/sh` 和 PATH，否则 SDK 的 Windows 路径会被 `sh` 吃掉）。
> - `flash.ps1` 会比较构建相关源码和固件产物的时间，默认拒绝烧录过旧的 bin。
>   确实要烧旧产物用 `-Force`，或 `-Bin artifacts\<image>.bin` 指定已验证的固件文件。
> - **烧完必须手动拨动开关重启**：SW2 ON → OFF → ON（拔 USB 不会断开电池供电）。ROM 下载模式状态记在 AON 域，
>   软复位清不掉——这是最容易忘的一步。
> - 烧录只覆盖 boot2 / 分区表 / 应用；**media 分区不擦**，所以自定义图片表盘/下半区图会保留，
>   想回内置图要 `facepanel clear`（或插件里"恢复内置"）。

## 调试命令开关（PROTORIG_DEBUG_SHELL）

`main.c` 用 `#if PROTORIG_DEBUG_SHELL` 控制显示和触摸诊断命令。

- 选项：`PROTORIG_DEBUG_SHELL`（CMake option，默认 `ON`）
- `OFF` 后失去这批命令：`pxpat`、`fill`、`fills`、`gramrd`、`minroll`、`rollsim`、`drain`、
  `winguard`、`flushdiag`、`wrclk`、`pxorder`、`tptrace`、`tpaccept`、`tpgate`、`tpsleep`、
  `tpwake`、`tpgesture`、`tpprox`、`tpmode`、`tpnormal`、`i2cfreq`、`pdstest`
- 仍然保留：`batt`、`battlog`、`screen`、`idle`、`settings`、`lowbatt`、`panel`、`panelpwr`、
  `tpstat`、`rtcstat`、`settime`、`rtcalarm`、`te`、`stats`、`face`、`faceimg`、`facepanel`、
  `clear`、`uidiag`、`pcmon`、`pds`、`pdslog`、`motor`、`chrg`、`alwayson`、`wakefull`、`appstat`

发布构建示例：

```powershell
# 把 lvgl_test/CMakeLists.txt 里的 option(PROTORIG_DEBUG_SHELL ... ON) 改成 OFF，再编译
```

## 代码结构

| 文件 / 目录 | 干什么 |
|---|---|
| **main.c** | 主程序：任务、shell 命令、RTC 轮询、电量采样、低电分级、电压记录仪、版本号 |
| **drivers/st7802.c** + **bsp/board_display.c** | ST7802 AMOLED 驱动（QSPI DBI，40 MHz，TE 同步）+ LVGL 显示端口（flush/绘制缓冲） |
| **face_lvgl.c** | 默认表盘 face 0（LVGL 9.5）：表圈刻度/指针/框标自绘；下半区 480×232 图来自 `watch_photo.c` 或 facepanel 面板 |
| **watch_photo.c** | `tools/make_watch_photo.py` 生成的 Flash RGB565（480×232）；默认图为 DSH×终末地联动标 |
| **pc_monitor.c / monitor_protocol.c** | USB 监控页（电脑模式）、透传协议 v1.1、像素帧与触摸回传 |
| **services/svc_pc.c** | 电脑模式服务：USB/BLE 两条链路、像素透传、状态帧、断线回表盘 |
| **eez_ui_glue.c** | 分页/手势层（`PAGE_N=8`）：0 表盘、1 协议网络（占位）、2 相遇记录（占位）、3 设置、4 USB 监控、5 秒表、6 计时器、7 闹钟 |
| **apps/** | App 注册表、圆形启动器（协议网络/相遇记录当前 hidden）、通知卡、表盘注册、页面 App 描述符 |
| **ui/** | 原生页面组件（ui_widgets / device_pages / app_pages / ui_theme），EEZ 工程只是历史参考 |
| **services/** | 时间/设置/电池/设备身份/秒表/计时器/闹钟/图片表盘/下半区面板/BLE/电脑模式 |
| **bsp/** | board_power（息屏/PDS/充电脚）、board_display（LVGL 显示端口）、usb_pc_link |
| **drivers/** | st7802、tp（CST9220）、sd3010（RTC）、i2c_bus、board_adc、motor |
| **app/** | app_state 快照 + event_queue（BLE/电源事件） |

> `terrain565.c`、`ble_ui.c` 是历史模块，不参与当前构建。

## 主机测试 / 工具

按改动选测试、查实机验收判据，见[测试与验收](lvgl_test/docs/TESTING.md)。下面的命令从 `firmware/lvgl_test/` 运行。

| 命令 | 作用 |
|---|---|
| `python tools/device_ui_host.py` | 用 mingw gcc 编原生页面/启动器等，跑手势回归并输出 `tools/ui_host_output/*.png` |
| `python tools/face_preview_host.py` | 用同一份 `face_lvgl.c` 在 PC 上渲真表盘一帧 → `tools/face_host_output/face.png`，并更新插件预览图 |
| `powershell -File tests/host/run_host_tests.ps1` | settings_codec / power 时序等纯逻辑单测 |
| `python tools/make_watch_photo.py` | 由 `assets/watch_photo/source.png` 生成 `watch_photo.c` + `preview.png`（需要 Pillow） |
| `python -X utf8 tools/gen_lvgl_fonts_open.py` | 从思源黑体 OTF 生成 `fonts_gen/*.c`，准备步骤见 [字体说明](lvgl_test/fonts_src/README.md) |
| `python tools/face_install.py` / `face_panel_install.py` | 命令行安装图片表盘 / 下半区面板（USB） |

> 主机工具编出来的 exe / ppm / png 都在 `tools/*_output/`，已 gitignore。

## Shell 速查（真机）

| 命令 | 作用 |
|---|---|
| `batt` | 电池电压/百分比/充电/温度 |
| `battlog [clear]` | RTC RAM 里的电压记录仪（每 2 分钟一条） |
| `lowbatt 0\|1\|2\|auto` | 强制低电档位（验 UI/震动） |
| `screen 0\|1`、`panel on`、`panelpwr 0\|1` | 息屏/亮屏、面板状态自证、面板电源 |
| `idle <ms>`、`settings` | 空闲熄屏时间、查看/设置亮度与息屏档 |
| `rtcstat`、`settime Y M D h m s`、`rtcalarm ...` | RTC 状态 / 对时 / 闹钟 |
| `face`、`faceimg`、`facepanel [info\|clear\|begin…]` | 表盘状态、图片表盘状态、下半区面板 |
| `pcmon ...` | 电脑模式协议 v1.1（宿主插件用） |
| `uidiag`、`appstat`、`tpstat`、`stats` | 页面/触摸/送屏诊断 |
| `pds`、`pdslog`、`motor`、`chrg`、`alwayson`、`wakefull` | 低功耗/马达/充电/常亮 |
| `adminid` | 打印设备身份（与 BLE 广播名一致） |

## 字体

- 编译用 `lvgl_test/fonts_gen/*.c`（已生成，直接编）；
- 重新生成固件字库时按 [字体说明](lvgl_test/fonts_src/README.md)准备 OTF，并设置 `PROTORIG_FONT_DIR`。
- 插件面板字体是另一套（`plugin/dsh-protorig-panel/assets/fonts/`，Noto 子集 + OFL）。

## 调试注意事项

### 串口 / 脚本

- **`printf` 里只能写 ASCII**：中文会乱码甚至让 shell 卡住。
- **`.ps1` 必须存成 UTF-8 with BOM**（Windows PowerShell 5.1 否则按 ANSI 解析中文注释，
  可能导致脚本解析异常）。
- **`.c/.h` 的 EOL 不要被工具整文件改掉**：仓库是 LF；整文件重写会产生巨大的假 diff。
- 烧录脚本使用 SDK 的 BLFlashCommand。工具路径见[固件工具](tools/README.md)。

### 中断

- BL616 的 GPIO 中断是分组合并的，`gpio_all_isr` 遇到没挂载的脚会 assert。只 unmask 真正用到的脚。
- `bflb_gpio_pad_check()` 内部有 `ebreak`，别在正常路径调。

### 显示

- 全屏刷新 ~62 ms（40 MHz QSPI），别指望 60 fps；表盘只刷变化区域。
- 刷屏策略见 `lvgl_test/flush_policy.h`，显示时序和读回探针见 [测试与验收](lvgl_test/docs/TESTING.md)。
- `te 0` 能跑但快速刷新会撕裂。

### RTC

- SD3010 的 VDD 是 BAT+，掉电就停，读出来可能是"看着合法"的乱值。判据在 `rtc_seed.c`：
  日期合法性 + 星期自洽 + user-RAM 魔数；判为丢电会自动播种。
- `settime` 之后必须写魔数，否则下次开机又会被判成丢电。

### 电量 / 低功耗

- LVGL 定时器只在亮屏时跑，息屏期间不采样，唤醒时补一次（不是 bug）。
- 低功耗路径见 `lvgl_test/bsp/board_power.c`。手表模式的 PDS15 深睡通过复位唤醒，USB 在启动时重新枚举。功耗需在电池支路实测。
- 3.2 V 低电关机态只留 CHRG 下降沿唤醒（避免保护板硬切把 RTC 打丢）。
- USB 供电或电脑模式期间禁止自动 PDS。电脑模式息屏后继续处理 USB/BLE 帧和掉线看门狗。

### 马达

- IO30 PWM CH2（20 kHz）；启动瞬间 BAT+ 会掉十几 mV，电量采样要避开马达运行。
