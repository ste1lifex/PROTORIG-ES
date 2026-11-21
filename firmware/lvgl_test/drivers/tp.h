/**
 * @file  tp.h
 * @brief 触摸控制器 CST9220（PROTORIG ES V1.6）
 *
 * V1.6 硬件（SCH_Schematic1_1-P1_2026-10-18）：
 *   I2C0  SCL = IO0         （与 SD3010 RTC 共用，R1/R2 = 5.1k 上拉）
 *         SDA = IO1
 *   TP_RST = IO22           10k 下拉，低电平复位，正常拉高
 *   TP_INT = IO16           ★ AON 脚，低电平有效（按下时拉低）
 *
 *   ⚠️ 总线默认 400 kHz（2026-10-29 触摸回归修复，见 i2c_bus.h）；SD3010 吃
 *      BAT+，400 kHz 只在 4.5~5.5 V 下保证，sd3010_read() 首次失败会自动降到
 *      100 kHz 并保持到重启。触摸不独占总线，所有访问走 i2c_bus.c。
 *   ⚠️ IO16 是 BL616 仅有的 AON 脚之一，PDS 触摸唤醒靠它。
 *
 * CST9220 用 16bit 寄存器地址，且**读寄存器前必须先写 0xD101 = D1 01
 * 进命令模式**，否则读出来全是 0xFFFF。
 *
 * 参考：厂商 esp_lcd_touch_cst9220.c
 */
#ifndef __TP_H
#define __TP_H

#include <stdint.h>

#define TP_I2C_SCL_PIN 0
#define TP_I2C_SDA_PIN 1
#define TP_I2C_FREQ_HZ 100000 /* 旧值，已无人引用：实际频率 = i2c_bus_init(I2C_BUS_FREQ_HZ) */

#define TP_RST_PIN 22
#define TP_INT_PIN 16

#define TP_ADDR_DEFAULT 0x5A

/* 寄存器 */
#define TP_REG_DATA        0xD000 /* 触摸数据，10 字节 */
#define TP_REG_CMD_MODE    0xD101
#define TP_REG_CHECKCODE   0xD1FC
#define TP_REG_RESOLUTION  0xD1F8
#define TP_REG_PROJECT_ID  0xD204

#define TP_ACK_VALUE 0xAB

/** GPIO（RST/INT）初始化 + 一次硬件复位；I2C0 由 i2c_bus_init() 负责。0 = 成功 */
int tp_init(void);

/** 只做硬件复位：RST 拉低 10 ms -> 拉高，再等 150 ms（手册 TRON 典型 100 ms） */
void tp_hw_reset(void);

/** 读 INT 引脚电平（0 = 有触摸）。返回原始电平 */
int tp_int_level(void);

/** INT 是否有效（低电平） */
int tp_int_asserted(void);

/** 读 16bit 寄存器 */
int tp_read_reg16(uint8_t addr7, uint16_t reg, uint8_t *data, uint32_t len);

/** 写 16bit 寄存器 */
int tp_write_reg16(uint8_t addr7, uint16_t reg, const uint8_t *data, uint32_t len);

/** 进命令模式 + 打印 checkcode / 分辨率 / 项目号。返回 0 成功 */
int tp_cst9220_info(uint8_t addr7);

/** 读一次触摸点。返回按下点数（0 = 没按） */
int tp_cst9220_read_points(uint8_t addr7, uint16_t *x, uint16_t *y);

/**
 * @brief 触摸读取统计（诊断用）
 *
 * 用来判断「拖尾不跟手」到底是哪一环：
 *   - i2c_err / bad_ack 高  -> 读不到（I2C 或时序问题）
 *   - nopoint 高            -> 帧有效但芯片没报点（模式问题）
 *   - hit 高但画面不动       -> 瓶颈在刷屏（DBI），不在触摸
 */
typedef struct {
    uint32_t reads;      /* 总共读了多少次 */
    uint32_t i2c_err;    /* I2C 传输失败 */
    uint32_t bad_ack;    /* data[6] != 0xAB，帧无效 */
    uint32_t nopoint;    /* 帧有效但报点数为 0 */
    uint32_t bad_status; /* 报了点但 status != 0x06 */
    uint32_t hit;        /* 有效触摸点 */
    uint32_t status_hist[16]; /* p[0] & 0x0F 的分布，用来看芯片到底在报什么状态 */
    uint32_t ack_hist[16];    /* data[6] 低 4 位的分布 */
    uint32_t pts_hist[8];     /* 报文里点数的分布：0 / 1 / 2 / 3 / 4 / 5 / 6+ */
} tp_stats_t;

void tp_get_stats(tp_stats_t *s);
void tp_reset_stats(void);

/* LVGL task only: arm a bounded raw capture, or stop and dump it. */
void tp_trace_start(void);
void tp_trace_dump(void);

/**
 * @brief 允许哪些 status（p[0] & 0x0F）算作**新**触摸点，位掩码
 *
 * 默认只认 0x06。这是抓原始报文确认的：
 *   status=6 -> 真的新报点（坐标会变），INT 拉低也基本同时出现
 *   status=0 -> 「位置没变」的回显（坐标是上一次的，会重复很多次）
 * **不要把 0 放进来**，否则松开手指后芯片继续回显最后一个坐标，
 * 会变成幽灵触摸（长时间间隔的两次触摸被连成一条线）。
 *
 * 保留这个开关是为了能现场 A/B（shell `tpaccept`，只在 PROTORIG_DEBUG_SHELL 里编）：
 *   tpaccept 0x40   只认 6（默认）
 *   tpaccept 0x41   认 0 和 6（2026-10-12 实测失败，已退回）
 *   tpaccept 0x7F   认 0~6
 */
void tp_set_accept_status(uint32_t mask);
uint32_t tp_get_accept_status(void);

/* ------------------------------------------------------------------ */
/* CST9220 模式命令（厂商《触摸芯片移植手册 v3.2》§11）                */
/*                                                                    */
/* 写法：往 0xD101 写 2 字节 {0xD1, cmd}。                            */
/* 安全性：这些命令只改芯片 **RAM 状态**，不写 flash，硬复位          */
/*         （tp_hw_reset，把 TP_RST 拉低 10ms）一定能救回来，         */
/*         芯片内部还有看门狗兜底。                                   */
/* ⚠️ 绝对不要碰：以 0xA0 开头的 3 字节写（bootloader 通道）、        */
/*    0xD114 / 0xD119(factory) / 0xD108（手册与 SensorLib 名字冲突）。*/
/* ------------------------------------------------------------------ */
#define TP_CMD_DEBUG_INFO 0x01 /* 进命令模式（读版本信息前必须） */
#define TP_CMD_RESET      0x02 /* 软复位 */
#define TP_CMD_RECALIB    0x04 /* 重新校准 */
#define TP_CMD_DEEP_SLEEP 0x05 /* 深度睡眠 2µA，⚠️ 触摸叫不醒，只能 RST */
#define TP_CMD_DEBUG_PTS  0x08 /* debug 报点模式 */
#define TP_CMD_NORMAL     0x09 /* 正常报点模式（默认）—— 读完版本信息必须写回它 */
#define TP_CMD_FACTORY    0x19 /* 工厂模式 */

#define TP_REG_MODE_STATUS 0x0002 /* 回显最后一条模式命令（天然自检） */
#define TP_REG_GESTURE     0xD04C /* bit7=使能/状态, bit6:0=手势码 */
#define TP_REG_PROXIMITY   0xD04B /* 接近感应：bit7 使能, &0x7F: 0=远 1=近 */

/** 模式命令：往 0xD101 写 {0xD1, cmd}。0 = 成功 */
int tp_set_mode(uint8_t addr7, uint8_t cmd);

/** 读 0x0002 的模式回显（2 字节，out[1] 应等于最后一条命令码） */
int tp_read_mode_status(uint8_t addr7, uint8_t out[2]);

/** 裸写（不加寄存器地址），手势/接近这类 3 字节命令要用它 */
int tp_raw_write(uint8_t addr7, const uint8_t *data, uint32_t len);

/** CST92XX: 发 D1 06（退出低功耗扫描）裸命令；返回 tp_raw_write 结果 */
int tp_exit_lp_scan(void);

/** CST92XX: 发 D1 07（进入低功耗扫描）裸命令；返回 tp_raw_write 结果 */
int tp_enter_lp_scan(void);

/** 手势检测模式开/关（3 字节裸命令 D0 4C 80 / D0 4C 00）。
 *  写完等 10ms 回读 0xD04C 自检：bit7==1 表示进成功。
 *  返回 0xD04C 的原值（bit6:0 = 手势码），-1 = I2C 失败 */
int tp_gesture(uint8_t addr7, int on);

/** 接近感应开/关（D0 4B 80 / D0 4B 00）。返回 0xD04B 原值，-1 = 失败 */
int tp_proximity(uint8_t addr7, int on);

/* ---- 低功耗扫描（V1.6 PDS 用）---- */
/** 进厂商 gesture 低功耗扫描（D0 4C 80）；返回 0xD04C 值（bit7=1 成功），<0 失败 */
int tp_lowpower_enter(void);
/** 退出低功耗：硬件复位 + 回 normal 模式；返回 0 成功 */
int tp_lowpower_exit(void);

/** 运行时切换共享 I2C0 速率（默认 400 kHz 由 i2c_bus_init 设定；调之前先确认 SD3010 不丢应答） */
int tp_set_i2c_freq(uint32_t hz);

#endif /* __TP_H */
