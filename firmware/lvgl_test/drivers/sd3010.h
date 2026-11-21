/**
 * @file  sd3010.h
 * @brief 兴威帆 SD3010 内置晶振实时时钟（I2C，7bit 地址 0x32）
 *
 * 数据手册 SD3010 DATASHEET V2.0
 *   器件代码 0110010B = 0x32
 *   0x00~0x06 : 秒 分 时 星期 日 月 年，全部 BCD
 *   0x0F CTR1 : bit7=WRTC3 bit2=WRTC2 bit0=RTCF ...
 *   0x10 CTR2 : bit7=WRTC1 ...
 *
 * 写保护：先 WRTC1=1，再 WRTC2/WRTC3=1 才允许写 00H~71H；
 *         解除时先 WRTC2/WRTC3=0，再 WRTC1=0。
 * 写时间必须一次把 7 个字节全写下去（不能只改一个字节）。
 */
#ifndef __SD3010_H
#define __SD3010_H

#include <stdint.h>

#define SD3010_I2C_ADDR 0x32

#define SD3010_REG_SEC  0x00
#define SD3010_REG_MIN  0x01
#define SD3010_REG_HOUR 0x02
#define SD3010_REG_WEEK 0x03
#define SD3010_REG_DAY  0x04
#define SD3010_REG_MON  0x05
#define SD3010_REG_YEAR 0x06
/* 报警寄存器（DATASHEET V2.0 §4.3，07H~0DH + 允许位 0EH） */
#define SD3010_REG_ALM_SEC  0x07
#define SD3010_REG_ALM_MIN  0x08
#define SD3010_REG_ALM_HOUR 0x09
#define SD3010_REG_ALM_WEEK 0x0A  /* 位图：AW0=周日 ... AW6=周六 */
#define SD3010_REG_ALM_DAY  0x0B
#define SD3010_REG_ALM_MON  0x0C
#define SD3010_REG_ALM_YEAR 0x0D
#define SD3010_REG_ALM_EN   0x0E  /* bit0=EAS 秒, bit1=EAMN 分, bit2=EAH 时,
                                     bit3=EAW 星期, bit4=EAD 日, bit5=EAMO 月, bit6=EAY 年 */
#define SD3010_REG_CTR1 0x0F
#define SD3010_REG_CTR2 0x10
#define SD3010_REG_CTR3 0x11
#define SD3010_REG_CTR5 0x1A
#define SD3010_REG_USER_RAM 0x2C   /* 2CH~71H 用户 RAM（70 字节，VDD 域） */

typedef struct {
    uint8_t year;  /* 00~99  -> 20xx */
    uint8_t month; /* 01~12 */
    uint8_t day;   /* 01~31 */
    uint8_t week;  /* 00=周日 01=周一 ... 06=周六 */
    uint8_t hour;  /* 00~23 */
    uint8_t min;   /* 00~59 */
    uint8_t sec;   /* 00~59 */
} sd3010_time_t;

/** 探测器件，0 = 在线 */
int sd3010_init(void);

/** 读时间（一次突发读 7 字节） */
int sd3010_read(sd3010_time_t *tm);

/** 写时间（自动做写允许/写禁止） */
int sd3010_set_time(const sd3010_time_t *tm);

/** 读状态寄存器，方便排查（RTCF / OSF / SYS / OSC_RDY） */
int sd3010_get_status(uint8_t *ctr1, uint8_t *ctr5);

/** 清 OSF（停振标志）—— 重新对时之后调用，否则下次开机仍会被判成"丢过时间" */
int sd3010_clear_osf(void);

/** 检查/写入"掉电魔数"：0 = 没丢过电（用户 RAM 里的标记还在） */
int sd3010_check_magic(void);
int sd3010_write_magic(void);

/** 用户 RAM 通用读写（2CH~71H，前 4 字节留给魔数）。addr+len 越界返回 -1 */
int sd3010_ram_read(uint8_t addr, uint8_t *buf, uint8_t len);
int sd3010_ram_write(uint8_t addr, const uint8_t *buf, uint8_t len);

/** Sakamoto 算法，0=周日 */
uint8_t sd3010_calc_week(uint16_t full_year, uint8_t month, uint8_t day);

/* ------------------------------------------------------------------ */
/* 中断 / 报警（V1.6：SD3010 INT -> IO17，开漏低有效，5.1k 上拉）      */
/*                                                                    */
/* 用法（分钟对齐）：sd3010_alarm_set_sec(0) 后，每到整分钟拉低 INT；  */
/* 报警标志 INTAF(CTR1 bit5) 需要 sd3010_int_clear() 清，否则一直低。  */
/* 注意：每次写 0EH 都会自动清 INTAF（手册 §4.3）。                    */
/* ------------------------------------------------------------------ */
int sd3010_alarm_set_sec(uint8_t sec);   /* 0~59，只使能秒报警 */
/* 每天 HH:MM:SS 报警（2026-11-08：闹钟用；只比较 秒/分/时，不比较星期/日/月/年）。
 * 小时寄存器最高位恒为 0（手册 §4.3）；INT 仍是"单事件、低电平保持到清 INTAF"，
 * 因此 PDS 的 IO17 低电平唤醒可以直接用。 */
int sd3010_alarm_set_hhmm(uint8_t hour, uint8_t min, uint8_t sec);
int sd3010_alarm_disable(void);
int sd3010_int_clear(void);              /* 清 INTAF/INTDF */
int sd3010_int_get(uint8_t *ctr1, uint8_t *ctr2, uint8_t *alm_en, uint8_t *alm_sec);

#endif /* __SD3010_H */
