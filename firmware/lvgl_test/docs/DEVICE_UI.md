# 圆屏应用与设备设置

界面采用纯黑底、白色正文、灰色说明和黄色选中态。应用列表、设置总览/子页
使用同一组原生 LVGL 组件（`ui/ui_widgets.c`）。

## 页面与 App 列表

- 固件共 8 个页面容器（`eez_ui_glue.c` 的 `PAGE_N=8`）：
  0 默认表盘 / 1 协议网络（**占位，列表隐藏**）/ 2 相遇记录（**占位，列表隐藏**）/
  3 终端设置 / 4 USB 监控（电脑模式，表盘右滑进入）/ 5 秒表 / 6 计时器 / 7 闹钟。
- 应用列表（`apps/app_list.c`）每页 3 项；`hidden=1` 的条目不显示。
  当前可见 4 项：**第 1 页 = 设置 / 秒表 / 计时器，第 2 页 = 闹钟**。
  上滑、下滑和底部箭头换页，顶部返回回表盘。
- 协议网络和相遇记录的占位布局在 `ui/device_pages.c`，没有用户入口。

## 秒表 / 计时器 / 闹钟

- **秒表**：大号 `mm:ss.cc`；「开始 / 暂停 / 继续」+「复位」。状态在
  `services/svc_stopwatch.c`，离开页面回列表或切到别的 App 继续计时。界面 33 ms 刷新百分秒。
  PDS 深睡唤醒是复位启动，秒表状态不跨 PDS。
- **计时器**：预设分钟数（1/3/5/10/30 循环，另可 +/- 调 1..99 分钟）；「开始 / 暂停 / 继续」+
  「取消」；到点 `services/svc_timer.c` 上报，主循环亮屏 + 震动 + 通知卡。计时器运行期间
  **不进入 PDS**（PDS15 唤醒=复位会丢状态），到点后恢复正常睡眠。
- **闹钟**：HH:MM + 每天重复开关 + 持久化（settings v2 的 `alarm_h/alarm_m/alarm_flags`）；
  到点震动 + 通知。深睡唤醒走 **SD3010 的 HH:MM 报警寄存器 → INT(IO17) AON 低电平唤醒 →
  PDS15 复位启动 → 启动补触发**；关机态和 HBN 模式不保证唤醒。

设备三页（设置/协议网络/相遇记录）+ 三个 App 页都走统一组件库；布局按 480×480 圆屏、
安全半径 207 摆放。

## 通知卡

文本通知卡（`apps/app_notify.c`）：**居中 368×136、圆角 24**（四角到圆心 196.2 < 207），
标题 accent、正文 `LV_LABEL_LONG_WRAP` 折行（336×76），3 s 自动消失，可点按关闭；
位图卡优先于文本通知卡。

PC 模式（USB 透传 / BLE PC）下到点也弹这张卡：固件**不退出 PC 模式、不拆 session**，
暂停接收 PC 像素帧 6 s 并恢复本地渲染。通知关闭后，固件发送 `0x92 REDRAW_REQ` 请求宿主重发全屏。

设置总览显示真实亮度、息屏时间和连接模式。点击条目进入三选一页面，选中态由黄字、勾和短线提示。
选择后沿用 power/BLE 服务与持久化钩子。顶部返回或右滑先回设置总览，再回应用列表；
外部修改设置后当前页面会同步。

## 源码

- `ui/ui_theme.h` 定义颜色和字体。
- `ui/ui_widgets.c` 提供原生标题、线条图标、列表条目与文字按钮。
- `ui/device_pages.c` 创建设置页（page 3）与协议网络/相遇记录占位（page 1/2）。
- `ui/app_pages.c` 创建秒表/计时器/闹钟三个 App 页，处理触摸目标。
- `apps/app_list.c` 负责应用清单、可见性（`hidden`/`s_vis`）、翻页和启动。
- `apps/pages.c` 注册 page_id 0..7 的前台页面 App。
- `services/svc_stopwatch.c` / `svc_timer.c` / `svc_alarm.c` 保存状态机与到点判定（不碰 LVGL）。
- `eez_ui_glue.c` 保留统一输入与防误触路径（PAGE_N = 8）。

主条目热区 256×96 px，三个条目从 y=112 开始排列。原 EEZ 工程（`firmware/eez_ui/`）
保留为历史参考，不参与编译。

## 预览与验证

```powershell
python -X utf8 firmware/lvgl_test/tools/device_ui_host.py
```

输出在 `firmware/lvgl_test/tools/ui_host_output/`（已 gitignore）。覆盖：进列表、启动 App、
三组设置与持久化调用、移开取消点击、设置子页返回、列表返回防回弹、翻页、秒表开始/暂停/复位、
计时器预设与到点、闹钟调时与开关，并检查可见标签的字形覆盖与圆内边界。
主机侧电源/BLE I/O 用桩，输入路由和界面用固件真实源码。

新增中文文案时，可按[字体说明](../fonts_src/README.md)准备思源黑体 OTF 并重生成字库。

```powershell
$env:PROTORIG_FONT_DIR='<目录，含 SourceHanSansSC-*.otf>'
python -X utf8 firmware/lvgl_test/tools/gen_lvgl_fonts_open.py
python -X utf8 firmware/lvgl_test/tools/font_coverage.py
```
