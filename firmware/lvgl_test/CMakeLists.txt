cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_include_directories(.)
# 目录分层：drivers/ 驱动、bsp/ 板级、app/ 状态与事件、services/ 服务、
# apps/ App 注册表与表盘、ui/ 原生页面。头文件按 basename include。
sdk_add_include_directories(drivers)
sdk_add_include_directories(bsp)
sdk_add_include_directories(app)
sdk_add_include_directories(services)
sdk_add_include_directories(apps)
sdk_add_include_directories(ui)

# ---------------------------------------------------------------------------
# 历史（2026-10-28）：LVGL demo 素材已移除
#
# 原来用 file(GLOB_RECURSE LV_DEMO_SOURCES .../demos/*.c) 把 SDK 例程里的
# 30 MB demo 源码编进来，只为了 shell 的 `bench` 跑 lv_demo_benchmark()。
# bench 命令已删除，这里也不再引用 demo 源文件 —— 省下约 346 KB Flash。
# ---------------------------------------------------------------------------

sdk_set_main_file(main.c)

# 固件目标名沿用 protorig_m2：产物 build/build_out/protorig_m2_bl616.bin
# 被 build/flash_prog_cfg.ini 和插件烧录（tools.mjs / flash.mjs）按名字引用，
# 改名要同步这两处，否则"编译成功但烧录找不到 bin"。
project(protorig_m2)

# USB CDC 虚拟串口沿用 ROM 下载模式的 VID/PID，Windows 不用另装驱动
sdk_add_compile_definitions(-D CONFIG_CONSOLE_USB_CDC_VID=0x349B)
sdk_add_compile_definitions(-D CONFIG_CONSOLE_USB_CDC_PID=0x6160)

# ---------------------------------------------------------------------------
# 电脑模式协议 v1.1 的 `pcmon tick ...` 有 21 个 token；SDK shell 默认
# SHELL_ARG_NUM=16，超了会直接报 "Too many args" 并拒绝执行。
# 提到 32（shell 任务里 argv[] 每处 +64 B，可接受）。
# ---------------------------------------------------------------------------
sdk_add_compile_definitions(-DSHELL_ARG_NUM=32)

# ---------------------------------------------------------------------------
# 调试 shell 命令开关
#   main.c 里「整分花屏 / 触摸低功耗」两轮排查留下的一次性诊断命令都包在
#   #if PROTORIG_DEBUG_SHELL 里。默认 ON，编入诊断命令。
#   发布构建时可用 -DPROTORIG_DEBUG_SHELL=OFF 把这批命令整批去掉（见 ../README.md）。
# ---------------------------------------------------------------------------
option(PROTORIG_DEBUG_SHELL "Compile one-shot diagnostic shell commands" ON)
if(PROTORIG_DEBUG_SHELL)
    sdk_add_compile_definitions(-DPROTORIG_DEBUG_SHELL=1)
else()
    sdk_add_compile_definitions(-DPROTORIG_DEBUG_SHELL=0)
endif()

# ST7802 自研驱动（addr 相位 1 线，SDK 的 bl_mipi_dbi 做不到）
target_sources(app PRIVATE drivers/st7802.c)

# R1（纯搬移）：LVGL 显示端口（flush + TE + 绘制缓冲）
target_sources(app PRIVATE bsp/board_display.c)

# 电脑模式：USB 原始二进制上行（CDC OUT 端点收 A5 5A len 帧 → svc_pc_stage）
# 依赖 sdk_patches/bouffalo_sdk_usb_pc_rx.patch 注入的弱钩子，见 build.ps1
target_sources(app PRIVATE bsp/usb_pc_link.c)

# ---------------------------------------------------------------------------
# 默认表盘 face 0（LVGL）
#   face_lvgl.c   —— 版面：标题/时间/日期用 lv_label，表圈刻度、框标、指针、
#                    分隔线与电量全在一次 LV_EVENT_DRAW_MAIN 里自绘（没有 lv_arc）。
#                    下半区 480x232 @ y=248：内置图来自 watch_photo.c，若装了
#                    facepanel 面板则改用 media 分区的 I8 图（svc_face_panel）。
#   watch_photo.c —— tools/make_watch_photo.py 从 assets/watch_photo/source.png
#                    生成的 Flash RGB565（2026-11-15 起默认图是 DSH×终末地联动标）
# 预览：python tools/face_preview_host.py —— 用同一份 face_lvgl.c 在 PC 上渲一帧，
#       产出插件 plugin/dsh-protorig-panel/assets/faces/default.png
# ---------------------------------------------------------------------------
target_sources(app PRIVATE face_lvgl.c)
target_sources(app PRIVATE watch_photo.c)

# ---------------------------------------------------------------------------
# 触摸（CST9220）+ 息屏/唤醒
#   drivers/tp.c      —— 板级触摸驱动（V1.6 引脚/I2C0，见 README）
#   tp_lvgl.c         —— 包成 LVGL indev
#   bsp/board_power.c —— 空闲息屏 + 触摸唤醒 + PDS（原 power.c 搬入 bsp/）
# ---------------------------------------------------------------------------
target_sources(app PRIVATE drivers/tp.c)
target_sources(app PRIVATE tp_lvgl.c)
target_sources(app PRIVATE bsp/board_power.c)

# ---------------------------------------------------------------------------
# 真实时钟：SD3010 RTC（I2C0 @0x32）
#   i2c_bus.c  —— I2C0 总线（GPIO0/1，使用板上的 I2C 引脚配置）
#   sd3010.c   —— RTC 驱动（OSF/掉电魔数/user RAM）
#   rtc_seed.c —— 无电池/断电后 RTC 寄存器是乱的时候，用编译时间兜底
# ---------------------------------------------------------------------------
target_sources(app PRIVATE drivers/i2c_bus.c)
target_sources(app PRIVATE drivers/sd3010.c)
target_sources(app PRIVATE rtc_seed.c)
# 电池电压（IO20/ADC_CH0）+ MCU 片内温度；移植自 V1.4 watchface/board_adc.c
target_sources(app PRIVATE drivers/board_adc.c)

# 振动马达（IO30 -> Q2 低边驱动 U4 LCM0720A3134F），PWM CH2
target_sources(app PRIVATE drivers/motor.c)

# lv_font_conv 生成的字库（tools/gen_lvgl_fonts.py）
file(GLOB LV_FONT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/fonts_gen/*.c")
target_sources(app PRIVATE ${LV_FONT_SOURCES})

# Native round device pages share the launcher's geometry and visual components.
# The historical EEZ project remains available as a reference.
target_sources(app PRIVATE ui/ui_widgets.c ui/device_pages.c)
# 2026-11-08（APP 第一批）：秒表 / 计时器 / 闹钟三个 App 页
target_sources(app PRIVATE ui/app_pages.c)

target_sources(app PRIVATE eez_ui_glue.c)
target_sources(app PRIVATE pc_monitor.c monitor_protocol.c)

# ---------------------------------------------------------------------------
# app_state（快照）+ event_queue（单队列，event_t 只有 type/len/data[64]）
#   已在使用：main.c（UI 主循环消费）、svc_battery.c（电压/充电事件）、
#   svc_ble.c（连接状态）。event_queue 的 ISR 版本用 FromISR；UI 任务用 pop(...,0)。
# ---------------------------------------------------------------------------
target_sources(app PRIVATE app/app_state.c app/event_queue.c)

# ---------------------------------------------------------------------------
# 版本化设置（SD3010 用户 RAM 0x30 起 14 B，避开 0x2C 的 RTC 魔数）
#   settings_codec.c —— 纯逻辑编解码 + CRC16，host 单测：
#       tests/host/run_host_tests.ps1
#   svc_settings.c   —— SD3010 读写 + v1/旧 0x31 迁移 + 回读校验
# ---------------------------------------------------------------------------
target_sources(app PRIVATE services/settings_codec.c services/svc_settings.c)

# RTC 时间服务（中断/轮询/播种/对时；无 LVGL）
target_sources(app PRIVATE services/svc_time.c)
# 2026-11-08（APP 第一批）：秒表 / 倒计时 / 闹钟服务（纯状态机，无 LVGL；
# 闹钟的持久化与 RTC arming 通过回调注入，host 可测）
target_sources(app PRIVATE services/svc_stopwatch.c services/svc_timer.c
                     services/svc_alarm.c)
# 电池/充电服务（ADC/低电分级/息屏兜底；无 LVGL）
target_sources(app PRIVATE services/svc_battery.c)
# F1（2026-11-08）：设备身份 —— eFuse chip ID -> FNV-1a -> 低 24 bit 管理员编号；
#   表盘 / BLE 广播名 / GET_INFO / shell adminid 共用（无 LVGL 依赖）。
target_sources(app PRIVATE services/svc_device_id.c)
# ---------------------------------------------------------------------------
# App 注册表 + 表盘注册 + 通知 + ui_theme + svc_demo 接口桩
#   * app_framework_init() 注册后，页面切换由 app_launch_page() 驱动（已接管）。
#   * 2026-11-08：face 1（极简）/ face 2（监控）已删除，只留
#     face 0（默认表盘）+ face 3（图片表盘）。
#   * svc_demo.c 只是 demo 模式的接口桩：main.c 里只有 svc_demo_init()，
#     没有 shell 命令、也没有消费者，别当成可用的演示模式。
# ---------------------------------------------------------------------------
target_sources(app PRIVATE apps/app_framework.c apps/faces.c
                     apps/face_default.c)
target_sources(app PRIVATE apps/pages.c)
# B（表盘可替换）：face 3 = media 分区里的 480x480 索引图 + 存储层（无 LVGL）
target_sources(app PRIVATE apps/face_image.c)
target_sources(app PRIVATE services/svc_face_store.c)
# 默认表盘下半区面板：face 0 的 480x232 可替换图 + 存储层（无 LVGL；
# flash 槽 0x3B2000，header+112,384 B I8，shell `facepanel` 安装/清除）
target_sources(app PRIVATE services/svc_face_panel.c)
target_sources(app PRIVATE apps/app_notify.c)
target_sources(app PRIVATE apps/app_list.c)
target_sources(app PRIVATE ui/ui_theme.c)
target_sources(app PRIVATE services/svc_demo.c)


# ---------------------------------------------------------------------------
# BLE —— 已编入并实机可用（CONFIG_BLUETOOTH / CONFIG_RF 已在 defconfig 打开）
#     ble_link.c —— rfparam_init -> controller -> host -> GATT 注册 -> 广播
#     ble_svc.c  —— DIS 0x180A / BAS 0x180F / CTS 0x1805 / 自定义服务（0x50-0x59）
#     services/svc_ble.c —— 连接/电源模式/数字通道；数字通道与卡片层由宿主插件驱动
#
#   ble_ui.c 是早期"BLE 事件 -> LVGL"的历史桩：**不参与编译**（CMake 不引用），
#   不要以为它在跑。当前 BLE 的 UI 交互走 svc_ble.c + app_notify / 面板渲染。
# ---------------------------------------------------------------------------
target_sources(app PRIVATE ble_link.c ble_svc.c)
target_sources(app PRIVATE services/svc_ble.c)
target_sources(app PRIVATE services/svc_pc.c)
