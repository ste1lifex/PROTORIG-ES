/*
 * board_adc.c —— 电池电压 + 片内温度（共用 adc 设备）
 *
 * 移植自 V1.4 的 watchface/board_adc.c（那版已上板验证过）。
 *
 * 两个通道对 ADC 的要求不一样，所以每次测量前重新配一次：
 *   电池 IO20/ADC_CH0：单次转换，VREF 3.2V（电池/2 最高约 2.1V，2.0V 参考会削顶）
 *   片内温度 TSEN   ：连续转换，VREF 2.0V（SDK examples/peripherals/adc/adc_tsen 就是这么配的，
 *                     用 3.2V 参考读出来是 -100 多度的假值）
 */
#include "board_adc.h"

#include "bflb_adc.h"
#include "bflb_gpio.h"
#include "bflb_mtimer.h"

#include "FreeRTOS.h"
#include "semphr.h"

static struct bflb_device_s *adc_dev;
static struct bflb_device_s *gpio_dev;

/*
 * ADC 只有一个硬件，但电池电压（LVGL 定时器）和片内温度（shell `batt`）
 * 分属不同任务，而且每次测量都要重新配置参考电压/通道。没有互斥时，
 * 一方刚把 ADC 切到 TSEN，另一方就可能在错误的配置下采样 —— 电池读数
 * 会被拉低，进而误触发 3.2V 关机。这里用递归互斥把"配置+转换+读值"
 * 锁成一次完整事务。
 */
static SemaphoreHandle_t s_adc_mtx;

static void adc_lock(void)
{
    if (s_adc_mtx != NULL) {
        (void)xSemaphoreTakeRecursive(s_adc_mtx, portMAX_DELAY);
    }
}

static void adc_unlock(void)
{
    if (s_adc_mtx != NULL) {
        (void)xSemaphoreGiveRecursive(s_adc_mtx);
    }
}

static void adc_setup(uint8_t vref, bool continuous, uint8_t pos_chan)
{
    struct bflb_adc_config_s cfg;
    struct bflb_adc_channel_s chan;

    cfg.clk_div = ADC_CLK_DIV_32;
    cfg.scan_conv_mode = false;
    cfg.continuous_conv_mode = continuous;
    cfg.differential_mode = false;
    cfg.resolution = ADC_RESOLUTION_16B;
    cfg.vref = vref;

    bflb_adc_init(adc_dev, &cfg);

    chan.pos_chan = pos_chan;
    chan.neg_chan = ADC_CHANNEL_GND;
    bflb_adc_channel_config(adc_dev, &chan, 1);

    if (pos_chan == ADC_CHANNEL_TSEN_P) {
        bflb_adc_tsen_init(adc_dev, ADC_TSEN_MOD_INTERNAL_DIODE);
    }
}

int board_adc_init(void)
{
    adc_dev = bflb_device_get_by_name("adc");
    gpio_dev = bflb_device_get_by_name("gpio");
    if (adc_dev == NULL || gpio_dev == NULL) {
        return -1;
    }

    if (s_adc_mtx == NULL) {
        s_adc_mtx = xSemaphoreCreateRecursiveMutex();
    }

    /* IO20 = ADC_CH0，走模拟输入 */
    bflb_gpio_init(gpio_dev, GPIO_PIN_20, GPIO_ANALOG | GPIO_SMT_EN | GPIO_DRV_0);

    adc_setup(ADC_VREF_3P2V, false, BOARD_ADC_CH_BAT);
    return 0;
}

int board_adc_battery_mv(void)
{
    struct bflb_adc_result_s r;
    int acc = 0;
    int n = 0;
    int i;

    if (adc_dev == NULL) {
        return -1;
    }

    adc_lock();
    adc_setup(ADC_VREF_3P2V, false, BOARD_ADC_CH_BAT);

    /* 多采几次取平均；分压电阻 100k 阻抗不低，单次会有噪声 */
    for (i = 0; i < 8; i++) {
        uint32_t raw;
        int guard = 1000;

        bflb_adc_start_conversion(adc_dev);
        while (bflb_adc_get_count(adc_dev) == 0 && guard-- > 0) {
            bflb_mtimer_delay_us(100);
        }
        if (guard <= 0) {
            bflb_adc_stop_conversion(adc_dev);
            break;
        }
        raw = bflb_adc_read_raw(adc_dev);
        bflb_adc_stop_conversion(adc_dev);
        bflb_adc_parse_result(adc_dev, &raw, &r, 1);
        acc += (int)r.millivolt;
        n++;
        bflb_mtimer_delay_us(300);
    }
    if (n == 0) {
        adc_unlock();
        return -1;
    }
    adc_unlock();
    return (acc / n) * BOARD_ADC_DIV;
}

int board_adc_battery_percent(int mv)
{
    /*
     * 单节锂电的「开路电压 → 电量」分段表（典型值）。
     *
     * 为什么不用线性：原来 3.4V=0% / 4.2V=100% 的直线在中段碰巧还行
     * （3.93V → 66%），但两端错得厉害 —— 3.6V 会报 25%（真实约 17%），
     * 4.2V 附近又被压成一片。锂电的特性是中间一大段平台、末尾陡降，
     * 所以按表插值。
     *
     * ⚠️ 这是"典型值"，不同电芯差 ±10% 很正常。插着 USB 时读到的是
     * 充电电压（不是开路电压），这时百分比只能当"接近满"看。V1.6 已把
     * TP4056 的 CHRG(IO3)/STDBY(IO21) 接出来，充电判断在 main.c，不在本文件。
     */
    static const uint16_t ocv_mv[] = {
        3300, 3400, 3450, 3500, 3550, 3600, 3650, 3700,
        3750, 3800, 3850, 3900, 3950, 4000, 4100, 4200
    };
    static const uint8_t ocv_pc[] = {
        0, 2, 4, 7, 11, 17, 25, 33,
        42, 50, 58, 66, 73, 80, 90, 100
    };
    const int n = (int)(sizeof(ocv_mv) / sizeof(ocv_mv[0]));
    int i;

    if (mv <= 0) {
        return -1;
    }
    if (mv <= ocv_mv[0]) {
        return 0;
    }
    if (mv >= ocv_mv[n - 1]) {
        return 100;
    }

    for (i = 1; i < n; i++) {
        if (mv <= (int)ocv_mv[i]) {
            int mv0 = ocv_mv[i - 1];
            int mv1 = ocv_mv[i];
            int p0 = ocv_pc[i - 1];
            int p1 = ocv_pc[i];
            return p0 + (mv - mv0) * (p1 - p0) / (mv1 - mv0);
        }
    }
    return 100;
}

float board_adc_mcu_temp(void)
{
    float t;

    if (adc_dev == NULL) {
        return -1000.0f;
    }
    adc_lock();
    adc_setup(ADC_VREF_2P0V, true, ADC_CHANNEL_TSEN_P);
    bflb_mtimer_delay_ms(20); /* 让 TSEN 稳定一下 */
    t = bflb_adc_tsen_get_temp(adc_dev);
    adc_unlock();
    return t;
}
