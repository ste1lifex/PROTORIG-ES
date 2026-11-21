/**
 * @file  i2c_bus.h
 * @brief PROTORIG ES V1.6 板载 I2C0 总线（IO0/IO1）
 *
 * V1.6 硬件（SCH_Schematic1_1-P1_2026-10-18）：
 *   IO0 -> I2C0_SCL        IO1 -> I2C0_SDA
 *   上拉 R1/R2 = 5.1k 到 3V3
 *   U2  SD3010  RTC   地址 0x32   （VDD = BAT+，无 32.768 kHz 晶振）
 *       CST9220 触摸  地址 0x5A
 *
 * 一条总线两个器件，所有访问必须串行化：本模块内部用**递归互斥锁**包住
 * 每次 transfer，所以调用方不需要自己加锁。确需多事务原子性时再用
 * i2c_bus_lock()/i2c_bus_unlock()（可重入，不会死锁）。
 *
 * 总线速度（2026-10-29 触摸回归修复）：默认 400 kHz —— 真机实测触摸丢点
 * 明显改善，与 V1.5 历史结论一致。SD3010 的 400 kHz 只在 4.5~5.5 V 下保证，
 * 而它吃 BAT+（3.0~4.2 V）：sd3010_read() 在 400k 首次失败会自动降级 100k
 * 并重试一次，之后保持 100k 到下次启动（低电量安全）。
 */
#ifndef __I2C_BUS_H
#define __I2C_BUS_H

#include <stdint.h>

#define I2C_BUS_SCL_PIN 0
#define I2C_BUS_SDA_PIN 1

/*
 * 默认 400 kHz（触摸回归修复）；SD3010 读取失败时 sd3010_read() 自动降级
 * 到 100 kHz 并重试，见 drivers/sd3010.c。
 */
#define I2C_BUS_FREQ_HZ 400000

/** 初始化 GPIO 复用 + I2C0 控制器 + 总线锁，返回 0 表示成功 */
int i2c_bus_init(uint32_t freq_hz);

/* ---- 8 位寄存器地址（SD3010 等）---- */
int i2c_bus_write_regs(uint8_t addr7, uint8_t reg, const uint8_t *data, uint32_t len);
int i2c_bus_read_regs(uint8_t addr7, uint8_t reg, uint8_t *data, uint32_t len);

/* ---- 16 位寄存器地址（CST9220：D0xx，高字节是命令字节）---- */
int i2c_bus_write_regs16(uint8_t addr7, uint16_t reg, const uint8_t *data, uint32_t len);
int i2c_bus_read_regs16(uint8_t addr7, uint16_t reg, uint8_t *data, uint32_t len);

/** 裸写（不带寄存器头，CST9220 的手势/接近感应命令用） */
int i2c_bus_write_raw(uint8_t addr7, const uint8_t *data, uint32_t len);

/** 探测器件是否应答（用一个 1 字节写试探）。返回 0 表示在 */
int i2c_bus_probe(uint8_t addr7);

/** 运行时改总线频率（诊断/降级用；返回 0 成功） */
int i2c_bus_set_freq(uint32_t freq_hz);

/** 当前总线频率（Hz）；未 init 时为 I2C_BUS_FREQ_HZ */
uint32_t i2c_bus_get_freq(void);

/** 可重入总线锁（正常 bus_* 已自动加锁，这里给"多事务原子"用） */
void i2c_bus_lock(void);
void i2c_bus_unlock(void);

#endif /* __I2C_BUS_H */
