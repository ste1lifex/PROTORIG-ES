/*
 * motor.c —— 振动马达（U4 = LCM0720A3134F，V1.5 起沿用；IO30）
 *
 * ===========================================================================
 * 硬件（V1.5 原理图 SCH_Schematic1_1-P1_2026-10-11 核过；V1.6 沿用同一电路，
 *       见 schematic/SCH_Schematic1_1-P1_2026-10-18.png）
 *
 *   BAT+ ──[ U4 马达 70Ω ]──┬────────────── D4(1N5819WS) 反向续流
 *                           └── R18 10Ω ── Q2(AO3400) 漏极
 *                                           源极 = GND
 *                                           栅极 = MOTOR(IO30)，R21 10k 下拉
 *
 *   · IO30 高 → Q2 导通 → 马达上电；占空比 ≈ 平均电压 ≈ 转速/振幅
 *   · 满压电流 ≈ 4.0 V / (70 + 10) Ω ≈ 50 mA，**全部从 BAT+ 抽**（不走 3V3 LDO）
 *     → 震动的那几十毫秒里 BAT_ADC 会看到十几 mV 的压降，电量采样要避开它
 *   · 马达电压跟着电池走（3.0~4.2 V），所以"同占空比、不同电量"手感会略有差别
 *
 * PWM 通道（Ai-M62-CBS 手册 pin 表）：
 *   "GPIO30/SPI_MISO/I2S_DI/I2S_RCLK_O/I2C_SCL/PWM2"  →  **PWM CH2**
 *   同一张表里 IO20=PWM0 / IO22=PWM2 / IO28=PWM0 / IO29=PWM1，即 通道 = 脚号 % 4。
 *
 * 为什么用 20 kHz：ERM 马达的机械惯性远低于这个频率，占空比就是平均电压；
 * 而 20 kHz 在听觉之外，不会听到线圈啸叫。低于 1 kHz 会有明显的"吱"。
 * ===========================================================================
 */
#include "motor.h"

#include <stdio.h>

#include "bflb_gpio.h"
#include "bflb_mtimer.h"
#include "bflb_clock.h"      /* BFLB_SYSTEM_XCLK */
#include "bflb_pwm_v2.h"
#include "bl616_glb.h"
#include "hardware/pwm_v2_reg.h"

#define MOTOR_PIN_NUM   30             /* U1 pin 43 = IO30（默认为马达脚） */
#define MOTOR_PIN       (s_pin)        /* 实际用哪根脚，motor usepin 可改 */
#define CH_DEF          PWM_CH2        /* 手册：GPIO30 = PWM2 */
#define MOTOR_XCLK_HZ   40000000u      /* BL616 XCLK = 40 MHz，PWM clk_div 固定 1 */
#define MOTOR_HZ_DEF    20000u
#define PWM_BASE_ADDR   0x2000a400u    /* bl616_memorymap.h: PWM_BASE */
#define GLB_PWM_CFG0    0x200001C0u    /* GLB + 0x1C0 */

static struct bflb_device_s *s_gpio;
static struct bflb_device_s *s_pwm;
static uint16_t s_period;
static uint32_t s_hz = MOTOR_HZ_DEF;
static int s_pct = -1;
static int s_ch = CH_DEF;
static uint8_t s_pin = MOTOR_PIN_NUM;   /* 默认 IO30；motor usepin 可以换（飞线时用） */

/* 频率 → 计数值（clk_div = 1）。period 只能是 2..65535。 */
static uint16_t period_for(uint32_t hz)
{
    uint32_t p;

    if (hz == 0) {
        hz = MOTOR_HZ_DEF;
    }
    p = MOTOR_XCLK_HZ / hz;
    if (p < 2) {
        p = 2;
    }
    if (p > 65535) {
        p = 65535;
    }
    return (uint16_t)p;
}

static void pin_as_gpio(void)
{
    /* 故意同时开 GPIO_INPUT：只有 IE=1 时 CFG0 的 I 位（bit28）才是"脚上真实电平"，
     * 这样 motor_pad_read() 才能自证"输出确实到脚上了"。 */
    bflb_gpio_init(s_gpio, MOTOR_PIN, GPIO_OUTPUT | GPIO_INPUT | GPIO_PULLDOWN | GPIO_DRV_1);
    bflb_gpio_reset(s_gpio, MOTOR_PIN);         /* 低 = 关 */
}

static void pin_as_pwm(void)
{
    bflb_gpio_init(s_gpio, MOTOR_PIN,
                   GPIO_FUNC_PWM0 | GPIO_ALTERNATE | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_1);
}

/* 建 PWM 单元（只建一次）。成功返回 0。 */
static int pwm_build(void)
{
    struct bflb_pwm_v2_config_s cfg;
    struct bflb_pwm_v2_channel_config_s ch;

    if (s_pwm) {
        return 0;
    }

    /* PWM 外设的 AHB 时钟门；不清这一位的话寄存器写得进、脚上没有波形 */
    GLB_PER_Clock_UnGate(GLB_AHB_CLOCK_PWM);

    s_pwm = bflb_device_get_by_name("pwm_v2_0");
    if (s_pwm == NULL) {
        printf("[motor] pwm_v2_0 not found\r\n");
        return -1;
    }

    s_period = period_for(s_hz);

    cfg.clk_source = BFLB_SYSTEM_XCLK;
    cfg.clk_div = 1;
    cfg.period = s_period;

    bflb_pwm_v2_feature_control(s_pwm, PWM_CMD_IO_SEL, PWM_IO_SEL_SINGLE_END);
    bflb_pwm_v2_init(s_pwm, &cfg);

    /* 停止时输出低（PSI = 0），免得关马达时栅极被留在高电平 */
    ch.positive_polarity = PWM_POLARITY_ACTIVE_HIGH;
    ch.negative_polarity = PWM_POLARITY_ACTIVE_HIGH;
    ch.positive_stop_state = PWM_STATE_INACTIVE;
    ch.negative_stop_state = PWM_STATE_INACTIVE;
    ch.positive_brake_state = PWM_STATE_INACTIVE;
    ch.negative_brake_state = PWM_STATE_INACTIVE;
    ch.dead_time = 0;
    bflb_pwm_v2_channel_init(s_pwm, s_ch, &ch);

    bflb_pwm_v2_channel_set_threshold(s_pwm, s_ch, 0, 0);
    bflb_pwm_v2_channel_positive_start(s_pwm, s_ch);
    bflb_pwm_v2_start(s_pwm);

    printf("[motor] pwm_v2_0 CH%d  period=%u  ->  %u Hz\r\n",
           (int)s_ch, (unsigned)s_period,
           (unsigned)(MOTOR_XCLK_HZ / s_period));
    return 0;
}

void motor_init(void)
{
    if (s_gpio == NULL) {
        s_gpio = bflb_device_get_by_name("gpio");
        if (s_gpio == NULL) {
            return;
        }
    }
    pin_as_gpio();
    s_pct = 0;
    printf("[motor] init: IO30 low (stopped)\r\n");
}

void motor_gpio(int on)
{
    if (s_gpio == NULL) {
        motor_init();
    }
    pin_as_gpio();
    if (on) {
        bflb_gpio_set(s_gpio, MOTOR_PIN);       /* 高 = 满速 */
    }
    s_pct = on ? 100 : 0;
    printf("[motor] gpio %s\r\n", on ? "ON (full speed)" : "off");
}

void motor_set(int pct)
{
    uint16_t high;

    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    if (s_gpio == NULL) {
        motor_init();
    }
    if (s_gpio == NULL) {
        return;
    }
    if (pwm_build() != 0) {
        return;
    }

    /* 每次都重新复用一遍脚：便宜，而且能盖掉之前 motor_gpio() 留下的 GPIO 模式 */
    pin_as_pwm();

    if (pct == 0) {
        /* 关就不要靠"占空比 0"去赌阈值比较的边界，直接把通道停掉 */
        bflb_pwm_v2_channel_positive_stop(s_pwm, s_ch);
        bflb_pwm_v2_channel_set_threshold(s_pwm, s_ch, 0, 0);
    } else {
        high = (uint16_t)(((uint32_t)pct * s_period) / 100u);
        if (high == 0) {
            high = 1;
        }
        bflb_pwm_v2_channel_set_threshold(s_pwm, s_ch, 0, high);
        bflb_pwm_v2_channel_positive_start(s_pwm, s_ch);
    }

    s_pct = pct;
}

int motor_pct(void)
{
    return s_pct;
}

uint32_t motor_pwm_hz(void)
{
    return s_hz;
}

uint32_t motor_set_pwm_hz(uint32_t hz)
{
    if (hz < 100) {
        hz = 100;
    }
    if (hz > 100000) {
        hz = 100000;
    }
    s_hz = hz;

    if (s_pwm) {
        /* 频率改了要重算 period；整块重建最省事，反正没人会高频调它 */
        bflb_pwm_v2_channel_positive_stop(s_pwm, s_ch);
        bflb_pwm_v2_stop(s_pwm);
        s_pwm = NULL;
        s_period = 0;
        if (s_pct > 0) {
            int pct = s_pct;
            s_pct = 0;
            motor_set(pct);                     /* 重建后按原占空比重开 */
        }
    }
    return MOTOR_XCLK_HZ / period_for(s_hz);
}

void motor_pulse(int pct, uint32_t ms)
{
    motor_set(pct);
    bflb_mtimer_delay_ms(ms);
    motor_set(0);
}

/* ================================================================== */
/* 上板自检                                                            */
/* ================================================================== */

static volatile uint32_t *pin_cfg_reg(void)
{
    return (volatile uint32_t *)(GLB_BASE + GLB_GPIO_CFG0_OFFSET + ((uint32_t)s_pin << 2));
}

int motor_pad_read(void)
{
    return (int)((*pin_cfg_reg() >> GLB_REG_GPIO_0_I_POS) & 1u);
}

int motor_channel(void)
{
    return s_ch;
}

int motor_set_channel(int ch)
{
    if (ch < 0 || ch > 3) {
        ch = CH_DEF;
    }
    if (ch == s_ch) {
        return s_ch;
    }
    s_ch = ch;
    if (s_pwm) {
        int pct = s_pct;
        bflb_pwm_v2_stop(s_pwm);
        s_pwm = NULL;
        s_period = 0;
        s_pct = 0;
        if (pct > 0) {
            motor_set(pct);
        }
    }
    printf("[motor] channel -> CH%d (IO30)\r\n", s_ch);
    return s_ch;
}

void motor_diag(void)
{
    uint32_t c;
    int i_pwm0, i_pwm100, i_gpio1, i_gpio0;

    motor_init();                       /* 先把脚弄成 GPIO 低 */

    motor_set(0);
    c = *pin_cfg_reg();
    i_pwm0 = motor_pad_read();

    printf("[motor] ---- IO30 self check ----\r\n");
    printf("[motor] cfg=0x%08X  IE=%d OE=%d O=%d I=%d FUNC=%d DRV=%d PU=%d PD=%d\r\n",
           (unsigned)c,
           (int)(c & 1u), (int)((c >> 6) & 1u), (int)((c >> 24) & 1u), (int)((c >> 28) & 1u),
           (int)((c >> 8) & 0x1fu), (int)((c >> 2) & 3u), (int)((c >> 4) & 1u), (int)((c >> 5) & 1u));

    motor_set(100);
    i_pwm100 = motor_pad_read();
    printf("[motor] pwm CH%d 100%%  -> I=%d  (expect 1)\r\n", s_ch, i_pwm100);
    motor_set(0);

    motor_gpio(1);
    i_gpio1 = motor_pad_read();
    printf("[motor] gpio 1        -> I=%d  (expect 1)\r\n", i_gpio1);

    motor_gpio(0);
    i_gpio0 = motor_pad_read();
    printf("[motor] gpio 0        -> I=%d  (expect 0)\r\n", i_gpio0);

    printf("[motor] pwm0 I=%d\r\n", i_pwm0);

    if (i_gpio1 && !i_gpio0) {
        if (i_pwm100) {
            printf("[motor] => pad follows both GPIO and PWM: MCU side OK, check Q2 / motor / solder\r\n");
        } else {
            printf("[motor] => GPIO drives the pad, but PWM does NOT reach it (try: motor ch 0|1|3)\r\n");
        }
    } else if (!i_gpio1) {
        printf("[motor] => pad does not go high even in GPIO mode: wrong pin / pad shorted / no bond\r\n");
    } else {
        printf("[motor] => pad stuck high (short to 3V3?)\r\n");
    }
}

void motor_blink(uint32_t hz, uint32_t ms)
{
    uint32_t half;
    uint32_t elapsed = 0;
    int on = 0;

    if (hz < 1) {
        hz = 1;
    }
    if (hz > 100) {
        hz = 100;
    }
    half = 500u / hz;                  /* 半周期 ms */
    if (half == 0) {
        half = 1;
    }

    printf("[motor] blink %u Hz for %u ms (GPIO mode, for a meter/scope)\r\n",
           (unsigned)hz, (unsigned)ms);

    motor_gpio(0);
    while (elapsed < ms) {
        on = !on;
        if (on) {
            bflb_gpio_set(s_gpio, MOTOR_PIN);
        } else {
            bflb_gpio_reset(s_gpio, MOTOR_PIN);
        }
        bflb_mtimer_delay_ms(half);
        elapsed += half;
    }
    motor_gpio(0);
}

/* ------------------------------------------------------------------ */
/* "脚上到底挂了东西没有" —— 用节点电荷保持时间判断                      */
/*                                                                     */
/* 原理：把 IO30 输出高 5 ms（节点充到 3.3V），然后切成"输入 + 下拉"， */
/* 立刻读一次。                                                         */
/*   · 如果脚上真的连着 Q2 的栅极（Ciss ~500 pF）+ R21 10k，          */
/*     放电时间常数 τ ≈ 9k × 0.5nF ≈ 5 ms → 刚切完还是高电平；        */
/*   · 如果这根脚是悬空的（走线断 / Q2 没焊上），                          */
/*     只有焊盘本身几 pF，τ ≈ 几十~几百 ns → 立刻掉到低。              */
/*                                                                     */
/* 这个测试不需要万用表，能把"MCU 没驱动"和"板上没连上"分开。          */
/* ------------------------------------------------------------------ */
int motor_padtest(void)
{
    int after, later;

    pin_as_gpio();
    bflb_gpio_set(s_gpio, MOTOR_PIN);
    bflb_mtimer_delay_ms(5);

    /* 一次性切成 输入 + 下拉（OE 关、IE 开、PD 开），然后马上读 */
    bflb_gpio_init(s_gpio, MOTOR_PIN, GPIO_INPUT | GPIO_PULLDOWN | GPIO_DRV_1);
    after = motor_pad_read();

    bflb_mtimer_delay_ms(5);
    later = motor_pad_read();

    printf("[motor] padtest: right-after-HiZ=%d, 5ms-later=%d\r\n", after, later);
    if (after && !later) {
        printf("[motor] => node held its charge: something IS attached to IO30 (Q2 gate / trace OK)\r\n");
    } else if (!after && !later) {
        printf("[motor] => node collapsed instantly: IO30 looks FLOATING (open trace, Q2 not soldered)\r\n");
    } else {
        printf("[motor] => node still high after 5 ms: hard short to 3V3?\r\n");
    }
    return after;
}

/* ------------------------------------------------------------------ */
/* padtest2：区分「MCU 没驱动」和「脚是悬空的」                        */
/*                                                                     */
/* padtest 只能证明"脚上没有电容"，但分不清是 MCU 没驱动出去、        */
/* 还是驱动了但外面没挂东西。这里改成：                                */
/*   驱动高 5 ms → 切成【输入、不带上下拉】→ 立刻读。                  */
/*     · 读到 1 且 3 ms 后仍是 1 → 这条脚确实被 MCU 拉到了 3.3V，      */
/*       而且没有泄放路径（外面什么都没接）⇒ 走线断 / 焊盘没连上      */
/*     · 读到 0 → MCU 根本没把这条脚驱动起来                          */
/* ------------------------------------------------------------------ */
int motor_padtest2(void)
{
    int a, b, c, d;

    pin_as_gpio();
    bflb_gpio_set(s_gpio, MOTOR_PIN);
    bflb_mtimer_delay_ms(5);

    bflb_gpio_init(s_gpio, MOTOR_PIN, GPIO_INPUT | GPIO_DRV_1);   /* 只开输入，无上下拉 */
    a = motor_pad_read();
    bflb_mtimer_delay_ms(3);
    b = motor_pad_read();

    /* 对照组：驱动低，再悬空 */
    pin_as_gpio();
    bflb_gpio_reset(s_gpio, MOTOR_PIN);
    bflb_mtimer_delay_ms(5);
    bflb_gpio_init(s_gpio, MOTOR_PIN, GPIO_INPUT | GPIO_DRV_1);
    c = motor_pad_read();
    bflb_mtimer_delay_ms(3);
    d = motor_pad_read();

    printf("[motor] padtest2: HI->float now=%d +3ms=%d | LO->float now=%d +3ms=%d\r\n", a, b, c, d);
    if (a && b) {
        printf("[motor] => pad WAS driven high by the MCU and nothing drains it: IO%d pad is ISOLATED from Q2\r\n", s_pin);
    } else if (!a && !c) {
        printf("[motor] => pad never went high: the MCU is NOT driving IO%d\r\n", s_pin);
    } else {
        printf("[motor] => ambiguous: a=%d b=%d c=%d d=%d\r\n", a, b, c, d);
    }
    return a;
}

/* 换马达脚（飞线到别的 GPIO 时用）；通道按"脚号 % 4"跟着换 */
int motor_set_pin(int n)
{
    if (n < 0 || n > 31) {
        return s_pin;
    }
    if (s_pwm) {
        bflb_pwm_v2_channel_positive_stop(s_pwm, s_ch);
        bflb_pwm_v2_stop(s_pwm);
        s_pwm = NULL;
        s_period = 0;
    }
    if (s_gpio) {
        bflb_gpio_init(s_gpio, s_pin, GPIO_INPUT | GPIO_DRV_1);   /* 旧脚放成高阻 */
    }
    s_pin = (uint8_t)n;
    s_ch = n % 4;
    s_pct = 0;
    motor_init();
    printf("[motor] pin -> IO%d (pwm CH%d)\r\n", s_pin, s_ch);
    return s_pin;
}

int motor_pin(void)
{
    return s_pin;
}

/* PWM 外设寄存器快照：证明"外设到底跑没跑" */
void motor_dump_regs(void)
{
    uint32_t cfg0 = *(volatile uint32_t *)(PWM_BASE_ADDR + PWM_MC0_CONFIG0_OFFSET);
    uint32_t cfg1 = *(volatile uint32_t *)(PWM_BASE_ADDR + PWM_MC0_CONFIG1_OFFSET);
    uint32_t per = *(volatile uint32_t *)(PWM_BASE_ADDR + PWM_MC0_PERIOD_OFFSET);
    uint32_t th = *(volatile uint32_t *)(PWM_BASE_ADDR + PWM_MC0_CH0_THRE_OFFSET + (uint32_t)s_ch * 4u);
    uint32_t glb = *(volatile uint32_t *)GLB_PWM_CFG0;

    printf("[pwm] glb_cfg0(0x1C0) = 0x%08X\r\n", (unsigned)glb);
    printf("[pwm] cfg0 = 0x%08X   period = %u\r\n",
           (unsigned)cfg0, (unsigned)((per & PWM_PERIOD_MASK) >> PWM_PERIOD_SHIFT));
    printf("[pwm] cfg1 = 0x%08X   CH%d enable(PEN) = %d\r\n",
           (unsigned)cfg1, s_ch, (int)((cfg1 >> (4 * s_ch)) & 1u));
    printf("[pwm] CH%d_THRE = 0x%08X  low=%u  high=%u\r\n",
           s_ch, (unsigned)th, (unsigned)(th & 0xFFFFu), (unsigned)(th >> 16));
    printf("[pwm] gpio30 cfg = 0x%08X  (I bit = %d)\r\n",
           (unsigned)*pin_cfg_reg(), motor_pad_read());
}

