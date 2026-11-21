# 测试与验收

从仓库根目录运行下面的命令。固件版本读 `firmware/lvgl_test/fw_version.h`，上板后用 `adminid`
同时核对设备编号和构建时间。插件版本读 `plugin/dsh-protorig-panel/package.json`，DSH 联调时再确认
当前 profile 加载的版本。

## 无硬件回归

| 改动范围 | 从仓库根目录执行 | 覆盖与前置 |
|---|---|---|
| 设置编码、息屏与唤醒时序 | `powershell -NoProfile -ExecutionPolicy Bypass -File firmware/lvgl_test/tests/host/run_host_tests.ps1` | `gcc` 在 PATH。覆盖旧设置格式、CRC、唤醒计时对齐，硬件调用用桩。 |
| 圆屏、页面、手势、App | `python -X utf8 firmware/lvgl_test/tools/device_ui_host.py` | SDK 内的 LVGL、`HOST_CC` 或 PATH 的 gcc。检查流程、字形和圆内边界，截图位于 `firmware/lvgl_test/tools/ui_host_output/`。详见 [DEVICE_UI.md](DEVICE_UI.md)。 |
| 默认表盘渲染 | `python -X utf8 firmware/lvgl_test/tools/face_preview_host.py --no-copy` | SDK 的 LVGL、gcc、Pillow。图在 `firmware/lvgl_test/tools/face_host_output/face.png`。确认要更新插件预览时去掉 `--no-copy`。 |
| 图片表盘 PFI | `python -X utf8 firmware/lvgl_test/tools/face_image_host_render.py <样本.pfi>` | SDK 的 LVGL、Pillow；`HOST_CC` 填 gcc 的完整路径。检查 PFI 加载、CRC 与渲染，默认 PNG 为 `firmware/lvgl_test/tmp/face_image_host.png`。 |
| ST7802 探针 | 见下方命令 | 验 CRC、测试图样编码、GRAM 读回偏移与整分探针状态机。面板实际读回还要上板。 |
| DSH 插件 | `npm --prefix plugin/dsh-protorig-panel test` | Node ≥ 22，装好插件依赖。覆盖串口/BLE 状态机、卡片、触摸、表盘打包/上传、烧录编排、设置页。 |
| 插件发行依赖 | `npm --prefix plugin/dsh-protorig-panel run check:release` | 核对 npm 依赖和包内资源。查看最终退出码和 `PASS`。 |

改动固件后，按 [固件 README](../../README.md) 的环境配置，在 `firmware/lvgl_test/` 运行 `build.ps1`
（Windows）或 `build-mac.sh`（macOS）。构建入口负责临时 SDK [补丁](BUILD_SDK_PATCH.md)。
纯主机测试能验逻辑和渲染，USB 枚举、BLE 无线链路、RTC/PDS 唤醒及电流需要实机证据。

用 gcc 编译并运行 ST7802 主机探针。

```powershell
$probeOut = 'firmware/lvgl_test/tools/host_output'
New-Item -ItemType Directory -Force -Path $probeOut | Out-Null
gcc -O1 -std=c99 -DST7802_HOST_VERIFY -I firmware/lvgl_test -I firmware/lvgl_test/drivers firmware/lvgl_test/tools/host/st7802_verify.c -o "$probeOut/st7802_verify.exe"
if ($LASTEXITCODE -eq 0) { & "$probeOut/st7802_verify.exe" }
```

## 实机冒烟与链路定位

1. 烧录结束后手动拨 SW2 **ON → OFF → ON**。电池仍接通时，拔 USB 无法清掉 ROM 下载模式。
   确认应用口重新枚举，再用 `adminid` 核对目标板和构建时间。
2. 按当前连接方式查表盘、设置、触摸、息屏唤醒。Windows 可用
   `powershell -File firmware/tools/verify_v16.ps1 -Port COM8` 读 `batt`、`chrg`、`tpstat`、`rtcstat`。
   脚本打印串口结果，需人工判读；`COM8` 换成实际应用口。
3. 先让 DSH 插件、串口终端或其他测试工具释放目标口/连接，再按下表测试。多块表时明确指定目标；
   有线与无线分开测，记录是否有 USB 供电、电脑模式开关和手表连接模式。

| 要确认的行为 | 入口与判据 |
|---|---|
| USB 像素透传、触摸回传 | `powershell -File firmware/lvgl_test/tools/usb_pc_test.ps1 -Port COM8 -Band -TouchMs 15000`。验证 HELLO、CLEAR、瓦片/像素带、触摸坐标与退出后 shell 恢复。先看 `bad` 和 `ring_drop`。 |
| USB 宿主退出后回表盘 | 用 shell `pcmode 1` 后释放串口，运行 `node firmware/lvgl_test/tools/pc_host_loss_test.mjs COM8 5000`（依赖插件的 `serialport`）。主机读流量时保留会话，脚本退出后约 3 秒回表盘；重开串口查 `pcmode`，应见 `active=0`、`passthrough=0`，`host_gone` 增加。普通无链路的退出阈值是 60 秒。 |
| USB/BLE 卡片台架 | `python firmware/lvgl_test/tools/card_test.py --transport usb --port COM8 --stage all`，或 `python firmware/lvgl_test/tools/card_test.py --transport ble --stage all`。USB 需 `pyserial`，BLE 需 `bleak` 且先进入电脑模式等待广播；多块表用 `--name` 或 `--address` 指定。检查 `0x59` 回执、`0x58` 热区，`--stage p3 --hold 20` 可手点并观察 `0x04`。USB 台架保持本地页，另用插件确认像素透传和端到端审批/问答。 |
| 电脑模式息屏保留会话 | USB、BLE 各跑一轮。关掉对应通道常亮，设短息屏时间，等待黑屏并确认宿主会话仍在线。BLE 一轮用纯电池供电，并覆盖手表「省电」档。唤醒后确认画面及交互继续，再验证显式退出或失链能回表盘。 |
| 链路回切、双表识别 | 在 DSH 的 `/state` 和设置页核对设备编号、`transport` 和 `pcMode`。同一块表切 USB/BLE 保持同一 `admin_id`；另一块表广播或插线时保持已选设备。 |
| 表盘、下半区、闹钟 | 用插件或 `firmware/lvgl_test/tools/` 下的 `face_install.py`、`face_panel_install.py` 安装，核对设备端 CRC 和重启恢复。`facepanel clear` 清除下半区。刷固件保留 media。电脑模式里再触发倒计时/闹钟，观察弹窗、实际震动及 `0x92` 重绘恢复。 |
| 手表模式睡眠与 RTC | 纯电池供电、退出电脑模式，再测省电/智能档行为。核对 `pdslog`、`rtcstat`、唤醒源与设置持久化。屏幕黑了只能证明息屏，电流需在电池支路测。 |

## DSH 端到端

在测试 profile 启动新宿主进程后再验收。用设置页或 `/state` 核对连接与版本，然后发起一轮测试会话。

1. 手表随回合切换状态并显示会话标题。中途工具报错后回合仍正常完成时，应显示完成态。
2. 同时触发两张测试审批或问答卡，确认先来先显示，选项回传给对应会话。验证无人应答后回到电脑确认。
3. USB 下测试通知卡超时恢复及触摸，BLE 下测试卡片回执和震动。打开系统蓝牙权限，分别观察断开和回切。

## 留存结果

- 主机测试记命令、提交号、环境和退出码。涉及画面时附对应截图。
- 实机测试再记板号、`adminid` 的版本与构建时间、插件/profile、供电、传输、测试动作和观察到的结果。
  串口可用 `firmware/lvgl_test/tools/g0_serial.ps1 -Port COM8 -Probe -NoLog` 看即时结果，留档时用 `-OutDir` 显式指定位置。
- 区分直接观测、模型估算和待复测项。记录失败后补上恢复步骤及验证结果。
