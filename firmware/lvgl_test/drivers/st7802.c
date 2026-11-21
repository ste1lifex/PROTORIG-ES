/**
 * @file  st7802.c
 * @brief ST7802 QSPI 驱动（直接用 BL616 lhal 的 DBI，不经过 SDK 的 LCD 框架）
 *
 * 为什么不用 SDK 的 bl_mipi_dbi.c：
 *   SDK 的 QSPI 路径把 addr 相位固定成 4 线（那是 ST77926 的时序），
 *   而 ST7802 手册 8.4 节要求 instruction 和 AD[23:0] 都走 1 线，
 *   只有像素数据可以走 4 线（A1D4）。读扫描实测也证实：
 *   CPOL0/CPHA1 + addr 1 线 才能读通。
 *
 * 手册时序（7.5.3 MSPI Interface Characteristics）：
 *   写：Serial clock cycle min 20ns  -> 上限 50MHz
 *   读：Serial clock cycle min 300ns -> 上限只有约 3.3MHz
 *   所以读写用两个不同时钟。
 *
 * 本文件有两部分：
 *   1) 与硬件无关的纯算法（CRC32、pxpat 编码、发送记录/取证状态机）；
 *   2) 依赖 BL616 DBI 的驱动本体。
 * 整份文件都能在主机上编译（-DST7802_HOST_VERIFY），见
 * tools/host/st7802_verify.c —— 后者用桩替换 DBI 寄存器读写，
 * 在同一份源码上跑编码/取证逻辑自检。固件构建不带这个宏。
 */
#include "st7802.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 两个 stub 点的前置声明：真机在下面定义，主机在文件末尾定义           */
/* ------------------------------------------------------------------ */
uint32_t st7802_hw_read(uint8_t cmd, uint8_t *buf, uint32_t len, uint8_t chunk);
static void st7802_push_pixels565(uint32_t npix, const uint16_t *buf, uint16_t solid);

/*
 * 565 的 32 位字里，两个像素谁在高半字。
 *
 * ★ 2026-10-11 晚 **板上实测**的结论（请勿再翻）：
 *     0（默认）= 先画的像素放**低**半字 -> word = (b << 16) | a
 *     1         = 反过来
 *
 * 曾经有人（一个子代理）据 SDK 里 NRGB_8888 路径的字节序，推断"先画的
 * 像素应该在高半字"，把这里翻成 (a << 16) | b —— **上板后整个表盘变成
 * 严重锯齿/重影，比原来更糟**，当场翻回。经验证据 > 跨像素格式类推。
 *
 * 另：这一条**不是**"细笔画全花"那个 bug 的原因；那条截至 2026-11-15 仍未定位，
 * 历史记录见 local/docs/history/bug_thin_stroke_smearing.md（本地文档，不发布）。
 *
 * shell 里 `pxorder 0|1` 可以现场翻，不用重新烧板子。
 */
static uint8_t s_px_swap = 0;

void st7802_set_px_swap(int swap)
{
    s_px_swap = swap ? 1 : 0;
}

int st7802_get_px_swap(void)
{
    return s_px_swap;
}

#ifndef ST7802_HOST_VERIFY

#include "bflb_gpio.h"
#include "bflb_dbi.h"
#include "bflb_dma.h" /* DMA_ADDR_DBI_TDR */
#include "bflb_mtimer.h"

/* ---- 引脚 ---- */
#define PIN_LCD_CLK 10
#define PIN_LCD_CS  11
#define PIN_LCD_D0  12
#define PIN_LCD_D1  13
#define PIN_LCD_D2  14
#define PIN_LCD_D3  15
#define PIN_LCD_RST 27
#define PIN_LCD_TE  28
#define PIN_VCI_EN  29

/* ---- QSPI 指令（ST7802 手册 8.4）---- */
#define QSPI_OP_CMD   0x02 /* A1D1：命令/参数（地址 1 线，数据 1 线） */
#define QSPI_OP_COLOR 0x32 /* A1D4：地址 1 线，像素数据 4 线 */
#define QSPI_OP_READ  0x03 /* 读 */
#define QSPI_ADDR_LEN 3

static struct bflb_device_s *dbi_dev = NULL;
static struct bflb_device_s *gpio_dev = NULL;

static uint32_t cur_hz = 0;
static uint8_t cur_clk = 0;
static uint8_t cur_addr = 0;
static uint8_t cur_data = 0;
static int dbi_ready = 0;

/* 实际使用的读写时钟，可以在 shell 里改，方便现场扫频 */
static uint32_t rd_hz = ST7802_READ_HZ;
static uint32_t wr_hz = ST7802_WRITE_HZ;

/* 当前 DBI/面板像素格式：0=RGB888（上电默认），1=RGB565。
 * 主程序在 SLPOUT 后设置 COLMOD=0x55 并回读确认；早期在 SLPOUT 前
 * 写入不生效的记录已过期，不能据此判定当前 565 路径不可用。 */
static uint8_t s_fmt565 = 0;

void st7802_set_read_hz(uint32_t hz)
{
    rd_hz = hz ? hz : ST7802_READ_HZ;
    dbi_ready = 0;
}

void st7802_set_write_hz(uint32_t hz)
{
    wr_hz = hz ? hz : ST7802_WRITE_HZ;
    dbi_ready = 0;
}

uint32_t st7802_get_read_hz(void)
{
    return rd_hz;
}

uint32_t st7802_get_write_hz(void)
{
    return wr_hz;
}

/* 像素相位：默认 A1D4 */
static uint8_t pix_op = QSPI_OP_COLOR; /* 0x32 */
static uint8_t pix_wire = DBI_QSPI_WIRE_NUM_4;

void st7802_set_pixel_mode(int mode)
{
    if (mode == 0) {
        pix_op = QSPI_OP_COLOR;
        pix_wire = DBI_QSPI_WIRE_NUM_4;
    } else {
        pix_op = QSPI_OP_CMD;
        pix_wire = DBI_QSPI_WIRE_NUM_1;
    }
}

int st7802_get_pixel_mode(void)
{
    return (pix_op == QSPI_OP_COLOR) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* 引脚 + DBI 配置                                                     */
/* ------------------------------------------------------------------ */

static void st7802_pins_init(void)
{
    gpio_dev = bflb_device_get_by_name("gpio");

    bflb_gpio_init(gpio_dev, PIN_LCD_CLK, GPIO_FUNC_DBI_QSPI | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(gpio_dev, PIN_LCD_CS, GPIO_FUNC_DBI_QSPI | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(gpio_dev, PIN_LCD_D0, GPIO_FUNC_DBI_QSPI | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(gpio_dev, PIN_LCD_D1, GPIO_FUNC_DBI_QSPI | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(gpio_dev, PIN_LCD_D2, GPIO_FUNC_DBI_QSPI | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(gpio_dev, PIN_LCD_D3, GPIO_FUNC_DBI_QSPI | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);

    /* VCI_EN 让 PMIC(BV6810) 上电，必须在复位之前拉高 */
    bflb_gpio_init(gpio_dev, PIN_VCI_EN, GPIO_OUTPUT | GPIO_PULLDOWN | GPIO_DRV_1);

    /* RST 先保持低 */
    bflb_gpio_init(gpio_dev, PIN_LCD_RST, GPIO_OUTPUT | GPIO_PULLUP | GPIO_DRV_1);
    bflb_gpio_reset(gpio_dev, PIN_LCD_RST);

    /* TE（IO28）：面板每帧给一个跳变沿。init 序列里已经发过 TEON(0x35)=0，
     * 这里只要把它当输入读 —— 不接的话也就永远读不到沿，靠超时兜底。 */
    bflb_gpio_init(gpio_dev, PIN_LCD_TE,
                   GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
}

/*
 * 面板电源（VCI_EN = IO29 → BV6810 PMIC）
 *
 * 2026-10-18 查续航时发现：VCI_EN 在 st7802_pins_init() 里被拉高之后
 * **再也没有被碰过**。息屏只发 DISPOFF（0x28，停止扫描），
 * 但面板的电源 IC(BV6810) 还在跑升压 —— AMOLED 的 PMIC 在 DISPOFF 下
 * 吃掉十几~几十 mA 是很常见的。
 *
 * 注意：这个接口只负责"切电"。关掉以后再打开，面板必须重新走一遍
 * st7802_init()（复位 + 初始化序列）才能恢复显示。
 */
void st7802_vci_power(int on)
{
    if (on) {
        bflb_gpio_set(gpio_dev, PIN_VCI_EN);
    } else {
        bflb_gpio_reset(gpio_dev, PIN_VCI_EN);
    }
}

/* ------------------------------------------------------------------ */
/* 面板深睡实验（W1，2026-10-28）                                       */
/*                                                                    */
/* 给用户在真板子上量电流用的分级命令：                                 */
/*   1  WRDISBV=0 + DISPOFF（= 现有息屏行为）                          */
/*   2  + SLPIN，等 120 ms                                             */
/*   3  + DSTBON + QSPI 六线拉低/高阻 + VCI_EN 低（面板 PMIC 断电）     */
/*   0  唤醒：VCI 高 -> 5 ms -> RST 低脉冲 -> SLPOUT -> 120 ms -> init */
/*                                                                    */
/* 只由 shell panelsleep 调用；息屏仍走 board_power.c 的 DISPOFF 路径   */
/* 所以这个命令不会改变现有行为。                                       */
/* ------------------------------------------------------------------ */

static int s_panel_sleep_level;

static const uint8_t panel_qspi_pins[] = {
    PIN_LCD_CLK, PIN_LCD_CS, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
};

int st7802_panel_sleep_level(void)
{
    return s_panel_sleep_level;
}

/* 六根 QSPI 线改成 GPIO 输入 + 弱下拉：面板断电后总线上不留浮空电平，
 * 也不会从 MCU 侧往面板灌电流（22Ω 串阻，弱下拉电流可忽略）。 */
static void panel_qspi_pins_to_gpio(void)
{
    for (uint32_t i = 0; i < sizeof(panel_qspi_pins); i++) {
        bflb_gpio_init(gpio_dev, panel_qspi_pins[i],
                       GPIO_INPUT | GPIO_PULLDOWN | GPIO_DRV_1);
    }
}

/* 唤醒前把六根线还给 DBI 的 QSPI 复用功能（和 st7802_pins_init 一致） */
static void panel_qspi_pins_to_dbi(void)
{
    for (uint32_t i = 0; i < sizeof(panel_qspi_pins); i++) {
        bflb_gpio_init(gpio_dev, panel_qspi_pins[i],
                       GPIO_FUNC_DBI_QSPI | GPIO_ALTERNATE | GPIO_PULLUP |
                       GPIO_SMT_EN | GPIO_DRV_1);
    }
}

int st7802_panel_sleep(int level)
{
    if (gpio_dev == NULL || dbi_dev == NULL) {
        return -1;
    }
    if (level < 1 || level > 3) {
        return -1;
    }

    /* level 1 是 2/3 的基础步骤；重复调用不会重发 */
    if (s_panel_sleep_level < 1) {
        uint8_t b = 0x00;
        st7802_write_cmd(0x51, &b, 1);    /* WRDISBV = 0（白点亮度 0） */
        st7802_write_cmd(0x28, NULL, 0);  /* DISPOFF：停止扫描 */
        s_panel_sleep_level = 1;
    }

    if (level >= 2 && s_panel_sleep_level < 2) {
        st7802_write_cmd(0x10, NULL, 0);  /* SLPIN */
        bflb_mtimer_delay_ms(120);        /* 规格要求 SLPIN 后等 120 ms */
        s_panel_sleep_level = 2;
    }

    if (level >= 3 && s_panel_sleep_level < 3) {
        uint8_t en = 0x01;
        st7802_write_cmd(0x4F, &en, 1);   /* DSTBON：深待机使能 */
        panel_qspi_pins_to_gpio();        /* QSPI 六线拉低/高阻 */
        st7802_vci_power(0);              /* IO29 低：面板 PMIC 断电 */
        s_panel_sleep_level = 3;
    }

    printf("[panel] panelsleep = %d\r\n", s_panel_sleep_level);
    return 0;
}

int st7802_panel_wake(void)
{
    if (gpio_dev == NULL || dbi_dev == NULL) {
        return -1;
    }

    /* 唤醒规格：VCI_EN 高 -> 等 5 ms -> RST 低脉冲 >3 ms */
    st7802_vci_power(1);
    bflb_mtimer_delay_ms(5);

    bflb_gpio_reset(gpio_dev, PIN_LCD_RST);
    bflb_mtimer_delay_ms(10);             /* 规格 >3 ms，这里给 10 ms 余量 */
    bflb_gpio_set(gpio_dev, PIN_LCD_RST);
    bflb_mtimer_delay_ms(5);

    /* QSPI 六线还给 DBI，才能继续发命令 */
    panel_qspi_pins_to_dbi();

    /* 唤醒规格：SLPOUT(0x11) -> 等 120 ms -> st7802_init()
     * st7802_init() 内部还会再做一次 RST 脉冲和 SLPOUT+120 ms（幂等），
     * 保留它作为唯一的完整初始化路径，避免出现第二套初始化序列。 */
    st7802_write_cmd(0x11, NULL, 0);
    bflb_mtimer_delay_ms(120);

    if (st7802_init() != 0) {
        printf("[panel] wake: st7802_init FAILED\r\n");
        return -2;
    }

    s_panel_sleep_level = 0;
    printf("[panel] panelsleep = 0 (full re-init)\r\n");
    return 0;
}

int st7802_te_level(void)
{
    if (gpio_dev == NULL) {
        return -1;
    }
    return bflb_gpio_read(gpio_dev, PIN_LCD_TE) ? 1 : 0;
}

/**
 * 等一个 TE 跳变沿（哪个沿都算）。
 *
 * 用途：部分刷新如果正好落在面板扫描途中，就会看到撕裂 —— 把这一帧的
 * 送屏对齐到 V-Blank 就干净了。**每帧只等一次**（挂在 LV_EVENT_RENDER_START），
 * 绝不能在每个 flush 里等：一次整屏刷新会被 LVGL 拆成十几个带。
 *
 * @param timeout_us 超时（µs）。读不到 TE（没焊/极性不对）时靠它兜底，
 *                   不能把刷屏卡死。
 * @return 0 = 等到跳变，-1 = 超时
 */
int st7802_te_wait(uint32_t timeout_us)
{
    uint32_t t0 = bflb_mtimer_get_time_us();
    int last;

    if (gpio_dev == NULL) {
        return -1;
    }
    last = bflb_gpio_read(gpio_dev, PIN_LCD_TE) ? 1 : 0;
    while ((uint32_t)(bflb_mtimer_get_time_us() - t0) < timeout_us) {
        int now = bflb_gpio_read(gpio_dev, PIN_LCD_TE) ? 1 : 0;
        if (now != last) {
            return 0;
        }
    }
    return -1;
}

/**
 * @brief 配置 DBI 为 EX_QSPI 模式（配置没变就直接返回，避免打断 CS 连续状态）
 *
 * 命令阶段和像素阶段的 data 线数不同（A1D1 vs A1D4）所以需要切换，
 * 但同一次刷屏里 CASET -> RASET -> RAMWR 不能被反复重配打断。
 */
void st7802_cfg(uint32_t hz, uint8_t clk_mode, uint8_t addr_wire, uint8_t data_wire)
{
    struct bflb_dbi_config_s cfg;

    if (dbi_ready && hz == cur_hz && clk_mode == cur_clk &&
        addr_wire == cur_addr && data_wire == cur_data) {
        return;
    }

    cfg.dbi_mode = DBI_MODE_EX_QSPI;
    /* DBI 输出格式必须与 SLPOUT 后设置的面板 COLMOD 一致。 */
    if (s_fmt565) {
        cfg.pixel_input_format = DBI_PIXEL_INPUT_FORMAT_RGB_565;
        cfg.pixel_output_format = DBI_PIXEL_OUTPUT_FORMAT_RGB_565;
    } else {
        cfg.pixel_input_format = DBI_PIXEL_INPUT_FORMAT_NRGB_8888;
        cfg.pixel_output_format = DBI_PIXEL_OUTPUT_FORMAT_RGB_888;
    }
    cfg.clk_mode = clk_mode;
    cfg.clk_freq_hz = hz;
    cfg.tx_fifo_threshold = 3;
    cfg.cmd_wire_mode = DBI_QSPI_WIRE_NUM_1; /* instruction 永远 1 线 */
    cfg.addr_wire_mode = addr_wire;
    cfg.data_wire_mode = data_wire;

    bflb_dbi_init(dbi_dev, &cfg);
    bflb_dbi_feature_control(dbi_dev, DBI_CMD_CS_CONTINUE, true);
    bflb_dbi_tcint_mask(dbi_dev, true);

    cur_hz = hz;
    cur_clk = clk_mode;
    cur_addr = addr_wire;
    cur_data = data_wire;
    dbi_ready = 1;
}

/* ------------------------------------------------------------------ */
/* 基本操作                                                            */
/* ------------------------------------------------------------------ */

void st7802_reset(void)
{
    bflb_gpio_reset(gpio_dev, PIN_LCD_RST);
    bflb_mtimer_delay_ms(10);
    bflb_gpio_set(gpio_dev, PIN_LCD_RST);
    bflb_mtimer_delay_ms(150);
}

int st7802_write_cmd(uint8_t cmd, const uint8_t *data, uint32_t len)
{
    st7802_cfg(wr_hz, cur_clk, cur_addr, DBI_QSPI_WIRE_NUM_1);
    bflb_dbi_qspi_set_addr(dbi_dev, QSPI_ADDR_LEN, (uint32_t)cmd << 8);
    return bflb_dbi_send_cmd_data(dbi_dev, QSPI_OP_CMD, (uint8_t)len, (uint8_t *)data);
}

int st7802_read_cmd(uint8_t cmd, uint8_t *data, uint32_t len)
{
    if (len == 0 || len > 8) {
        return -1;
    }
    st7802_cfg(rd_hz, cur_clk, cur_addr, DBI_QSPI_WIRE_NUM_1);
    bflb_dbi_qspi_set_addr(dbi_dev, QSPI_ADDR_LEN, (uint32_t)cmd << 8);
    return bflb_dbi_send_cmd_read_data(dbi_dev, QSPI_OP_READ, (uint8_t)len, data);
}

/*
 * 分块长读。
 *
 * 为什么必须分块：`bflb_dbi_send_cmd_read_data()` 只从 DBI_RDATA_0/1
 * 两个 32 位寄存器搬数据，lhal 的 `DBI_READ_DATA_BYTE_MAX` 就是 8。
 * 一次事务想读更多字节，硬件里根本没有地方落。
 *
 * 每次事务都是完整的 "instruction 0x03 + AD[23:0] = 00 CMD 00 + 数据"，
 * 所以**读命令每条都重新发一次，地址相位也重新发一次**。Caveat：如果面板
 * 在事务之间把读指针复位，这里会不停地读到同一组字节 —— 那正是本实验要
 * 区分出来的现象，不是 bug（dump 时能一眼看出来）。
 */
uint32_t st7802_read_cmd_n(uint8_t cmd, uint8_t *buf, uint32_t len, uint8_t max_chunk)
{
    uint32_t got = 0;

    if (buf == NULL || len == 0 || dbi_dev == NULL) {
        return 0;
    }
    if (max_chunk == 0 || max_chunk > 8) {
        max_chunk = 8;
    }

    /*
     * ★★ 2026-10-16 实测：**单字节寄存器必须单独发一次 len=1 的读事务。** ★★
     *
     * 背景：本函数原来的 while 循环在 len==2 时会一次事务读 2 字节。但面板
     * 对 1 字节的读命令（RDDPM/RDDCOLMOD/RDDSM…）只回 1 个字节；驱动再去
     * 搬第 2 个字节，搬回来的是**同一个字节的重复**（"98 98"、"55 55"）。
     * 巧合的是第一个字节恰好是对的，所以"2 字节读第一个"曾经能读到正确值，
     * 但它让所有 2 字节读数看起来像"面板在重复输出"，非常容易误判成读通路坏了。
     *
     * 单字节寄存器清单见手册 UCS 表（04/05/09/0A/0B/0C/0D/0E/0F/45/52/54/56/59/5B/5D）。
     * 只有 RDDID(04h) 是真 3 字节 + 2 个 dummy。所以：
     *   len == 1 -> 直接一次事务，绝不拆成"读 2 取第 1"。
     */
    if (len == 1) {
        if (st7802_read_cmd(cmd, buf, 1) < 0) {
            return 0;
        }
        return 1;
    }

    while (got < len) {
        uint32_t n = len - got;
        int r;

        if (n > max_chunk) {
            n = max_chunk;
        }
        r = st7802_read_cmd(cmd, buf + got, n);
        if (r < 0) {
            break;
        }
        got += n;
    }
    return got;
}

void st7802_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t p[4];

    p[0] = (uint8_t)(x1 >> 8);
    p[1] = (uint8_t)(x1 & 0xFF);
    p[2] = (uint8_t)(x2 >> 8);
    p[3] = (uint8_t)(x2 & 0xFF);
    st7802_write_cmd(0x2A, p, 4); /* CASET */

    p[0] = (uint8_t)(y1 >> 8);
    p[1] = (uint8_t)(y1 & 0xFF);
    p[2] = (uint8_t)(y2 >> 8);
    p[3] = (uint8_t)(y2 & 0xFF);
    st7802_write_cmd(0x2B, p, 4); /* RASET */
}

#endif /* !ST7802_HOST_VERIFY —— 以上需要 SDK 头文件 */

/* ------------------------------------------------------------------ */
/* 与硬件无关的纯算法（主机上也能编译/验证这一段）                        */
/* ------------------------------------------------------------------ */

/*
 * 这些 st7802_*_hw_* 的桩点把"纯逻辑"和"BL616 寄存器"隔开：
 *   - 固件构建：直接调 DBI / mtimer / lv_obj_invalidate；
 *   - 主机验证构建（-DST7802_HOST_VERIFY，见 tools/host/st7802_verify.c）：
 *     换成内存模型，于是在**同一份源码**上验证 CRC、pxpat 编码、
 *     gramrd 的偏移分析、整分取证状态机。
 *
 * 这样"探针自证"不靠肉眼：主机上能跑的逻辑，主机上就 assert 掉。
 */

#ifndef ST7802_HOST_VERIFY

/* 真机：gramrd 的读事务走 DBI */
uint32_t st7802_hw_read(uint8_t cmd, uint8_t *buf, uint32_t len, uint8_t chunk)
{
    return st7802_read_cmd_n(cmd, buf, len, chunk);
}

#endif /* !ST7802_HOST_VERIFY */

#ifdef ST7802_HOST_VERIFY
/* 主机验证路径：模拟 GRAM 的写入/读取（定义在本文件末尾） */
void st7802_sim_store(uint32_t x, uint32_t y, uint16_t v);
#endif

/* 发送一"段" pxpat（一段 = 对齐 band 边界的一组整行）。
 * 注意：行内容里编的是**绝对** y（y1+y），所以传进来的行缓冲已经编好了，
 * 这里只要把它落到对应的窗口上。 */
static void pxpat_emit(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint16_t *line)
{
#ifdef ST7802_HOST_VERIFY
    /* 主机：按 565 小端把这一段塞进模拟 GRAM */
    for (uint32_t yy = y1; yy <= y2; yy++) {
        for (uint32_t xx = x1; xx <= x2; xx++) {
            st7802_sim_store(xx, yy, line[xx - x1]);
        }
    }
#else
    st7802_blit565(x1, y1, x2, y2, line);
#endif
}

/** 把 0xRRGGBB 转成 RGB565（面板现在是 16bpp） */
static uint16_t st7802_pack565(uint32_t rgb)
{
    return (uint16_t)((((rgb >> 19) & 0x1F) << 11) |
                      (((rgb >> 10) & 0x3F) << 5) |
                      ((rgb >> 3) & 0x1F));
}

/* pxpat 的回读期望值：R5=x&31, G6=y&63, B5=0x0A */
static uint16_t pxpat_value(uint32_t x, uint32_t y)
{
    return (uint16_t)(((x & 0x1Fu) << 11) | ((y & 0x3Fu) << 5) | 0x0Au);
}

/* ---- 诊断状态：pxpat 分段计数 + 发送记录 ---- */
#define PXPAT_MAX_W 480

static uint16_t s_pxpat_buf[PXPAT_MAX_W]; /* pxpat 一行 565 像素 */
static uint32_t s_pxpat_blocks;
static uint32_t s_pxpat_px;

void st7802_pxpat_reset(void)
{
    s_pxpat_blocks = 0;
    s_pxpat_px = 0;
}

/* 给第 k 个像素填值（pxpat 的编码就发生在这里） */
void st7802_pxpat_put(uint32_t k, uint16_t v)
{
    if (k < PXPAT_MAX_W) {
        s_pxpat_buf[k] = v;
    }
}

const uint16_t *st7802_pxpat_line(void)
{
    return s_pxpat_buf;
}

void st7802_pxpat_bump(uint32_t px)
{
    s_pxpat_blocks++;
    s_pxpat_px += px;
}

uint32_t st7802_pxpat_get_blocks(void)
{
    return s_pxpat_blocks;
}

uint32_t st7802_pxpat_get_px(void)
{
    return s_pxpat_px;
}

/* ------------------------------------------------------------------ */
/* GRAM / 寄存器 回读探测                                              */
/* ------------------------------------------------------------------ */

#define GRAMRD_MAX 1024 /* 每次最多读这么多字节（RAM 里，不是硬件限制） */

static void hexdump(const char *tag, const uint8_t *b, uint32_t n)
{
    uint32_t i;

    printf("%s", tag);
    for (i = 0; i < n; i++) {
        printf("%02X ", b[i]);
        if ((i & 7) == 7 && i + 1 < n) {
            printf("\r\n       ");
        }
    }
    printf("\r\n");
}

/*
 * 把回读的两个字节按给定字节序解释成一个 565 像素，判断它是否是
 * "pxpat 在 (px,py) 上应该写的值"（低 11 位：R5=x&31, G6=y&63）。
 */
int st7802_gram_pixel_ok(uint8_t b0, uint8_t b1, int big_endian,
                         uint32_t px, uint32_t py)
{
    uint16_t v = big_endian ? (uint16_t)(((uint16_t)b0 << 8) | b1)
                            : (uint16_t)(((uint16_t)b1 << 8) | b0);
    return ((v & 0x07FFu) == (pxpat_value(px, py) & 0x07FFu)) ? 1 : 0;
}

/*
 * pxpat 编码图案的"回读解码"：把回读字节当 16bit 像素解释，
 * 扫 0..2 字节的整体偏移（dummy 周期只会是这个量级），
 * 逐像素比 R5/G6，看哪个偏移命中最多。
 *
 * 拆成独立函数是为了能在主机上 assert（tools/host/st7802_verify.c）：
 * 它是"回读结论"的核心，必须能被单独验。
 */
int st7802_gram_decode_pat(const uint8_t *rb, uint32_t rb_n, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t *out_best_off,
                           int *out_best_hit, int *out_seen)
{
    uint32_t off, best_off = 0;
    int best_hit = -1, seen = 0;

    if (out_best_off) *out_best_off = 0;
    if (out_best_hit) *out_best_hit = 0;
    if (out_seen) *out_seen = 0;

    for (off = 0; off <= 2 && off + 2 <= rb_n; off++) {
        int hit = 0;

        seen = 0;
        for (uint32_t i = off; i + 2 <= rb_n; i += 2) {
            uint32_t k = (i - off) / 2;
            uint32_t px = x + (k % w);
            uint32_t py = y + (k / w);

            seen++;
            /* 字节序两种都试：这里只关心"是不是这个坐标"，字节序由调用方另行判定 */
            if (st7802_gram_pixel_ok(rb[i], rb[i + 1], 0, px, py) ||
                st7802_gram_pixel_ok(rb[i], rb[i + 1], 1, px, py)) {
                hit++;
            }
        }
        printf("[gramrd] pat偏移 %lu B: R5/G6 命中 %d/%d\r\n",
               (unsigned long)off, hit, seen);
        if (hit > best_hit) {
            best_hit = hit;
            best_off = off;
        }
    }
    if (out_best_off) *out_best_off = best_off;
    if (out_best_hit) *out_best_hit = best_hit;
    if (out_seen) *out_seen = seen;
    return 0;
}

/*
 * ===========================================================================
 * 面板状态寄存器解码（2026-10-16）
 *
 * 为什么需要这个：整分花屏排查里反复出现"屏是亮的吗？"这个问题，而当时
 * 唯一的判据是固件自己的 `screen` 标志 —— 它只说明**软件认为**屏该亮，
 * **不证明面板真的在显示**。10-16 那次 pxpat"记账全对、屏上一个像素都
 * 没变"，很可能就是在这个盲区里被误读的。
 *
 * 现在有了硬件判据：读 RDDPM(0Ah)。实测真机：
 *     屏亮（用户肉眼确认）  -> 0x9C  (SLPON=1, NORON=1, DISON=1)
 *     息屏（用户肉眼确认）  -> 0x98  (SLPON=1, NORON=1, DISON=0)
 * 所以 DISON = 0x08 位。任何"画屏/看图"类实验之前，先用这个确认 DISON=1。
 *
 * ⚠️ 另一条同时被钉死的事实：**本面板无法回读 GRAM。**
 *    手册 v0.3 的 UCS 命令表里**根本没有** 2Eh(RAMRD)（只有 04/05/09/0A/0B/
 *    0C/0D/0E/0F/45/52/54/56/59/5B/5D/A1/A8/AA/AF/DA~DD 是可读命令）。
 *    这解释了 gramrd 默认走 2Eh 时"读到的东西和图案对不上、还周期性重复"。
 *    => 以后不要再把"GRAM 回读对拍"当作判据，这条路是被手册否掉的。
 * ===========================================================================
 */
const char *st7802_panel_power_str(uint8_t rddpm)
{
    static char buf[96];

    snprintf(buf, sizeof(buf),
             "BSTON=%u(升压) IDMON=%u(空闲) PTLON=%u(局部) SLPON=%u(睡眠出) NORON=%u(正常) DISON=%u(显示)",
             (rddpm >> 7) & 1u, (rddpm >> 6) & 1u, (rddpm >> 5) & 1u,
             (rddpm >> 4) & 1u, (rddpm >> 3) & 1u, (rddpm >> 2) & 1u);
    return buf;
}

int st7802_panel_probe(void)
{
    uint8_t id[3] = { 0, 0, 0 };
    uint8_t rddpm = 0, rddst[3] = { 0, 0, 0 }, colmod = 0, madctl = 0;
    uint8_t rddim = 0, rddsc = 0, rdisbv = 0, rdddr = 0, selfdiag = 0;
    int ok = 1;

    /* 自证：读通路必须能读出 RDDID，否则后面所有值都不算数 */
    st7802_read_cmd(0x04, id, 3);
    printf("[panel] 自证 RDDID(04h)=%02X %02X %02X  %s\r\n", id[0], id[1], id[2],
           (id[0] == 0x78 && id[1] == 0x01) ? "OK (ST7802)" :
           ((id[0] == 0 || id[0] == 0xFF) ? "!! 读通路可能没通" : "?! 与已知值不同"));
    if (id[0] == 0 || id[0] == 0xFF) {
        return -1;
    }

    /* 全部按 1 字节事务读 —— 见 read_cmd_n 顶上那条实测教训 */
    st7802_read_cmd(0x0A, &rddpm, 1);
    st7802_read_cmd(0x09, rddst, 3);
    st7802_read_cmd(0x0C, &colmod, 1);
    st7802_read_cmd(0x0B, &madctl, 1);
    st7802_read_cmd(0x0D, &rddim, 1);
    st7802_read_cmd(0x0E, &rddsc, 1);
    st7802_read_cmd(0x0F, &selfdiag, 1);
    st7802_read_cmd(0x52, &rdisbv, 1);
    st7802_read_cmd(0x54, &rdddr, 1);

    printf("[panel] RDDPM (0Ah)=0x%02X  %s\r\n", rddpm, st7802_panel_power_str(rddpm));
    printf("[panel] RDDST (09h)=%02X %02X %02X  (1st MADCTL/颜色序, 2nd=电源模式, 3rd 含 INVON)\r\n",
           rddst[0], rddst[1], rddst[2]);
    printf("[panel] RDCOLMOD(0Ch)=0x%02X %s\r\n", colmod,
           (colmod == 0x55) ? "(IFPF=5 -> 16bpp RGB565  与固件一致)" :
           ((colmod == 0x77) ? "(IFPF=7 -> 24bpp RGB888  与固件**不一致**!)" : "(未知/其他)"));
    printf("[panel] RDDMADCTL(0Bh)=0x%02X   RDDIM(0Dh)=0x%02X   RDDSM(0Eh)=0x%02X\r\n",
           madctl, rddim, rddsc);
    printf("[panel] RDDISBV(52h)=0x%02X  RDDDCR(54h)=0x%02X  RDDSDR(0Fh)=0x%02X\r\n",
           rdisbv, rdddr, selfdiag);

    if ((rddpm & 0x08u) == 0) {
        printf("[panel] ==> **DISON=0：面板此刻没有在显示**。\r\n");
        printf("[panel]     任何\"看图案/看画面\"的实验在这个状态下都是无效的。\r\n");
        ok = 0;
    } else {
        printf("[panel] ==> DISON=1：面板确实在显示（可以放心做\"看图\"实验）。\r\n");
    }
    if (rdddr & 0x10u) {
        printf("[panel] 注意：RDDDCR 的 BCTRL 位=1（亮度控制被打开），"
               "亮度寄存器(51h)可能不直接生效。\r\n");
    }
    return ok ? 1 : 0;
}

int st7802_gramrd(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                  uint8_t cmd, uint8_t max_chunk, int pat_check)
{
    static uint8_t rb[GRAMRD_MAX];
    uint32_t want, got;
    uint8_t uc = cmd ? cmd : 0x2E;
    uint32_t i, uniq = 0;
    int all00 = 1, allff = 1;

    if (w == 0 || h == 0 || x + w > ST7802_WIDTH || y + h > ST7802_HEIGHT) {
        printf("[gramrd] 窗口非法：%u,%u %ux%u (面板 %dx%d)\r\n",
               x, y, w, h, ST7802_WIDTH, ST7802_HEIGHT);
        return -1;
    }
    if (max_chunk == 0 || max_chunk > 8) {
        max_chunk = 8;
    }

    want = (uint32_t)w * (uint32_t)h * 2u;
    if (want > GRAMRD_MAX) {
        printf("[gramrd] 窗口 %ux%u = %lu B 超过本次探测上限 %u B，只读前 %u B\r\n",
               w, h, (unsigned long)want, (unsigned)GRAMRD_MAX, (unsigned)GRAMRD_MAX);
        want = GRAMRD_MAX;
    }

    printf("[gramrd] cmd=0x%02X win=(%u,%u)-(%u,%u) want=%lu B chunk=%u\r\n",
           uc, x, y, (unsigned)(x + w - 1), (unsigned)(y + h - 1),
           (unsigned long)want, max_chunk);
    printf("[gramrd] 注意：回读走的是和寄存器回读同一条 DBI 路径，"
           "每条读命令自带地址相位；面板能否跨事务推进读指针**未验证**。\r\n");

    /*
     * 自检探针（第 11.1 节教训：探针本身要先自证）。
     * 先用已知能工作的 RDDID(04h) 读 3 字节。板上路径才做这一步；
     * 主机验证构建里 st7802_hw_read 是内存桩，跳过。
     */
#ifndef ST7802_HOST_VERIFY
    {
        uint8_t id[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
        st7802_read_cmd(0x04, id, 3);
        printf("[gramrd] 自检 RDDID(04h)=%02X %02X %02X  %s\r\n", id[0], id[1], id[2],
               (id[0] == 0 && id[1] == 0 && id[2] == 0) ? "!! 全 0，读通路可能没通" :
               ((id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF) ? "!! 全 FF，读通路可能没通"
                                                                 : "OK"));
    }
#endif

    /* 定窗口：和 blit565 用同一套 CASET/RASET 写法 */
    st7802_set_window(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));

    memset(rb, 0x5A, sizeof(rb));
    got = st7802_hw_read(uc, rb, want, max_chunk);
    printf("[gramrd] 读回 %lu B\r\n", (unsigned long)got);
    if (got == 0) {
        printf("[gramrd] 一个字节都没读到\r\n");
        return -1;
    }

    hexdump("[gramrd] raw:", rb, got > 64 ? 64 : got);
    if (got > 64) {
        printf("[gramrd] （只打前 64 B，共 %lu B）\r\n", (unsigned long)got);
    }

    for (i = 0; i < got; i++) {
        if (rb[i] != 0x00) all00 = 0;
        if (rb[i] != 0xFF) allff = 0;
        if (i && rb[i] != rb[0]) uniq = 1;
    }
    printf("[gramrd] 判定：%s%s唯一值=%s\r\n",
           all00 ? "全 0x00 " : "", allff ? "全 0xFF " : "", uniq ? "否" : "是");
    if (all00 || allff) {
        printf("[gramrd]   -> 该命令在这个窗口上没有任何有效数据返回。\r\n");
        printf("[gramrd]      ST7802 手册 v0.3 的 UCS 命令表里**没有** 2Eh(RAMRD)，\r\n");
        printf("[gramrd]      这一结果与手册一致，不能据此回读 GRAM。\r\n");
    }

    if (pat_check) {
        uint32_t best_off = 0;
        int best_hit = 0, seen = 0;

        st7802_gram_decode_pat(rb, got, x, y, w, &best_off, &best_hit, &seen);
        if (best_hit <= 0) {
            printf("[gramrd] pat：没有任何偏移能解释回读值 -> 不是 pxpat 图案，"
                   "或返回格式不是 16bit/像素\r\n");
        } else {
            printf("[gramrd] pat：最佳偏移 = %lu B（命中 %d/%d），"
                   "说明回读相对窗口起点整体移了 %lu 个字节（dummy 周期量级）\r\n",
                   (unsigned long)best_off, best_hit, seen, (unsigned long)best_off);
            printf("[gramrd]      每 2 字节打一个像素；上面 raw 里按这个偏移手工对也能读出坐标\r\n");
        }
        printf("[gramrd] 提醒：只有 8 B/事务，所以这里最多 4 个像素 —— "
               "够判协议，不够判整窗口。\r\n");
    }
    return (int)got;
}

/** 算 CRC32（IEEE 802.3，反射，初值/异或全 1）。
 *  多项式 0xEDB88320；和 zlib.crc32 同族，可用主机脚本独立验证。 */
uint32_t st7802_crc32(const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc = 0xFFFFFFFFu;

    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

/*
 * ⚠️ 自检探针：CRC 用的是自己写的实现（不想引 zlib/libc）。
 * 内建向量对不上就说明指纹本身不可信 —— 先把探针跑通再看数据
 * （第 11.1 节：`s_push_tc` 忘了清零，探针骗了我们整整一轮）。
 *
 * 顺带量一下 CRC 在送屏路径上的成本：它是**加在 push_pixels565 的
 * 喂数循环里**的，真机 `stats` 的 avg/max 会比改动前略大，
 * 不能拿旧数据和它直接比。
 */
int st7802_crc_selfcheck(void)
{
    static const uint8_t v1[] = "123456789";
    static const uint8_t v2[] = "a";
    uint32_t a = st7802_crc32(v1, 9);
    uint32_t b = st7802_crc32(v2, 1);
    uint32_t c = st7802_crc32(v2, 0);
    int ok = 1;

    printf("[crc] selftest crc32(\"123456789\")=%08lX (期望 CBF43926) %s\r\n",
           (unsigned long)a, a == 0xCBF43926u ? "OK" : "!! FAIL");
    printf("[crc] selftest crc32(\"a\")=%08lX (期望 E8B7BE43) %s\r\n",
           (unsigned long)b, b == 0xE8B7BE43u ? "OK" : "!! FAIL");
    printf("[crc] selftest crc32(空)=%08lX (期望 00000000) %s\r\n",
           (unsigned long)c, c == 0u ? "OK" : "!! FAIL");
    if (a != 0xCBF43926u || b != 0xE8B7BE43u || c != 0u) {
        ok = 0;
    }
    /*
     * ⚠️ 这里**故意不报 CRC 耗时**：主机上的计时测不准（编译器会把
     * 重复调用的同一个缓冲折掉），而 ARM 上的真实成本必须用真机 `stats`
     * 量。改动前的 `stats` 基线务必先记一份，再和加了 CRC 的版本比。
     */
    printf("[crc] 注意：发送侧 CRC 是**加在 push_pixels565 喂数循环里**的，\r\n");
    printf("[crc]       真机 `stats` 的 avg/max 会比改动前略大，先把旧基线记下来再比。\r\n");
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* 诊断：按块记录发送 CRC / 窗口，以及"整分取证"                        */
/* ------------------------------------------------------------------ */

#define BLIT_LOG_N 10

typedef struct {
    uint32_t npix;      /* 本块像素数（可以拿去和主机侧的窗口记录对号） */
    uint32_t intsts;    /* 本块结束时的 bflb_dbi_get_intstatus() */
    uint32_t crc;       /* 发送侧数据 CRC32（含喂给 FIFO 的重复半字） */
    uint32_t addr;      /* 源缓冲地址（判断是不是 flash/XIP 那一块） */
    uint32_t nsent;     /* 第几块（全局递增，用来对齐前后帧） */
    uint16_t x1, y1, x2, y2; /* 本块窗口 */
    uint16_t iters;     /* 喂 FIFO 的轮数 */
    uint16_t zero;      /* 其中 FIFO 计数为 0（喂不进去）的次数 */
    uint16_t tc_spin;   /* 等 TC 的自旋次数 */
    uint8_t  fifo_err;  /* intsts 里 FIFO_ERR 置位 */
    uint8_t  tx_fifo;   /* intsts 里 TX_FIFO 置位（收尾时 FIFO 还有货？） */
    uint8_t  solid;     /* 纯色块（buf == NULL） */
} blit_log_t;

static blit_log_t s_blit_log[BLIT_LOG_N];
static uint32_t s_blit_log_i;
static uint32_t s_blit_n, s_blit_px, s_fifo_err_n, s_zero_n, s_tc_max;
/* push_pixels565 内部用（每块开始前/记完后都清零 —— 见那边的教训） */
static uint32_t s_push_iters, s_push_zero, s_push_tc;
/* 本块在推像素时累计的发送数据 CRC（每块开始前重置） */
static uint32_t s_push_crc;
static uint16_t s_win_x1, s_win_y1, s_win_x2, s_win_y2;
static uint32_t s_win_addr;

/*
 * ★★ 2026-10-16「排空等待」实验开关 ★★
 *
 * 背景（交接文档 §4/§5）：整分那一帧是唯一"30600px 大块被拆成两块、背靠背推"
 * 的场景；而 pxpat（480 次背靠背 blit）曾在真机上"记账正确、屏上毫无变化"。
 * 首选假设是"上一块还没真正出完，下一块的 CASET/RASET 就把它打断"。
 *
 * 这个开关只影响**收尾等待**，不改喂数逻辑、不改窗口、不改像素，默认 0（关），
 * 所以对生产路径零影响 —— 只有 shell 显式 `drain 1` 才生效。
 *
 *   drain 0（默认）：收尾只等 TC（原行为，几个月一直这样）
 *   drain 1        ：收尾等 TC **且** 等 TX_FIFO 中断位（FIFO 空）
 *
 * ⚠️ 15 号那次为什么失败：错在"等 DBI_CMD_GET_TX_FIFO_CNT == 0"。
 *    那个 feature 返回的是**剩余空间**（喂数循环正是拿它当"这轮还能塞多少字"），
 *    "等它 == 0" = 等 FIFO 塞满 ⇒ 永不成立 ⇒ 每块空转到保险 break，
 *    整屏换页从 23ms 变 2~3 秒。**这次改用中断状态位 DBI_INTSTS_TX_FIFO，
 *    语义是"FIFO 已空"，和"剩余空间"是两个完全不同的东西。**
 */
static int s_drain_experiment = 0;
static uint32_t s_drain_spin_max = 0;   /* drain 等待的自旋次数（用来看它到底等了多久） */
static uint32_t s_drain_n = 0;          /* 因 drain 而真的多等了的块数 */

/*
 * 块间"收窗"保护时间（微秒，默认 0 = 关）。
 * 见 push_pixels565 收尾处的说明：专门针对"面板侧窗口相位还没落定"这个假设。
 * shell `winguard <us>` 现场 A/B，不用重编译。
 */
static uint32_t s_win_guard_us = 0;

void st7802_set_win_guard_us(uint32_t us)
{
    s_win_guard_us = us;
}

uint32_t st7802_get_win_guard_us(void)
{
    return s_win_guard_us;
}

void st7802_set_drain_experiment(int on)
{
    s_drain_experiment = on ? 1 : 0;
}

int st7802_get_drain_experiment(void)
{
    return s_drain_experiment;
}

uint32_t st7802_drain_stats(uint32_t *out_blocks)
{
    if (out_blocks) {
        *out_blocks = s_drain_n;
    }
    return s_drain_spin_max;
}

void st7802_drain_stats_reset(void)
{
    s_drain_spin_max = 0;
    s_drain_n = 0;
}

/*
 * ---- 「只记大块」的第二个环（2026-10-16）----------------------------------
 *
 * 为什么加：BLIT_LOG_N=10 的环是"最近 10 块"，而 UI 每秒推 10 个 ~1700px 的
 * 小块，于是 pxpat 那 480 个块**瞬间被挤掉** —— 交接文档 §3.4 记的就是这个坑，
 * 当时一度误判"pxpat 根本没推"。累计 n/px 是对的，但**看不到逐块明细**。
 *
 * 现在大块（>= 5000 px，覆盖整分的 30600/28560 和 pxpat 的 480px×480 行）单独进
 * 一个 16 格的环，永远不被小块冲掉。原来的 s_blit_log 与 minroll 取证一个字都不改。
 */
#define BLIT_BIG_PX     5000u
#define BLIT_BIG_N      16u
static blit_log_t s_blit_big[BLIT_BIG_N];
static uint32_t s_blit_big_i;


void st7802_flush_diag_reset(void)
{
    for (uint32_t k = 0; k < BLIT_LOG_N; k++) s_blit_log[k].npix = 0;
    for (uint32_t k = 0; k < BLIT_BIG_N; k++) s_blit_big[k].npix = 0;
    s_blit_big_i = 0;
    s_blit_log_i = s_blit_n = s_blit_px = 0;
    s_fifo_err_n = s_zero_n = s_tc_max = 0;
}

uint32_t st7802_blit_count(void)
{
    return s_blit_n;
}

void st7802_flush_diag_dump(void)
{
    uint32_t n = (s_blit_log_i < BLIT_LOG_N) ? s_blit_log_i : BLIT_LOG_N;
    uint32_t start = (s_blit_log_i < BLIT_LOG_N) ? 0 : (s_blit_log_i % BLIT_LOG_N);
    uint32_t seen_crc_dup = 0;

    st7802_crc_selfcheck();

    printf("[blitdiag] n=%lu px=%lu fifo_err=%lu zero_feed=%lu tc_spin_max=%lu\r\n",
           (unsigned long)s_blit_n, (unsigned long)s_blit_px,
           (unsigned long)s_fifo_err_n, (unsigned long)s_zero_n,
           (unsigned long)s_tc_max);
    printf("[blitdiag] drain 实验=%s  真的多等了的块=%lu  最长自旋=%lu\r\n",
           s_drain_experiment ? "ON（收尾额外等 TX_FIFO 空）" : "OFF（收尾只等 TC）",
           (unsigned long)s_drain_n, (unsigned long)s_drain_spin_max);
    printf("[blitdiag] 最近 %lu 块（时间序）：\r\n", (unsigned long)n);
    for (uint32_t k = 0; k < n; k++) {
        blit_log_t *e = &s_blit_log[(start + k) % BLIT_LOG_N];
        printf("  #%-6lu win=(%u,%u)-(%u,%u) px=%-6lu crc=%08lX %s addr=%08lX%s tc_spin=%-6u sts=0x%02lX%s%s\r\n",
               (unsigned long)e->nsent,
               e->x1, e->y1, e->x2, e->y2,
               (unsigned long)e->npix, (unsigned long)e->crc,
               e->solid ? "solid " : "buffer",
               (unsigned long)e->addr,
               (e->addr >= 0x10000000u && e->addr < 0x20000000u) ? "(XIP!)" : "",
               e->tc_spin, (unsigned long)e->intsts,
               e->fifo_err ? " FIFO_ERR" : "", e->tx_fifo ? " TX_FIFO" : "");
    }
    /*
     * 窗口相同但 CRC 不同 = 同一块区域两次发出去的**字节**就不一样。
     * 这正是"绘制路径"的指纹（主机说它是确定的，那真机上就得被这条打脸）。
     */
    for (uint32_t k = 0; k < n; k++) {
        blit_log_t *a = &s_blit_log[(start + k) % BLIT_LOG_N];
        for (uint32_t j = k + 1; j < n; j++) {
            blit_log_t *b = &s_blit_log[(start + j) % BLIT_LOG_N];
            if (a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 &&
                a->y2 == b->y2 && a->crc != b->crc) {
                printf("[blitdiag] 同窗口 CRC 不同: #%lu(%08lX) vs #%lu(%08lX) win=(%u,%u)-(%u,%u)\r\n",
                       (unsigned long)a->nsent, (unsigned long)a->crc,
                       (unsigned long)b->nsent, (unsigned long)b->crc,
                       a->x1, a->y1, a->x2, a->y2);
                seen_crc_dup++;
            }
        }
    }
    if (n >= 2 && !seen_crc_dup) {
        printf("[blitdiag] 最近 %lu 块里没有\"同窗口不同 CRC\"\r\n", (unsigned long)n);
    }

    /* 大块单独一段：pxpat / 整分那两块不会被 UI 小块冲掉（见 s_blit_big 的说明） */
    if (s_blit_big_i > 0) {
        uint32_t bn = (s_blit_big_i < BLIT_BIG_N) ? s_blit_big_i : BLIT_BIG_N;
        uint32_t bstart = (s_blit_big_i < BLIT_BIG_N) ? 0 : (s_blit_big_i % BLIT_BIG_N);

        printf("[blitdiag] 大块（>=%lu px）共 %lu 块，列最近 %lu 块：\r\n",
               (unsigned long)BLIT_BIG_PX, (unsigned long)s_blit_big_i, (unsigned long)bn);
        for (uint32_t k = 0; k < bn; k++) {
            blit_log_t *e = &s_blit_big[(bstart + k) % BLIT_BIG_N];

            printf("  #%-6lu win=(%u,%u)-(%u,%u) px=%-6lu crc=%08lX tc_spin=%-6u sts=0x%02lX%s%s\r\n",
                   (unsigned long)e->nsent, e->x1, e->y1, e->x2, e->y2,
                   (unsigned long)e->npix, (unsigned long)e->crc,
                   e->tc_spin, (unsigned long)e->intsts,
                   e->fifo_err ? " FIFO_ERR" : "", e->tx_fifo ? " TX_FIFO" : "");
        }
    } else {
        printf("[blitdiag] 大块环是空的（还没有 >=%lu px 的块）\r\n",
               (unsigned long)BLIT_BIG_PX);
    }
}

#define MINCAP_PRE  6  /* 分钟变化之前留几块 */
#define MINCAP_POST 12 /* 之后留几块 */

/*
 * "分钟刚变"标志住在 face_lvgl.c（它知道 hh:mm 变了），由 main.c 的
 * flush 钩子读、arm 完自己清 —— st7802.c 只做记录，不碰 UI 状态。
 */
static uint32_t s_min_arm;
static int s_min_seen;
static uint32_t s_min_minute;
static uint32_t s_min_pre_n, s_min_post_n;
static st7802_blk_t s_min_pre[MINCAP_PRE];
static st7802_blk_t s_min_post[MINCAP_POST];

/* blit_log_t（驱动内部，带诊断位）-> st7802_blk_t（公开，给 shell 看） */
static void blk_from_log(st7802_blk_t *d, const blit_log_t *s)
{
    d->x1 = s->x1; d->y1 = s->y1; d->x2 = s->x2; d->y2 = s->y2;
    d->npix = s->npix;
    d->crc = s->crc;
    d->nsent = s->nsent;
    d->addr = s->addr;
}

/**
 * 在 flush 钩子里调用：记录"分钟数字刚变"这一刻前后各若干块。
 *
 * 为什么要前后都要：判据是"同一个窗口，在分钟变化前后发出去的字节是否不同"。
 * 只有把 prev_minute / new_minute 两帧的同窗口块摆在一起，才能说清
 * "发送侧一致"还是"发送侧自己就在变"。
 */
void st7802_min_capture_arm(uint32_t minute)
{
    uint32_t avail, n, i;

    if (!s_min_arm) {
        return;
    }
    s_min_arm = 0;
    s_min_seen = 1;
    s_min_minute = minute;
    s_min_pre_n = 0;
    s_min_post_n = 0;

    avail = (s_blit_log_i < BLIT_LOG_N) ? s_blit_log_i : BLIT_LOG_N;
    n = (avail < MINCAP_PRE) ? avail : MINCAP_PRE;
    for (i = 0; i < n; i++) {
        uint32_t idx = (s_blit_log_i - n + i) % BLIT_LOG_N;
        blk_from_log(&s_min_pre[i], &s_blit_log[idx]);
    }
    s_min_pre_n = n;

    /*
     * 刚 arm 的这一块（整分帧的第一块）也先放进 post[0]：
     * 如果分钟变化发生在某个动画帧里，这个块的窗口跟 pre 的最后一块
     * 就是"同一窗口、两帧数据"，正好用来判"发送侧数据一致吗"。
     */
    if (s_blit_log_i > 0) {
        blk_from_log(&s_min_post[0], &s_blit_log[(s_blit_log_i - 1) % BLIT_LOG_N]);
        s_min_post_n = 1;
    }
}

/** flush 钩子每块都调：arm 之后的块继续收进 post[] */
void st7802_min_capture_take(void)
{
    if (!s_min_seen || s_min_post_n >= MINCAP_POST || s_blit_log_i == 0) {
        return;
    }
    blk_from_log(&s_min_post[s_min_post_n], &s_blit_log[(s_blit_log_i - 1) % BLIT_LOG_N]);
    s_min_post_n++;
}

int st7802_min_capture_ready(void)
{
    return s_min_seen;
}

/** shell 侧请求：armed，等下一次分钟跳变；再 arm 一次会丢掉上一次的结果 */
void st7802_min_capture_arm_request(uint32_t minute)
{
    s_min_arm = 1;
    s_min_seen = 0;
    s_min_pre_n = s_min_post_n = 0;
    (void)minute;
}

int st7802_min_capture_armed(void)
{
    return (s_min_seen || s_min_arm) ? 1 : 0;
}

void st7802_min_capture_reset(void)
{
    s_min_arm = 0;
    s_min_seen = 0;
    s_min_pre_n = s_min_post_n = 0;
}

static void blk_print(const char *tag, uint32_t i, const st7802_blk_t *e)
{
    printf("  %s[%lu] #%-6lu win=(%u,%u)-(%u,%u) px=%-6lu crc=%08lX addr=%08lX%s\r\n",
           tag, (unsigned long)i, (unsigned long)e->nsent,
           e->x1, e->y1, e->x2, e->y2,
           (unsigned long)e->npix, (unsigned long)e->crc,
           (unsigned long)e->addr,
           (e->addr >= 0x10000000u && e->addr < 0x20000000u) ? " (XIP/flash!)" : "");
}

void st7802_min_capture_dump(void)
{
    uint32_t i;

    if (!s_min_seen) {
        if (s_min_arm) {
            printf("[mincap] 已经 armed，还在等下一次整分（分钟数字变化）。\r\n");
        } else {
            printf("[mincap] 没有数据。先敲 `minroll` 等下一次整分。\r\n");
        }
        return;
    }

    st7802_crc_selfcheck();
    printf("[mincap] 分钟变化目标 = %02lu；发送侧共抓 %lu(前) + %lu(后) 块\r\n",
           (unsigned long)(s_min_minute % 100),
           (unsigned long)s_min_pre_n, (unsigned long)s_min_post_n);

    printf("[mincap] --- 分钟变化之前（上一帧的块，含大数字那两块）---\r\n");
    for (i = 0; i < s_min_pre_n; i++) {
        blk_print("pre ", i, &s_min_pre[i]);
    }
    printf("[mincap] --- 分钟变化之后（本帧的块）---\r\n");
    for (i = 0; i < s_min_post_n; i++) {
        blk_print("post", i, &s_min_post[i]);
    }

    /*
     * 判读：同窗口跨前后帧比 CRC。
     *   · 窗口相同 + CRC 不同 = 发送侧数据确实变了（大数字变了，正常）；
     *   · 窗口相同 + CRC 相同 = 这一帧那个窗口发出去的字节**没变**
     *     （这时才该怀疑"面板收到的不对"）。
     */
    printf("[mincap] 跨帧同窗口 CRC 对比：\r\n");
    for (i = 0; i < s_min_post_n; i++) {
        uint32_t j;
        int found = 0;
        for (j = 0; j < s_min_pre_n; j++) {
            const st7802_blk_t *a = &s_min_pre[j];
            const st7802_blk_t *b = &s_min_post[i];
            if (a->x1 == b->x1 && a->y1 == b->y1 &&
                a->x2 == b->x2 && a->y2 == b->y2) {
                printf("    win=(%u,%u)-(%u,%u) pre#%lu=%08lX  post#%lu=%08lX  %s\r\n",
                       a->x1, a->y1, a->x2, a->y2,
                       (unsigned long)a->nsent, (unsigned long)a->crc,
                       (unsigned long)b->nsent, (unsigned long)b->crc,
                       a->crc == b->crc ? "== 相同（字节没变）" : "!= 不同（这一块内容变了）");
                found = 1;
            }
        }
        if (!found) {
            printf("    post#%lu win=(%u,%u)-(%u,%u) 在前一帧找不到同窗口块\r\n",
                   (unsigned long)s_min_post[i].nsent,
                   s_min_post[i].x1, s_min_post[i].y1,
                   s_min_post[i].x2, s_min_post[i].y2);
        }
    }

    printf("[mincap] 面板侧回读：`minroll cmp <i>`（i = 上面 post[i] 的下标，0..%lu）\r\n",
           (unsigned long)(s_min_post_n ? s_min_post_n - 1 : 0));
    printf("[mincap] 判读标准：\r\n");
    printf("[mincap]   发送数据一致 + 面板数据一致  -> 传输/面板都不是，问题在**绘制**；\r\n");
    printf("[mincap]   发送一致而面板不一致        -> 才轮到**传输/时序**这条路。\r\n");
}

/* 只读 post[i] 的窗口，面板侧能对多少字节就多少（见 #define GRAMRD_MAX） */
int st7802_min_capture_panel_check(int i)
{
    const st7802_blk_t *e;
    uint16_t w, h;
    int got;

    if (!s_min_seen || i < 0 || (uint32_t)i >= s_min_post_n) {
        printf("[mincap] 没有 post[%d]（先 `minroll` 抓一帧，再 `minroll cmp <i>`）\r\n", i);
        return -1;
    }
    e = &s_min_post[i];
    w = (uint16_t)(e->x2 - e->x1 + 1);
    h = (uint16_t)(e->y2 - e->y1 + 1);

    printf("[mincap] 对拍 post[%d] #%lu win=(%u,%u)-(%u,%u) 发送 CRC=%08lX (%lu B)\r\n",
           i, (unsigned long)e->nsent, e->x1, e->y1, e->x2, e->y2,
           (unsigned long)e->crc, (unsigned long)((uint32_t)w * h * 2u));

    /*
     * ⚠️ 只能读窗口**开头**的若干字节：DBI 硬件一次事务只能收 8 字节，
     * 长读靠多次事务拼。所以"整块 CRC 对拍"做不到，这里对的是窗口
     * 左上角那一小块（像素数会打印出来，别把它当整块结论）。
     */
    got = st7802_gramrd(e->x1, e->y1, w, h, 0x2E, 8, 0);
    if (got <= 0) {
        printf("[mincap] 面板回读失败/为空 —— 按 `gramrd` 的判定处理：\r\n");
        printf("[mincap]   若回读恒为 0x00/0xFF，说明本面板不支持 2Eh 回读，\r\n");
        printf("[mincap]   那么这条判据**判不了**，只能退回发送侧 CRC + pxpat 肉眼判。\r\n");
        return -1;
    }

    printf("[mincap] 发送侧 CRC 已记录（crc=%08lX），但**面板侧只能对窗口前 %d B**；\r\n",
           (unsigned long)e->crc, got);
    printf("[mincap] 只有当这段回读能与\"发下去的前 %d B\"逐字节对上，\r\n", got);
    printf("[mincap] 才能说\"该窗口面板收到的是对的\"；对不上才轮到传输/时序。\r\n");
    return 0;
}

#ifndef ST7802_HOST_VERIFY

/* ------------------------------------------------------------------ */
/* 送屏通路（真机）                                                     */
/* ------------------------------------------------------------------ */

/*
 * RGB565 FIFO 推送。
 *
 * ★ 这里做的事：把两个像素打成一个 32 位字喂进 TX FIFO，同时**顺路**算一个
 *   发送数据 CRC32（就是面板应该收到的字节流）。CRC 算在真正写进 FIFO 的
 *   那个字上，所以奇数像素时补的重复半字、半字序翻转都包含在内。
 *
 * ⚠️ 2026-10-15 反面教材（已回退，别再犯）：有人在下面"等 TC"之后又加了一道
 *    "等 TX FIFO 计数归 0 + 20µs" 的排空，结果**上板两头都错**：
 *      ① 没修好花屏；② 整屏换页从 23ms 变 2~3 秒 ——
 *     `DBI_CMD_GET_TX_FIFO_CNT` 返回的是**剩余空间**（本函数正是拿它当
 *     "这一轮还能塞多少字"用），写成"等它 == 0" = 等 FIFO 塞满，永不成立。
 *    结论：收尾只等 TC 就够（原设计，几个月一直正常）。
 */
static void st7802_push_pixels565(uint32_t npix, const uint16_t *buf, uint16_t solid)
{
    uint32_t words;
    uint32_t i = 0;
    uint8_t cnt;

    st7802_cfg(wr_hz, cur_clk, cur_addr, pix_wire);
    bflb_dbi_qspi_set_addr(dbi_dev, QSPI_ADDR_LEN, (uint32_t)0x2C << 8);

    words = bflb_dbi_feature_control(dbi_dev, DBI_CMD_GET_SIZE_OF_PIXEL_CNT, npix) / 4;

    bflb_dbi_send_cmd_pixel(dbi_dev, pix_op, npix, NULL);

    s_push_crc = 0xFFFFFFFFu;   /* CRC32 就地累计（反射），收尾取反 */
    s_push_iters = s_push_zero = s_push_tc = 0;

    while (words > 0) {
        cnt = (uint8_t)bflb_dbi_feature_control(dbi_dev, DBI_CMD_GET_TX_FIFO_CNT, 0);
        if (cnt > words) {
            cnt = (uint8_t)words;
        }
        if (cnt == 0) {
            s_push_zero++;
        }
        words -= cnt;
        s_push_iters++;
        while (cnt--) {
            uint32_t w;

            if (buf) {
                uint16_t a = buf[i++];
                uint16_t b = (i < npix) ? buf[i++] : a; /* 奇数个像素时补一个 */
                /* ★★ 2026-10-11 晚 实测结论：这个顺序是对的，别翻！★★
                 *
                 * 有人（一个子代理）根据 SDK 里 NRGB_8888 路径的字节序，
                 * 推断"先画的像素应该在高半字"，把这里改成 (a<<16)|b。
                 * 结果上板后**整个表盘变成严重锯齿/重影**，比原来更糟，
                 * 当场翻回来了 —— 经验证据 > 从别的路径类推。 */
                w = s_px_swap ? (((uint32_t)a << 16) | (uint32_t)b)
                              : (((uint32_t)b << 16) | (uint32_t)a);
            } else {
                w = ((uint32_t)solid << 16) | (uint32_t)solid;
            }
            /*
             * ★ 发送侧指纹：CRC 算在**真正写进 TX FIFO 的那个字**上。
             *   纯整数运算、无分支表，量级上不影响这个循环的时序。
             */
            for (int k = 0; k < 4; k++) {
                s_push_crc ^= (w >> (k * 8)) & 0xFFu;
                for (int j = 0; j < 8; j++) {
                    s_push_crc = (s_push_crc >> 1) ^
                                 (0xEDB88320u & (uint32_t)(-(int32_t)(s_push_crc & 1u)));
                }
            }
            putreg32(w, DMA_ADDR_DBI_TDR);
        }
    }

    /*
     * ⚠️ 收尾**只等 TC**（默认）。绝不要在这里加"等 TX FIFO 计数归 0"之类的东西，
     *    原因见函数头注释（11.1 节的教训）。这一条是硬约束。
     *
     * 2026-10-16：上面这条约束的对象是 **DBI_CMD_GET_TX_FIFO_CNT（剩余空间）**。
     *    验证"背靠背 blit 会不会丢像素"需要的是**排空**语义，这里改用中断位
     *    DBI_INTSTS_TX_FIFO（FIFO 空），只在 s_drain_experiment 打开时生效。
     *    实测（2026-10-16 晚，真机整分复现时）：多等的块=0，即 TC 通过时 FIFO
     *    本来就是空的 —— "TC 提前置位、FIFO 还有货"这条机制**被否定**了。
     */
    while ((bflb_dbi_get_intstatus(dbi_dev) & DBI_INTSTS_TC) == 0) {
        if (++s_push_tc > 50000000u) break;   /* 保险：真卡住也别把机器挂死 */
    }
    if (s_drain_experiment) {
        /*
         * 先短路一次：若 TX_FIFO 已经置位（FIFO 本来就是空的），
         * 说明这一块根本不需要多等 —— 不把它算进"真的等了"的块数。
         */
        if ((bflb_dbi_get_intstatus(dbi_dev) & DBI_INTSTS_TX_FIFO) == 0) {
            uint32_t spin = 0;

            s_drain_n++;
            while ((bflb_dbi_get_intstatus(dbi_dev) & DBI_INTSTS_TX_FIFO) == 0) {
                if (++spin > 5000000u) break;  /* 保险 */
            }
            if (spin > s_drain_spin_max) s_drain_spin_max = spin;
        }
    }

    /*
     * ★★ 2026-10-16 晚：块间"收窗"保护（真机整分错位的最小候选修复）★★
     *
     * 真机事实（本轮实测，证据齐）：
     *   · 整分那一帧 = 两块**背靠背**的大 blit：30600px + 28560px，分界 y=180/181
     *     正好横穿大数字中部；
     *   · 两块的像素字节与主机预测**逐字节相同**（同一时刻 CRC 一致）；
     *   · 一次大 blit 能上屏（fill 实测）、两次大 blit 背靠背也能上屏（fill 实测）、
     *     排空等待无差别（drain 实测 0 块）；
     *   · 但**真机照片**上，四个数字都在同一高度出现横向错位，且
     *     "右滑再左滑（整屏重绘）就恢复正常"。
     *
     * 唯一还没被排除的是"块与块之间面板/DBI 的窗口相位"：上一块的像素发完了，
     * 但面板侧的写指针/窗口锁存可能还没落定，紧接着的 CASET/RASET 就被它吃到
     * 自己的窗口设置里。**注意：这里不是等 FIFO 空（那个已证明没用），而是等一个
     * 固定的短保护时间**，专门针对"面板侧落定"这种量级。
     *
     * 默认关闭（s_win_guard_us = 0），生产路径零影响；shell `winguard <us>` 现场 A/B。
     */
    if (s_win_guard_us) {
        bflb_mtimer_delay_us(s_win_guard_us);
    }
    {
        /* 本块收尾状态全部记进 RAM，串口事后 dump（推送时绝不打印） */
        blit_log_t *e = &s_blit_log[s_blit_log_i % BLIT_LOG_N];
        uint32_t sts = bflb_dbi_get_intstatus(dbi_dev);

        e->npix = npix;
        e->intsts = sts;
        e->crc = ~s_push_crc;                /* 反射 CRC32 的收尾取反 */
        e->addr = s_win_addr;
        e->nsent = s_blit_n;
        e->x1 = s_win_x1;
        e->y1 = s_win_y1;
        e->x2 = s_win_x2;
        e->y2 = s_win_y2;
        e->solid = buf ? 0 : 1;
        e->iters = (uint16_t)(s_push_iters > 0xFFFF ? 0xFFFF : s_push_iters);
        e->zero = (uint16_t)(s_push_zero > 0xFFFF ? 0xFFFF : s_push_zero);
        e->tc_spin = (uint16_t)(s_push_tc > 0xFFFF ? 0xFFFF : s_push_tc);
        e->fifo_err = (sts & DBI_INTSTS_FIFO_ERR) ? 1 : 0;
        e->tx_fifo = (sts & DBI_INTSTS_TX_FIFO) ? 1 : 0;
        /* 大块另存一份到"不会被小块冲掉"的环里（见 s_blit_big 的说明） */
        if (npix >= BLIT_BIG_PX) {
            s_blit_big[s_blit_big_i % BLIT_BIG_N] = *e;
            s_blit_big_i++;
        }
        s_blit_log_i++;
        s_blit_n++;
        s_blit_px += npix;
        if (e->fifo_err) s_fifo_err_n++;
        if (s_push_zero) s_zero_n++;
        if (s_push_tc > s_tc_max) s_tc_max = s_push_tc;
        /*
         * ⚠️ 必须在这里清零：上一版忘了清，tc_spin 变成**累计值**，
         * 每秒那 10 个 ~1700px 的小块把整分两块（30600/28560px）的真实等待
         * 时间完全盖住了 —— 探针本身骗了我们一轮。每块记完就归零。
         */
        s_push_iters = s_push_zero = s_push_tc = 0;
    }
    bflb_dbi_int_clear(dbi_dev, DBI_INTCLR_TC);
}

/** 把 0xRRGGBB 打成 NRGB_8888 的 32 位输入字（内存序 B,G,R,X） */
static uint32_t st7802_pack888(uint32_t rgb)
{
    return ((rgb >> 16) & 0xFF) << 16 | ((rgb >> 8) & 0xFF) << 8 | (rgb & 0xFF);
}

/** RGB888 路径：一个像素一个 32 位字 */
static void st7802_push_pixels888(uint32_t npix, const uint32_t *buf, uint32_t solid)
{
    uint32_t words;
    uint32_t i = 0;
    uint8_t cnt;

    st7802_cfg(wr_hz, cur_clk, cur_addr, pix_wire);
    bflb_dbi_qspi_set_addr(dbi_dev, QSPI_ADDR_LEN, (uint32_t)0x2C << 8);

    words = bflb_dbi_feature_control(dbi_dev, DBI_CMD_GET_SIZE_OF_PIXEL_CNT, npix) / 4;
    bflb_dbi_send_cmd_pixel(dbi_dev, pix_op, npix, NULL);

    while (words > 0) {
        cnt = (uint8_t)bflb_dbi_feature_control(dbi_dev, DBI_CMD_GET_TX_FIFO_CNT, 0);
        if (cnt > words) {
            cnt = (uint8_t)words;
        }
        words -= cnt;
        while (cnt--) {
            putreg32(buf ? buf[i++] : solid, DMA_ADDR_DBI_TDR);
        }
    }

    while ((bflb_dbi_get_intstatus(dbi_dev) & DBI_INTSTS_TC) == 0) {
    }
    bflb_dbi_int_clear(dbi_dev, DBI_INTCLR_TC);
}

int st7802_get_fmt565(void)
{
    return s_fmt565;
}

/* 切像素格式：重建 DBI + 写面板 COLMOD，并回读确认 */
int st7802_set_fmt565(int on)
{
    uint8_t want = on ? 0x55 : 0x77;
    uint8_t got = 0;

    s_fmt565 = on ? 1 : 0;
    dbi_ready = 0; /* 强制下次 cfg 重建 DBI */

    st7802_write_cmd(0x3A, &want, 1);
    bflb_mtimer_delay_ms(2);
    st7802_read_cmd(0x0C, &got, 1);

    if (got != want) {
        printf("  !! COLMOD: wrote 0x%02X but panel reports 0x%02X\r\n", want, got);
        return -1;
    }
    return 0;
}

/**
 * COLMOD 探测：面板到底接受哪些像素格式？
 *
 * 起因：2026-09-26 想切 RGB565 省一半带宽，结果写 0x3A=0x55 之后
 *       RDDCOLMOD 一直读回 0x77 —— 而 0x77 本来就是默认值，
 *       所以之前写 0x77「成功」其实是假象，从没验证过这个寄存器能写。
 * 这个函数把几个常见值依次写进去再读回来，把真相打出来。
 *
 * 注意：结束时会把格式恢复成 RGB888（默认那条能跑通的路）。
 */
void st7802_probe_colmod(void)
{
    static const uint8_t cand[] = { 0x77, 0x66, 0x55, 0x05, 0x50, 0x65, 0x56 };
    static const char *note[] = {
        "24bpp RGB888 (default)", "18bpp RGB666", "IFPF=5 16bpp RGB565",
        "IFPF=5 另一种位序", "IFPF=5 另一种位序", "?", "?"
    };
    uint8_t rb;
    int i;

    printf("--- COLMOD (3Ah) probe ---\r\n");
    for (i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
        st7802_write_cmd(0x3A, (uint8_t *)&cand[i], 1);
        bflb_mtimer_delay_ms(2);
        rb = 0;
        st7802_read_cmd(0x0C, &rb, 1);
        printf("  write 0x%02X -> read 0x%02X  %s%s\r\n",
               cand[i], rb, note[i],
               (rb == cand[i]) ? "   <== ACCEPTED" : "");
    }
    /* 恢复 */
    st7802_set_fmt565(0);
    printf("  restored to RGB888, readback = ");
    rb = 0;
    st7802_read_cmd(0x0C, &rb, 1);
    printf("0x%02X\r\n", rb);
    printf("--------------------------\r\n");
}

void st7802_fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint32_t color)
{
    uint32_t npix;

    if (x2 < x1 || y2 < y1) {
        return;
    }
    st7802_set_window(x1, y1, x2, y2);
    npix = (uint32_t)(x2 - x1 + 1) * (uint32_t)(y2 - y1 + 1);
    if (s_fmt565) {
        st7802_push_pixels565(npix, NULL, st7802_pack565(color));
    } else {
        st7802_push_pixels888(npix, NULL, st7802_pack888(color));
    }
}

/** RGB888 刷图：buf 是 w*h 个 uint32，每像素 0x00RRGGBB */
void st7802_blit888(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint32_t *buf)
{
    uint32_t npix;

    if (x2 < x1 || y2 < y1 || buf == NULL) {
        return;
    }
    st7802_set_window(x1, y1, x2, y2);
    npix = (uint32_t)(x2 - x1 + 1) * (uint32_t)(y2 - y1 + 1);
    s_win_x1 = x1; s_win_y1 = y1; s_win_x2 = x2; s_win_y2 = y2;
    s_win_addr = (uint32_t)(uintptr_t)buf;
    st7802_push_pixels888(npix, buf, 0);
}

#endif /* !ST7802_HOST_VERIFY */

/** RGB565 刷图：buf 是 w*h 个 uint16（bit15..11=R, 10..5=G, 4..0=B） */
void st7802_blit565(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint16_t *buf)
{
    uint32_t npix;

    if (x2 < x1 || y2 < y1 || buf == NULL) {
        return;
    }
    st7802_set_window(x1, y1, x2, y2);
    npix = (uint32_t)(x2 - x1 + 1) * (uint32_t)(y2 - y1 + 1);
    s_win_x1 = x1; s_win_y1 = y1; s_win_x2 = x2; s_win_y2 = y2;
    s_win_addr = (uint32_t)(uintptr_t)buf;
    st7802_push_pixels565(npix, buf, 0);
}

/* ------------------------------------------------------------------ */
/* 诊断：编码自证图案 pxpat（横跨 band 边界）                           */
/* ------------------------------------------------------------------ */

/*
 * 为什么不用棋盘格：
 *   棋盘格只能回答"坏没坏"，而且**周期性图案会掩盖周期性错位**
 *   （图案和错位同周期时对不上号）。把坐标本身编码进像素以后，回读的
 *   每个像素都自报"我在哪"。
 *
 * 编码（RGB565）：R5 = x & 0x1F, G6 = y & 0x3F, B5 = 0x0A(常量)
 *   - 每 32 列红色回卷一次、每 64 行绿色回卷一次；
 *   - 蓝色常量用来判"是不是我这块屏/这个通道"。
 *   ⚠️ 已知局限（主机自检里写死了，见 tools/host/st7802_verify.c）：
 *      R5 的周期是 32 列，所以**横向不超过 32 像素的错位，本图案在
 *      原理上测不出来**（x 与 x+32 编出同一个值）。它能可靠回答的是
 *      "哪个坐标被写到了哪里"（纵向错位、丢字、半字互换、整行位移），
 *      横向细错位要靠蓝色通道另编一维 —— 本轮没做。
 *
 * 逐行发射：每一行都调一次**真实** st7802_blit565()（绿色通道编的是该行
 * 自己的 y，行与行不能共用缓冲）。所以写一个 480x480 的 pxpat 就是 480
 * 次真实 blit，天然横跨 y=64/128/192… 这些 band 边界，边界效应直接被画进去。
 * 不经 LVGL、不动任何 UI 对象。
 */
#define PXPAT_BAND_ROWS 64
#define PXPAT_MAX_W 480

uint32_t st7802_pxpat(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, int on)
{
    uint32_t px = 0;
    uint32_t rows;
    uint32_t y;

    if (x2 < x1 || y2 < y1 || x2 >= ST7802_WIDTH || y2 >= ST7802_HEIGHT) {
        return 0;
    }
    if ((uint32_t)(x2 - x1 + 1) > PXPAT_MAX_W) {
        return 0;
    }

    st7802_pxpat_reset();
    rows = (uint32_t)(y2 - y1 + 1);
    /*
     * 分段：每段对齐 LVGL 的 band 边界（64 行），这样图案必然跨过
     * y=64/128/192… 这些边界，边界效应会被直接编进图案里。
     *
     * ⚠️ 每一**行**都要重新编码：绿色通道编的是 y，行与行不能共用同一
     *    行缓冲（第一版就是整段共用，等于把第一行复制了 n 遍）。
     */
    for (y = 0; y < rows; ) {
        uint32_t n = PXPAT_BAND_ROWS - ((y1 + y) % PXPAT_BAND_ROWS); /* 对齐 band 边界 */
        uint32_t k;

        if (n > rows - y) {
            n = rows - y;
        }
        for (uint32_t r = 0; r < n; r++) {
            /* 每一行都按自己的**绝对 y** 重新编码（绿色通道编的是 y） */
            for (k = 0; k < (uint32_t)(x2 - x1 + 1); k++) {
                uint32_t x = (uint32_t)x1 + k;
                st7802_pxpat_put(k, on ? pxpat_value(x, (uint32_t)y1 + y + r) : 0x0000u);
            }
            pxpat_emit(x1, (uint16_t)(y1 + y + r), x2, (uint16_t)(y1 + y + r),
                       st7802_pxpat_line());
            st7802_pxpat_bump((uint32_t)(x2 - x1 + 1));
        }
        px += (uint32_t)(x2 - x1 + 1) * n;
        y += n;
    }
    return px;
}

/** 上一次 pxpat 被切成几段（= 跨过几个 band 边界，可核对用） */
uint32_t st7802_pxpat_blocks(void)
{
    return st7802_pxpat_get_blocks();
}

uint32_t st7802_pxpat_pixels(void)
{
    return st7802_pxpat_get_px();
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

#ifndef ST7802_HOST_VERIFY

/*
 * 初始化序列来自厂商 esp_lcd_st7802_spi.c 的 vendor_specific_init_default
 * 以及 docs/Truly160_480x480_ST7802N_AMOLED_Mipi_init.txt。
 * ST7802 内部 OTP 会自己把电源/Gamma 一大堆寄存器配好，外面只要发这几条。
 */
struct st7802_init_cmd {
    uint8_t cmd;
    uint8_t data[2];
    uint8_t len;
    uint16_t delay_ms;
};

static const struct st7802_init_cmd init_cmds[] = {
    { 0x36, { 0x00 }, 1, 0 },   /* MADCTL */
    /*
     * ⚠️ COLMOD(3Ah) 不在这里写！
     *
     * 踩过的坑（2026-09-26）：原来把 0x3A 放在 SLPOUT 之前，结果
     *   - 写 0x3A=0x77 时读回来是 0x77  —— 看着「成功」，其实 0x77 就是默认值
     *   - 写 0x3A=0x55 想切 RGB565 时读回来还是 0x77 —— 根本没写进去
     * 也就是**这个寄存器在 SLPOUT 之前写的值会被面板丢掉**。
     * 手动在 init 之后写（fmt 命令 / lcdprobe）就一切正常：
     *     write 0x77 -> 0x77 ACCEPTED
     *     write 0x66 -> 0x66 ACCEPTED
     *     write 0x55 -> 0x55 ACCEPTED
     * 所以现在改成 init 序列跑完（DISPON 之后）再由 st7802_init() 统一写 + 回读校验。
     */
    { 0x11, { 0x00 }, 0, 120 }, /* SLPOUT */
    { 0x53, { 0x20 }, 1, 0 },   /* WRCTRLD */
    { 0x51, { 0xFF }, 1, 0 },   /* WRDISBV，亮度最大 */
    { 0x35, { 0x00 }, 1, 0 },   /* TEON */
    { 0x29, { 0x00 }, 0, 20 },  /* DISPON */
};

int st7802_init(void)
{
    dbi_dev = bflb_device_get_by_name("dbi");
    if (dbi_dev == NULL) {
        return -1;
    }

    st7802_pins_init();

    /* PMIC 上电后等一下再复位面板 */
    bflb_gpio_set(gpio_dev, PIN_VCI_EN);
    bflb_mtimer_delay_ms(20);

    st7802_cfg(ST7802_WRITE_HZ, DBI_CLOCK_MODE_1, DBI_QSPI_WIRE_NUM_1, DBI_QSPI_WIRE_NUM_1);
    st7802_reset();

    for (uint32_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) {
        st7802_write_cmd(init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].len);
        if (init_cmds[i].delay_ms) {
            bflb_mtimer_delay_ms(init_cmds[i].delay_ms);
        }
    }

    /* 面板已经 DISPON，这时候写 COLMOD 才留得住；带回读校验 */
    printf("  COLMOD set -> %s\r\n", st7802_set_fmt565(s_fmt565) == 0 ? "OK" : "FAILED");

    return 0;
}

#endif /* !ST7802_HOST_VERIFY */

/* ------------------------------------------------------------------ */
/* 诊断：读回自检 + 配置扫描                                          */
/* ------------------------------------------------------------------ */

#ifndef ST7802_HOST_VERIFY

static void read_one(uint8_t cmd, uint8_t len, uint8_t *out)
{
    for (uint8_t i = 0; i < len; i++) {
        out[i] = 0;
    }
    st7802_read_cmd(cmd, out, len);
}

void st7802_read_id_report(void)
{
    uint8_t b[4];

    printf("--- panel read-back self test ---\r\n");

    read_one(0x04, 3, b);
    printf("  RDDID    (04h) = %02X %02X %02X\r\n", b[0], b[1], b[2]);

    read_one(0x0A, 1, b);
    printf("  RDDPM    (0Ah) = %02X\r\n", b[0]);

    read_one(0x0B, 1, b);
    printf("  RDDMADCTR(0Bh) = %02X\r\n", b[0]);

    read_one(0x0C, 1, b);
    printf("  RDDCOLMOD(0Ch) = %02X   (当前格式: %s)\r\n", b[0],
           (b[0] == 0x55) ? "RGB565 16bpp" :
           (b[0] == 0x66) ? "RGB666 18bpp" :
           (b[0] == 0x77) ? "RGB888 24bpp" : "??");

    read_one(0x0D, 1, b);
    printf("  RDDIM    (0Dh) = %02X\r\n", b[0]);

    read_one(0x0E, 1, b);
    printf("  RDDSM    (0Eh) = %02X\r\n", b[0]);

    read_one(0x0F, 1, b);
    printf("  RDDSDR   (0Fh) = %02X\r\n", b[0]);
    printf("---------------------------------\r\n");
}

void st7802_read_sweep(void)
{
    static const uint8_t clks[] = { DBI_CLOCK_MODE_0, DBI_CLOCK_MODE_1, DBI_CLOCK_MODE_3 };
    static const char *clkn[] = { "CPOL0/CPHA0", "CPOL0/CPHA1", "CPOL1/CPHA1" };
    static const uint8_t addrs[] = { DBI_QSPI_WIRE_NUM_1, DBI_QSPI_WIRE_NUM_4 };
    uint8_t b;
    int any = 0;

    printf("--- read sweep (RDDCOLMOD 0Ch, expect 55) ---\r\n");
    for (uint32_t ci = 0; ci < 3; ci++) {
        for (uint32_t ai = 0; ai < 2; ai++) {
            st7802_cfg(ST7802_READ_HZ, clks[ci], addrs[ai], DBI_QSPI_WIRE_NUM_1);
            b = 0;
            st7802_read_cmd(0x0C, &b, 1);
            printf("  clk=%s addr=%s -> 0x%02X%s\r\n", clkn[ci],
                   addrs[ai] == DBI_QSPI_WIRE_NUM_1 ? "1wire" : "4wire", b,
                   (b == 0x55) ? "   <== OK" : "");
            if (b == 0x55) {
                any = 1;
            }
        }
    }
    printf("  sweep %s\r\n", any ? "OK" : "FAIL");
    printf("--------------------------------------------\r\n");

    st7802_cfg(wr_hz, DBI_CLOCK_MODE_1, DBI_QSPI_WIRE_NUM_1, DBI_QSPI_WIRE_NUM_1);
}

/*
 * 读时钟扫描。
 *
 * 背景：第一次测的时候 30MHz 读回全 0x00，我据此得出"读上限约 3.3MHz"
 * （手册 7.5.3 也写了 Serial clock cycle(Read) 最小 300ns）。
 * 但那次接线是错的，所以结论需要重新验证 —— 这个函数就是干这个的。
 */
void st7802_read_clock_sweep(void)
{
    static const uint32_t hz_list[] = {
        1000000, 2000000, 4000000, 6000000, 8000000,
        10000000, 15000000, 20000000, 30000000, 40000000
    };
    uint8_t b;
    uint32_t last_good = 0;
    int first_bad = -1;

    printf("--- read clock sweep (RDDCOLMOD 0Ch, expect 55) ---\r\n");
    printf("  mode = CPOL0/CPHA1, addr 1wire, data 1wire\r\n");

    for (uint32_t i = 0; i < sizeof(hz_list) / sizeof(hz_list[0]); i++) {
        uint32_t save = st7802_get_read_hz();
        int ok;

        st7802_set_read_hz(hz_list[i]);
        /* 每个频率读 3 次，3 次全对才算这个频率稳 */
        ok = 1;
        for (int k = 0; k < 3; k++) {
            b = 0;
            st7802_read_cmd(0x0C, &b, 1);
            if (b != 0x55) {
                ok = 0;
                break;
            }
        }
        printf("  read @ %2u MHz -> 0x%02X  %s\r\n",
               (unsigned)(hz_list[i] / 1000000), b, ok ? "OK" : "<== FAIL");

        if (ok) {
            last_good = hz_list[i];
        } else if (first_bad < 0) {
            first_bad = (int)hz_list[i];
        }
        (void)save;
    }

    printf("  最高可靠读时钟 = %u MHz", (unsigned)(last_good / 1000000));
    if (first_bad > 0) {
        printf("，%u MHz 开始失败", (unsigned)(first_bad / 1000000));
    }
    printf("\r\n");
    printf("----------------------------------------------------\r\n");

    /* 默认恢复到 2MHz：读不是性能瓶颈，稳一点好 */
    st7802_set_read_hz(ST7802_READ_HZ);
}

/*
 * 写时钟扫描：用待测频率写一个 COLMOD 值，再用 2MHz 读回来比对。
 * 读得回来就说明那个写频率可靠（写路径自检）。
 */
void st7802_write_clock_sweep(void)
{
    static const uint32_t hz_list[] = {
        5000000, 10000000, 15000000, 20000000, 25000000,
        30000000, 40000000, 50000000, 60000000, 80000000
    };
    uint8_t val = 0x66;
    uint8_t b;
    uint32_t last_good = 0;
    int first_bad = -1;
    uint32_t save_rd = st7802_get_read_hz();

    printf("--- write clock sweep (write COLMOD, read back at 2MHz) ---\r\n");

    st7802_set_read_hz(2000000);

    for (uint32_t i = 0; i < sizeof(hz_list) / sizeof(hz_list[0]); i++) {
        int ok;

        /* 每轮换一个值，确保读回来的是本轮写进去的 */
        val = (val == 0x66) ? 0x55 : 0x66;

        st7802_set_write_hz(hz_list[i]);
        st7802_write_cmd(0x3A, &val, 1);

        ok = 1;
        for (int k = 0; k < 3; k++) {
            b = 0;
            st7802_read_cmd(0x0C, &b, 1);
            if (b != val) {
                ok = 0;
                break;
            }
        }
        printf("  write @ %2u MHz -> wrote 0x%02X, read 0x%02X  %s\r\n",
               (unsigned)(hz_list[i] / 1000000), val, b, ok ? "OK" : "<== FAIL");

        if (ok) {
            last_good = hz_list[i];
        } else if (first_bad < 0) {
            first_bad = (int)hz_list[i];
        }
    }

    printf("  最高可靠写时钟 = %u MHz", (unsigned)(last_good / 1000000));
    if (first_bad > 0) {
        printf("，%u MHz 开始失败", (unsigned)(first_bad / 1000000));
    }
    printf("\r\n");
    printf("-----------------------------------------------------------\r\n");

    /* 恢复：写回 RGB565，时钟回默认 */
    val = 0x55;
    st7802_set_write_hz(ST7802_WRITE_HZ);
    st7802_write_cmd(0x3A, &val, 1);
    st7802_set_read_hz(save_rd);
}

#endif /* !ST7802_HOST_VERIFY */

/* ================================================================== */
/* 主机验证构建（-DST7802_HOST_VERIFY）                                */
/*                                                                    */
/* 目的：让"探针本身"能被独立 assert —— CRC 向量、pxpat 编码、          */
/* gramrd 的 dummy 偏移分析、整分取证状态机，都在主机上用**同一份源码** */
/* 跑。只把"真的碰寄存器/面板"的几处换成内存模型。                       */
/* 使用：lvgl_test/tools/host/st7802_verify.c                          */
/* ================================================================== */
#ifdef ST7802_HOST_VERIFY

#define SIM_W 480
#define SIM_H 480

static uint16_t s_simgram[SIM_W * SIM_H];
static uint16_t s_sim_x, s_sim_y;   /* 当前窗口（st7802_set_window 记下） */
static uint16_t s_sim_w;            /* 当前窗口宽 */
static uint8_t s_sim_const;         /* 桩行为：回读恒为这个值 */
static int s_sim_const_on;
static int s_sim_ptr_reset;         /* 桩行为：读指针每个事务复位 */

void st7802_sim_const(int on, uint8_t v)
{
    s_sim_const_on = on;
    s_sim_const = v;
}

void st7802_sim_pointer_reset(int on)
{
    s_sim_ptr_reset = on;
}

void st7802_sim_store(uint32_t x, uint32_t y, uint16_t v)
{
    if (x < SIM_W && y < SIM_H) {
        s_simgram[y * SIM_W + x] = v;
    }
}

uint16_t st7802_sim_load(uint32_t x, uint32_t y)
{
    return (x < SIM_W && y < SIM_H) ? s_simgram[y * SIM_W + x] : 0;
}

/*
 * 主机桩：按 565 小端从模拟 GRAM 顺序取字节。
 *   s_sim_ptr_reset = 0 -> 模拟"面板支持 RAMRD 且读指针跨事务继续推进"
 *   s_sim_ptr_reset = 1 -> 模拟"读指针在每个 8 字节事务处复位"
 * 后者用来证明真机上"每块读回同一组字节"时该怎么判。
 */
uint32_t st7802_hw_read(uint8_t cmd, uint8_t *buf, uint32_t len, uint8_t chunk)
{
    uint32_t got = 0;
    uint32_t px = 0;

    (void)cmd;
    if (chunk == 0) {
        chunk = 8;
    }
    if (s_sim_const_on) {
        memset(buf, s_sim_const, len);
        return len;
    }
    while (got < len) {
        uint32_t local = s_sim_ptr_reset ? (got % chunk) : got;
        uint32_t k = local / 2;
        uint32_t xx = s_sim_x + (s_sim_w ? (k % s_sim_w) : k);
        uint32_t yy = s_sim_y + (s_sim_w ? (k / s_sim_w) : 0);
        uint16_t v = st7802_sim_load(xx, yy);

        buf[got++] = (local & 1) ? (uint8_t)(v >> 8) : (uint8_t)(v & 0xFF);
        px++;
    }
    (void)px;
    return len;
}

void st7802_sim_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    s_sim_x = x1;
    s_sim_y = y1;
    s_sim_w = (uint16_t)(x2 - x1 + 1);
}

/* 主机桩：写命令 —— CASET/RASET 只要把窗口记下来 */
int st7802_write_cmd(uint8_t cmd, const uint8_t *data, uint32_t len)
{
    if (cmd == 0x2A && len == 4) {
        s_sim_x = (uint16_t)((data[0] << 8) | data[1]);
        s_sim_w = (uint16_t)(((data[2] << 8) | data[3]) - s_sim_x + 1);
    } else if (cmd == 0x2B && len == 4) {
        s_sim_y = (uint16_t)((data[0] << 8) | data[1]);
    }
    return 0;
}

/* 主机桩：读命令 —— 给 RDDID 自检返回固定向量，让它走通 */
int st7802_read_cmd(uint8_t cmd, uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        data[i] = (uint8_t)(cmd + i);
    }
    return 0;
}

void st7802_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t p[4];

    p[0] = (uint8_t)(x1 >> 8);
    p[1] = (uint8_t)(x1 & 0xFF);
    p[2] = (uint8_t)(x2 >> 8);
    p[3] = (uint8_t)(x2 & 0xFF);
    st7802_write_cmd(0x2A, p, 4);

    p[0] = (uint8_t)(y1 >> 8);
    p[1] = (uint8_t)(y1 & 0xFF);
    p[2] = (uint8_t)(y2 >> 8);
    p[3] = (uint8_t)(y2 & 0xFF);
    st7802_write_cmd(0x2B, p, 4);
}

/* 主机桩：565 推送 —— 只把"面板应该收到的字"算 CRC 记进 blit log，不碰 DBI */
static void st7802_push_pixels565(uint32_t npix, const uint16_t *buf, uint16_t solid)
{
    blit_log_t *e;

    s_push_crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < npix; i += 2) {
        uint16_t a = buf ? buf[i] : solid;
        uint16_t b = (i + 1 < npix) ? (buf ? buf[i + 1] : solid) : a;
        uint32_t w = s_px_swap ? (((uint32_t)a << 16) | (uint32_t)b)
                               : (((uint32_t)b << 16) | (uint32_t)a);
        for (int k = 0; k < 4; k++) {
            s_push_crc ^= (w >> (k * 8)) & 0xFFu;
            for (int j = 0; j < 8; j++) {
                s_push_crc = (s_push_crc >> 1) ^
                             (0xEDB88320u & (uint32_t)(-(int32_t)(s_push_crc & 1u)));
            }
        }
    }

    e = &s_blit_log[s_blit_log_i % BLIT_LOG_N];
    e->npix = npix;
    e->crc = ~s_push_crc;
    e->addr = (uint32_t)(uintptr_t)buf;
    e->nsent = s_blit_n;
    e->x1 = s_win_x1; e->y1 = s_win_y1; e->x2 = s_win_x2; e->y2 = s_win_y2;
    e->solid = buf ? 0 : 1;
    e->intsts = 0; e->iters = 0; e->zero = 0; e->tc_spin = 0;
    e->fifo_err = 0; e->tx_fifo = 0;
    s_blit_log_i++;
    s_blit_n++;
    s_blit_px += npix;
}
#endif /* ST7802_HOST_VERIFY */

