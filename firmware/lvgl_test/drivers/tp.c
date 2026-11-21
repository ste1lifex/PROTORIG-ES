/**
 * @file  tp.c
 * @brief CST9220 触摸驱动
 */
#include "tp.h"

#include <stdio.h>
#include <string.h>

#include "bflb_gpio.h"
#include "bflb_mtimer.h"

#include "i2c_bus.h"

static struct bflb_device_s *gpio_dev = NULL;

void tp_hw_reset(void)
{
    if (gpio_dev == NULL) {
        return;
    }
    /*
     * CST9220 手册 10.5 节的上电/复位时序：
     *   TRST  复位脉冲时间      最小 0.1 ms  -> 这里给 10 ms
     *   TRON  复位后重新初始化  典型 100 ms  -> 这里给 150 ms
     * （原来只等 60 ms，比手册少 40 ms）
     * 另外 RSTn 内置上拉 + RC 滤波，手册说可以悬空不接。
     */
    bflb_gpio_reset(gpio_dev, TP_RST_PIN);
    bflb_mtimer_delay_ms(10);
    bflb_gpio_set(gpio_dev, TP_RST_PIN);
    bflb_mtimer_delay_ms(150);
}

int tp_int_level(void)
{
    if (gpio_dev == NULL) {
        return 1;
    }
    return bflb_gpio_read(gpio_dev, TP_INT_PIN) ? 1 : 0;
}

int tp_int_asserted(void)
{
    return tp_int_level() == 0;
}

int tp_init(void)
{
    gpio_dev = bflb_device_get_by_name("gpio");

    /* TP_RST：输出，先拉低 */
    bflb_gpio_init(gpio_dev, TP_RST_PIN, GPIO_OUTPUT | GPIO_PULLUP | GPIO_DRV_1);
    bflb_gpio_reset(gpio_dev, TP_RST_PIN);

    /* TP_INT：输入，低有效，上拉 */
    bflb_gpio_init(gpio_dev, TP_INT_PIN,
                   GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);

    /*
     * I2C0（IO0/IO1）由 i2c_bus_init() 统一初始化（与 SD3010 共用一条总线）。
     * main() 里的顺序是 i2c_bus_init() -> sd3010_init() -> tp_init()，
     * 所以这里只做触摸自己的复位时序。
     */
    printf("[tp] I2C0 shared bus @ %lu Hz (CST9220 0x%02X)\r\n",
           (unsigned long)I2C_BUS_FREQ_HZ, TP_ADDR_DEFAULT);

    /* 上电后要等 TPOR = 100 ms 芯片才初始化完，再打复位 */
    bflb_mtimer_delay_ms(150);
    tp_hw_reset();

    return 0;
}

int tp_read_reg16(uint8_t addr7, uint16_t reg, uint8_t *data, uint32_t len)
{
    return i2c_bus_read_regs16(addr7, reg, data, len);
}

int tp_write_reg16(uint8_t addr7, uint16_t reg, const uint8_t *data, uint32_t len)
{
    return i2c_bus_write_regs16(addr7, reg, data, len);
}

int tp_cst9220_info(uint8_t addr7)
{
    uint8_t data[4];
    uint8_t cmd_mode[2] = { 0xD1, 0x01 };
    uint16_t res_x, res_y, chip_type, project_id;

    memset(data, 0, sizeof(data));

    /* 关键：不进命令模式的话读出来全是 0xFFFF */
    if (tp_write_reg16(addr7, TP_REG_CMD_MODE, cmd_mode, sizeof(cmd_mode)) < 0) {
        printf("    write CMD_MODE failed\r\n");
        return -1;
    }
    bflb_mtimer_delay_ms(10);

    if (tp_read_reg16(addr7, TP_REG_CHECKCODE, data, 4) < 0) {
        printf("    read CHECKCODE failed\r\n");
        return -2;
    }
    printf("    checkcode   = %02X %02X %02X %02X\r\n", data[0], data[1], data[2], data[3]);

    memset(data, 0, sizeof(data));
    if (tp_read_reg16(addr7, TP_REG_RESOLUTION, data, 4) < 0) {
        printf("    read RESOLUTION failed\r\n");
        return -3;
    }
    res_x = (uint16_t)((data[1] << 8) | data[0]);
    res_y = (uint16_t)((data[3] << 8) | data[2]);
    printf("    resolution  = X %u, Y %u\r\n", res_x, res_y);

    memset(data, 0, sizeof(data));
    if (tp_read_reg16(addr7, TP_REG_PROJECT_ID, data, 4) < 0) {
        printf("    read PROJECT_ID failed\r\n");
        return -4;
    }
    project_id = (uint16_t)((data[1] << 8) | data[0]);
    chip_type = (uint16_t)((data[3] << 8) | data[2]);
    printf("    project_id  = 0x%04X, chip_type = 0x%04X\r\n", project_id, chip_type);
    printf("    TP_INT(IO%d) = %d (0 = touched)\r\n", TP_INT_PIN, tp_int_level());

    /*
     * ⚠️ 厂商《触摸芯片移植手册 v3.2》明确要求：
     *    「读完版本信息必须退回 normal 模式，否则触摸数据异常」。
     *    而我们的 tp.c、厂商自己的 ESP-IDF 驱动、Waveshare cst9217、
     *    ESPHome cst9220 组件**全都没写这一条**。
     *    这很可能就是 README §6.7 那个「88% 帧 status=0 回显」的根源。
     */
    if (tp_set_mode(addr7, TP_CMD_NORMAL) < 0) {
        printf("    !! 写回 NORMAL 模式失败\r\n");
        return -5;
    }
    bflb_mtimer_delay_ms(10);
    {
        uint8_t ms[2] = { 0, 0 };
        if (tp_read_mode_status(addr7, ms) == 0) {
            printf("    mode echo   = %02X %02X (期望低字节 09)\r\n", ms[0], ms[1]);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 模式命令 / 手势 / 接近感应                                          */
/* ------------------------------------------------------------------ */

int tp_raw_write(uint8_t addr7, const uint8_t *data, uint32_t len)
{
    return i2c_bus_write_raw(addr7, data, len);
}

/* CST92XX: D1 06 = 退出低功耗扫描（2 字节裸命令，非寄存器写） */
int tp_exit_lp_scan(void)
{
    uint8_t cmd[2] = { 0xD1, 0x06 };
    return tp_raw_write(TP_ADDR_DEFAULT, cmd, sizeof(cmd));
}

/* CST92XX: D1 07 = 进入低功耗扫描（2 字节裸命令） */
int tp_enter_lp_scan(void)
{
    uint8_t cmd[2] = { 0xD1, 0x07 };
    return tp_raw_write(TP_ADDR_DEFAULT, cmd, sizeof(cmd));
}


int tp_set_mode(uint8_t addr7, uint8_t cmd)
{
    uint8_t d[2];

    d[0] = 0xD1;
    d[1] = cmd;
    return tp_write_reg16(addr7, TP_REG_CMD_MODE, d, 2);
}

int tp_read_mode_status(uint8_t addr7, uint8_t out[2])
{
    return tp_read_reg16(addr7, TP_REG_MODE_STATUS, out, 2);
}

int tp_gesture(uint8_t addr7, int on)
{
    uint8_t cmd[3];
    uint8_t v = 0;

    cmd[0] = 0xD0;
    cmd[1] = 0x4C;
    cmd[2] = on ? 0x80 : 0x00;

    if (tp_raw_write(addr7, cmd, 3) < 0) {
        return -1;
    }
    bflb_mtimer_delay_ms(10);

    if (tp_read_reg16(addr7, TP_REG_GESTURE, &v, 1) < 0) {
        return -1;
    }
    return v;
}

int tp_proximity(uint8_t addr7, int on)
{
    uint8_t cmd[3];
    uint8_t v = 0;

    cmd[0] = 0xD0;
    cmd[1] = 0x4B;
    cmd[2] = on ? 0x80 : 0x00;

    if (tp_raw_write(addr7, cmd, 3) < 0) {
        return -1;
    }
    bflb_mtimer_delay_ms(10);

    if (tp_read_reg16(addr7, TP_REG_PROXIMITY, &v, 1) < 0) {
        return -1;
    }
    return v;
}

/* V1.6 PDS 低功耗：厂商唯一实际在用的低功耗扫描路径是 gesture 模式 D0 4C 80，
 * 它仍然会在触摸时拉 INT（用来叫醒 MCU）；醒来用硬件复位回到 normal。
 * 注意：gesture 模式不一定还报正常坐标，所以只用于"息屏睡眠期间"。 */
int tp_lowpower_enter(void)
{
    return tp_gesture(TP_ADDR_DEFAULT, 1);
}

int tp_lowpower_exit(void)
{
    int r;

    tp_hw_reset();                                  /* RSTn 低 10ms -> 高 150ms */
    r = tp_set_mode(TP_ADDR_DEFAULT, TP_CMD_NORMAL);
    bflb_mtimer_delay_ms(10);
    return r;
}

/* 触摸和 RTC 共用 I2C0，默认 400 kHz（2026-10-29 触摸回归修复，见 i2c_bus.h）；
 * SD3010 在低电压下可能丢应答，sd3010_read() 首次失败会自动降到 100 kHz。
 * 保留这个命令做现场 A/B：换完频率先跑 `rtcstat` 确认 RTC 还读得到。
 */
int tp_set_i2c_freq(uint32_t hz)
{
    if (hz == 0) {
        return -1;
    }
    if (i2c_bus_set_freq(hz) != 0) {
        return -1;
    }
    printf("[tp] I2C0 (shared) -> %lu Hz\r\n", (unsigned long)hz);
    if (hz > I2C_BUS_FREQ_HZ) {
        printf("     WARN: >400k 可能让 SD3010 掉应答，读完立刻 rtcstat 复查\r\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 统计（诊断用）                                                      */
/* ------------------------------------------------------------------ */

static tp_stats_t tp_st;

/*
 * 只接受 status = 6。
 *
 * 这是抓原始报文（shell 的 `tpraw`）确认的：
 *   d0 & 0x0F = 6  -> 真正的新报点，坐标会变
 *   d0 & 0x0F = 0  -> 「位置没变」的回显，坐标是上一次的（重复很多次）
 *   而且 INT 拉低几乎总是伴随 status = 6
 *
 * ⚠️ 别把 0 也放进来！松开手指后芯片会继续用 status=0 回显最后一个坐标，
 *    放进来就会变成幽灵触摸，长时间间隔的两次触摸会被连成一条线。
 */
/*
 * 允许哪些 status（p[0] & 0x0F）算作**有效触摸点**，位掩码。
 *
 * ⚠️ 2026-10-12 试过把它放宽成 "(1<<6)|(1<<0)"，**上板失败，已退回**：
 *   现象：完全判不出左滑还是右滑。
 *   原因：status=0 那些帧的坐标是**上一次位置的回显**（tp.h 里原本就写着这句，
 *         这次实测再次确认了），混进来会让坐标在新点和旧点之间来回跳，
 *         方向判定必然被搅乱。
 *   → 换句话说，"6 少 0 多"是芯片的报文习惯，但 **0 帧不能当位置更新用**。
 *   那次实验的原始数据（见 local/docs/history/swipe_smoothness_20261012.md 第 5.5/5.6 节）：
 *       reads=6544 hit=120 bad_status=961 bad_ack=5419
 *       status_hist: 0 -> 961 ; 6 -> 120
 *   用 `tpaccept 0x41` / `tpaccept 64` 仍可现场重做这个对照。
 *
 * 结论：只认 0x06（"真的新报点"）。"一次物理滑动被切成几段"这个问题
 * 截至 2026-11-15 **仍未解决**，下一步方向见上面那份文档的 §5.6。
 */
static uint32_t accept_status_mask = (1u << 0x06);

void tp_set_accept_status(uint32_t mask)
{
    accept_status_mask = mask;
}

uint32_t tp_get_accept_status(void)
{
    return accept_status_mask;
}

void tp_get_stats(tp_stats_t *s)
{
    if (s) {
        *s = tp_st;
    }
}

void tp_reset_stats(void)
{
    memset(&tp_st, 0, sizeof(tp_st));
}

/* Capture from the production reader; no additional I2C transactions and no
 * USB printing while recording. All calls are owned by the LVGL task. */
typedef struct {
    uint32_t ms;
    uint8_t raw[10];
    int8_t result;
    uint8_t irq;
} tp_trace_row_t;
static tp_trace_row_t tp_trace[192];
static unsigned tp_trace_count;
static int tp_trace_armed, tp_trace_started;

void tp_trace_start(void)
{
    tp_trace_count = 0;
    tp_trace_armed = 1;
    tp_trace_started = 0;
    printf("[tptrace] armed: next status=6 starts 192 samples; then run tptrace dump\r\n");
}

void tp_trace_dump(void)
{
    tp_trace_armed = 0;
    printf("[tptrace] rows=%u mask=0x%02lx; ms irq result raw[10]\r\n",
           tp_trace_count, (unsigned long)accept_status_mask);
    for(unsigned i=0; i<tp_trace_count; i++) {
        const tp_trace_row_t *r = &tp_trace[i];
        printf("TPRAW %lu %u %d", (unsigned long)r->ms, r->irq, r->result);
        for(unsigned j=0;j<10;j++) printf(" %02X", r->raw[j]);
        printf("\r\n");
    }
    printf("[tptrace] end\r\n");
}

static int tp_decode_points(const uint8_t data[10], uint16_t *x, uint16_t *y)
{
    uint8_t points;
    uint8_t status;
    const uint8_t *p;

    /* 报文里报了几个点（低 7 位） */
    {
        uint8_t np = data[5] & 0x7F;

        tp_st.pts_hist[np < 7 ? np : 7]++;
    }

    /* data[6] 是固定 ACK 值，不对说明这帧无效（也可能是「没有触摸」的帧） */
    if (data[6] != TP_ACK_VALUE) {
        tp_st.bad_ack++;
        tp_st.ack_hist[data[6] & 0x0F]++;
        return 0;
    }

    points = data[5] & 0x7F;
    if (points == 0 || points > 1) {
        tp_st.nopoint++;
        return 0;
    }

    p = &data[0];
    status = p[0] & 0x0F;
    tp_st.status_hist[status]++;
    if ((accept_status_mask & (1u << status)) == 0) {
        tp_st.bad_status++;
        return 0;
    }

    *x = (uint16_t)((p[1] << 4) | (p[3] >> 4));
    *y = (uint16_t)((p[2] << 4) | (p[3] & 0x0F));
    if (*x >= 480 || *y >= 480) return 0;
    tp_st.hit++;
    return 1;
}

int tp_cst9220_read_points(uint8_t addr7, uint16_t *x, uint16_t *y)
{
    uint8_t raw[10] = {0};
    if(!x || !y) return -1;
    uint32_t ms = (uint32_t)bflb_mtimer_get_time_ms();
    uint8_t irq = (uint8_t)tp_int_level();
    tp_st.reads++;
    int result;
    if(tp_read_reg16(addr7, TP_REG_DATA, raw, sizeof(raw)) < 0) {
        tp_st.i2c_err++;
        result = -1;
    } else result = tp_decode_points(raw, x, y);
    if(tp_trace_armed) {
        if(result == 1 && (raw[0] & 15) == 6) tp_trace_started = 1;
        if(tp_trace_started) {
            tp_trace_row_t *r = &tp_trace[tp_trace_count++];
            r->ms = ms; r->irq = irq; r->result = (int8_t)result;
            memcpy(r->raw, raw, sizeof(raw));
            if(tp_trace_count == sizeof(tp_trace)/sizeof(tp_trace[0])) tp_trace_armed = 0;
        }
    }
    return result;
}
