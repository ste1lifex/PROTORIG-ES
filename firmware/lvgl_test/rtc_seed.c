/*
 * rtc_seed.c —— RTC 兜底（无网络依赖）
 */
#include "rtc_seed.h"

#include <string.h>
#include <stdio.h>

bool rtc_seed_looks_invalid(int year, int month, int day, int week)
{
    static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dim;

    if (year < 2020 || year > 2099) {
        return true;
    }
    if (month < 1 || month > 12) {
        return true;
    }

    dim = mdays[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        dim = 29;
    }
    if (day < 1 || day > dim) {
        return true;
    }

    /*
     * 星期一致性 —— 2026-10-17 新加，用来抓"看着合法其实全错"的寄存器内容。
     *
     * SD3010 的 03H 是**七进制计数器**（手册原文："星期数据…七进制计数器，
     * 00 对应星期天"），所以只要年月日自洽，星期必然等于算出来的值。
     *
     * 实测：电池到位后 RTC 保时成功，但寄存器里存着 2040-11-05（真值应为
     * 星期一 = 1），芯片报 week=0。原来的判据只拒绝"早于编译日期"的值，
     * 2040 在将来 → 一路放行 → 表盘一直显示 2040 年。现在判死。
     */
    if (week < 0 || week > 6) {
        return true;
    }
    if ((int)sd3010_calc_week((uint16_t)year, (uint8_t)month, (uint8_t)day) != week) {
        return true;
    }

    /*
     * 还要"不早于构建日期"。
     *
     * ⚠️ 实测踩到：板上没接电池时反复断电，SD3010 会掉时间到一个**看着合法
     * 但明显不对**的值（照片上表盘显示了 2024·10·15），而原来只按
     * year<2020 判定 -> 当成有效 -> 不重新对时 -> 表盘一直显示错的日期。
     * 现在早于构建日期的直接判为无效，交给 rtc_seed_from_build() 补。
     */
    {
        static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char mon[4] = { 0 };
        int d, y, i, mi = -1;

        if (sscanf(__DATE__, "%3s %d %d", mon, &d, &y) == 3) {
            for (i = 0; i < 12; i++) {
                if (strncmp(mon, months + i * 3, 3) == 0) {
                    mi = i;
                    break;
                }
            }
            if (mi >= 0) {
                int build_key = y * 10000 + (mi + 1) * 100 + d;
                int rtc_key = year * 10000 + month * 100 + day;
                if (rtc_key < build_key) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool rtc_seed_from_build(sd3010_time_t *tm)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = { 0 };
    int day, year, hh, mm, ss;
    int i, mi = -1;
    const char *d = __DATE__; /* "Sep 26 2026" */
    const char *t = __TIME__; /* "17:41:34"   */

    if (sscanf(d, "%3s %d %d", mon, &day, &year) != 3) {
        return false;
    }
    for (i = 0; i < 12; i++) {
        if (strncmp(mon, months + i * 3, 3) == 0) {
            mi = i;
            break;
        }
    }
    if (mi < 0) {
        return false;
    }
    if (sscanf(t, "%d:%d:%d", &hh, &mm, &ss) != 3) {
        return false;
    }

    memset(tm, 0, sizeof(*tm));
    tm->year = (uint8_t)(year % 100);
    tm->month = (uint8_t)(mi + 1);
    tm->day = (uint8_t)day;
    tm->hour = (uint8_t)hh;
    tm->min = (uint8_t)mm;
    tm->sec = (uint8_t)ss;
    tm->week = sd3010_calc_week((uint16_t)year, (uint8_t)(mi + 1), (uint8_t)day);
    return true;
}
