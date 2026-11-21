/*
 * board_adc.h —— 板载 ADC：电池电压（IO20）+ MCU 片内温度
 *
 * 移植自 V1.4 的 watchface/board_adc.h；V1.5/V1.6 硬件都没变（BAT_ADC 仍走 IO20）。
 *
 * 电池：
 *   原理图 BAT_ADC 网络 -> IO20（U1 pin40），
 *   分压 R16 / R17 = 100k / 100k，C7 = 100nF 滤波。
 *   所以 Vbat = Vadc * 2。
 *   BL616 的 GPIO20 正好是 ADC_CH0（见 SDK bsp/board/bl616dk/board_gpio.c）。
 *
 * 温度：
 *   BL616 内部二极管 + eFuse 校准（ADC_TSEN_MOD_INTERNAL_DIODE）。
 *
 * 两者共用同一个 adc 设备，所以放在一个模块里统一管理，
 * 不要在别处再 bflb_adc_init()。
 */
#ifndef _BOARD_ADC_H_
#define _BOARD_ADC_H_

#include <stdint.h>

#define BOARD_ADC_CH_BAT 0 /* ADC_CH0 = GPIO20 */
#define BOARD_ADC_DIV    2 /* 100k/100k 分压 */

/** 初始化 ADC + IO20 模拟输入。0 = 成功 */
int board_adc_init(void);

/** 读一次电池电压（mV）；失败返回 -1 */
int board_adc_battery_mv(void);

/** 由电池电压粗估电量百分比 0..100（单节锂电，3.4V=0% / 4.2V=100%） */
int board_adc_battery_percent(int mv);

/** 读一次 MCU 片内温度（℃）；失败返回 -1000 */
float board_adc_mcu_temp(void);

#endif /* _BOARD_ADC_H_ */
