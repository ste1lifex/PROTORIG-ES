/*
 * protorig_m2 —— PROTORIG ES M2 主固件（LVGL v9.5 + FreeRTOS + Bouffalo SDK）
 *
 * CMake 工程名 protorig_m2（目录名沿用 V1.6 的 lvgl_test）。W1（2026-10-28）
 * 起在 V1.6 固件基础上并入 BLE（DIS/BAS/CTS/私有服务），并删除只服务
 * lv_demo_benchmark 的 LVGL demo 素材（约 336 KB Flash）；屏幕/触摸/RTC/
 * 电源管理沿用 V1.6 行为。
 *
 * 为什么自己写显示端口：
 *   SDK 的 components/graphics/lvgl_v9/port/lv_port_disp_mcu.c 依赖 BSP_LCD，
 *   而 BSP_LCD 里没有 ST7802；更关键的是它的 QSPI addr 相位固定 4 线
 *   （那是 ST77926 的时序），与 ST7802 要求的 1 线不兼容。
 *   所以这里只写一个 ~15 行的 flush，复用 V1.4 已验证的 st7802.c。
 *
 * 硬件对应（V1.6，见 ../README.md）：
 *   屏  AM160Q480480LK5 480x480 AMOLED / ST7802 / QSPI
 *   IO10..IO15 = CLK/CS/D0..D3（各串 22Ω），IO27=RST IO28=TE IO29=VCI_EN
 *   IO16=TP_INT(AON) IO17=RTC_INT(AON) IO0/IO1=I2C0（SD3010 + CST9220）
 *   写时钟实测可到 40 MHz；读时钟上限 ~15 MHz
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "board.h"
#include "bflb_gpio.h"
#include "bflb_mtimer.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "st7802.h"
#include "fw_version.h"        /* 版本号唯一来源：adminid fw= / BLE DIS / GET_INFO */

#include "lvgl.h"
#include "flush_policy.h"
#include "board_display.h"     /* R1：显示端口已搬到 bsp/board_display.c */
#include "app_state.h"         /* 全局状态快照（BLE/电源事件与 UI 共享） */
#include "event_queue.h"       /* 事件队列（svc_time/svc_battery/svc_ble -> UI） */
/* LVGL demo（demos/lv_demos.h）已移除：shell 的 bench 命令与 LV_DEMO_SOURCES
 * 一并删掉，约省 346 KB Flash。见 CMakeLists.txt 与本地 research/13_*.md（不发布）。 */

#include "face_lvgl.h"
#include "eez_ui_glue.h"
#include "tp.h"
#include "tp_lvgl.h"
#include "board_power.h"
#include "svc_settings.h"      /* R2b：版本化设置（0x30 起 14 B） */
#include "svc_time.h"         /* R3c：RTC 时间服务 */
#include "svc_battery.h"      /* R3c battery service */
#include "svc_stopwatch.h"    /* 2026-11-08：秒表服务（APP 第一批） */
#include "svc_timer.h"        /* 2026-11-08：倒计时服务 */
#include "svc_alarm.h"        /* 2026-11-08：闹钟服务（RTC 报警 + PDS 唤醒） */
#include "svc_ble.h"
#include "svc_pc.h"          /* R4：BLE 服务桥接 */
#include "svc_device_id.h"   /* F3：设备身份（shell adminid；BLE 同源） */
#include "app_framework.h"   /* R3a：App 注册表（纯新增） */
#include "app_notify.h"      /* R3a：通知弹层骨架 */
/* 76 §1：face 1/2 已删除，备用表盘 overlay 的头文件不再存在 */
#include "face_image.h"      /* B：图片表盘 App（face id 3） */
#include "svc_face_store.h"  /* B：media 分区图片表盘存储层 */
#include "svc_face_panel.h"  /* 76：默认表盘下半区面板（media 分区第二个槽） */
#include "svc_demo.h"        /* R3a：demo 接口桩 */
#include "ui_theme.h"        /* R3a：统一 UI 主题常量 */

/* PDS（power down sleep）实验用 —— 见 docs/power_budget_and_sleep_plan.md */
#include "bl616_pds.h"
#include "bl616_hbn.h"
#include "bl616_pm.h"
#include "bl616_glb.h"

#include "i2c_bus.h"
#include "sd3010.h"
#include "board_adc.h"
#include "motor.h"
#include "rtc_seed.h"

/* BLE 已编入（ble_link.c / ble_svc.c / services/svc_ble.c），实机可用。
 * 历史：W1 完成集成后按"先地基后楼层"推迟到 R4 才接入。ble_ui.c 是当年的
 * 事件消费桩，现在不编译 —— BLE 事件由 svc_ble_tick() 在 UI 任务里 drain。 */

#include "shell.h"
#include "usbd_core.h"
#include "pc_monitor.h"

/*
 * 调试 shell 命令开关（CMake option：PROTORIG_DEBUG_SHELL）
 *
 * 默认 = 1，行为与历史版本完全一致；发布构建可用 CMake 选项
 * -DPROTORIG_DEBUG_SHELL=OFF 关掉“整分花屏 / 触摸低功耗”两轮排查留下的一次性
 * 诊断命令，减小固件体积。这里兜底，避免不经 CMake 单独编译时宏未定义、
 * 把整批命令意外删掉。
 */
#ifndef PROTORIG_DEBUG_SHELL
#define PROTORIG_DEBUG_SHELL 1
#endif

static pm_link_t s_pc_link;
static volatile int s_pc_view_req;
static uint32_t pc_now(void) { return (uint32_t)xTaskGetTickCount()*portTICK_PERIOD_MS; }

/* A live pcmon session also implies a real USB host is attached. Do not enter
 * PDS while the PC is using the monitor; after unplug the session lease clears
 * within 5 s and PDS may proceed. */
int pc_link_active(void)
{
    int active;
    taskENTER_CRITICAL();
    active = s_pc_link.session;
    taskEXIT_CRITICAL();
    return active;
}

/* Shell only updates bounded protocol state. LVGL/DBI work happens in service. */
static void pc_monitor_service(void)
{
    pm_link_t state;
    bool usb_configured=usb_device_is_configured(0);
    taskENTER_CRITICAL();
    pm_poll(&s_pc_link,pc_now(),usb_configured);
    state=s_pc_link;
    int view=s_pc_view_req;s_pc_view_req=0;
    taskEXIT_CRITICAL();
    if(view==1) eez_ui_monitor();
    else if(view==2) eez_ui_leave();
    /* R2c：pc_monitor 不再 include board_power.h；监控页策略在这里应用 */
    /* 电脑模式下显示策略由 svc_pc 独占：两个 writer 每帧互相覆盖会把
     * 亮度寄存器反复重写，表现就是监控页的字在闪。 */
    if (!svc_pc_active()) {
        power_set_monitor_policy(state.session && state.frame && pc_monitor_page_active(),
                                 state.brightness, state.always_on);
    }
    /* P0-4：表盘右滑能不能进 USB 监控页，只看 USB 有没有被主机配置起来 */
    pc_monitor_set_usb_ready(usb_configured);
    pc_monitor_update(&state, pc_monitor_page_active());

    static uint32_t acked_revision;
    if(state.session && state.frame && acked_revision!=state.revision) {
        acked_revision=state.revision;
        printf("\r\n@PM1 ACK %s %lu %d %d %u %u %lu %d %d\r\n",state.nonce,
            (unsigned long)state.seq,eez_ui_page(),power_monitor_active(),
            power_effective_brightness(),power_get_brightness(),
            (unsigned long)power_get_idle_ms(),power_get_always_on(),power_is_on());
    }
}

static int cmd_pcmon(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"view") &&
       (!strcmp(argv[2],"monitor") || !strcmp(argv[2],"watch"))) {
        s_pc_view_req=!strcmp(argv[2],"monitor")?1:2;return 0;
    }
    if(argc==2 && !strcmp(argv[1],"status")) {
        taskENTER_CRITICAL();pm_link_t s=s_pc_link;taskEXIT_CRITICAL();
        printf("\r\n@PM1 STATUS usb=%d session=%d frame=%d seq=%lu page=%d override=%d brightness=%u watchBrightness=%u watchIdle=%lu watchAlways=%d screen=%d\r\n",
            usb_device_is_configured(0),s.session,s.frame,(unsigned long)s.seq,
            eez_ui_page(),power_monitor_active(),power_effective_brightness(),
            power_get_brightness(),(unsigned long)power_get_idle_ms(),power_get_always_on(),power_is_on());
        return 0;
    }
    char reply[160];
    bool configured=usb_device_is_configured(0);
    taskENTER_CRITICAL();
    bool ok=pm_receive(&s_pc_link,argc,argv,pc_now(),configured,reply);
    taskEXIT_CRITICAL();
    if(reply[0]) printf("\r\n%s\r\n",reply);
    else if(!ok) printf("\r\n@PM1 ERROR invalid-or-expired\r\n");
    return ok?0:-1;
}
SHELL_CMD_EXPORT_ALIAS(cmd_pcmon,pcmon,PC monitor protocol v1);

/* 电脑模式（-1 屏透传）：查询 / 强制进出 */
static int cmd_pcmode(int argc, char **argv)
{
    return svc_pc_cmd(argc, argv);
}
SHELL_CMD_EXPORT_ALIAS(cmd_pcmode,pcmode,pc mode (passthrough) on/off: pcmode [0|1]);

/*
 * 设备身份（F3，2026-11-08）：USB 探测用，在进 PC 模式**之前**跑。
 *   admin=XXXXXX       24 bit 管理员编号：默认表盘编号 / BLE 广播名后缀 /
 *                      GET_INFO 末尾的 admin_id，三处同一个值；
 *   chip=16 位 hex     8 B eFuse chip ID（读失败为全 0）；
 *   fw=1.6.0 build=Nov 14 2026 21:32
 *                      固件版本 + 构建时间（唯一来源 fw_version.h；同一版本号
 *                      靠构建时间区分不同 bin，插件概览显示 "1.6.0 · <build>"）。
 * 插件枚举所有 VID 349B 串口逐个发 `adminid`，按 admin 绑定设备，
 * 再进 PC 模式 —— 所以这条命令不依赖任何页面/UI 状态。
 */
static int cmd_adminid(int argc, char **argv)
{
    const uint8_t *chip = svc_device_id_chip();

    (void)argc;
    (void)argv;

    printf("admin=%06X chip=%02X%02X%02X%02X%02X%02X%02X%02X fw=%s build=%s %s\r\n",
           (unsigned)svc_device_id_admin(),
           (unsigned)chip[0], (unsigned)chip[1], (unsigned)chip[2], (unsigned)chip[3],
           (unsigned)chip[4], (unsigned)chip[5], (unsigned)chip[6], (unsigned)chip[7],
           PROTORIG_FW_VERSION_STR, __DATE__, __TIME__);
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_adminid, adminid, device identity for USB probe: admin_id + eFuse chip id);

/* ------------------------------------------------------------------ */
/* 配置                                                                */
/* ------------------------------------------------------------------ */

#define LCD_W 480
#define LCD_H 480

/* V1.4 实测：命令写 80 MHz 全对，像素写保守取 40 MHz */
#define QSPI_WRITE_HZ (40UL * 1000UL * 1000UL)

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

/* 非 static：bsp/board_display.c 通过 extern 共享（原计划 R2 收进 BSP，截至 2026-11-15 未做） */
SemaphoreHandle_t s_lcd_mtx;
static volatile int s_trace_req; /* 1=arm, 2=dump; handled on LVGL task */
static volatile int s_face_req;        /* shell 的 face 命令：重画表盘 */
static volatile int s_screen_req = -1; /* shell 的 screen 命令：-1 无 / 0 熄屏 / 1 亮屏 */
static volatile int s_panelsleep_req = -1; /* shell 的 panelsleep 命令：-1 无 / 0..3 层级 */
/* 2026-11-08：shell 的 `alarm fire` 只置标志 —— 亮屏/通知/震动必须在 UI 任务做 */
static volatile int s_clock_fire_req = -1;
static uint32_t s_evt_consumed;        /* R2a：主循环已消费的事件数（诊断） */

/* R3c：app_state -> 表盘时间（只在 UI 任务调用） */
static void ui_time_apply(void);

static void ui_batt_apply(int allow_feedback);
static void battery_policy_apply(int level, int mv);
/*
 * 刷新窗口策略：**宽度取偶** —— 真机 bug「细笔画全花 / 指针拖痕」的修复。
 *
 * 机制：LVGL 脏区宽度可能是奇数，而 RGB565 下 DBI 的一个 32 位 FIFO 字装两个
 * 像素，奇数宽度时最后一个像素只占半个字；st7802_push_pixels565() 补的
 * "幽灵像素"落在高半字、硬件取低半字 -> 该窗口**最右一列像素全错**。
 * 表现：1~2px 细笔画被随机打坏、纯色/大面积看不出来、
 *       指针擦除窗口漏掉最右一列 = 拖痕。
 *
 * 真机上用"同一套 UI、只换刷新窗口几何"做过单变量 A/B：
 *   原样模式 429 次 flush 出现 47~66 次奇数起点，肉眼可见拖痕；
 *   取偶模式 852 次 flush，odd_x=0，肉眼干净；
 *   切回原样立刻复发。
 * 代价可忽略（~29.5K px/flush，40MHz QSPI 下带宽增加可忽略），
 * 且主机侧 67 张输出画面 SHA-256 与原样模式完全相同（视觉零变化）。
 *
 * 根因、证据与边界见 docs/bug_thin_stroke_smearing.md 第 11 节。
 * 实现在 flush_policy.h，挂在 LV_EVENT_INVALIDATE_AREA（渲染之前）。
 */

static void power_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* R2c：power 服务不再自己取 LVGL tick；时间由 UI 任务传入。
     * 若唤醒策略要求整屏重画（wake_redraw=1），在这里（LVGL 任务）执行。 */
    svc_power_tick(lv_tick_get());
    if (power_take_full_redraw()) {
        lv_obj_invalidate(lv_screen_active());
    }
}

/* 100 ms 节拍：驱动当前原生页面/设置页的 tick（见 eez_ui_glue.c） */
static void eez_tick_cb(lv_timer_t *t)
{
    (void)t;
    eez_ui_tick();
}

/* ------------------------------------------------------------------ */
/* 电池：R3c 起采样/滤波/低电分级在 services/svc_battery.c；             */
/* UI 只按 batt_seq 把快照推给表盘（马达反馈也在 UI 任务）。                */
/* 开机 3 秒后第一次采样，之后每 3 秒一次；ADC init 延迟到首帧之后。      */
/* ------------------------------------------------------------------ */

void batt_log_tick(void);

static uint32_t s_ui_batt_seq;
static int s_ui_batt_level;

static void ui_batt_apply(int allow_feedback)
{
    app_state_t st;
    int lvl;

    app_state_get(&st);
    if (st.batt_seq == s_ui_batt_seq) {
        return;
    }
    s_ui_batt_seq = st.batt_seq;

    face_lvgl_set_battery(st.battery_mv, st.battery_pct);
    face_lvgl_set_charging(st.charging);
    svc_ble_battery_update(st.battery_pct);   /* R4：BAS 只在变化时 notify */

    lvl = st.battery_level;
    if (lvl != s_ui_batt_level) {
        s_ui_batt_level = lvl;
        face_lvgl_set_power_warn(lvl);
        if (allow_feedback) {
            if (lvl == 1) {
                motor_pulse(60, 150);
            } else if (lvl == 2) {
                motor_pulse(80, 400);
            }
        }
    }
}

/* 低电策略：idle 档位由 power 侧执行（服务不直接调 power_set_idle_ms） */
static void battery_policy_apply(int level, int mv)
{
    (void)mv;
    if (level == 2) {
        power_set_idle_ms(3000);
    } else if (level == 1) {
        power_set_idle_ms(8000);
    } else {
        power_set_idle_ms(15000);
    }
}

static void batt_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (svc_battery_tick(lv_tick_get())) {
        ui_batt_apply(1);
        batt_log_tick();        /* 电压飞行记录仪（诊断，仍在 UI 任务） */
    }
}


/* ------------------------------------------------------------------ */
/* 真实时钟：R3c 起由 services/svc_time.c 管理；这里只留锁宏给 shell 命令  */
/* ------------------------------------------------------------------ */

#define RTC_LOCK()   svc_time_lock()
#define RTC_UNLOCK() svc_time_unlock()

/* ------------------------------------------------------------------ */
/* 电压飞行记录仪（写在 SD3010 的用户 RAM 里）                          */
/* ------------------------------------------------------------------ */
/*
 * 2026-10-18：为了量"息屏静置到底多少 mA"，又不想盯着表盘读数
 * （把面板 PMIC 断电之后屏是黑的），就把电池电压每 2 分钟写进
 * SD3010 的用户 RAM —— 它和 RTC 共 VDD(BAT+)，断电短暂也不会丢。
 *
 * 用法：
 *   拔 USB 跑一段（一轮最多 28 × 2 = 56 分钟）
 *   插回来 → shell 敲 `battlog` → 倒出整条曲线
 *
 * 只在不充电时记录：充电曲线没有意义，而且会把记录槽占满。
 */
/* R2b：设置记录 v1 占用 0x30..0x3D，battlog 顺延（28 -> 21 条） */
#define BATTLOG_HEAD   0x3E          /* 写指针（0x3E，在设置记录之后） */
#define BATTLOG_BASE   0x40          /* 记录区：21 × 2 字节 = 0x40..0x69 */
#define BATTLOG_N      21

static uint32_t s_battlog_next;

static void batt_log_store(int mv)
{
    uint8_t head = 0;
    uint8_t b[2];

    RTC_LOCK();
    (void)sd3010_ram_read(BATTLOG_HEAD, &head, 1);
    if (head >= BATTLOG_N) {
        head = 0;
    }
    b[0] = (uint8_t)(mv & 0xFF);
    b[1] = (uint8_t)((mv >> 8) & 0xFF);
    (void)sd3010_ram_write((uint8_t)(BATTLOG_BASE + head * 2u), b, 2);
    head = (uint8_t)((head + 1u) % BATTLOG_N);
    (void)sd3010_ram_write(BATTLOG_HEAD, &head, 1);
    RTC_UNLOCK();
}

/* PDS 定时醒来时强制记一条（不走 FreeRTOS tick 节流，PDS 里 tick 会停） */
void batt_log_force(void)
{
    int mv;

    if (!s_adc_ready || chrg_charging()) {
        return;
    }
    mv = board_adc_battery_mv();
    if (mv < 0 || mv > 5000) {
        return;
    }
    batt_log_store(mv);
}

void batt_log_tick(void)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int mv;

    if (now < s_battlog_next) {
        return;
    }
    s_battlog_next = now + (uint32_t)BATTLOG_SEC * 1000u;

    if (chrg_charging()) {
        return;                      /* 充电中不记 */
    }

    mv = board_adc_battery_mv();
    if (mv < 0 || mv > 5000) {
        return;
    }
    batt_log_store(mv);
}

/*
 * 触摸唤醒时：先把睡眠期间走掉的时间补上，再只重画时间那几块。
 *
 * 2026-10-18：这里还要**补一次电量**。
 * 原因是电量采样是 LVGL 定时器，息屏期间 `lv_timer_handler()` 根本不跑，
 * 所以电量标签停在熄屏前的旧值 —— 充电时一开屏就会"跳一下"（实测从 58% 跳到 66%）。
 * 在画第一帧之前采一次，画面就不会有这一跳。
 */
static void face_wake_cb(void)
{
    svc_time_tick(lv_tick_get());
    ui_time_apply();

    /* R3c：电池采样在 svc_battery；唤醒补采一次，再按快照刷表盘 */
    (void)svc_battery_sample_now();
    ui_batt_apply(0);

    face_lvgl_wake_refresh();
}

#define LCD_LOCK()   xSemaphoreTake(s_lcd_mtx, portMAX_DELAY)
#define LCD_UNLOCK() xSemaphoreGive(s_lcd_mtx)

/* ------------------------------------------------------------------ */
/* 空闲钩子：让 CPU 真的睡下去（WFI）                                  */
/* ------------------------------------------------------------------ */
/*
 * ⚠️ 2026-10-18 查续航时发现的大坑：
 *   这个 SDK 的 FreeRTOS RISC-V port 里**一次 WFI 都没有**
 *   （`grep -r wfi components/os/freertos/portable/GCC/RISC-V/` 结果为空），
 *   也就是说空闲任务就是**满速空转**。息屏以后 MCU 仍然在 320 MHz 上空跑，
 *   "屏幕关掉"几乎没有省下任何电。
 *
 *   实测：电池从 4.03 V 放到 3.10 V 用了 12h26m，反推平均 ≈33 mA ——
 *   和"BL616 320 MHz 运行态 25~35 mA"完全吻合，几乎全是这颗 CPU。
 *
 * 现在空闲任务里执行 WFI：任何**使能的中断**都会把它叫醒（这里最快的是
 * 1 kHz 的 tick），所以每个 tick 醒几十微秒、其余时间都在睡。
 *
 * 这一下不会变成 µA 级（PLL / 外设还在跑），但它是"不换架构就能拿到"的那部分。
 * 真正要上一周续航还是得走 PDS（见 docs/power_budget_and_sleep_plan.md）。
 */
void vApplicationIdleHook(void)
{
    __asm volatile("wfi");
}

/* R2c：整分取证钩子实现留在 diag/UI 侧，通过 board_display 注册回调；
 * 这样 bsp/board_display 不再反向 include face_lvgl.h。 */
static uint32_t s_flush_minute_call;

static void min_capture_flush_hook(void)
{
    if (!st7802_min_capture_armed()) {
        return;
    }
    if (s_time_minute_changed && s_flush_minute_call == 0) {
        /* 那一刻的这一帧：把分钟变化前后各若干块的发送记录存下来 */
        s_flush_minute_call++;
        st7802_min_capture_arm((uint32_t)s_time_minute);
    } else {
        st7802_min_capture_take();
    }
}

/* 一帧的活分两半：送屏（下面 stats 命令看）和渲染。分开量才知道该优化哪边。 */
static int cmd_stats(int argc, char **argv)
{
    if (argc >= 2 && atoi(argv[1]) == 0) {
        s_flush_us_sum = s_flush_us_max = s_flush_n = 0;
        printf("stats reset\r\n");
        return 0;
    }
    printf("flush(dbi 送屏): n=%lu  avg=%.1f ms  max=%.1f ms\r\n",
           (unsigned long)s_flush_n,
           s_flush_n ? (double)s_flush_us_sum / s_flush_n / 1000.0 : 0.0,
           (double)s_flush_us_max / 1000.0);
    printf("te: ok=%lu timeout=%lu  sync=%s  IO28=%d\r\n",
           (unsigned long)s_te_ok, (unsigned long)s_te_to,
           s_te_sync ? "ON" : "OFF", st7802_te_level());
    printf("  avg 送屏 > 11 ms（半屏）就说明 40MHz QSPI 已经到顶，改 UI 参数没用\r\n");
    printf("  stats 0 = 清零\r\n");
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_stats, stats, flush/te timing: stats [0]);

#if PROTORIG_DEBUG_SHELL
/*
 * 送屏通路的"按块"诊断：只在 RAM 里记，事后一次打出来（推送时不打印，免得动时序）。
 *   flushdiag      -> dump 最近 10 块 + 累计 FIFO_ERR / TC 自旋
 *   flushdiag 0    -> 清零
 * 查"整分时大时间数字那块发花"：若整分那两块出现 FIFO_ERR 或收尾 TX_FIFO，
 * 就是送屏通路（FIFO 欠载 → 送出去的是废字），而不是渲染逻辑。
 */
static int cmd_flushdiag(int argc, char **argv)
{
    if (argc >= 2 && atoi(argv[1]) == 0) {
        st7802_flush_diag_reset();
        printf("flushdiag reset\r\n");
        return 0;
    }
    st7802_flush_diag_dump();
    if (st7802_min_capture_ready()) {
        printf("\r\n");
        st7802_min_capture_dump();
    }
    return 0;
}SHELL_CMD_EXPORT_ALIAS(cmd_flushdiag, flushdiag, flush band diagnostics + per-block CRC: flushdiag [0]);
#endif /* PROTORIG_DEBUG_SHELL */

/* ------------------------------------------------------------------ */
/* 诊断命令组（2026-10-15，查"整分时大时间数字那块发花"）               */
/*                                                                    */
/* 三个都不改正常渲染/送屏路径的语义，只在 RAM 里记录、事后 printf。     */
/* 判读标准写在 docs/minute_digit_artifact_20261015.md 第 13.2 节。     */
/* ------------------------------------------------------------------ */

#if PROTORIG_DEBUG_SHELL
/*
 * pxpat <x1> <y1> <x2> <y2> [on|off]
 *
 * 编码自证图案：R5=x&31, G6=y&63, B5=0x0A，走**真实** st7802_blit565()，
 * 不经 LVGL。为什么要编码而不是棋盘格：棋盘格只回答"坏没坏"，而且
 * **周期性图案会掩盖周期性错位**；编码图案让每个像素自报坐标，回读时
 * 能直接读出"偏了多少、往哪偏"。
 *
 * 分段按 LVGL 的 band 高度（64 行）切，图案天然横跨 band 边界。
 */
static int cmd_pxpat(int argc, char **argv)
{
    int x1, y1, x2, y2, on = 1;
    uint32_t px;

    if (argc < 5) {
        printf("usage: pxpat <x1> <y1> <x2> <y2> [on|off]\r\n");
        printf("  编码图案：R5=x&31, G6=y&63, B5=0x0A；按 64 行分段跨 band 边界\r\n");
        printf("  on=off 时画 0x0000（关掉图案），随手敲 pxpat 0 0 479 479 off\r\n");
        return 0;
    }
    x1 = atoi(argv[1]);
    y1 = atoi(argv[2]);
    x2 = atoi(argv[3]);
    y2 = atoi(argv[4]);
    if (argc >= 6) {
        /*
         * ★ 2026-10-16 修的解析 bug：原来只判 "off"，于是 `pxpat ... on`
         *   会落到 atoi("on")==0 ⇒ 被当成 **off（画黑）**。
         *   交接文档 §3.4 记的正是这个坑，现在把 on/off 两个字面量都显式判掉。
         */
        if (strcmp(argv[5], "off") == 0) {
            on = 0;
        } else if (strcmp(argv[5], "on") == 0) {
            on = 1;
        } else {
            on = atoi(argv[5]) ? 1 : 0;
        }
    }
    if (x1 < 0 || y1 < 0 || x2 < x1 || y2 < y1 || x2 >= LCD_W || y2 >= LCD_H) {
        printf("pxpat: 区域必须是 0<=x1<=x2<=%d, 0<=y1<=y2<=%d\r\n", LCD_W - 1, LCD_H - 1);
        return -1;
    }

    LCD_LOCK();
    px = st7802_pxpat((uint16_t)x1, (uint16_t)y1, (uint16_t)x2, (uint16_t)y2, on);
    LCD_UNLOCK();

    printf("pxpat %s (%d,%d)-(%d,%d): %lu px, %lu 段（每段 %d 行，跨 %lu 个 band 边界）\r\n",
           on ? "on" : "off", x1, y1, x2, y2, (unsigned long)px,
           (unsigned long)st7802_pxpat_blocks(), BAND_LINES,
           (unsigned long)(st7802_pxpat_blocks() ? st7802_pxpat_blocks() - 1 : 0));
    printf("  期望值：像素(x,y) = ((x&31)<<11)|((y&63)<<5)|0x0A\r\n");
    printf("  ⚠️ 会盖住表盘；`pxpat 0 0 %d %d off` 关掉，`face` 重画表盘。\r\n", LCD_W - 1, LCD_H - 1);
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_pxpat, pxpat, encoded self-id pattern: pxpat x1 y1 x2 y2 [on|off]);
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
/*
 * minroll [dump|cmp <i>|0]
 *
 * 整分取证（deliverable 2）：
 *   minroll          -> 等下一次整分，把那一帧各块的 CRC + 窗口存进 RAM（前后各若干块）
 *   minroll dump     -> 把发送侧记录打出来（含跨帧同窗口 CRC 对比）
 *   minroll cmp <i>  -> 立刻回读 post[i] 那个窗口，和发送侧一起看
 *   minroll 0        -> 取消/清空
 *
 * 判读标准：发送数据一致 + 面板数据一致 -> 传输/面板都不是，问题在**绘制**；
 *           发送一致而面板不一致 -> 才轮到**传输/时序**这条路。
 */
static int cmd_minroll(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "0") == 0) {
        st7802_min_capture_reset();
        printf("minroll: 已清空/取消\r\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
        st7802_min_capture_dump();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "cmp") == 0) {
        int i;
        int r;

        if (!st7802_min_capture_ready()) {
            printf("minroll: 还没抓到。先敲 `minroll`，等一次整分，再 `minroll dump`。\r\n");
            return -1;
        }
        i = (argc >= 3) ? atoi(argv[2]) : 0;
        LCD_LOCK();
        r = st7802_min_capture_panel_check(i);
        LCD_UNLOCK();
        return r;
    }

    st7802_flush_diag_reset();          /* 顺便清一遍，方便对号 */
    st7802_min_capture_arm_request(0);
    s_time_minute_changed = 0;
    s_flush_minute_call = 0;
    printf("minroll: armed —— 等下一次整分（分钟数字变化）那一刻的第一次 flush。\r\n");
    printf("  到时候敲 `minroll dump` 看发送侧 CRC/窗口，再 `minroll cmp <i>` 回读对拍。\r\n");
    printf("  期间请让表盘停在主界面别翻页；抓完（或取消）用 `minroll 0`。\r\n");
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_minroll, minroll, minute-roll send-side CRC capture: minroll [dump|cmp i|0]);
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
/*
 * gramrd <x> <y> <w> <h> [pat=0|1] [cmd=0xNN] [chunk=1..8]
 *
 * 面板回读探测（deliverable 1，先做的那一步）：定窗口 -> 发读命令 -> 读回并打十六进制。
 *
 * ⚠️ 硬限制：BL616 的 DBI 一次读事务最多收 8 字节（只有 RDATA_0/1 两个 32 位
 *    回读寄存器），所以这里靠**多次 8 字节事务拼**成长读。面板能否跨事务推进
 *    读指针**未验证** —— 若每次读回同一组字节，就是不能这么用。
 * ⚠️ 使用前先看自检行：RDDID(04h) 若全 0/全 FF，说明读通路本身没通，
 *    后面的结果都不必解释。
 */
static int cmd_gramrd(int argc, char **argv)
{
    int x, y, w, h, pat = 0, chunk = 8;
    unsigned cmd = 0x2E;
    int i;

    if (argc < 5) {
        printf("usage: gramrd <x> <y> <w> <h> [pat=0|1] [cmd=0xNN] [chunk=1..8]\r\n");
        printf("  例：gramrd 120 160 8 4            (2Eh RAMRD，默认块长 8)\r\n");
        printf("      gramrd 120 160 8 4 pat=1      (把回读当 pxpat 编码解释，扫 dummy 偏移)\r\n");
        printf("      gramrd 120 160 2 1 cmd=0x04   (自检命令：读 RDDID)\r\n");
        printf("  先决条件：要读的区域**先被写过**（pxpat 或正常 UI 都算）。\r\n");
        return 0;
    }
    x = atoi(argv[1]);
    y = atoi(argv[2]);
    w = atoi(argv[3]);
    h = atoi(argv[4]);
    for (i = 5; i < argc; i++) {
        if (strncmp(argv[i], "pat=", 4) == 0) pat = atoi(argv[i] + 4);
        else if (strncmp(argv[i], "cmd=", 4) == 0) cmd = (unsigned)strtoul(argv[i] + 4, NULL, 0);
        else if (strncmp(argv[i], "chunk=", 6) == 0) chunk = atoi(argv[i] + 6);
        else { printf("gramrd: 不认识的参数 %s\r\n", argv[i]); return -1; }
    }
    if (chunk < 1 || chunk > 8) {
        printf("gramrd: chunk 必须在 1..8（BL616 DBI 单次读事务上限就是 8 字节）\r\n");
        return -1;
    }

    LCD_LOCK();
    i = st7802_gramrd((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h,
                      (uint8_t)cmd, (uint8_t)chunk, pat);
    LCD_UNLOCK();
    return i < 0 ? -1 : 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_gramrd, gramrd, read panel back probe: gramrd x y w h [pat=0|1] [cmd=0xNN] [chunk=1..8]);
#endif /* PROTORIG_DEBUG_SHELL */

/*
 * panel [on|off]  ——  面板状态自证 / 强制送 DISPON
 *
 * 为什么要它：整分花屏排查里，"屏到底亮没亮"一度只能靠固件的 `screen` 标志，
 * 而那只说明**软件认为**屏该亮。2026-10-16 真机实测拿到硬件判据：
 *     RDDPM(0Ah) 屏亮 = 0x9C（DISON=1）、息屏 = 0x98（DISON=0）。
 * 所以：
 *     panel        -> 只读，打印 RDDPM/RDDST/RDCOLMOD... 并明确回答"面板在不在显示"
 *     panel on     -> 送 DISPON(0x29) 后再读一次，确认面板真的回到显示态
 *                     （排查"唤醒没生效 / 屏没亮导致看图实验全废"用）
 *
 * ⚠️ `screen 1` 走的是 power.c 的状态机（将来可能改）；`panel on` 只发面板命令，
 *    是**绕过状态机**的直接手段，两者不冲突，排查时优先信 RDDPM。
 */
static int cmd_panel(int argc, char **argv)
{
    int r;

    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        uint8_t b = 0xFF;

        LCD_LOCK();
        st7802_write_cmd(0x29, NULL, 0);     /* DISPON */
        bflb_mtimer_delay_ms(5);
        st7802_write_cmd(0x51, &b, 1);       /* WRDISBV = 最亮（排查时别被亮度骗） */
        bflb_mtimer_delay_ms(5);
        r = st7802_panel_probe();
        LCD_UNLOCK();
        printf("panel on: 已送 DISPON + 亮度 0xFF，上面是面板自己报的状态\r\n");
        return r < 0 ? -1 : 0;
    }

    LCD_LOCK();
    r = st7802_panel_probe();
    LCD_UNLOCK();

    printf("注意：`screen` 报的是固件**软件标志**；这里报的是**面板寄存器**。\r\n");
    printf("      两者不一致时（软件说 ON、面板说 DISON=0）以寄存器为准。\r\n");
    return r < 0 ? -1 : 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_panel, panel, panel status self-proof via RDDPM: panel [on]);

#if PROTORIG_DEBUG_SHELL
/*
 * drain [0|1]  ——  「收尾排空等待」实验开关（只影响收尾，不改喂数/窗口/像素）
 *
 *   0（默认）= 收尾只等 TC
 *   1        = 收尾额外等 DBI_INTSTS_TX_FIFO（FIFO 空）
 *
 * 用法（验证交接文档 §4 的首选假设）：
 *   idle 600000 ; panel on（确认 DISON=1）; flushdiag 0
 *   pxpat 0 0 479 479 on        <- drain=0：若屏上毫无变化，就复现了 10-16 的现象
 *   flushdiag                   <- 看"真的多等了的块/最长自旋"是否为 0
 *   drain 1 ; pxpat 0 0 479 479 on
 *   flushdiag                   <- 这次记到了等待；屏上出现了吗？
 *
 * 判读：图案出现 => "背靠背 blit 丢像素"成立；图案还是不出现 => 否掉，转查窗口命令相位。
 */
static int cmd_drain(int argc, char **argv)
{
    uint32_t blocks = 0, spin;

    if (argc >= 2) {
        st7802_set_drain_experiment(atoi(argv[1]) ? 1 : 0);
        st7802_drain_stats_reset();
        printf("drain 实验 = %s\r\n", st7802_get_drain_experiment() ? "ON" : "OFF");
        return 0;
    }
    spin = st7802_drain_stats(&blocks);
    printf("drain 实验 = %s  真的多等了的块=%lu  最长自旋=%lu\r\n",
           st7802_get_drain_experiment() ? "ON" : "OFF",
           (unsigned long)blocks, (unsigned long)spin);
    if (!st7802_get_drain_experiment()) {
        printf("  （OFF 时这几个数恒为 0，属正常；用 `drain 1` 打开）\r\n");
    } else if (blocks == 0) {
        printf("  ⚠️ 一块都没多等 => 收尾时 TX_FIFO 已经置位。\r\n");
        printf("     这**否定**了\"TC 提前置位、FIFO 里还有货\"这条机制（至少在本路径上）。\r\n");
    } else {
        printf("  => TC 通过后 FIFO 并不空，确实存在\"还没出完就进下一块\"的窗口。\r\n");
    }
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_drain, drain, drain-wait experiment (blit tail): drain [0|1]);
#endif /* PROTORIG_DEBUG_SHELL */

/*
 * clear  ——  整屏重绘（清掉已发生的错位），用于单变量 A/B
 *
 * 为什么必须要有它（用户 2026-10-16 晚指出，非常重要）：
 *   整分花屏**一旦出现就保留到下一次整屏重绘**。所以"改完再等一个整分看还有没有错位"
 *   是**无效实验** —— 你看到的很可能是上一次留下的残留。正确协议是：
 *     ① `clear`（整屏重绘，确认屏已干净）
 *     ② 等**下一整分**，只看"这次有没有**新**产生错位"
 *   用户实测"右滑再左滑回表盘"就能清掉，本命令把这一步做成一条串口命令。
 *
 * ⚠️ 2026-10-16 晚踩的坑（第一版把整机搞死了，别重犯）：
 *   shell 跑在**自己的任务**里。第一版在这里 `LCD_LOCK(); lv_obj_invalidate(); lv_refr_now();`
 *   —— 死锁。因为 `lv_obj_invalidate()` 内部要取 **LVGL 自己的锁**，而 LVGL 任务
 *   正在渲染时又会去等 LCD 锁，两边互等。**规矩：任何碰 LVGL 对象的调用都只能在
 *   LVGL 任务上下文里做，shell 只能置标志。**（和 `face` / `screen` 一致。）
 */
static volatile int s_clear_req;

static int cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    s_clear_req = 1;      /* 由 LVGL 任务执行，见 lvgl_task 的主循环 */
    printf("clear: 已请求整屏重绘（在 LVGL 任务里执行）\r\n");
    printf("  现在等**下一整分**，只判\"这次有没有新产生错位\"。\r\n");
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_clear, clear, full-screen redraw (clears an existing artifact): clear);

#if PROTORIG_DEBUG_SHELL
/*
 * winguard <us>  ——  块间"收窗"保护时间（候选修复的现场 A/B 开关）
 *
 * 判读办法（真机，一分钟见分晓）：
 *   `winguard 0`    -> 等下一个整分：四个数字在同一高度横向错位（已确认复现）
 *   `winguard 200`  -> 再等一个整分：错位消失 => 假设成立，把它做成正式修复
 *                      错位依旧  => 假设否掉，换方向（面板写指针/扫描相位）
 * 每块只多等一次，代价 = 块数 × us（每秒约 10 块 ⇒ 200us 合计 ~2ms/s）。
 *
 * ⚠️ 2026-10-16 晚实测结论：**无效**。0 / 200 / 8000 us 三种取值下，整分那一帧
 *    两块发送的 CRC 完全一样（0B698BA8 / D2AE9EEF），面板上的错位也都在。
 *    保留命令只为将来复核，默认 0 = 关。
 */
static int cmd_winguard(int argc, char **argv)
{
    if (argc < 2) {
        printf("winguard = %u us  (0 = 关；见命令说明里的 A/B 判读办法)\r\n",
               (unsigned)st7802_get_win_guard_us());
        return 0;
    }
    st7802_set_win_guard_us((uint32_t)strtoul(argv[1], NULL, 0));
    printf("winguard = %u us\r\n", (unsigned)st7802_get_win_guard_us());
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_winguard, winguard, inter-block window-settle guard (us): winguard [us]);
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
/*
 * fill <x1> <y1> <x2> <y2> <RRGGBB>  ——  一次 blit 的纯色块（走 buf==NULL 的 solid 分支）
 *
 * 为什么要它（2026-10-16 晚，接手本 bug 的关键工具）：
 *   真机实测到一条硬事实：`pxpat` 的像素**确实全部进了 DBI**（`flushdiag` 的累计
 *   px 一位不差），面板也在显示（RDDPM 的 DISON=1），**但屏上没有任何变化**。
 *   而 UI 的小块却能看见。
 *
 *   `pxpat` 与 UI 共用 st7802_blit565()，看似无差别，于是要把它拆成两个变量：
 *     ① **数据来源**：pxpat 用缓冲（buf != NULL），fill 用纯色（buf == NULL）；
 *     ② **blit 次数/节奏**：pxpat 是 480 次背靠背的 480px 行，fill 是**一次**大 blit。
 *   两个命令各占一边，一眼就能分清是哪一维在坏事：
 *     `fill` 可见 + `pxpat` 不可见  -> 问题在"背靠背多次 blit"或"缓冲数据"；
 *     `fill` 也不可见              -> 问题在"不经 LVGL 的直接推送"这一整条路上。
 *   （fill 用的 st7802_fill 是月初"红方块干净"那个测试的同一个原语，它当时是有效的。）
 */
static int cmd_fill(int argc, char **argv)
{
    int x1, y1, x2, y2;
    unsigned long rgb;
    uint32_t t0, us;

    if (argc < 6) {
        printf("usage: fill <x1> <y1> <x2> <y2> <RRGGBB>\r\n");
        printf("  例：fill 30 120 180 300 FF00FF   (一次 blit，纯色块，绕过 LVGL)\r\n");
        printf("  配合 pxpat 判读：见本命令上方的注释。用完 `face` 恢复表盘。\r\n");
        return 0;
    }
    x1 = atoi(argv[1]);
    y1 = atoi(argv[2]);
    x2 = atoi(argv[3]);
    y2 = atoi(argv[4]);
    rgb = strtoul(argv[5], NULL, 16);
    if (x1 < 0 || y1 < 0 || x2 < x1 || y2 < y1 || x2 >= LCD_W || y2 >= LCD_H) {
        printf("fill: 区域必须是 0<=x1<=x2<=%d, 0<=y1<=y2<=%d\r\n", LCD_W - 1, LCD_H - 1);
        return -1;
    }

    t0 = (uint32_t)bflb_mtimer_get_time_us();
    LCD_LOCK();
    st7802_fill((uint16_t)x1, (uint16_t)y1, (uint16_t)x2, (uint16_t)y2, (uint32_t)rgb);
    LCD_UNLOCK();
    us = (uint32_t)bflb_mtimer_get_time_us() - t0;

    printf("fill (%d,%d)-(%d,%d) rgb=0x%06lX : %lu px, 一次 blit 耗时 %lu us\r\n",
           x1, y1, x2, y2, rgb,
           (unsigned long)((uint32_t)(x2 - x1 + 1) * (uint32_t)(y2 - y1 + 1)),
           (unsigned long)us);
    printf("  期望：面板上出现一个纯色矩形。没有的话见本命令注释里的判读表。\r\n");
    if (st7802_get_fmt565()) {
        printf("  格式：RGB565（bit15..11=R, 10..5=G, 4..0=B）\r\n");
    } else {
        printf("  !! 当前不是 565（st7802_get_fmt565()==0），颜色解释会不同\r\n");
    }
    printf("  `face` 恢复表盘。\r\n");
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_fill, fill, one-shot solid blit (bypasses LVGL): fill x1 y1 x2 y2 RRGGBB);
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
/*
 * fills <n> [gap_us]  ——  n 次"小行 blit"（每行 480x1 纯色），测"多次小 blit"这一维
 *
 * 为什么要它（2026-10-16 晚）：已经用真机单变量排掉两维 ——
 *   `fill` 一次大纯色 blit            -> 上屏 ✓
 *   `fill` 两次大 blit 背靠背          -> 都上屏 ✓
 * 而 `pxpat`（480 次背靠背的 480x1 行 blit，缓冲数据）**完全不上屏**。
 * 于是只剩两个候选：① "连续很多次小 blit"；② "pxpat 那条缓冲数据路径"。
 * 本命令占 ① 的一边（纯色、无缓冲、行 blit 的尺寸/次数与 pxpat 完全一样）：
 *   `fills 480` 可见 + `pxpat` 不可见 -> 问题在"缓冲数据"或 pxpat 自身；
 *   `fills 480` 也不可见             -> 问题在"频繁小 blit"，与缓冲无关。
 *
 * gap_us 是每行之间的间隔（默认 0 = 与 pxpat 一样背靠背）。给一个 >0 的值就能
 * 顺带确认"拉开间隔是否恢复"，这是最省事的 A/B。
 */
static int cmd_fills(int argc, char **argv)
{
    int n = 480;
    uint32_t gap_us = 0;
    int rows_done = 0;
    uint32_t t0, us;

    if (argc >= 2) {
        n = atoi(argv[1]);
    }
    if (argc >= 3) {
        gap_us = (uint32_t)strtoul(argv[2], NULL, 0);
    }
    if (n < 1 || n > LCD_H) {
        printf("fills: n 必须在 1..%d\r\n", LCD_H);
        return -1;
    }

    t0 = (uint32_t)bflb_mtimer_get_time_us();
    LCD_LOCK();
    for (int y = 0; y < n; y++) {
        /* 每行用不同颜色，这样"只写了前几行"或"行错位"一眼能看出来 */
        uint32_t rgb = (uint32_t)(((y * 37) & 0xFF) << 16) |
                       (uint32_t)(((y * 91) & 0xFF) << 8) |
                       (uint32_t)((y * 53) & 0xFF);

        st7802_fill(0, (uint16_t)y, LCD_W - 1, (uint16_t)y, rgb);
        rows_done++;
        if (gap_us) {
            bflb_mtimer_delay_us(gap_us);
        }
    }
    LCD_UNLOCK();
    us = (uint32_t)bflb_mtimer_get_time_us() - t0;

    printf("fills n=%d gap=%luus : %d 行 x 480px = %d px, 共 %lu us（平均 %lu us/行）\r\n",
           n, (unsigned long)gap_us, rows_done, rows_done * LCD_W,
           (unsigned long)us, (unsigned long)(rows_done ? us / (uint32_t)rows_done : 0));
    printf("  期望：屏幕上部 %d 行变成一条条彩色横线（每行颜色不同）。\r\n", n);
    printf("  用法对照：`fills 480` vs `fills 480 1000`（每行间隔 1ms），看间隔是否有影响。\r\n");
    printf("  `face` 恢复表盘。\r\n");
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_fills, fills, many small row-blits (solid): fills [n] [gap_us]);
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
/*
 * rollsim  ——  精确复刻真机"整分那一帧"的送屏序列（纯色版，无缓冲、无 LVGL）
 *
 * 为什么需要它（2026-10-16 晚，本 bug 排查的主线）：
 *   --- 修 bug 的目标 ---
 *   真机 bug：**每次整分，大时间标签重绘时只有"分钟末位数字"那一小块发花/错位，
 *   并保持到下一次重画（约一分钟）**。主机逐像素 A/B 是 0 差异，所以渲染是对的。
 *
 *   `minroll` 已经在真机上抓到那一帧的真实分块（交接文档 §3.2）：
 *       (70, 91)-(409,180)  30600 px     <- 大数字上半，分块边界 y=180/181 横穿数字中部
 *       (70,181)-(409,264)  28560 px     <- 大数字下半
 *       (378,190)-(425,237)  2304 px     <- 秒数标签
 *       (74, 69)-(111,106)   1444 px
 *       (152,433)-(193,471)  1638 px
 *   合计 64,546 px，**五块背靠背、中间零间隔**。
 *
 *   已经用 `fill` 真机单变量排掉：一次大 blit 能上屏；两次大 blit 背靠背也能上屏。
 *   本命令把"两次"升级成"与整分完全相同的那五块、尺寸与顺序一模一样"，
 *   唯一变量就是"五块背靠背"，从而判定：
 *     五块都可见          -> 这个送屏序列本身是健康的，花屏不在送屏侧；
 *     某块/某条边界错位   -> 复现了！而且能直接看出是哪一块、哪条边界。
 *
 *   ⚠️ 用的是纯色（buf==NULL）而不是缓冲数据，所以它只测"送屏序列/时序"这一维，
 *      不测"像素数据内容"。这一维已经由主机 A/B（0 px differ）和 `flushdiag` 的
 *      同窗口 CRC 对比负责。
 */
static int cmd_rollsim(int argc, char **argv)
{
    /* 与交接文档 §3.2 真机 minroll 抓到的一帧逐字节一致 */
    static const struct { uint16_t x1, y1, x2, y2; uint32_t rgb; } blocks[] = {
        {  70,  91, 409, 180, 0xFF0000 },  /* 大数字上半：红 */
        {  70, 181, 409, 264, 0x00FF00 },  /* 大数字下半：绿 */
        { 378, 190, 425, 237, 0x0000FF },  /* 秒数标签：蓝 */
        {  74,  69, 111, 106, 0xFFFF00 },  /* 黄 */
        { 152, 433, 193, 471, 0x00FFFF },  /* 青 */
    };
    uint32_t gap_us = 0;
    uint32_t total = 0;
    uint32_t t0, us;

    if (argc >= 2) {
        gap_us = (uint32_t)strtoul(argv[1], NULL, 0);
    }

    t0 = (uint32_t)bflb_mtimer_get_time_us();
    LCD_LOCK();
    for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        st7802_fill(blocks[i].x1, blocks[i].y1, blocks[i].x2, blocks[i].y2, blocks[i].rgb);
        total += (uint32_t)(blocks[i].x2 - blocks[i].x1 + 1) *
                 (uint32_t)(blocks[i].y2 - blocks[i].y1 + 1);
        if (gap_us) {
            bflb_mtimer_delay_us(gap_us);
        }
    }
    LCD_UNLOCK();
    us = (uint32_t)bflb_mtimer_get_time_us() - t0;

    printf("rollsim gap=%luus : 5 块 %lu px, 共 %lu us\r\n",
           (unsigned long)gap_us, (unsigned long)total, (unsigned long)us);
    printf("  期望：数字上半红、下半绿（边界正好在数字中部）、秒数标签蓝、另两块黄/青。\r\n");
    printf("  ★如果上下两半的**接缝处**出现错位/花边，就复现了整分花屏。\r\n");
    printf("  对照：`rollsim 0`（背靠背）vs `rollsim 20000`（每块间隔 20ms，等效 UI 的节奏）。\r\n");
    printf("  `face` 恢复表盘。\r\n");
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_rollsim, rollsim, exact replay of the minute-roll blit sequence: rollsim [gap_us]);
#endif /* PROTORIG_DEBUG_SHELL */


/* ------------------------------------------------------------------ */
/* panelsleep 实验（W1，2026-10-28）                                   */
/*                                                                    */
/* shell 只置请求标志，真正动 DBI/面板在本函数里、由 LVGL 任务调用：      */
/*   1 = 亮度 0 + DISPOFF                                             */
/*   2 = + SLPIN，等 120 ms                                            */
/*   3 = + DSTBON + QSPI 六线拉低/高阻 + VCI_EN 低                     */
/*   0 = 唤醒（VCI 高 -> RST 脉冲 -> SLPOUT -> 120 ms -> st7802_init）  */
/* 唤醒后面板 GRAM 已丢，必须整屏重画；同时把 st7802_init 写回的 0xFF   */
/* 亮度恢复成用户设置。                                                */
/* ------------------------------------------------------------------ */
static void panelsleep_apply(int level)
{
    int rc;

    if (level == 0) {
        rc = st7802_panel_wake();
        if (rc == 0) {
            if (power_is_on()) {
                power_apply_brightness();       /* st7802_init 写的是 0xFF */
                lv_obj_invalidate(lv_screen_active());
                lv_refr_now(NULL);              /* 面板刚复位，GRAM 空了 */
            } else {
                uint8_t b = 0x00;
                st7802_write_cmd(0x51, &b, 1);
                st7802_write_cmd(0x28, NULL, 0);
            }
        }
    } else {
        rc = st7802_panel_sleep(level);
    }

    printf("[panel] panelsleep %d rc=%d (level now %d)\r\n",
           level, rc, st7802_panel_sleep_level());
}

/* ------------------------------------------------------------------ */
/* R3c：时间 UI 适配（app_state -> 表盘）                               */
/* ------------------------------------------------------------------ */

static uint32_t s_ui_time_seq;

/* 2026-11-08：秒表/计时器/闹钟的 UI 侧 glue（函数体在通知区之后） */
static void app_clock_tick(void);

/* 只在 UI 任务调用：time_seq 变化时才推表盘（电量写 batt_seq 不触发） */
static void ui_time_apply(void)
{
    app_state_t st;

    app_state_get(&st);
    if (st.time_seq == s_ui_time_seq) {
        return;
    }
    s_ui_time_seq = st.time_seq;
    face_lvgl_set_time(st.year, st.mon, st.day, st.week % 7, st.hh, st.mm, st.ss);
}

/* 500 ms 定时器：服务读 RTC/投事件，UI 只负责把快照推给表盘 */
static void rtc_timer_cb(lv_timer_t *t)
{
    (void)t;
    svc_time_tick(lv_tick_get());
    ui_time_apply();
    face_image_tick();      /* B：图片表盘叠层（内部 500 ms 节流 + 可见性判断） */
    app_clock_tick();       /* 2026-11-08：计时器到点 / 闹钟到点 */
}

/* R4：BLE 通知 -> 简单卡片 + 200 ms 震动（UI 任务） */
static int s_notify_wake;
static uint32_t s_notify_wake_until;
static int s_find_left;
static uint32_t s_find_next_ms;

/* ------------------------------------------------------------------ */
/* 2026-11-08：秒表 / 计时器 / 闹钟的 UI 侧 glue                        */
/*                                                                    */
/*   服务层（services/svc_*）只管状态与判定；这里负责：                 */
/*     · 到点 → 亮屏 + 震动 + 通知弹层（复用 BLE 通知的 3 s 卡片）；    */
/*     · RTC 报警 arming 回调（sd3010_alarm_set_hhmm / 整分报警）；     */
/*     · 睡前问询（闹钟 arm）与禁睡（倒计时运行）；                     */
/*     · PDS15 复位唤醒后的启动补触发。                                */
/* ------------------------------------------------------------------ */
#define CLOCK_FIRE_TIMER 0
#define CLOCK_FIRE_ALARM 1

static int s_buzz_left;
static uint32_t s_buzz_next_ms;
static uint32_t s_clock_off_poll_ms;
static int s_clock_off_polled;

static void ui_buzz_start(int times)
{
    s_buzz_left = times;
    s_buzz_next_ms = 0;
    printf("[buzz] start %d\r\n", times);
}

/* 非阻塞震动节拍：每 400 ms 一下、每下 200 ms（和 FIND 的节奏一致） */
static void ui_buzz_tick(uint32_t now_ms)
{
    if (s_buzz_left <= 0) {
        return;
    }
    if ((int32_t)(now_ms - s_buzz_next_ms) < 0) {
        return;
    }
    motor_pulse(100, 200);
    s_buzz_left--;
    s_buzz_next_ms = now_ms + 400u;
    printf("[buzz] pulse t=%lu left=%d\r\n", (unsigned long)now_ms, s_buzz_left);
}

/* 读一次 RTC（息屏时 app_state 里的时间是冻结的，必须直接读 RTC） */
static int clock_now(svc_alarm_time_t *out)
{
    sd3010_time_t tm;

    if (out == NULL || svc_time_get(&tm) != 0) {
        return -1;
    }
    out->year = (uint16_t)(2000 + tm.year);
    out->month = tm.month;
    out->day = tm.day;
    out->hour = tm.hour;
    out->min = tm.min;
    out->sec = tm.sec;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 2026-11-08：PC 模式下的本地弹窗（闹钟/计时器到点）                   */
/*                                                                    */
/* 用户要求：不退出 PC 模式、不拆 BLE session，只**暂停接收 PC 像素帧**，*/
/* 把本地圆屏通知卡画出来；5–6 s 后恢复收帧并请求主机整屏重发。          */
/* 普通模式（非 PC）svc_pc_active()==0，这里什么都不做，行为完全不变。   */
/* ------------------------------------------------------------------ */
#define CLOCK_PC_POPUP_MS 6000u

static int s_pc_popup_active;
static uint32_t s_pc_popup_until;

static void app_clock_pc_popup_begin(void)
{
    if (!svc_pc_active()) {
        return;
    }
    svc_pc_suspend_frames(1);
    s_pc_popup_active = 1;
    s_pc_popup_until = lv_tick_get() + CLOCK_PC_POPUP_MS;
    printf("[clock] PC mode: local popup, PC pixel frames suspended %u ms\r\n",
           (unsigned)CLOCK_PC_POPUP_MS);
}

static void app_clock_pc_popup_tick(void)
{
    if (!s_pc_popup_active) {
        return;
    }
    if ((int32_t)(lv_tick_get() - s_pc_popup_until) < 0) {
        return;
    }
    s_pc_popup_active = 0;
    svc_pc_suspend_frames(0);
    printf("[clock] PC mode: popup done, PC frames resumed\r\n");
}

static void app_clock_fire(int kind)
{
    const char *title = (kind == CLOCK_FIRE_ALARM) ? "闹钟" : "计时器";
    const char *body = (kind == CLOCK_FIRE_ALARM) ? "时间到" : "倒计时结束";
    int was_off = !power_is_on();

    printf("[clock] fire %s\r\n", (kind == CLOCK_FIRE_ALARM) ? "alarm" : "timer");
    /* 2026-11-08：PC 模式下不退出 PC 模式，只暂停接收 PC 像素帧，把本地通知卡画出来 */
    app_clock_pc_popup_begin();
    if (was_off) {
        screen_on_safe();
    }
    (void)app_notify_show(title, body);
    ui_buzz_start((kind == CLOCK_FIRE_ALARM) ? 6 : 4);
    if (was_off) {
        /* 和 BLE 通知一样：为通知亮起来的屏，停几秒后自动息屏；
         * 本来亮着就不动（用户可能正在用）。 */
        s_notify_wake = 1;
        s_notify_wake_until = lv_tick_get() + ((kind == CLOCK_FIRE_ALARM) ? 8000u : 4000u);
    }
}

/* 醒着时 500 ms 一拍：倒计时到点 + 闹钟到点 */
static void app_clock_tick(void)
{
    svc_alarm_time_t now;

    if (svc_timer_tick(lv_tick_get())) {
        app_clock_fire(CLOCK_FIRE_TIMER);
    }
    if (clock_now(&now) == 0 && svc_alarm_poll(&now)) {
        app_clock_fire(CLOCK_FIRE_ALARM);
    }
}

/* 息屏（LVGL 定时器停摆）时的轮询：节流 200 ms，到点就亮屏 + 触发 */
static void app_clock_poll_offline(void)
{
    uint32_t now = lv_tick_get();
    svc_alarm_time_t tm;

    if (s_clock_off_polled && (uint32_t)(now - s_clock_off_poll_ms) < 200u) {
        return;
    }
    s_clock_off_polled = 1;
    s_clock_off_poll_ms = now;

    if (svc_timer_tick(now)) {
        app_clock_fire(CLOCK_FIRE_TIMER);
    }
    if (clock_now(&tm) == 0 && svc_alarm_poll(&tm)) {
        app_clock_fire(CLOCK_FIRE_ALARM);
    }
}

/* RTC 报警 arming：开了闹钟写"每天 HH:MM:00"，关了恢复原来的整分报警 */
static void alarm_arm_rtc(int enabled, uint8_t hour, uint8_t minute)
{
    int r;

    if (enabled) {
        r = sd3010_alarm_set_hhmm(hour, minute, 0);
        printf("[alarm] RTC daily alarm %02u:%02u armed rc=%d\r\n",
               (unsigned)hour, (unsigned)minute, r);
    } else {
        r = sd3010_alarm_set_sec(0);
        printf("[alarm] disabled -> minute alarm rc=%d\r\n", r);
    }
}

/* 睡前问询：1 = 已 arm（board_power 会跳过 alarm_disable，靠 IO17 唤醒） */
static int alarm_sleep_arm(void)
{
    return svc_alarm_arm_for_sleep();
}

/* 唤醒/启动后按当前配置重设 RTC 报警 */
static void alarm_sleep_apply(void)
{
    svc_alarm_apply_rtc();
}

/* 倒计时运行中禁睡（PDS15 唤醒=复位，RAM 状态会丢） */
static int clock_sleep_veto(void)
{
    return svc_timer_is_running();
}

static void ui_handle_notify(const uint8_t *p, uint16_t len)
{
    char title[40];
    char body[96];
    uint8_t cat, tlen;
    uint16_t blen;

    if (p == NULL || len < 3) {
        return;
    }
    cat = p[0];
    tlen = p[1];
    if ((uint16_t)(2u + tlen + 1u) > len || p[2 + tlen] != 0) {
        printf("[notify] bad frame\r\n");
        return;
    }
    if (tlen > sizeof(title) - 1) {
        tlen = sizeof(title) - 1;
    }
    memcpy(title, p + 2, tlen);
    title[tlen] = '\0';

    blen = (uint16_t)(len - 3u - tlen);
    if (blen > sizeof(body) - 1) {
        blen = sizeof(body) - 1;
    }
    memcpy(body, p + 3 + tlen, blen);
    body[blen] = '\0';

    printf("[notify] cat=%u title=\"%s\" body=\"%s\"\r\n", cat, title, body);
    (void)app_notify_show(title, body);
    motor_pulse(100, 200);
}

static void ui_find_tick(uint32_t now_ms)
{
    if (s_find_left <= 0) {
        return;
    }
    if ((int32_t)(now_ms - s_find_next_ms) < 0) {
        return;
    }
    motor_pulse(100, 200);
    s_find_left--;
    s_find_next_ms = now_ms + 400u;
}


/* ------------------------------------------------------------------ */
/* LVGL 任务                                                           */
/* ------------------------------------------------------------------ */

static void lvgl_task(void *param)
{
    (void)param;

    printf("\r\n");
    printf("==================================================\r\n");
    printf("  PROTORIG ES V1.6   LVGL v9.5 bring-up spike\r\n");
    printf("  panel  ST7802 480x480 QSPI   RGB565\r\n");
    printf("  band   %d lines = %u bytes x2 buffers\r\n",
           BAND_LINES, (unsigned)BAND_BYTES);
    printf("  build  %s %s\r\n", __DATE__, __TIME__);
    printf("==================================================\r\n");


    /* ---- R2a：app_state + event_queue（纯新增；R3c 起 svc_time 写时间）----
     * event_t 只有 {type,len,data[64]}：禁止放指针/大结构；>64 B 走共享缓冲。
     * app_state 只在临界区整结构拷贝；初始化必须先于 svc_time_init。 */
    app_state_init();
    if (event_queue_init() != 0) {
        printf("!! event_queue_init failed\r\n");
    } else {
        printf("[app] app_state + event_queue ready (len=%u, data=%u B)\r\n",
               (unsigned)EVENT_QUEUE_LEN, (unsigned)EVENT_DATA_MAX);
    }

    /* ---- 面板 ---- */
    printf("st7802_init ...\r\n");
    st7802_init();
    st7802_read_id_report();

    st7802_set_write_hz(QSPI_WRITE_HZ);
    if (st7802_set_fmt565(1) != 0) {
        printf("!! 切 RGB565 失败，LVGL 颜色会不对\r\n");
    }
    st7802_set_px_swap(0);
    st7802_fill(0, 0, LCD_W - 1, LCD_H - 1, 0x000000);
    printf("panel: fmt565=%d px_swap=%d write=%u Hz\r\n",
           st7802_get_fmt565(), st7802_get_px_swap(),
           (unsigned)st7802_get_write_hz());

    /* ---- LVGL ---- */
    lv_init();
    lv_tick_set_cb(lv_tick_ms);
    port_disp_init();
    board_display_set_flush_hook(min_capture_flush_hook);  /* R2c：diag 钩子 */
    printf("lvgl: %d.%d.%d  color_depth=%d  disp=%dx%d\r\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
           LV_COLOR_DEPTH, LCD_W, LCD_H);

    /* ---- 充电状态脚（V1.6：CHRG=IO3 / STDBY=IO21）---- */
    chrg_init();

    /* ---- 真实时钟 SD3010（I2C0）---- */
    printf("[rtc] i2c_bus_init = %d\r\n", i2c_bus_init(I2C_BUS_FREQ_HZ));
    printf("[rtc] sd3010_init  = %d\r\n", sd3010_init());
    settings_load();                    /* PDS15 复位唤醒后先恢复用户设置 */
    power_set_persist_cb(settings_save);

    /* ---- R3c：RTC 服务（互斥量/IRQ/掉电播种/分钟报警/PDS 回调）---- */
    svc_time_init();

    /* ---- 2026-11-08：秒表 / 计时器 / 闹钟（APP 第一批）----
     * 闹钟配置存在 settings v2 预留的 alarm_h/alarm_m/alarm_flags 里，
     * 这里读回 + 注册持久化/arming 回调，并按配置重设 RTC 报警。
     * 睡前策略钩子同时注册：闹钟 arm（IO17 唤醒）/ 倒计时期间禁睡。 */
    {
        settings_payload_t sp;

        settings_get_payload(&sp);
        svc_stopwatch_init();
        svc_timer_init();
        svc_alarm_init(sp.alarm_h, sp.alarm_m, (sp.alarm_flags & 0x01u) != 0);
        svc_alarm_set_save_cb(settings_save_alarm);
        svc_alarm_set_arm_cb(alarm_arm_rtc);
        svc_alarm_apply_rtc();
        board_power_set_alarm_callbacks(alarm_sleep_arm, alarm_sleep_apply);
        board_power_set_sleep_veto_cb(clock_sleep_veto);
    }

    /* ---- R3c：电池服务 + 低电策略回调（服务不直接调 power）---- */
    svc_battery_init();
    svc_battery_set_policy_cb(battery_policy_apply);

    /* ---- R4：BLE（广播/连接/DIS/BAS/CTS/5 命令）---- */
    svc_ble_init();

    /* ---- 电脑模式（-1 屏透传显示）---- */
    svc_pc_init();

    {
        settings_payload_t sp;
        settings_get_payload(&sp);
        svc_ble_power_mode_load(sp.ble_power_mode);
        svc_ble_set_persist_cb(settings_save_power_mode);
    }

    /* ---- PDS 持久计数：开机次数 + 上次 PDS 记录 ---- */
    pds_log_load();
    pds_log_bump(0);
    {
        uint8_t ev = 0;
        HBN_Get_Reset_Event(&ev);
        printf("[pdslog] boot#%u enter=%u touch=%u rtc=%u other=%u reset=0x%02X\r\n",
               s_pds_log[0], s_pds_log[1], s_pds_log[2], s_pds_log[3],
               s_pds_log[4], ev);
    }

    /* ---- 触摸（CST9220 -> LVGL indev）---- */
    if (tp_init() != 0) {
        printf("!! tp_init failed\r\n");
    }
    tp_lvgl_init();
    board_power_set_touch_poll_cb(tp_lvgl_poll_raw);   /* R2c：BSP 不直接依赖 UI 层 */

    /*
     * 触摸中断唤醒（V1.6：IO16 = TP_INT，AON 脚）
     *
     * 以前息屏是「每 15 ms 读一次 I2C」，CPU 每 15 ms 被叫醒一次。
     * 现在：IO16 下降沿中断 → 二分信号量 → 息屏时 CPU 完全不动。
     *
     * 几个要点：
     *  · CST9220 的 INT 是**低有效短脉冲**（实测 16~20 ms），所以
     *    用**边沿**而不是电平来等 —— 电平会被"一直按着"骗过去。
     *  · V1.6 起 CHRG/STDBY 是独立脚（IO3/IO21），IO16 只挂触摸 INT，
     *    不会再出现"充电把触摸中断线恒拉低"的问题。
     *  · IO16 是 AON 脚，下一步 PDS 深睡也用它做唤醒源（IO17 给 RTC）。
     */
    s_tp_wake_sem = xSemaphoreCreateBinary();
    bflb_gpio_irq_attach(GPIO_PIN_16, tp_int_isr);
    bflb_irq_enable(bflb_device_get_by_name("gpio")->irq_num);
    printf("[tp] int wake armed on IO16 (falling edge, AON)\r\n");

    /* PDS：默认 level 15（CPU 掉电、复位唤醒；摸屏约 1 秒亮，USB 自然重枚举）。
     * 注释里曾写"默认 level 1"，与代码不符，已按实测结论改正。 */
    power_pds_init(15);
    power_pds_set_restore_cb(pds_restore_irq);

    /* ---- 画表盘（LVGL 版「协议源石容器」）---- */
    face_lvgl_create();
    ui_time_apply();                /* 立刻把 svc_time 已填好的快照推上去 */

    /* ---- B：图片表盘存储（media 分区单槽；只读 header + 校验，不碰 LVGL）---- */
    {
        int fr = face_store_init();
        printf("[face] store init=%d (%s)\r\n", fr, fr == 0 ? "valid" : "none");
    }

    /* ---- 原生页面（PAGE_N=8）+ 表盘上的入口；firmware/eez_ui/ 只是历史参考，不编译 ---- */
    eez_ui_init();
    /* App 注册表：注册 2 个表盘 + 8 个页面 App；R3c A+B 起切页由 App 生命周期驱动 */
    app_framework_init();
    (void)app_launch_page(0);          /* R3c：建立 current + page=0 状态 */

    /* ---- B：恢复上次的表盘（face_id 持久化在 SD3010 用户 RAM）----
     * 76 §1（2026-11-08）：face 1/2 已删除 —— 旧 settings 里存着 1/2 的一律回退 0
     * 并写回 0，别让人停在已经不存在的表盘上（否则 `face` 命令和 UI 都对不上）。
     * face 3 且 media 里没有有效图片 → 同样回落默认表盘并清掉持久值，
     * 否则用户会"重启后停在一张未安装的提示页"。 */
    {
        settings_payload_t sp;

        settings_get_payload(&sp);
        if (sp.face_id > 0) {
            if (sp.face_id != 3) {
                printf("[face] face_id=%u 已删除（只支持 0/3）-> fallback 0\r\n",
                       (unsigned)sp.face_id);
                (void)settings_save_face_id(0);
            } else if (!face_image_available()) {
                printf("[face] face_id=3 but no valid image -> fallback 0\r\n");
                (void)settings_save_face_id(0);
            } else {
                (void)app_launch_face(3);
                printf("[face] restored face_id=3 from settings\r\n");
            }
        }
    }
    app_notify_init();
    app_notify_ui_init();       /* R4：简单通知卡片（UI 任务懒创建） */
    svc_demo_init();
    ui_theme_init();
    lv_timer_create(eez_tick_cb, 100, NULL);

    /* ---- 息屏 / 触摸唤醒 ---- */
    power_set_wake_cb(face_wake_cb);
    power_init();
    power_apply_brightness();           /* st7802_init 默认写 0xFF，把读回的亮度盖回去 */
    /*
     * 2026-10-16 晚：查"整分花屏"期间这里曾临时打开开机常亮（`power_set_always_on(1)`），
     * 因为息屏后 LVGL 停渲染，而取证要盯着屏看几十秒，且每次上电 idle 都被重置回 15000ms。
     * bug 已修复（见 docs/minute_digit_fixed_20261016.md），**已改回默认不常亮**，
     * 恢复"触摸唤醒 + 15s 空闲息屏"的电池优先行为。
     * 需要取证时用 shell 的 `alwayson 1` 临时打开即可。
     */
    power_set_always_on(0);
    lv_timer_create(power_timer_cb, 500, NULL);
    lv_timer_create(rtc_timer_cb, 500, NULL);   /* 500ms 轮询 RTC */

    /* ---- 振动马达（IO30 / PWM CH2）----
     * 只把脚初始化成"低 = 停"，不建 PWM（PWM 单元等第一次 motor_set() 再建）。
     * 马达从 BAT+ 直接取电，满压约 50 mA；开机这一下不震。 */
    motor_init();

    /* ---- 2026-11-08：闹钟启动补触发 ----
     * PDS15（默认深睡级别）到点唤醒是**复位启动**，RTC 报警标志在 boot 里
     * 已随 sd3010_init/svc_time_init 被清掉，所以这里按"当前 RTC 时间是否
     * 落在闹钟那一分钟"补触发。正常开机（不在那一分钟）不会响。
     * 放在 motor_init/power_init 之后：震动和亮屏的依赖都已就绪。 */
    {
        svc_alarm_time_t now;

        if (clock_now(&now) == 0 && svc_alarm_boot_check(&now)) {
            printf("[alarm] boot inside alarm minute -> fire now\r\n");
            app_clock_fire(CLOCK_FIRE_ALARM);
        }
    }

    /* ---- 电池采样（IO20 / ADC_CH0，100k+100k 分压）----
     * V1.4 就有实现（watchface/board_adc.c），这里移植进来。
     * 之前表盘上的电量是写死的 87%，Codex 把那块删成了占位符 —— 现在补上真实值。
     *
     * 注意：这里**什么都不做**，第一次采样放在 3 秒后的定时器里（见 batt_timer_cb）。
     * 这样可以保证首帧一定先渲染出来，不会被 ADC 拖住。 */
    lv_timer_create(batt_timer_cb, 3000, NULL);
    printf("[batt] first sample in 3 s\r\n");


    printf("ready. shell: face / faceimg / facepanel / screen 0|1 / panelsleep 0-3 / idle <ms> / tpstat / settime / rtcstat / batt / motor / alarm\r\n");

    for (;;) {
        /* R3c C：screen 域，只在 UI 主循环边沿写（唯一写入者） */
        {
            static int s_last_screen_on = -1;
            int on = power_is_on() ? 1 : 0;
            if (on != s_last_screen_on) {
                s_last_screen_on = on;
                app_state_set_screen((uint8_t)on);
                if (on) {
                    int r = tp_exit_lp_scan();
                    printf("[tp] exit LP scan (D1 06) = %d\r\n", r);
                    s_tp_lp_scan = 0;
                    svc_ble_start();
                } else {
                    if (!s_tp_lp_scan) {
                        int r = tp_enter_lp_scan();
                        printf("[tp] enter LP scan (D1 07) = %d\r\n", r);
                        if (r >= 0) {
                            s_tp_lp_scan = 1;
                        }
                    }
                    if (svc_ble_power_mode_get() == 0) {
                        svc_ble_stop();
                    }
                }
            }
        }

        /* R4：drain BLE 事件 -> event_t（只在 UI 任务） */
        svc_ble_tick();

        /* 事件消费点：svc_time / svc_battery / svc_ble 投事件，这里分发给
         * UI（通知/查找/对时）并交给 app_event_dispatch()；`appstat` 看计数。 */
        {
            event_t ev;
            while (event_queue_pop(&ev)) {
                s_evt_consumed++;
                switch (ev.type) {
                case EV_BLE_CONNECTED:
                    printf("[ui] BLE connected\r\n");
                    break;
                case EV_BLE_DISCONNECTED:
                    printf("[ui] BLE disconnected\r\n");
                    break;
                case EV_NOTIFY:
                    ui_handle_notify(ev.data, ev.len);
                    if (!power_is_on()) {
                        screen_on_safe();
                        s_notify_wake = 1;
                        s_notify_wake_until = lv_tick_get() + 3000u;
                    }
                    break;

                case EV_NOTIFY_BMP:
                    motor_pulse(100, 200);
                    if (!power_is_on()) {
                        screen_on_safe();
                        s_notify_wake = 1;
                        s_notify_wake_until = lv_tick_get() + 3000u;
                    }
                    break;
                case EV_FIND:
                    s_find_left = 3;
                    s_find_next_ms = 0;
                    printf("[ui] FIND: 3 buzzes\r\n");
                    break;
                case EV_TIME_SET:
                    ui_time_apply();
                    break;
                default:
                    break;
                }
                app_event_dispatch(&ev);
            }
        }

        ui_find_tick(lv_tick_get());
        ui_buzz_tick(lv_tick_get());    /* 2026-11-08：闹钟/计时器震动节拍 */
        app_clock_pc_popup_tick();      /* 2026-11-08：PC 模式弹窗 6 s 后恢复收帧 */
        app_notify_ui_tick(lv_tick_get());
        if (s_notify_wake && power_is_on() && !app_notify_bmp_expanded() &&
            (int32_t)(lv_tick_get() - s_notify_wake_until) >= 0) {
            if (tp_int_asserted()) {
                s_notify_wake_until = lv_tick_get() + 1000u;
            } else {
                s_notify_wake = 0;
                power_screen_off();
            }
        }

        pc_monitor_service();
        svc_pc_tick();          /* 电脑模式：帧处理/链路/常亮 */

        /* shell 的 panelsleep：必须在 LVGL 任务里执行（会和刷屏抢 DBI）。
         * 该命令只用于上板量电流，默认息屏路径完全不变。 */
        if (s_panelsleep_req >= 0) {
            int lvl = s_panelsleep_req;
            s_panelsleep_req = -1;
            panelsleep_apply(lvl);
        }

        /* 面板按 panelsleep 睡下去了：停掉 lv_timer_handler（不刷屏、
         * 不跑 power_tick/看门狗定时器），等 shell panelsleep 0 唤醒。
         * 这样量电流时是一个稳定的静态状态，也不会被自动亮屏/自动 PDS 打扰。 */
        if (st7802_panel_sleep_level() > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        taskENTER_CRITICAL();
        int trace_req = s_trace_req;
        s_trace_req = 0;
        taskEXIT_CRITICAL();
        if(trace_req == 1) { screen_on_safe(); power_note_touch(); tp_trace_start(); }
        if(trace_req == 2) tp_trace_dump();
        if (s_face_req) {
            int id = s_face_req - 1;
            s_face_req = 0;
            (void)app_launch_face(id);   /* page/face 由 launcher 写 */
        }
        /* shell 的 clear：整屏标脏（必须在 LVGL 任务里做，见 cmd_clear 的注释） */
        if (s_clear_req) {
            s_clear_req = 0;
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(NULL);          /* 立刻刷完，好让"干净"这件事是确定的 */
            printf("[clear] 整屏重绘完成（%d px）\r\n", LCD_W * LCD_H);
        }

        /* shell 发来的屏幕开关请求：必须在本任务里执行（会和刷屏抢 DBI）*/
        if (s_screen_req >= 0) {
            int r = s_screen_req;
            s_screen_req = -1;
            if (r) {
                screen_on_safe();
            } else {
                power_screen_off();
                if (svc_ble_power_mode_get() == 0) {
                    svc_ble_stop();   /* 手表模式省电：先停 BLE，再允许同轮 PDS */
                }
            }
        }

        /* shell 的 `alarm fire`：亮屏 + 通知 + 震动只能在 UI 任务里做 */
        if (s_clock_fire_req >= 0) {
            int kind = s_clock_fire_req;
            s_clock_fire_req = -1;
            app_clock_fire(kind);
        }

        /* ---- 息屏状态：能睡就睡，绝不轮询 ----
         *
         * 以前是「每 15 ms 读一次 I2C」，CPU 每 15 ms 被叫醒。
         * 现在分两种情况：
         *   · IO16 高电平 → 阻塞等触摸中断（二分信号量），CPU 全程 WFI；
         *   · IO16 低电平 → 手指正按着 / 中断已挂起，退回 50 ms 轮询。
         * 手表模式通常等 20 秒才超时，顺手给飞行记录仪补一条电压；
         * 电脑模式在 BSP 里缩短等待，及时处理帧和掉线，同时禁止 PDS。
         */
        if (!power_is_on()) {
            /* R2c：自动睡决策已搬到 bsp/board_power.c（纯搬移，continue 语义由返回值表达）。
             * 前置条件在本分支内天然成立；无论睡没睡，调用方都 continue 主循环。 */
            /* R2.1：屏灭也 drain 一次 BLE，防止事件卡到睡前；
             * 通知/查找/对时已在本轮事件分发里处理。 */
            svc_ble_tick();
            (void)board_power_auto_sleep(svc_pc_active());
            /* 2026-11-08：息屏期间到点（倒计时/闹钟）→ 亮屏 + 震动 + 通知。
             * PDS15 唤醒是复位，走启动补触发；这里覆盖 PDS1/3 与"禁睡"两种情况。 */
            app_clock_poll_offline();
            continue;
        }

        /* B：图片表盘叠层刷新（内部 500 ms 节流；放在息屏 continue 之后，
         * 屏幕灭时这里根本不会执行，不会为它多耗电）。 */
        face_image_tick();

        uint32_t d = lv_timer_handler();
        if (d < 2) {
            d = 2;
        }
        if (d > 20) {
            d = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(d));
    }
}

/* ------------------------------------------------------------------ */
/* shell 命令                                                          */
/* ------------------------------------------------------------------ */

/* （W1 2026-10-28：原 cmd_bench / bench 命令已删除 —— 它只服务 LVGL demo
 *  benchmark，而 demo 源文件已从 CMakeLists 移除。需要基准数据时请另开一个
 *  LVGL demo 工程跑，不要在本工程恢复这个依赖。） */

/* 重新画表盘（唤醒/切页后用它把界面切回来）
 * 76 §1（2026-11-08 用户拍板）：只保留 face 0（默认）/ face 3（图片）；
 * face 1（极简）/ face 2（监控）已删除，传 1/2 直接拒绝并提示"只支持 0/3"，
 * 绝不把请求转成某个别的表盘（那样用户会以为切成功了）。 */
static int cmd_face(int argc, char **argv)
{
    int id = 0;

    if (argc >= 2) {
        id = atoi(argv[1]);
    }
    if (id != 0 && id != 3) {
        printf("face: 只支持 0/3（0=default 3=image），1/2 已删除\r\n");
        return -1;
    }
    s_face_req = id + 1;   /* 0 = idle; 1..4 = face 0..3 */
    printf("face -> %d (0=default 3=image)\r\n", id);
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_face, face, re-create the LVGL watch face);

/* ------------------------------------------------------------------ */
/* B：图片表盘安装命令（契约 70 §3.5）                                  */
/*                                                                    */
/*   faceimg                              看状态（不重算 231 KB CRC）  */
/*   faceimg info                         额外重算 image CRC32        */
/*   faceimg erase                        擦槽（之后 = 未安装）        */
/*   faceimg begin <len> <crc32>          开始接收（len 必须 231424）  */
/*   faceimg w <off> <hex>                写 image blob（hex ≤ 48 B）  */
/*   faceimg end <x> <y> <font> <flags> <rrggbb>  校验 CRC → 写 header */
/*   faceimg apply                        切到 face 3（LVGL 任务执行 + 持久化） */
/*                                                                    */
/* shell 单行上限 CONFIG_SHELL_CMD_SIZE = 120 字符：48 B = 96 个 hex     */
/* 字符，加上 "faceimg w 231376 "（17）正好 113，留了余量。            */
/* 真正的 flash/CRC 逻辑都在 services/svc_face_store.c，这里只做参数解析 */
/* 和 printf；header 写成功前不会碰 image 区以外的任何东西。            */
/* ------------------------------------------------------------------ */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static const char *faceimg_rc_str(int rc)
{
    switch (rc) {
    case FACE_STORE_OK:         return "ok";
    case FACE_STORE_ERR_ARGS:   return "bad args/len/align";
    case FACE_STORE_ERR_CRC:    return "crc mismatch (refused)";
    case FACE_STORE_ERR_FLASH:  return "flash error";
    case FACE_STORE_ERR_STATE:  return "state error (begin? received?)";
    case FACE_STORE_ERR_FORMAT: return "bad header format";
    default:                    return "unknown";
    }
}

static int faceimg_print_status(int with_crc)
{
    face_store_info_t info;
    int rc = face_store_info(&info, with_crc);

    if (rc != FACE_STORE_OK) {
        printf("[faceimg] valid=0 rc=%d (%s)\r\n", rc, faceimg_rc_str(rc));
        return rc;
    }
    printf("[faceimg] valid=1 crc=%08lX len=%lu w=%u h=%u time=(%d,%d) font=%u flags=%u color=%06lX%s\r\n",
           (unsigned long)info.image_crc32, (unsigned long)info.image_len,
           info.width, info.height, info.time_x, info.time_y,
           info.font_id, info.flags, (unsigned long)info.time_color,
           with_crc ? " crc-checked" : "");
    return FACE_STORE_OK;
}

static int cmd_faceimg(int argc, char **argv)
{
    static uint8_t hexbuf[64];      /* 别放栈上：shell exec 线程栈只有 1 KB */
    const char *sub;

    if (argc < 2) {
        (void)faceimg_print_status(0);
        return 0;
    }
    sub = argv[1];

    if (strcmp(sub, "info") == 0) {
        return faceimg_print_status(1);
    }
    if (strcmp(sub, "erase") == 0) {
        int rc = face_store_erase();
        printf("[faceimg] erase %s\r\n", faceimg_rc_str(rc));
        return rc;
    }
    if (strcmp(sub, "begin") == 0) {
        uint32_t len, crc;
        int rc;

        if (argc < 4) {
            printf("[faceimg] usage: faceimg begin <len> <crc32>\r\n");
            return -1;
        }
        len = (uint32_t)strtoul(argv[2], NULL, 0);
        crc = (uint32_t)strtoul(argv[3], NULL, 16);
        rc = face_store_begin(len, crc);
        printf("[faceimg] begin len=%lu crc=%08lX %s\r\n",
               (unsigned long)len, (unsigned long)crc, faceimg_rc_str(rc));
        return rc;
    }
    if (strcmp(sub, "w") == 0) {
        const char *hex;
        uint32_t off;
        size_t hlen, n, i;
        int rc;

        if (argc < 4) {
            printf("[faceimg] usage: faceimg w <off> <hex>\r\n");
            return -1;
        }
        off = (uint32_t)strtoul(argv[2], NULL, 0);
        hex = argv[3];
        hlen = strlen(hex);
        if (hlen == 0 || (hlen & 1u) != 0 || hlen > 96u) {
            printf("[faceimg] W-ERR hex len %u (want even, <=96)\r\n",
                   (unsigned)hlen);
            return FACE_STORE_ERR_ARGS;
        }
        n = hlen / 2u;
        for (i = 0; i < n; i++) {
            int hi = hex_nibble(hex[2 * i]);
            int lo = hex_nibble(hex[2 * i + 1]);

            if (hi < 0 || lo < 0) {
                printf("[faceimg] W-ERR bad hex at %u\r\n", (unsigned)(2 * i));
                return FACE_STORE_ERR_ARGS;
            }
            hexbuf[i] = (uint8_t)((hi << 4) | lo);
        }
        rc = face_store_write(off, hexbuf, (uint32_t)n);
        if (rc != FACE_STORE_OK) {
            printf("[faceimg] W-ERR off=%lu rc=%d (%s)\r\n",
                   (unsigned long)off, rc, faceimg_rc_str(rc));
            return rc;
        }
        printf("W-OK\r\n");
        return 0;
    }
    if (strcmp(sub, "end") == 0) {
        face_store_info_t layout;
        long x, y;
        int font, flags;
        unsigned long color;
        int rc;

        if (argc < 7) {
            printf("[faceimg] usage: faceimg end <x> <y> <font> <flags> <rrggbb>\r\n");
            return -1;
        }
        x = strtol(argv[2], NULL, 0);
        y = strtol(argv[3], NULL, 0);
        font = atoi(argv[4]);
        flags = atoi(argv[5]);
        color = strtoul(argv[6], NULL, 16);
        if (x < -32768 || x > 32767 || y < -32768 || y > 32767 ||
            font < 0 || font > 3 || flags < 0 || flags > 3 || color > 0xFFFFFFul) {
            printf("[faceimg] install ERR bad layout args\r\n");
            return FACE_STORE_ERR_ARGS;
        }
        memset(&layout, 0, sizeof(layout));
        layout.width = (uint16_t)FACE_IMAGE_W;
        layout.height = (uint16_t)FACE_IMAGE_H;
        layout.time_x = (int16_t)x;
        layout.time_y = (int16_t)y;
        layout.font_id = (uint8_t)font;
        layout.flags = (uint8_t)flags;
        layout.time_color = (uint32_t)color;
        rc = face_store_finish(&layout);
        if (rc == FACE_STORE_OK) {
            face_store_info_t info;

            (void)face_store_info(&info, 0);
            printf("[faceimg] install OK crc=%08lX\r\n",
                   (unsigned long)info.image_crc32);
        } else {
            printf("[faceimg] install ERR rc=%d (%s)\r\n", rc, faceimg_rc_str(rc));
        }
        return rc;
    }
    if (strcmp(sub, "apply") == 0) {
        if (!face_store_is_valid()) {
            printf("[faceimg] apply: 没有有效图片，仍切到 face 3（显示未安装提示页）\r\n");
        }
        s_face_req = 4;   /* face 3 → LVGL 任务里 app_launch_face(3) + 持久化 */
        printf("[faceimg] apply queued (face 3)\r\n");
        return 0;
    }

    printf("[faceimg] usage: faceimg [info|erase|begin <len> <crc>|w <off> <hex>|"
           "end <x> <y> <font> <flags> <rrggbb>|apply]\r\n");
    return -1;
}
SHELL_CMD_EXPORT_ALIAS(cmd_faceimg, faceimg, face image slot: status/info/erase/begin/w/end/apply);

/* ------------------------------------------------------------------ */
/* 76 §2.2：默认表盘下半区面板安装命令（契约 76 §2.1/§2.2）            */
/*                                                                    */
/*   facepanel                     看状态（不重算 112 KB CRC）         */
/*   facepanel info                额外重算 image CRC32               */
/*   facepanel erase               擦整槽（0x3B2000..0x3CEFFF）        */
/*   facepanel clear               只擦 header → face 0 回内置 photo   */
/*   facepanel begin <len> <crc32> 开始接收（len 必须 112384）         */
/*   facepanel w <off> <hex>       写 image blob（hex ≤ 96 = 48 B）    */
/*   facepanel end                 校验 CRC → 写 header → 重建 face 0  */
/*                                                                    */
/* 和上一条 `faceimg` 的分工：faceimg 装的是 face 3（480x480 全屏图）， */
/* facepanel 装的是 face 0 的下半区（480x232，y=248，盖掉内置 watch      */
/* photo）。两个槽的 header/CRC/擦除区完全独立，互不影响。               */
/*                                                                    */
/* shell 单行上限 CONFIG_SHELL_CMD_SIZE = 120 字符：最坏一行           */
/* "facepanel w 112336 "（19）+ 96 个 hex = 115，留了 5 个字符余量。    */
/* 真正的 flash/CRC 逻辑都在 services/svc_face_panel.c，这里只做参数解析 */
/* 和 printf；header 写成功前不会碰 image 区以外的任何东西。            */
/* ------------------------------------------------------------------ */

static const char *facepanel_rc_str(int rc)
{
    switch (rc) {
    case FACE_PANEL_OK:         return "ok";
    case FACE_PANEL_ERR_ARGS:   return "bad args/len/align";
    case FACE_PANEL_ERR_CRC:    return "crc mismatch";
    case FACE_PANEL_ERR_FLASH:  return "flash error";
    case FACE_PANEL_ERR_STATE:  return "state error (begin? received?)";
    case FACE_PANEL_ERR_FORMAT: return "bad header format";
    default:                    return "unknown";
    }
}

static int facepanel_print_status(int with_crc)
{
    face_panel_info_t info;
    int rc = face_panel_info(&info, with_crc);

    if (rc != FACE_PANEL_OK) {
        printf("[facepanel] valid=0 rc=%d (%s)\r\n", rc, facepanel_rc_str(rc));
        return rc;
    }
    printf("[facepanel] valid=1 crc=%08lX len=%lu w=%u h=%u y=%u%s\r\n",
           (unsigned long)info.image_crc32, (unsigned long)info.image_len,
           info.width, info.height, info.y,
           with_crc ? " crc-checked" : "");
    return FACE_PANEL_OK;
}

static int cmd_facepanel(int argc, char **argv)
{
    static uint8_t hexbuf[64];      /* 别放栈上：shell exec 线程栈只有 1 KB */
    const char *sub;

    if (argc < 2) {
        (void)facepanel_print_status(0);
        return 0;
    }
    sub = argv[1];

    if (strcmp(sub, "info") == 0) {
        return facepanel_print_status(1);
    }
    if (strcmp(sub, "erase") == 0) {
        int rc = face_panel_erase();

        /* erase 是上传流程的第 0 步，这里**不**请求重建 face 0（避免上传中途
         * 抖一下屏幕）；要立刻回内置 photo 请用 clear。 */
        printf("[facepanel] erase %s\r\n", facepanel_rc_str(rc));
        return rc;
    }
    if (strcmp(sub, "clear") == 0) {
        int rc = face_panel_clear();

        printf("[facepanel] clear %s\r\n", facepanel_rc_str(rc));
        if (rc == FACE_PANEL_OK) {
            s_face_req = 1;   /* face 0 → LVGL 任务里重建默认表盘（回内置 photo） */
            printf("[facepanel] clear queued (rebuild face 0)\r\n");
        }
        return rc;
    }
    if (strcmp(sub, "begin") == 0) {
        uint32_t len, crc;
        int rc;

        if (argc < 4) {
            printf("[facepanel] usage: facepanel begin <len> <crc32>\r\n");
            return -1;
        }
        len = (uint32_t)strtoul(argv[2], NULL, 0);
        crc = (uint32_t)strtoul(argv[3], NULL, 16);
        rc = face_panel_begin(len, crc);
        printf("[facepanel] begin len=%lu crc=%08lX %s\r\n",
               (unsigned long)len, (unsigned long)crc, facepanel_rc_str(rc));
        if (rc == FACE_PANEL_OK) {
            /* 2026-11-08 上板发现：begin 会整片擦掉正在显示的 XIP 面板，
             * 上传 26 s 期间下半区会读到被擦的 flash（花屏/黑）。
             * 这里立刻请求重建 face 0：header 已擦 -> 回内置 watch photo，
             * 上传完成后 end 再重建一次显示新面板。 */
            s_face_req = 1;
            printf("[facepanel] begin queued rebuild (built-in while uploading)\r\n");
        }
        return rc;
    }
    if (strcmp(sub, "w") == 0) {
        const char *hex;
        uint32_t off;
        size_t hlen, n, i;
        int rc;

        if (argc < 4) {
            printf("[facepanel] usage: facepanel w <off> <hex>\r\n");
            return -1;
        }
        off = (uint32_t)strtoul(argv[2], NULL, 0);
        hex = argv[3];
        hlen = strlen(hex);
        if (hlen == 0 || (hlen & 1u) != 0 || hlen > 96u) {
            printf("[facepanel] W-ERR hex len %u (want even, <=96)\r\n",
                   (unsigned)hlen);
            return FACE_PANEL_ERR_ARGS;
        }
        n = hlen / 2u;
        for (i = 0; i < n; i++) {
            int hi = hex_nibble(hex[2 * i]);
            int lo = hex_nibble(hex[2 * i + 1]);

            if (hi < 0 || lo < 0) {
                printf("[facepanel] W-ERR bad hex at %u\r\n", (unsigned)(2 * i));
                return FACE_PANEL_ERR_ARGS;
            }
            hexbuf[i] = (uint8_t)((hi << 4) | lo);
        }
        rc = face_panel_write(off, hexbuf, (uint32_t)n);
        if (rc != FACE_PANEL_OK) {
            printf("[facepanel] W-ERR off=%lu rc=%d (%s)\r\n",
                   (unsigned long)off, rc, facepanel_rc_str(rc));
            return rc;
        }
        printf("W-OK\r\n");
        return 0;
    }
    if (strcmp(sub, "end") == 0) {
        int rc = face_panel_finish();

        if (rc == FACE_PANEL_OK) {
            face_panel_info_t info;

            (void)face_panel_info(&info, 0);
            printf("[facepanel] install OK crc=%08lX\r\n",
                   (unsigned long)info.image_crc32);
            /* 面板已经进了 header：请求重建 face 0，让下半区立刻换成新图。
             * 只是置标志，真正的 LVGL 重建在 UI 任务里做（不能在这里碰 LVGL）。 */
            s_face_req = 1;
            printf("[facepanel] apply queued (rebuild face 0)\r\n");
        } else {
            printf("[facepanel] install ERR rc=%d (%s)\r\n", rc, facepanel_rc_str(rc));
        }
        return rc;
    }

    printf("[facepanel] usage: facepanel [info|erase|clear|begin <len> <crc>|"
           "w <off> <hex>|end]\r\n");
    return -1;
}
SHELL_CMD_EXPORT_ALIAS(cmd_facepanel, facepanel,
                       default watch face bottom panel: status/info/erase/clear/begin/w/end);

/* 手动开关屏：screen 0 = 熄屏, screen 1 = 亮屏 */
static int cmd_screen(int argc, char **argv)
{
    if (argc < 2) {
        printf("screen is %s, idle timeout %u ms, off#%lu on#%lu\r\n",
               power_is_on() ? "ON" : "OFF",
               (unsigned)power_get_idle_ms(),
               (unsigned long)power_get_off_count(),
               (unsigned long)power_get_on_count());
        return 0;
    }
    s_screen_req = atoi(argv[1]) ? 1 : 0;
    printf("screen request queued\r\n");
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_screen, screen, power the panel: screen 0|1);

/* 2026-11-08：闹钟诊断/上板测试（不动 UI 也能验证 RTC 报警与唤醒）。
 *   alarm            状态
 *   alarm on|off     开关（写 settings v2 + 重设 RTC 报警）
 *   alarm HH:MM      直接设时间（例如 alarm 07:30）
 *   alarm fire       立刻触发一次"闹钟到点"效果（震动 + 通知 + 亮屏）
 *   alarm in <min>   把闹钟设成"当前时间 + N 分钟"，用于验证 PDS 唤醒 */
static int cmd_alarm(int argc, char **argv)
{
    if (argc < 2) {
        svc_alarm_time_t now = { 0, 0, 0, 0, 0, 0 };
        int rtc = clock_now(&now);
        svc_timer_state_t ts = svc_timer_state();

        printf("alarm %02u:%02u %s | rtc %s %04u-%02u-%02u %02u:%02u:%02u | "
               "timer %s preset=%lu min | sleep_veto=%d\r\n",
               (unsigned)svc_alarm_hour(), (unsigned)svc_alarm_minute(),
               svc_alarm_enabled() ? "ON" : "OFF",
               (rtc == 0) ? "ok" : "ERR",
               (unsigned)now.year, (unsigned)now.month, (unsigned)now.day,
               (unsigned)now.hour, (unsigned)now.min, (unsigned)now.sec,
               (ts == SVC_TIMER_RUNNING) ? "running" :
               (ts == SVC_TIMER_PAUSED) ? "paused" :
               (ts == SVC_TIMER_DONE) ? "done" : "idle",
               (unsigned long)svc_timer_preset_min(), clock_sleep_veto());
        return 0;
    }
    if (!strcmp(argv[1], "on")) {
        svc_alarm_set_enabled(1);
    } else if (!strcmp(argv[1], "off")) {
        svc_alarm_set_enabled(0);
    } else if (!strcmp(argv[1], "fire")) {
        s_clock_fire_req = CLOCK_FIRE_ALARM;   /* 由 UI 任务执行（亮屏/通知/震动） */
        printf("alarm: fire queued (UI task)\r\n");
    } else if (!strcmp(argv[1], "in") && argc >= 3) {
        svc_alarm_time_t now;
        int add = atoi(argv[2]);
        int total;

        if (clock_now(&now) != 0) {
            printf("alarm: RTC read failed\r\n");
            return -1;
        }
        total = ((int)now.hour * 60 + now.min + add) % (24 * 60);
        if (total < 0) {
            total += 24 * 60;
        }
        svc_alarm_set_time((uint8_t)(total / 60), (uint8_t)(total % 60));
        svc_alarm_set_enabled(1);
        printf("alarm in %d min -> %02u:%02u (sleep now to test RTC wake)\r\n",
               add, (unsigned)svc_alarm_hour(), (unsigned)svc_alarm_minute());
    } else if (strchr(argv[1], ':') != NULL) {
        int h = 0, m = 0;
        if (sscanf(argv[1], "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
            printf("alarm: bad time (use HH:MM)\r\n");
            return -1;
        }
        svc_alarm_set_time((uint8_t)h, (uint8_t)m);
        printf("alarm time -> %02d:%02d %s\r\n", h, m,
               svc_alarm_enabled() ? "(ON)" : "(OFF, use 'alarm on')");
    } else {
        printf("usage: alarm | alarm on|off | alarm HH:MM | alarm in <min> | alarm fire\r\n");
    }
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_alarm, alarm, alarm clock: status/set/fire/RTC-wake test);


/* 面板深睡实验：panelsleep 0|1|2|3
 *   0 = 唤醒（VCI 高 -> RST 脉冲 -> SLPOUT -> 120ms -> st7802_init）
 *   1 = 亮度 0 + DISPOFF(0x28)
 *   2 = 再加 SLPIN(0x10) + 120 ms
 *   3 = 再加 DSTBON(0x4F) + QSPI 六线拉低/高阻 + VCI_EN(IO29) 低
 *
 * 只置标志，真正的 DBI/GPIO 操作在 LVGL 任务里执行（panelsleep_apply），
 * 避免和刷屏抢 DBI。**这是实验命令，不改变默认息屏行为。**
 * 建议量电流顺序：screen 1 -> panelsleep 3（等 5 s 稳定）-> 记录电流
 *                 -> panelsleep 0（约 1 s 恢复显示）。 */
static int cmd_panelsleep(int argc, char **argv)
{
    int lvl;

    if (argc < 2) {
        printf("panelsleep level = %d (0=awake, 1=DISPOFF, 2=+SLPIN, 3=+DSTBON+VCI off)\r\n",
               st7802_panel_sleep_level());
        printf("  screen=%s brightness=%u, 仅实验命令，不影响默认息屏\r\n",
               power_is_on() ? "ON" : "OFF", power_effective_brightness());
        return 0;
    }

    lvl = atoi(argv[1]);
    if (lvl < 0 || lvl > 3) {
        printf("panelsleep: level must be 0..3\r\n");
        return -1;
    }
    s_panelsleep_req = lvl;
    printf("panelsleep %d queued (LVGL task will apply)\r\n", lvl);
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_panelsleep, panelsleep, panel deep-sleep experiment: panelsleep 0|1|2|3);

/* appstat —— app_state 快照 + 事件队列计数（自检事件通路是否正常）。 */
static int cmd_appstat(int argc, char **argv)
{
    app_state_t st;
    event_queue_stats_t qs;

    (void)argc;
    (void)argv;

    app_state_get(&st);
    event_queue_get_stats(&qs);
    printf("[app] seq t=%lu b=%lu u=%lu time=%04d-%02d-%02d %02d:%02d:%02d week=%d\r\n",
           (unsigned long)st.time_seq, (unsigned long)st.batt_seq,
           (unsigned long)st.ui_seq,
           st.year, st.mon, st.day, st.hh, st.mm, st.ss, st.week);
    printf("[app] batt=%dmV/%d%% chg=%u lvl=%u screen=%u page=%u face=%u ble=%u notify=%u\r\n",
           st.battery_mv, st.battery_pct, st.charging, st.battery_level, st.screen_on,
           st.page_id, st.face_id, st.ble_connected, st.notify_count);
    {
        const app_desc_t *cur = app_current();
        const app_desc_t *fc = app_face_current();
        printf("[app] current=%s page=%u face=%u screen=%u face_current=%s\r\n",
               cur ? cur->id : "(none)", st.page_id, st.face_id, st.screen_on,
               fc ? fc->id : "(none)");
    }
    printf("[evt] pushed=%lu popped=%lu dropped=%lu depth=%lu consumed=%lu\r\n",
           (unsigned long)qs.pushed, (unsigned long)qs.popped,
           (unsigned long)qs.dropped, (unsigned long)qs.depth,
           (unsigned long)s_evt_consumed);
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_appstat, appstat, app_state snapshot + event queue stats);

/* apps —— R3a：只列注册表，不改变当前页面。 */
static int cmd_apps(int argc, char **argv)
{
    const app_desc_t *cur = app_current();
    (void)argc;
    (void)argv;
    printf("apps: %d registered, current=%s\r\n", app_count(), cur ? cur->id : "(none)");
    for (int i = 0; i < app_count(); i++) {
        const app_desc_t *a = app_at(i);
        printf("  [%d] %-8s %-6s gui=%u bg=%u\r\n", i, a->id, a->name, a->has_gui, a->allow_bg);
    }
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_apps, apps, list registered apps (R3a read-only));

/* 空闲熄屏时间：idle <ms>（0 表示不自动熄屏） */
static int cmd_idle(int argc, char **argv)
{
    if (argc < 2) {
        printf("idle = %u ms, %u ms left\r\n",
               (unsigned)power_get_idle_ms(), (unsigned)power_idle_left_ms());
        return 0;
    }
    power_set_idle_ms((uint32_t)atoi(argv[1]));
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_idle, idle, set auto screen-off timeout in ms: idle <ms>);

/* 设置持久化检查：settings 显示当前值与 RAM 字节；
 * settings <brightness> <idle_ms> 设置并落盘（只接受设置页三档）。 */
static int cmd_settings(int argc, char **argv)
{
    settings_payload_t p;
    uint8_t rec[SETTINGS_RECORD_LEN];
    int rd = sd3010_ram_read(SETTINGS_RAM_ADDR, rec, sizeof(rec));
    int dec = (rd == 0) ? settings_decode(rec, &p) : -1;

    if (argc < 3) {
        settings_get_payload(&p);
        printf("settings: brightness=%u idle=%lu idx=(%u,%u) face=%u flags=0x%02X mode=%u\r\n",
               power_get_brightness(), (unsigned long)power_get_idle_ms(),
               p.brightness_idx, p.idle_idx, p.face_id, p.flags,
               svc_ble_power_mode_get());
        printf("  v2 record @0x%02X rd=%d decode=%d hdr=%02X %02X %02X %02X\r\n",
               (unsigned)SETTINGS_RAM_ADDR, rd, dec, rec[0], rec[1], rec[2], rec[3]);
        return 0;
    }
    power_set_brightness((uint8_t)atoi(argv[1]));
    power_set_idle_ms((uint32_t)atoi(argv[2]));
    power_request_persist();
    if (argc >= 4) {
        int mode = atoi(argv[3]);
        if (mode < 0 || mode > 2 || svc_ble_power_mode_set(mode) != 0) {
            printf("settings: bad mode (0=省电 1=常连 2=智能)\r\n");
            return -1;
        }
    }
    printf("settings set + persist requested (mode=%d)\r\n", svc_ble_power_mode_get());
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_settings, settings, persisted watch settings: settings [brightness idle_ms [mode 0/1/2]]);

/*
 * 低电分级调试（2026-10-18）
 *
 *   lowbatt           看当前档位
 *   lowbatt 0|1|2     强制停在那一档（3 秒内生效）
 *   lowbatt auto      恢复按电压自动判断
 *
 * 为什么需要：真机验证 3.45 / 3.35 V 这两档，要么等电池放空（几小时），
 * 要么用这个强制。强制档同时也会触发空闲熄屏缩短和震动，能一次验完。
 */
static int cmd_lowbatt(int argc, char **argv)
{
    if (argc < 2) {
        printf("lowbatt: level=%d  force=%d  (1=low/3.45V, 2=critical/3.35V)\r\n",
               svc_battery_get_level(), svc_battery_get_force());
        printf("usage: lowbatt 0|1|2|auto\r\n");
        return 0;
    }
    if (strcmp(argv[1], "auto") == 0) {
        svc_battery_force_level(-1);
        printf("lowbatt -> auto\r\n");
        return 0;
    }
    {
        int lvl = atoi(argv[1]);
        if (lvl < 0) {
            lvl = 0;
        }
        if (lvl > 2) {
            lvl = 2;
        }
        svc_battery_force_level(lvl);
        printf("lowbatt -> force %d (applies within 3 s)\r\n", svc_battery_get_force());
    }
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_lowbatt, lowbatt, force low-battery level for test: lowbatt 0|1|2|auto);

/*
 * 面板电源开关（VCI_EN / IO29 → BV6810 PMIC）
 *
 * 用途：量"面板 PMIC 到底吃多少电"。
 *   panelpwr 0 → 彻底断面板电源（DISPOFF 之外的省电手段）
 *   panelpwr 1 → 恢复供电（但显示需要重启才会恢复，因为要重新 init）
 */
static int cmd_panelpwr(int argc, char **argv)
{
    int on;

    if (argc < 2) {
        printf("usage: panelpwr 0|1   (VCI_EN / IO29 -> panel PMIC)\r\n");
        return 0;
    }
    on = atoi(argv[1]) ? 1 : 0;
    st7802_vci_power(on);
    printf("panel PMIC (VCI_EN) -> %s\r\n", on ? "ON" : "OFF");
    if (!on) {
        printf("  NOTE: 显示要重启（重新 st7802_init）才能恢复\r\n");
    }
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_panelpwr, panelpwr, panel PMIC power via VCI_EN: panelpwr 0|1);

/* 倒出电压飞行记录仪（最新一条在前） */
static int cmd_battlog(int argc, char **argv)
{
    uint8_t head = 0;
    int i;

    if (argc > 1 && strcmp(argv[1], "clear") == 0) {
        uint8_t zero[2] = { 0, 0 };
        uint8_t h = 0;

        RTC_LOCK();
        for (i = 0; i < BATTLOG_N; i++) {
            (void)sd3010_ram_write((uint8_t)(BATTLOG_BASE + i * 2u), zero, 2);
        }
        (void)sd3010_ram_write(BATTLOG_HEAD, &h, 1);
        RTC_UNLOCK();
        s_battlog_next = 0;
        printf("battlog cleared\r\n");
        return 0;
    }

    RTC_LOCK();
    (void)sd3010_ram_read(BATTLOG_HEAD, &head, 1);
    RTC_UNLOCK();
    if (head > BATTLOG_N) {
        head = 0;
    }

    printf("[battlog] %d slots x %d s  head=%d\r\n", BATTLOG_N, BATTLOG_SEC, head);
    for (i = 0; i < BATTLOG_N; i++) {
        int idx = (head - 1 - i + BATTLOG_N * 2) % BATTLOG_N;
        uint8_t b[2] = { 0, 0 };
        int mv;

        RTC_LOCK();
        (void)sd3010_ram_read((uint8_t)(BATTLOG_BASE + idx * 2u), b, 2);
        RTC_UNLOCK();
        mv = (int)b[0] | ((int)b[1] << 8);
        if (mv < 2000 || mv > 5000) {
            continue;
        }
        printf("  -%3d min : %d mV\r\n", i * (BATTLOG_SEC / 60), mv);
    }
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_battlog, battlog, dump battery voltage log from RTC user RAM);

/*
 * alwayson [0|1]  ——  永久常亮（诊断用）
 *
 * 为什么要它（2026-10-16 晚）：息屏后 LVGL **完全停止渲染**，而查"整分花屏"
 * 必须盯着屏看几十秒到一分钟；`idle <ms>` 每次上电/复位都会被重置回 15000ms，
 * 于是"刚画上去的图案"经常在用户看之前就被息屏刷没了（本会话踩了两次）。
 * 打开后 power_tick() 直接短路，屏永不自动息。
 * 历史：查整分花屏期间曾开机默认常亮；现在**默认不常亮**
 * （见 lvgl_task 里的 power_set_always_on(0)），这条命令用来临时打开或确认状态。
 */
static int cmd_alwayson(int argc, char **argv)
{
    if (argc < 2) {
        printf("always-on = %s   (idle timeout %u ms, %u ms left)\r\n",
               power_get_always_on() ? "ON（屏永不自动息）" : "OFF（按 idle 计时）",
               (unsigned)power_get_idle_ms(), (unsigned)power_idle_left_ms());
        return 0;
    }
    power_set_always_on(atoi(argv[1]) ? 1 : 0);
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_alwayson, alwayson, keep the screen always on (diagnostics): alwayson [0|1]);

/* 唤醒模式：wakefull 0 = 靠面板 GRAM（快）/ 1 = 唤醒后整屏重画 */
static int cmd_wakefull(int argc, char **argv)
{
    if (argc < 2) {
        printf("wake mode = %s\r\n", power_get_wake_redraw() ? "full redraw" : "gram kept");
        return 0;
    }
    power_set_wake_redraw(atoi(argv[1]) ? 1 : 0);
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_wakefull, wakefull, wake mode: 0=gram kept(fast) 1=full redraw);

/* 触摸诊断 */
static int cmd_tpstat(int argc, char **argv)
{
    uint16_t x, y;
    int pressed;
    uint32_t taps;
    tp_stats_t st;
    int i;

    (void)argc;
    (void)argv;

    tp_lvgl_get_state(&x, &y, &pressed, &taps);
    printf("lvgl indev: %s  last=(%u,%u)  taps=%lu\r\n",
           pressed ? "PRESSED" : "released", x, y, (unsigned long)taps);
    printf("TP_INT(IO%d) = %d (0 = touched)\r\n", TP_INT_PIN, tp_int_level());

    tp_get_stats(&st);
    printf("raw: reads=%lu hit=%lu bad_status=%lu bad_ack=%lu nopoint=%lu i2c_err=%lu\r\n",
           (unsigned long)st.reads, (unsigned long)st.hit,
           (unsigned long)st.bad_status, (unsigned long)st.bad_ack,
           (unsigned long)st.nopoint, (unsigned long)st.i2c_err);
    printf("INT gate %s: 省掉 %lu 次 I2C 读（%lu 实读 / %lu 应读 = %.1f%% 流量）\r\n",
           tp_lvgl_get_int_gate() ? "ON" : "OFF",
           (unsigned long)tp_lvgl_get_gated(),
           (unsigned long)st.reads,
           (unsigned long)(st.reads + tp_lvgl_get_gated()),
           (st.reads + tp_lvgl_get_gated())
               ? 100.0 * st.reads / (st.reads + tp_lvgl_get_gated()) : 0.0);

    /*
     * 报文分布：跟手滑动能多细，取决于"滑动期间有没有稳定的 status=6 报点"。
     * status 直方图是唯一能回答这个问题的东西 —— 只认 0x06 是抓原始报文定的，
     * 但没人量过它在一整笔滑动里出现几次。数不准就先别调手势参数。
     */
    printf("pts_hist  (点数): ");
    for (i = 0; i < 8; i++) printf("%lu ", (unsigned long)st.pts_hist[i]);
    printf("  <- 0 = 没手指 / 1 = 单点\n");
    printf("status_hist(状态): ");
    for (i = 0; i < 16; i++) printf("%lu ", (unsigned long)st.status_hist[i]);
    printf("  <- 只认 6\n");
    printf("ack_hist  (ACK  ): ");
    for (i = 0; i < 16; i++) printf("%lu ", (unsigned long)st.ack_hist[i]);
    printf("\r\n");
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_tpstat, tpstat, dump touch + indev state);

#if PROTORIG_DEBUG_SHELL
/* INT 门控开关（省电）：tpgate 0|1 */
static int cmd_tpgate(int argc, char **argv)
{
    if (argc < 2) {
        printf("int gate = %s, 已省掉 %lu 次 I2C 读\r\n",
               tp_lvgl_get_int_gate() ? "ON" : "OFF",
               (unsigned long)tp_lvgl_get_gated());
        return 0;
    }
    tp_lvgl_set_int_gate(atoi(argv[1]));
    tp_reset_stats();
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_tpgate, tpgate, INT gate for touch I2C polling: tpgate 0|1);
#endif /* PROTORIG_DEBUG_SHELL */

/* 分页/手势层的 LVGL 状态诊断（原生页面；点了没反应时用它看是哪一环断的） */
static int cmd_uidiag(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    eez_ui_diag();
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_uidiag, uidiag, dump page/gesture state (page / labels / input));

/* 电池读数：batt */
static int cmd_batt(int argc, char **argv)
{
    int mv, pct, i;

    (void)argc;
    (void)argv;

    /* 连读 3 次看稳定性（分压 100k 阻抗高，单次可能有噪声） */
    for (i = 0; i < 3; i++) {
        mv = board_adc_battery_mv();
        pct = board_adc_battery_percent(mv);
        if (mv < 0) {
            printf("battery: 读取失败 (ADC)\r\n");
            return -1;
        }
        printf("battery[%d] = %d mV  (%d.%02d V)  %d%%\r\n",
               i, mv, mv / 1000, (mv % 1000) / 10, pct);
    }
    printf("CHRG(IO%d)=%d  STDBY(IO%d)=%d  ->  charging=%s full=%s usb=%s\r\n",
           PIN_CHRG, chrg_raw(PIN_CHRG), PIN_STDBY, chrg_raw(PIN_STDBY),
           chrg_charging() ? "YES" : "no",
           chrg_full() ? "YES" : "no",
           chrg_usb_present() ? "YES" : "no");
    printf("mcu_temp = %.1f C\r\n", (double)board_adc_mcu_temp());
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_batt, batt, read battery mV/percent + charging + MCU temp);

/* 充电状态：V1.6 的 CHRG(IO3) / STDBY(IO21) 真值表直读 */
static int cmd_chrg(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("CHRG(IO%d)=%d  STDBY(IO%d)=%d  ->  charging=%s full=%s usb=%s\r\n",
           PIN_CHRG, chrg_raw(PIN_CHRG), PIN_STDBY, chrg_raw(PIN_STDBY),
           chrg_charging() ? "YES" : "no",
           chrg_full() ? "YES" : "no",
           chrg_usb_present() ? "YES" : "no");
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_chrg, chrg, charging state: CHRG=IO3 / STDBY=IO21);

/*
 * 振动马达（IO30 -> Q2 低边驱动 U4 LCM0720A3134F）
 *
 *   motor                 看当前状态
 *   motor 0..100          设占空比（0 = 停）；这是 PWM 通路
 *   motor gpio 0|1        纯 GPIO 开关，用来单独验证"线/管/MOS 通不通"
 *   motor pulse <pct> <ms> 阻塞脉冲
 *   motor hz [n]          查/改 PWM 频率（默认 20000）
 *   motor read            自检：CFG 寄存器 + 三种状态下的脚电平（PWM/GPIO）
 *   motor pad             只打印 IO30 的输入位
 *   motor blink [hz] [ms] 慢速方波，给万用表/示波器看
 *   motor ch [0..3]       换 PWM 通道（手册说 IO30=PWM2，硅片映射现场验证）
 */
static int cmd_motor(int argc, char **argv)
{
    if (argc < 2) {
        printf("motor: pct=%d  pwm=%u Hz  ch=%d  pad=%d  (IO30)\r\n",
               motor_pct(), (unsigned)motor_pwm_hz(), motor_channel(), motor_pad_read());
        printf("  usage: motor <0..100> | gpio 0|1 | pulse <pct> <ms> | hz [n] | ch [0..3] | read | pad | blink [hz] [ms]\r\n");
        return 0;
    }
    if (strcmp(argv[1], "gpio") == 0) {
        motor_gpio((argc > 2) ? atoi(argv[2]) : 1);
        return 0;
    }
    if (strcmp(argv[1], "pulse") == 0 && argc > 3) {
        int pct = atoi(argv[2]);
        int ms = atoi(argv[3]);
        printf("[motor] pulse %d%% for %d ms\r\n", pct, ms);
        motor_pulse(pct, (uint32_t)ms);
        return 0;
    }
    if (strcmp(argv[1], "hz") == 0) {
        if (argc > 2) {
            printf("[motor] pwm -> %u Hz\r\n", (unsigned)motor_set_pwm_hz((uint32_t)atoi(argv[2])));
        } else {
            printf("[motor] pwm = %u Hz\r\n", (unsigned)motor_pwm_hz());
        }
        return 0;
    }
    if (strcmp(argv[1], "ch") == 0) {
        if (argc > 2) {
            motor_set_channel(atoi(argv[2]));
        } else {
            printf("[motor] ch = %d\r\n", motor_channel());
        }
        return 0;
    }
    if (strcmp(argv[1], "read") == 0) {
        motor_diag();
        return 0;
    }
    if (strcmp(argv[1], "padtest") == 0) {
        motor_padtest();
        return 0;
    }
    if (strcmp(argv[1], "padtest2") == 0) {
        motor_padtest2();
        return 0;
    }
    if (strcmp(argv[1], "usepin") == 0) {
        if (argc > 2) {
            motor_set_pin(atoi(argv[2]));
        } else {
            printf("[motor] pin = IO%d\r\n", motor_pin());
        }
        return 0;
    }
    if (strcmp(argv[1], "regs") == 0) {
        motor_dump_regs();
        return 0;
    }
    if (strcmp(argv[1], "pad") == 0) {
        printf("[motor] pad I = %d\r\n", motor_pad_read());
        return 0;
    }
    if (strcmp(argv[1], "blink") == 0) {
        uint32_t hz = (argc > 2) ? (uint32_t)atoi(argv[2]) : 2;
        uint32_t ms = (argc > 3) ? (uint32_t)atoi(argv[3]) : 10000;
        motor_blink(hz, ms);
        return 0;
    }
    motor_set(atoi(argv[1]));
    printf("[motor] pct = %d\r\n", motor_pct());
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_motor, motor, vibration motor on IO30: motor <0..100>|gpio|pulse|hz|ch|read|pad|blink);

#if PROTORIG_DEBUG_SHELL
/* 运行时切共享 I2C0 速率：i2cfreq 400000 / 100000 */
static int cmd_i2cfreq(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: i2cfreq <hz>   (默认 400000；SD3010 失败自动降级 100k，可切 100000 做对照)\r\n");
        return 0;
    }
    return tp_set_i2c_freq((uint32_t)atoi(argv[1]));
}
SHELL_CMD_EXPORT_ALIAS(cmd_i2cfreq, i2cfreq, set shared I2C0 clock: i2cfreq 400000);
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
/* ================================================================== *
 *  PDS（power down sleep）实验命令
 *
 *  目的：在**不做整套集成**的前提下，先把三件事测出来：
 *    1. 各 sleep level 下电流到底多少（要你的万用表）
 *    2. 唤醒之后是"继续执行"还是"复位重启"
 *    3. 唤醒延迟能不能接受
 *
 *  SDK 给出的分层表（bl616_pm.c 注释）：
 *
 *      PD_CORE  MISC_DIG  MISC_ANA  PD_CORE_CPU  PD_BZ  PD_USB
 *  PDS0   ON       ON        ON         ON         ON     ON
 *  PDS1   ON       ON        ON         ON         ON     OFF   ← CPU 不掉电，WFI 后继续
 *  PDS2   ON       ON        ON         ON         OFF    ON
 *  PDS3   ON       ON        ON         ON         OFF    OFF   ← CPU 不掉电，WFI 后继续
 *  PDS4   ON       ON        ON         OFF        ON     ON
 *  PDS5   ON       ON        ON         OFF        ON     OFF
 *  PDS6   ON       ON        ON         OFF        OFF    ON
 *  PDS7   ON       ON        ON         OFF        OFF    OFF   ← CPU 掉电，醒来=复位
 *  PDS15                                                          ← 最深，醒来=复位
 *
 *  ⚠️ 所有 PDS 都会关掉 PD_USB —— **插着 USB 时会掉串口**。
 *     建议在接了电池的情况下用；掉了就断电重上电，不会变砖。
 * ================================================================== */
static int cmd_pdstest(int argc, char **argv)
{
    int level = (argc >= 2) ? atoi(argv[1]) : 1;
    uint32_t ms = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 100;
    uint32_t ticks, t0;

    if (level != 1 && level != 2 && level != 3 && level != 7 && level != 15) {
        printf("level 只能是 1/2/3/7/15（SDK 的 enum 就这几个）\r\n");
        return -1;
    }
    if (ms < 2) {
        ms = 2;
    }
    ticks = (uint32_t)((uint64_t)32768 * ms / 1000);
    if (ticks <= PDS_WARMUP_LATENCY_CNT) {
        ticks = PDS_WARMUP_LATENCY_CNT + 1;     /* 太短会被 SDK 直接 return */
    }

    printf("\r\n[pds] level=%d  定时 %u ms (%lu ticks)\r\n",
           level, ms, (unsigned long)ticks);
    if (level <= 3) {
        printf("[pds] 这个级别 CPU 不掉电，醒来应该会**继续往下跑**\r\n");
    } else {
        printf("[pds] 这个级别 CPU 掉电，醒来是**复位重启**（你会看到重新开机）\r\n");
    }
    printf("[pds] ⚠️ 会关掉 USB/串口。醒了会有下文；没下文就是复位了。\r\n");
    printf("[pds] 现在开始量电流 —— 2 秒后进入睡眠\r\n");
    bflb_mtimer_delay_ms(2000);

    t0 = (uint32_t)bflb_mtimer_get_time_ms();

    pm_pds_mask_all_wakeup_src();
    if (level == 1) {
        pm_pds_irq_register();          /* PDS1 要求先注册中断 */
    }
    pm_pds_mode_enter((enum pm_pds_sleep_level)level, ticks);

    printf("[pds] ★ 醒来了 —— 睡了约 %lu ms。CPU 没掉电、继续执行 ✓\r\n",
           (unsigned long)((uint32_t)bflb_mtimer_get_time_ms() - t0));
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_pdstest, pdstest, PDS experiment: pdstest <1|2|3|7|15> <ms>);
#endif /* PROTORIG_DEBUG_SHELL */

/* 自动 PDS：pds | pds 0|1 | pds level <1|2|3|7|15> | pds now [ms] */
static int cmd_pds(int argc, char **argv)
{
    if (argc < 2) {
        printf("pds: auto=%s level=%d hbnmode=%s paneloff=%s tplp=%s usbreset=%s count=%lu last_wake=%d usb=%d touch=%d\r\n",
               s_pds_enabled ? "ON" : "OFF", power_pds_level(),
               s_pds_hbn ? "HBN" : "PDS",
               s_pds_panel_off ? "ON" : "OFF", s_tp_lp ? "ON" : "OFF",
               s_pds_usb_reset ? "ON" : "OFF",
               (unsigned long)power_pds_count(), power_pds_last_wake(),
               chrg_usb_present(), tp_int_asserted());
        return 0;
    }
    if (!strcmp(argv[1], "1") || !strcmp(argv[1], "on")) {
        s_pds_enabled = 1;
        printf("[pds] auto sleep ON (level %d)\r\n", power_pds_level());
    } else if (!strcmp(argv[1], "0") || !strcmp(argv[1], "off")) {
        s_pds_enabled = 0;
        printf("[pds] auto sleep OFF\r\n");
    } else if (!strcmp(argv[1], "level") && argc >= 3) {
        power_pds_set_level(atoi(argv[2]));
    } else if (!strcmp(argv[1], "panel") && argc >= 3) {
        s_pds_panel_off = atoi(argv[2]) ? 1 : 0;
        printf("[pds] panel VCI off in sleep = %s\r\n", s_pds_panel_off ? "ON" : "OFF");
    } else if (!strcmp(argv[1], "tplp") && argc >= 3) {
        s_tp_lp = atoi(argv[2]) ? 1 : 0;
        printf("[pds] touch low-power (gesture) = %s\r\n", s_tp_lp ? "ON" : "OFF");
    } else if (!strcmp(argv[1], "usbreset") && argc >= 3) {
        s_pds_usb_reset = atoi(argv[2]) ? 1 : 0;
        printf("[pds] USB reset on VBUS after PDS = %s\r\n", s_pds_usb_reset ? "ON" : "OFF");
    } else if (!strcmp(argv[1], "hbnmode") && argc >= 3) {
        s_pds_hbn = atoi(argv[2]) ? 1 : 0;
        printf("[pds] auto sleep mode = %s\r\n", s_pds_hbn ? "HBN" : "PDS15");
    } else if (!strcmp(argv[1], "shutdown")) {
        /*
         * 关机态只等 CHRG 下降沿。USB 插着时这个沿不会来（充电中已经是低，
         * 充满后为高但没有"下降"），睡下去就失联了。拒绝，而不是赌。
         */
        if (usb_present_any()) {
            printf("[pds] refuse shutdown: USB present, no CHRG falling edge to wake; unplug USB first\r\n");
            return -1;
        }
        printf("[pds] forced shutdown test (VBUS wake)...\r\n");
        (void)tp_set_mode(TP_ADDR_DEFAULT, TP_CMD_DEBUG_INFO);
        bflb_mtimer_delay_ms(10);
        (void)tp_set_mode(TP_ADDR_DEFAULT, TP_CMD_DEEP_SLEEP);
        bflb_mtimer_delay_ms(20);
        (void)sd3010_alarm_disable();
        (void)sd3010_int_clear();
        power_pds_shutdown();
    } else if (!strcmp(argv[1], "hbn")) {
        printf("[pds] HBN test: touch IO16 low-level wake...\r\n");
        (void)sd3010_alarm_disable();
        (void)sd3010_int_clear();
        power_hbn_enter();
    } else if (!strcmp(argv[1], "now")) {
        uint32_t ms = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 0;
        uint32_t ticks = (uint32_t)((uint64_t)32768 * ms / 1000);
        int src;

        if (chrg_usb_present()) {
            printf("[pds] WARN: USB 插着，睡下去串口会掉；5 秒后进入...\r\n");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        if (ticks > 0 && ticks <= PDS_WARMUP_LATENCY_CNT) {
            ticks = PDS_WARMUP_LATENCY_CNT + 1;
        }
        /* 2026-11-08：闹钟开着就保留 SD3010 报警，靠 IO17 低电平唤醒
         * （`alarm in 1` 之后 `pds now 0` 就是最省事的唤醒验证）。 */
        if (board_power_alarm_armed()) {
            printf("[pds] alarm armed: RTC %02u:%02u will wake via IO17\r\n",
                   (unsigned)svc_alarm_hour(), (unsigned)svc_alarm_minute());
        } else {
            (void)sd3010_alarm_disable();
            (void)sd3010_int_clear();
        }
        pds_log_bump(1);
        s_pds_log[6] = (uint8_t)power_pds_level();
        pds_log_save();
        printf("[pds] enter level=%d ticks=%lu\r\n",
               power_pds_level(), (unsigned long)ticks);
        src = power_pds_enter(ticks);
        pds_log_bump(src == 1 ? 2 : (src == 2 ? 3 : 4));
        s_pds_log[5] = (uint8_t)src;
        pds_log_save();
        printf("[pds] wake src=%d (1=touch 2=rtc 0=other)\r\n", src);
        if (s_pds_usb_reset && chrg_usb_present()) {
            usb_vbus_reboot();
        }
        if (src == 2) {
            svc_time_alarm_service();
        }
        if (src == 1) {
            s_screen_req = 1;      /* 亮屏只能由 LVGL 任务做 */
        }
        if (power_is_on()) {
            board_power_alarm_apply();   /* 按闹钟配置重设 RTC 报警 */
        }
    } else {
        printf("usage: pds | pds 0|1 | pds level <1|2|3|7|15> | pds hbnmode 0|1 | pds panel 0|1 | pds tplp 0|1 | pds usbreset 0|1 | pds now [ms] | pds shutdown | pds hbn\r\n");
    }
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_pds, pds, PDS sleep control: pds [0|1|level N|now ms]);

/* PDS 持久计数：pdslog | pdslog clear */
static int cmd_pdslog(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "clear")) {
        memset(s_pds_log, 0, sizeof(s_pds_log));
        s_pds_log[7] = PDSLOG_VALID;
        pds_log_save();
        printf("[pdslog] cleared\r\n");
        return 0;
    }
    printf("pdslog: boot=%u enter=%u wake_touch=%u wake_rtc=%u wake_other=%u last_src=%u last_level=%u\r\n",
           s_pds_log[0], s_pds_log[1], s_pds_log[2], s_pds_log[3],
           s_pds_log[4], s_pds_log[5], s_pds_log[6]);
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_pdslog, pdslog, PDS persistent counters: pdslog [clear]);

/* ================================================================== *
 *  CST9220 低功耗 / 工作模式 —— 现场验证命令
 *  依据：firmware/docs/cst9220_lowpower_investigation.md
 *        （厂商《触摸芯片移植手册 v3.2》§11 寄存器说明）
 *
 *  所有命令只改芯片 RAM 状态、不写 flash，tpwake 一定能救回来。
 * ================================================================== */

#if PROTORIG_DEBUG_SHELL
/* 读 0x0002：芯片会回显最后一条模式命令 —— 天然的生效自检寄存器 */
/* R3c_touch_dbg：任意 CST9220 寄存器/裸命令调试（只读/写芯片 RAM，不碰现有轮询） */
static int tpreg_hex(const char *s, uint32_t *out)
{
    unsigned long v;
    char *end;

    if (s == NULL || *s == '\0' || out == NULL) {
        return -1;
    }
    v = strtoul(s, &end, 16);
    if (*end != '\0' || v > 0xFFFFu) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static void tpreg_usage(void)
{
    printf("usage:\r\n");
    printf("  tpreg cmd <hex...>       raw write to 0x5A, e.g. tpreg cmd D1 06 01\r\n");
    printf("  tpreg r <addr> [len]    16-bit register read, e.g. tpreg r D106\r\n");
    printf("  tpreg w <addr> <val>    write one byte, e.g. tpreg w D106 01\r\n");
}

static int cmd_tpreg(int argc, char **argv)
{
    int r;

    if (argc < 2) {
        tpreg_usage();
        return -1;
    }

    if (strcmp(argv[1], "cmd") == 0) {
        uint8_t buf[16];
        int n = argc - 2;
        int i;

        if (n < 1 || n > (int)sizeof(buf)) {
            tpreg_usage();
            return -1;
        }
        for (i = 0; i < n; i++) {
            uint32_t v;
            if (tpreg_hex(argv[i + 2], &v) != 0 || v > 0xFFu) {
                printf("tpreg: bad byte '%s'\r\n", argv[i + 2]);
                return -1;
            }
            buf[i] = (uint8_t)v;
        }
        r = tp_raw_write(TP_ADDR_DEFAULT, buf, (uint32_t)n);
        printf("tpreg cmd -> %d (", r);
        for (i = 0; i < n; i++) {
            printf("%s%02X", i ? " " : "", buf[i]);
        }
        printf(")\r\n");
        return r < 0 ? -1 : 0;
    }

    if (strcmp(argv[1], "r") == 0) {
        uint32_t addr, len = 1;
        uint8_t buf[16];
        uint32_t i;

        if (argc < 3) {
            tpreg_usage();
            return -1;
        }
        if (tpreg_hex(argv[2], &addr) != 0) {
            printf("tpreg: bad addr '%s'\r\n", argv[2]);
            return -1;
        }
        if (argc >= 4 && (tpreg_hex(argv[3], &len) != 0 || len < 1 || len > sizeof(buf))) {
            printf("tpreg: bad len '%s'\r\n", argv[3]);
            return -1;
        }
        r = tp_read_reg16(TP_ADDR_DEFAULT, (uint16_t)addr, buf, len);
        if (r < 0) {
            printf("tpreg r %04X -> %d (I2C)\r\n", (unsigned)addr, r);
            return -1;
        }
        printf("tpreg r %04X len=%u ->", (unsigned)addr, (unsigned)len);
        for (i = 0; i < len; i++) {
            printf(" %02X", buf[i]);
        }
        printf("\r\n");
        return 0;
    }

    if (strcmp(argv[1], "w") == 0) {
        uint32_t addr, val;
        uint8_t v;

        if (argc != 4) {
            tpreg_usage();
            return -1;
        }
        if (tpreg_hex(argv[2], &addr) != 0 || tpreg_hex(argv[3], &val) != 0 || val > 0xFFu) {
            printf("tpreg: bad addr/val\r\n");
            return -1;
        }
        v = (uint8_t)val;
        r = tp_write_reg16(TP_ADDR_DEFAULT, (uint16_t)addr, &v, 1);
        printf("tpreg w %04X = %02X -> %d\r\n", (unsigned)addr, v, r);
        return r < 0 ? -1 : 0;
    }

    tpreg_usage();
    return -1;
}
SHELL_CMD_EXPORT_ALIAS(cmd_tpreg, tpreg, CST9220 raw register debug: cmd/r/w);


static int cmd_tpmode(int argc, char **argv)
{
    uint8_t ms[2] = { 0, 0 };

    (void)argc;
    (void)argv;

    if (tp_read_mode_status(TP_ADDR_DEFAULT, ms) < 0) {
        printf("读 0x0002 失败（I2C）\r\n");
        return -1;
    }
    printf("0x0002 模式回显 = %02X %02X   低字节 = 最后一条命令码\r\n", ms[0], ms[1]);
    printf("  01=debug_info  05=deep_sleep  08=debug_pts  09=NORMAL(默认)\r\n");
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_tpmode, tpmode, read CST9220 mode echo (reg 0x0002));
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
/* 写回 normal 模式。
 * 厂商手册 p16 明确要求：「读完版本信息必须退回 normal 模式」，
 * 而我们的 tp.c、厂商 ESP-IDF 驱动、Waveshare、ESPHome 全都没写这条。 */
static int cmd_tpnormal(int argc, char **argv)
{
    int r;

    (void)argc;
    (void)argv;

    r = tp_set_mode(TP_ADDR_DEFAULT, TP_CMD_NORMAL);
    bflb_mtimer_delay_ms(10);
    printf("写 D1 09 (NORMAL) -> %d\r\n", r);
    cmd_tpmode(0, NULL);
    return r;
}
SHELL_CMD_EXPORT_ALIAS(cmd_tpnormal, tpnormal, write D1 09 = normal mode);
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
/* 手势检测模式（厂商唯一实际在跑的灭屏低功耗扫描路径）
 *   D0 4C 80 进 / D0 4C 00 出，写完读 0xD04C 的 bit7 自检 */
static int cmd_tpgesture(int argc, char **argv)
{
    int on = (argc >= 2) ? (atoi(argv[1]) ? 1 : 0) : 1;
    int v = tp_gesture(TP_ADDR_DEFAULT, on);

    if (v < 0) {
        printf("手势命令 I2C 失败\r\n");
        return -1;
    }
    printf("写 D0 4C %02X  ->  回读 0xD04C = 0x%02X  bit7=%d  %s\r\n",
           on ? 0x80 : 0x00, v, (v >> 7) & 1,
           (((v >> 7) & 1) == on) ? "自检 OK" : "⚠️ 自检不符");
    if (on) {
        printf("  手势码 = 0xD04C & 0x7F ；参考: O=5 Z=13 C=6 上=2 下=4 左=3 右=1 双击=32 单击=31\r\n");
        printf("  ⚠️ 进手势模式后可能不再报坐标（固件要加状态机），退出用 tpgesture 0\r\n");
    }
    return v;
}
SHELL_CMD_EXPORT_ALIAS(cmd_tpgesture, tpgesture, gesture mode on/off: tpgesture 0|1);
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
/* 接近感应（报告里的"序列 D"，可能是"靠近亮屏"的正解） */
static int cmd_tpprox(int argc, char **argv)
{
    int on = (argc >= 2) ? (atoi(argv[1]) ? 1 : 0) : 1;
    int v = tp_proximity(TP_ADDR_DEFAULT, on);

    if (v < 0) {
        printf("接近感应命令 I2C 失败\r\n");
        return -1;
    }
    printf("写 D0 4B %02X  ->  回读 0xD04B = 0x%02X  bit7=%d  &&0x7F=%d (%s)\r\n",
           on ? 0x80 : 0x00, v, (v >> 7) & 1, v & 0x7F,
           (v & 0x7F) == 0 ? "远" : "近");
    return v;
}
SHELL_CMD_EXPORT_ALIAS(cmd_tpprox, tpprox, proximity on/off: tpprox 0|1);
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
/* 深度睡眠 2µA。⚠️ 触摸叫不醒，只能用 RST */
static int cmd_tpsleep(int argc, char **argv)
{
    uint8_t ms[2] = { 0, 0 };
    int r, rr;

    (void)argc;
    (void)argv;

    printf("⚠️  D1 05 深度睡眠约 2µA，但**触摸唤不醒**，只能 tpwake(RST) 叫醒\r\n");

    r = tp_set_mode(TP_ADDR_DEFAULT, TP_CMD_DEBUG_INFO);
    printf("   D1 01 (debug_info) -> %d\r\n", r);
    bflb_mtimer_delay_ms(10);

    r = tp_set_mode(TP_ADDR_DEFAULT, TP_CMD_DEEP_SLEEP);
    printf("   D1 05 (deep sleep) -> %d\r\n", r);
    bflb_mtimer_delay_ms(20);

    rr = tp_read_mode_status(TP_ADDR_DEFAULT, ms);
    printf("   睡后读 0x0002 -> %d（失败/NACK 才是真的睡着了，这是正确现象）\r\n", rr);
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_tpsleep, tpsleep, deep sleep 2uA (only hardware reset wakes it));
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
/* 硬复位唤醒 + 重新初始化 */
static int cmd_tpwake(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    tp_hw_reset();
    bflb_mtimer_delay_ms(160);
    printf("已硬复位触摸芯片（RST 拉低 10ms），重新读信息：\r\n");
    tp_cst9220_info(TP_ADDR_DEFAULT);
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_tpwake, tpwake, hardware reset (IO22) to wake the touch chip);
#endif /* PROTORIG_DEBUG_SHELL */

/* 对时：settime <yy> <mm> <dd> <hh> <mi> <ss>（年只写后两位） */
static int cmd_settime(int argc, char **argv)
{
    sd3010_time_t tm;
    int i;

    if (argc < 7) {
        printf("usage: settime <yy> <mm> <dd> <hh> <mi> <ss>\r\n");
        return -1;
    }
    memset(&tm, 0, sizeof(tm));
    tm.year = (uint8_t)atoi(argv[1]);
    tm.month = (uint8_t)atoi(argv[2]);
    tm.day = (uint8_t)atoi(argv[3]);
    tm.hour = (uint8_t)atoi(argv[4]);
    tm.min = (uint8_t)atoi(argv[5]);
    tm.sec = (uint8_t)atoi(argv[6]);
    tm.week = sd3010_calc_week((uint16_t)(2000 + tm.year), tm.month, tm.day);

    /* R3c：统一对时路径（svc_time_set 内部锁/清 OSF/写魔数/app_state/事件） */
    i = svc_time_set(&tm);

    /* 回读自检：写进去的和读回来的对不上，说明 RTC 没在跑或者写保护没解开 */
    if (svc_time_get(&tm) == 0) {
        printf("rtc now = %04d-%02d-%02d %02d:%02d:%02d week=%d\r\n",
               2000 + tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec, tm.week);
    } else {
        printf("rtc read-back failed\r\n");
    }
    return i;
}
SHELL_CMD_EXPORT_ALIAS(cmd_settime, settime, set RTC: settime yy mm dd hh mi ss);

/* RTC 报警测试：rtcalarm <0..59> 每分钟在指定秒触发；rtcalarm off 关闭 */
static int cmd_rtcalarm(int argc, char **argv)
{
    uint8_t c1 = 0, c2 = 0, aen = 0, asec = 0;
    int sec, ret;

    if (argc < 2) {
        sd3010_int_get(&c1, &c2, &aen, &asec);
        printf("alarm: ALM_EN=0x%02X ALM_SEC=%d  CTR1=0x%02X CTR2=0x%02X  INT(IO%d)=%d\r\n",
               aen, asec, c1, c2, SVC_TIME_RTC_INT_PIN, svc_time_int_level());
        return 0;
    }
    if (argv[1][0] == 'o' && argv[1][1] == 'f') {
        ret = sd3010_alarm_disable();
        printf("alarm off ret=%d\r\n", ret);
        return ret;
    }
    sec = atoi(argv[1]);
    if (sec < 0 || sec > 59) {
        printf("usage: rtcalarm <0..59> | off\r\n");
        return -1;
    }
    ret = sd3010_alarm_set_sec((uint8_t)sec);
    printf("alarm every minute at sec=%d ret=%d  INT(IO%d)=%d\r\n",
           sec, ret, SVC_TIME_RTC_INT_PIN, svc_time_int_level());
    return ret;
}
SHELL_CMD_EXPORT_ALIAS(cmd_rtcalarm, rtcalarm, RTC minute alarm: rtcalarm 0..59|off);

/* RTC 状态 */
static int cmd_rtcstat(int argc, char **argv)
{
    uint8_t c1 = 0, c5 = 0, c2 = 0, aen = 0, asec = 0;
    sd3010_time_t tm;
    int r;

    (void)argc;
    (void)argv;

    RTC_LOCK();
    sd3010_get_status(&c1, &c5);
    r = sd3010_read(&tm);
    (void)sd3010_int_get(&c1, &c2, &aen, &asec);
    RTC_UNLOCK();

    printf("sd3010 read=%d  CTR1=0x%02X  CTR5=0x%02X  CTR2=0x%02X\r\n", r, c1, c5, c2);
    {
        uint32_t irq_n = 0, svc_n = 0;
        svc_time_get_irq_stats(&irq_n, &svc_n);
        printf("INT(IO%d)=%d  irq=%lu serviced=%lu  ALM_EN=0x%02X ALM_SEC=%d\r\n",
               SVC_TIME_RTC_INT_PIN, svc_time_int_level(),
               (unsigned long)irq_n, (unsigned long)svc_n,
               aen, asec);
    }
    if (r == 0) {
        printf("RTC = %04d-%02d-%02d week=%d %02d:%02d:%02d\r\n",
               2000 + tm.year, tm.month, tm.day, tm.week,
               tm.hour, tm.min, tm.sec);
    }
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_rtcstat, rtcstat, dump SD3010 RTC status);

#if PROTORIG_DEBUG_SHELL
static int cmd_pxorder(int argc, char **argv)
{
    if (argc < 2) {
        printf("pxorder = %d\r\n", st7802_get_px_swap());
        return 0;
    }
    LCD_LOCK();
    st7802_set_px_swap(atoi(argv[1]) ? 1 : 0);
    LCD_UNLOCK();
    printf("pxorder = %d (再刷新一屏看效果)\r\n", st7802_get_px_swap());
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_pxorder, pxorder, RGB565 half-word order 0|1);
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
static int cmd_wrclk(int argc, char **argv)
{
    if (argc < 2) {
        printf("QSPI write clk = %u Hz\r\n", (unsigned)st7802_get_write_hz());
        return 0;
    }
    LCD_LOCK();
    st7802_set_write_hz((uint32_t)atoi(argv[1]) * 1000000UL);
    LCD_UNLOCK();
    printf("QSPI write clk = %u Hz\r\n", (unsigned)st7802_get_write_hz());
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_wrclk, wrclk, get/set QSPI write clock MHz);
#endif /* PROTORIG_DEBUG_SHELL */

/* TE 同步开关：te 0|1（现场 A/B 撕裂用） */
static int cmd_te(int argc, char **argv)
{
    if (argc < 2) {
        printf("te sync = %s   te_ok=%lu te_timeout=%lu   IO28 level=%d\r\n",
               s_te_sync ? "ON" : "OFF",
               (unsigned long)s_te_ok, (unsigned long)s_te_to, st7802_te_level());
        printf("  ON  = 每帧等一个 TE 沿再送屏（消撕裂，代价是最多等一帧）\r\n");
        printf("  如果 te_timeout 一直涨 -> TE 没接上/极性不对，这时 ON 等于白等\r\n");
        return 0;
    }
    s_te_sync = atoi(argv[1]) ? 1 : 0;
    s_te_ok = s_te_to = 0;
    printf("te sync = %s\r\n", s_te_sync ? "ON" : "OFF");
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_te, te, TE (tearing) sync: te 0|1);

#if PROTORIG_DEBUG_SHELL
/*
 * 触摸报点接受掩码：tpaccept [mask]
 *   status=6 = 新坐标；status=0 = 不可当成新坐标的回显
 * 默认 0x40（只认 6）；0x41 实测会误判方向，不作为修复。
 */
static int cmd_tptrace(int argc, char **argv)
{
    int req;
    if(argc == 2 && strcmp(argv[1], "start") == 0) req = 1;
    else if(argc == 2 && strcmp(argv[1], "dump") == 0) req = 2;
    else { printf("usage: tptrace start|dump\r\n"); return -1; }
    taskENTER_CRITICAL();
    s_trace_req = req;
    taskEXIT_CRITICAL();
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_tptrace, tptrace, capture production touch packets: start|dump);
#endif /* PROTORIG_DEBUG_SHELL */

#if PROTORIG_DEBUG_SHELL
static int cmd_tpaccept(int argc, char **argv)
{
    if (argc < 2) {
        printf("accept_status_mask = 0x%02lX\r\n", (unsigned long)tp_get_accept_status());
        printf("  0x40 = default, status 6 only; 0x41 = failed experiment, includes stale status 0\r\n");
        return 0;
    }
    tp_set_accept_status((uint32_t)strtol(argv[1], NULL, 0));
    tp_reset_stats();
    printf("accept_status_mask = 0x%02lX (stats 已清零，划几下手再看 tpstat)\r\n",
           (unsigned long)tp_get_accept_status());
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_tpaccept, tpaccept, which touch status counts as a point: tpaccept [0x41|0x40]);
#endif /* PROTORIG_DEBUG_SHELL */



/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/* R4-usbdelay: SDK board_init() sees this before main() runs. 1 = skip early
 * USB console init; BLE callback re-inits it after RF/PLL final state. */
int protorig_usb_console_delay = 1;

int main(void)
{
    board_init();

    s_lcd_mtx = xSemaphoreCreateMutex();

    xTaskCreate(lvgl_task, "lvgl", 4096, NULL, configMAX_PRIORITIES - 3, NULL);

    vTaskStartScheduler();

    for (;;) {
    }
}
