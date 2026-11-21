/*
 * rtc_seed.h —— RTC 兜底（和网络无关，WiFi 关掉也要能用）
 *
 * SD3010 的 VDD 直接吃 BAT+。有电池时走时会保留；电池彻底耗尽后会丢时间，
 * 而且手册写明上电不会复位时间寄存器，读出来可能是 2004-19-13 这种，
 * 也可能是**看着合法但完全不对**的值（实测出现过 2040-11-05）。
 *
 * 这里负责两件纯逻辑的事（不碰网络）：
 *   1. 判断读出来的年月日 + 星期是不是自相矛盾
 *   2. 用编译时间兜个底
 *
 * ⚠️ 2026-10-17：原判据只拒绝"早于编译日期"的值，2040 在将来 → 一路放行，
 * 表盘上就一直显示 2040 年。现在加了**星期一致性**检查 —— SD3010 的 03H 是
 * 七进制计数器（手册 §实时时钟数据寄存器），日期一旦自洽，星期必然对得上；
 * 对不上就说明寄存器内容是坏的。
 *
 * 网络对时（HTTP Date）没有实现；当前对时走 BLE CTS / 自定义 SET_TIME
 * （services/svc_ble.c 收到后调 svc_time_set 写 SD3010）。这里是纯兜底，不碰网络。
 */
#ifndef _RTC_SEED_H_
#define _RTC_SEED_H_

#include <stdbool.h>
#include <stdint.h>

#include "sd3010.h"

/** 年月日/星期看着就不对（丢过时间），返回 true。week: 0=周日 */
bool rtc_seed_looks_invalid(int year, int month, int day, int week);

/** 用编译时间兜底（总比 2004-19-13 强），返回 false 表示解析失败 */
bool rtc_seed_from_build(sd3010_time_t *tm);

#endif /* _RTC_SEED_H_ */
