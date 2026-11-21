/**
 * @file  sd3010.c
 * @brief 兴威帆 SD3010 实时时钟驱动
 */
#include "sd3010.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "i2c_bus.h"

/* CTR1 (0x0F) 位定义 */
#define SD3010_CTR1_WRTC3 0x80
#define SD3010_CTR1_OSF   0x40
#define SD3010_CTR1_INTAF 0x20
#define SD3010_CTR1_INTDF 0x10
#define SD3010_CTR1_BLF   0x08
#define SD3010_CTR1_WRTC2 0x04
#define SD3010_CTR1_PMF   0x02
#define SD3010_CTR1_RTCF  0x01

/* CTR2 (0x10) 位定义 */
#define SD3010_CTR2_WRTC1 0x80
#define SD3010_CTR2_IM    0x40
#define SD3010_CTR2_INTS1 0x20
#define SD3010_CTR2_INTS0 0x10
#define SD3010_CTR2_INTDE 0x04
#define SD3010_CTR2_INTAE 0x02
#define SD3010_CTR2_INTFE 0x01

/* CTR3 (0x11) 位定义 */
#define SD3010_CTR3_ARST  0x80

/* 报警允许（0x0E）位定义 */
#define SD3010_ALM_EN_EAS  0x01
#define SD3010_ALM_EN_EAMN 0x02
#define SD3010_ALM_EN_EAH  0x04

static uint8_t bcd2dec(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

static uint8_t dec2bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10) << 4) | (dec % 10));
}

uint8_t sd3010_calc_week(uint16_t full_year, uint8_t month, uint8_t day)
{
    /* Sakamoto's algorithm, 0 = Sunday */
    static const uint8_t t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    uint16_t y = full_year;

    if (month < 3) {
        y -= 1;
    }
    if (month < 1 || month > 12) {
        return 0;
    }
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7);
}

int sd3010_init(void)
{
    return i2c_bus_probe(SD3010_I2C_ADDR);
}

static int s_fallback_tried;   /* 400k->100k 只降一次，直到重启 */

int sd3010_read(sd3010_time_t *tm)
{
    uint8_t b[7];
    int ret;

    if (tm == NULL) {
        return -1;
    }

    /* 读命令一发出去，芯片就把整组时间数据锁存了，所以一次读 7 字节是安全的 */
    ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_SEC, b, sizeof(b));

    /* 2026-10-29：默认 400k 提升触摸；SD3010 在低电压下可能掉应答。
     * 首次失败且当前 >100k 时降到 100k 重试一次，成功后保持 100k 到重启。
     * 注意：i2c_bus_read_regs() 返回时已释放总线锁，才能安全切频率。 */
    if (ret < 0 && !s_fallback_tried && i2c_bus_get_freq() > 100000u) {
        s_fallback_tried = 1;
        if (i2c_bus_set_freq(100000u) == 0) {
            ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_SEC, b, sizeof(b));
            if (ret >= 0) {
                printf("[i2c] SD3010 read failed at 400k -> fallback 100k\r\n");
            }
        }
    }
    if (ret < 0) {
        return ret;
    }

    tm->sec = bcd2dec(b[0] & 0x7F);
    tm->min = bcd2dec(b[1] & 0x7F);

    if (b[2] & 0x80) {
        /* 24 小时制 */
        tm->hour = bcd2dec(b[2] & 0x3F);
    } else {
        /* 12 小时制：bit5 = AM/PM */
        uint8_t h = bcd2dec(b[2] & 0x1F);
        if (b[2] & 0x20) {
            h = (uint8_t)(h % 12 + 12); /* PM */
        } else {
            h = (uint8_t)(h % 12); /* AM */
        }
        tm->hour = h;
    }

    tm->week = (uint8_t)(b[3] & 0x07);
    tm->day = bcd2dec(b[4] & 0x3F);
    tm->month = bcd2dec(b[5] & 0x1F);
    tm->year = bcd2dec(b[6]);
    return 0;
}

static int sd3010_write_enable(void)
{
    uint8_t ctr1 = 0;
    uint8_t ctr2 = 0;
    int ret;

    /* 顺序不能反：先 WRTC1=1，再 WRTC2/WRTC3=1 */
    ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR2, &ctr2, 1);
    if (ret < 0) {
        return ret;
    }
    ctr2 |= SD3010_CTR2_WRTC1;
    ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_CTR2, &ctr2, 1);
    if (ret < 0) {
        return ret;
    }

    ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &ctr1, 1);
    if (ret < 0) {
        return ret;
    }
    ctr1 |= (SD3010_CTR1_WRTC2 | SD3010_CTR1_WRTC3);
    return i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &ctr1, 1);
}

static int sd3010_write_disable(void)
{
    uint8_t ctr1 = 0;
    uint8_t ctr2 = 0;
    int ret;

    /* 顺序不能反：先 WRTC2/WRTC3=0，再 WRTC1=0 */
    ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &ctr1, 1);
    if (ret < 0) {
        return ret;
    }
    ctr1 &= (uint8_t)~(SD3010_CTR1_WRTC2 | SD3010_CTR1_WRTC3);
    ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &ctr1, 1);
    if (ret < 0) {
        return ret;
    }

    ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR2, &ctr2, 1);
    if (ret < 0) {
        return ret;
    }
    ctr2 &= (uint8_t)~SD3010_CTR2_WRTC1;
    return i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_CTR2, &ctr2, 1);
}

int sd3010_set_time(const sd3010_time_t *tm)
{
    uint8_t b[7];
    int ret;

    if (tm == NULL || tm->month < 1 || tm->month > 12 || tm->day < 1 || tm->day > 31) {
        return -1;
    }

    ret = sd3010_write_enable();
    if (ret < 0) {
        return ret;
    }

    /* 7 个时间寄存器必须一次性写完 */
    b[0] = dec2bcd(tm->sec & 0x7F);
    b[1] = dec2bcd(tm->min & 0x7F);
    b[2] = (uint8_t)(0x80 | dec2bcd(tm->hour & 0x3F)); /* 固定 24 小时制 */
    b[3] = (uint8_t)(tm->week & 0x07);
    b[4] = dec2bcd(tm->day & 0x3F);
    b[5] = dec2bcd(tm->month & 0x1F);
    b[6] = dec2bcd(tm->year);

    ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_SEC, b, sizeof(b));

    /* 不管写成功没有，都把写保护恢复回去 */
    (void)sd3010_write_disable();

    return ret;
}

int sd3010_get_status(uint8_t *ctr1, uint8_t *ctr5)
{
    int ret;

    if (ctr1) {
        ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, ctr1, 1);
        if (ret < 0) {
            return ret;
        }
    }
    if (ctr5) {
        uint8_t v = 0;
        ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR5, &v, 1);
        if (ret < 0) {
            return ret;
        }
        *ctr5 = v;
    }
    return 0;
}

/*
 * 清 OSF（停振标志，手册：OSF=1 表示之前有过停振，默认 0）。
 *
 * 用途：判断"这组时间还能不能信"。
 *   · OSF=1  → 一定丢过时间，必须重新对时，对完把它清掉；
 *   · OSF=0  → 振荡器没停过，寄存器内容至少是"一直在走"的。
 *
 * ⚠️ 注意它不是万能的：实测这块板子上 RTC 内容已经烂成 2040-11-05，
 * 但 OSF 仍然是 0（CTR1=0x01），所以还要配合 rtc_seed_looks_invalid()
 * 的星期一致性检查一起用。
 *
 * 写 CTR1 在 00H~71H 区间内，必须先开写允许。
 */
int sd3010_clear_osf(void)
{
    uint8_t ctr1 = 0;
    int ret;

    ret = sd3010_write_enable();
    if (ret < 0) {
        return ret;
    }

    ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &ctr1, 1);
    if (ret >= 0) {
        ctr1 &= (uint8_t)~SD3010_CTR1_OSF;
        ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &ctr1, 1);
    }

    (void)sd3010_write_disable();
    return ret;
}

/*
 * 掉电标记（2026-10-18 加）
 *
 * 背景：电池跑了一晚耗尽后，RTC 的时间冻结在断电那一刻，第二天上电芯片
 * 继续从那个旧值往下走 —— 读出来是"看起来完全合法"的时间，只是慢了 12 小时。
 * 星期、日期、OSF 全都自洽，`rtc_seed_looks_invalid()` 抓不到。
 *
 * 办法：用 SD3010 自带的**用户 RAM（2CH~71H，70 字节，VDD 域）**放一个魔数。
 *   · 这块 RAM 和 RTC 共用 VDD（本板是 BAT+）→ 电池还在就一直在；
 *   · 彻底断电后 RAM 内容复位（手册默认值 XXXX-XXXX）→ 魔数没了；
 *   · 所以"开机时魔数还在"就等价于"RTC 没丢过电，时间可信"。
 *
 * 写完时间之后一定要调 sd3010_write_magic()，否则下次开机会被当成丢过电。
 */
static const uint8_t s_rtc_magic[4] = { 'P', 'R', 'T', 'C' };   /* 0x50 0x52 0x54 0x43 */

int sd3010_check_magic(void)
{
    uint8_t b[4];

    if (i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_USER_RAM, b, sizeof(b)) < 0) {
        return -1;
    }
    return (memcmp(b, s_rtc_magic, sizeof(b)) == 0) ? 0 : 1;   /* 0 = 没丢过电 */
}

int sd3010_write_magic(void)
{
    int ret;

    ret = sd3010_write_enable();
    if (ret < 0) {
        return ret;
    }
    ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_USER_RAM,
                             (uint8_t *)s_rtc_magic, sizeof(s_rtc_magic));
    (void)sd3010_write_disable();
    return ret;
}

/*
 * 用户 RAM 的通用读写（2CH~71H，70 字节）。
 *
 * 整片用户 RAM 都在用，完整地址表以 services/settings_codec.h 顶部为准：
 *   2CH~2FH  本驱动的掉电魔数 "PRTC"（sd3010_check_magic）
 *   30H~3DH  settings v2 记录 14 B（services/svc_settings.c）
 *   3EH      battlog 写指针；40H~69H battlog 21 × 2 B
 *   6AH~71H  pdslog 8 B（bsp/board_power.c）
 *
 * "电压飞行记录仪"（2026-10-18 加）现在只占 3EH~69H 这一段：每 2 分钟记一条
 * 电池 mV，拔掉 USB 跑一段再插回来，用 `battlog` 倒整条放电曲线 ——
 * 不用拆线、不用万用表、也不用盯着表盘读数。
 */
int sd3010_ram_read(uint8_t addr, uint8_t *buf, uint8_t len)
{
    if (addr < SD3010_REG_USER_RAM || ((uint16_t)addr + len) > 0x72u) {
        return -1;
    }
    return i2c_bus_read_regs(SD3010_I2C_ADDR, addr, buf, len);
}

int sd3010_ram_write(uint8_t addr, const uint8_t *buf, uint8_t len)
{
    int ret;

    if (addr < SD3010_REG_USER_RAM || ((uint16_t)addr + len) > 0x72u) {
        return -1;
    }
    ret = sd3010_write_enable();
    if (ret < 0) {
        return ret;
    }
    ret = i2c_bus_write_regs(SD3010_I2C_ADDR, addr, (uint8_t *)buf, len);
    (void)sd3010_write_disable();
    return ret;
}

/* ------------------------------------------------------------------ */
/* 报警 / 中断（V1.6，INT -> IO17）                                    */
/*                                                                    */
/* 手册 §4.3：                                                        */
/*   07H 秒报警、08H 分、09H 时 ... 0EH 报警允许位                    */
/*   CTR2(10H): IM=0 单事件(电平保持到清 INTAF), INTS1/0=01 报警输出  */
/*   CTR1(0F): bit5=INTAF, bit4=INTDF                                 */
/* 注意：写 0EH 会自动清 INTAF；写 0FH/10H 时必须保留 WRTC2/3、WRTC1。*/
/* 手册：小时报警寄存器 09H 的最高位**恒为 0**（不是时间的 12/24 位）。*/
static int sd3010_alarm_int_config(void)
{
    uint8_t v = 0;
    int ret;

    /* CTR2：报警中断，单事件（IM=0，INT 低电平保持到清 INTAF），INTS1/0=01 */
    ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR2, &v, 1);
    if (ret >= 0) {
        v &= (uint8_t)~(SD3010_CTR2_IM | SD3010_CTR2_INTS1 | SD3010_CTR2_INTS0 |
                        SD3010_CTR2_INTDE | SD3010_CTR2_INTAE | SD3010_CTR2_INTFE);
        v |= (SD3010_CTR2_INTS0 | SD3010_CTR2_INTAE);
        ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_CTR2, &v, 1);
    }

    /* 清 INTAF/INTDF（保留 WRTC2/WRTC3=1） */
    if (ret >= 0) {
        ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &v, 1);
        if (ret >= 0) {
            v &= (uint8_t)~(SD3010_CTR1_INTAF | SD3010_CTR1_INTDF);
            ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &v, 1);
        }
    }
    return ret;
}

int sd3010_alarm_set_sec(uint8_t sec)
{
    uint8_t v = 0;
    int ret;

    if (sec > 59) {
        return -1;
    }

    ret = sd3010_write_enable();
    if (ret < 0) {
        return ret;
    }

    /* 1) 秒报警值 + 只使能秒比较 */
    v = dec2bcd(sec);
    ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_ALM_SEC, &v, 1);
    if (ret >= 0) {
        v = SD3010_ALM_EN_EAS;
        ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_ALM_EN, &v, 1);
    }

    /* 2) 中断配置 + 清标志 */
    if (ret >= 0) {
        ret = sd3010_alarm_int_config();
    }

    (void)sd3010_write_disable();
    return ret;
}

/*
 * 每天 HH:MM:00 报警（闹钟用，2026-11-08）：
 *   09H 小时最高位恒 0；07H/08H/09H 一起写，0EH = EAS|EAMN|EAH。
 *   只使能 秒/分/时 比较 = 每天同一时刻触发一次（不比较星期/日/月/年）。
 * 唤醒路径：INT(IO17) 低电平保持到清 INTAF，是 AON 唤醒脚（见 board_power）。
 */
int sd3010_alarm_set_hhmm(uint8_t hour, uint8_t min, uint8_t sec)
{
    uint8_t buf[3];
    int ret;

    if (hour > 23 || min > 59 || sec > 59) {
        return -1;
    }

    ret = sd3010_write_enable();
    if (ret < 0) {
        return ret;
    }

    /* 07H/08H/09H：秒、分、时（小时 MSB 必须为 0） */
    buf[0] = dec2bcd(sec);
    buf[1] = dec2bcd(min);
    buf[2] = dec2bcd(hour);
    ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_ALM_SEC, buf, 3);
    if (ret >= 0) {
        buf[0] = (uint8_t)(SD3010_ALM_EN_EAS | SD3010_ALM_EN_EAMN | SD3010_ALM_EN_EAH);
        ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_ALM_EN, buf, 1);
    }
    if (ret >= 0) {
        ret = sd3010_alarm_int_config();
    }

    (void)sd3010_write_disable();
    return ret;
}


int sd3010_alarm_disable(void)
{
    uint8_t v = 0;
    int ret;

    ret = sd3010_write_enable();
    if (ret < 0) {
        return ret;
    }

    ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR2, &v, 1);
    if (ret >= 0) {
        v &= (uint8_t)~SD3010_CTR2_INTAE;
        ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_CTR2, &v, 1);
    }
    if (ret >= 0) {
        ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &v, 1);
        if (ret >= 0) {
            v &= (uint8_t)~(SD3010_CTR1_INTAF | SD3010_CTR1_INTDF);
            ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &v, 1);
        }
    }

    (void)sd3010_write_disable();
    return ret;
}

int sd3010_int_clear(void)
{
    uint8_t v = 0;
    int ret;

    ret = sd3010_write_enable();
    if (ret < 0) {
        return ret;
    }
    ret = i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &v, 1);
    if (ret >= 0) {
        v &= (uint8_t)~(SD3010_CTR1_INTAF | SD3010_CTR1_INTDF);
        ret = i2c_bus_write_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, &v, 1);
    }
    (void)sd3010_write_disable();
    return ret;
}

int sd3010_int_get(uint8_t *ctr1, uint8_t *ctr2, uint8_t *alm_en, uint8_t *alm_sec)
{
    uint8_t v;

    if (ctr1) {
        if (i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR1, ctr1, 1) < 0) {
            return -1;
        }
    }
    if (ctr2) {
        if (i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_CTR2, ctr2, 1) < 0) {
            return -2;
        }
    }
    if (alm_en) {
        if (i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_ALM_EN, alm_en, 1) < 0) {
            return -3;
        }
    }
    if (alm_sec) {
        if (i2c_bus_read_regs(SD3010_I2C_ADDR, SD3010_REG_ALM_SEC, &v, 1) < 0) {
            return -4;
        }
        *alm_sec = (uint8_t)(((v >> 4) & 0x07) * 10 + (v & 0x0F));
    }
    return 0;
}
