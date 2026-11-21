/**
 * @file  i2c_bus.c
 * @brief PROTORIG ES V1.6 板载 I2C0 总线实现（SD3010 + CST9220 共用）
 *
 * 锁：递归互斥锁。所有 bus_* 公共函数内部自动加锁，调用方无需关心；
 * 需要"多事务原子"时用 i2c_bus_lock()/i2c_bus_unlock() 包起来
 * （可重入，和内部锁嵌套不会死锁）。
 */
#include "i2c_bus.h"

#include <stdio.h>

#include "bflb_gpio.h"
#include "bflb_i2c.h"
#include "board.h"

#include "FreeRTOS.h"
#include "semphr.h"

static struct bflb_device_s *i2c0_dev = NULL;
static SemaphoreHandle_t s_bus_mtx = NULL;
static uint32_t s_freq_hz = I2C_BUS_FREQ_HZ;

uint32_t i2c_bus_get_freq(void)
{
    return s_freq_hz ? s_freq_hz : I2C_BUS_FREQ_HZ;
}

void i2c_bus_lock(void)
{
    if (s_bus_mtx != NULL) {
        (void)xSemaphoreTakeRecursive(s_bus_mtx, portMAX_DELAY);
    }
}

void i2c_bus_unlock(void)
{
    if (s_bus_mtx != NULL) {
        (void)xSemaphoreGiveRecursive(s_bus_mtx);
    }
}

int i2c_bus_init(uint32_t freq_hz)
{
    struct bflb_device_s *gpio;

    gpio = bflb_device_get_by_name("gpio");
    if (gpio == NULL) {
        return -1;
    }

    /* 本板 I2C0 的 SCL 在 GPIO0、SDA 在 GPIO1（不是 SDK 默认的 14/11） */
    bflb_gpio_init(gpio, I2C_BUS_SCL_PIN,
                   GPIO_FUNC_I2C0 | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(gpio, I2C_BUS_SDA_PIN,
                   GPIO_FUNC_I2C0 | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);

    i2c0_dev = bflb_device_get_by_name("i2c0");
    if (i2c0_dev == NULL) {
        return -2;
    }

    bflb_i2c_init(i2c0_dev, freq_hz);
    s_freq_hz = freq_hz;

    /* 调度器启动前创建也合法；创建失败就退化成"无锁"（单线程调试场景） */
    if (s_bus_mtx == NULL) {
        s_bus_mtx = xSemaphoreCreateRecursiveMutex();
        if (s_bus_mtx == NULL) {
            printf("[i2c] WARN: bus mutex create failed, running unlocked\r\n");
        }
    }
    return 0;
}

/* 一次写事务：hdr（1~2 字节寄存器地址）+ data（可为 0 字节） */
static int bus_write(uint8_t addr7, const uint8_t *hdr, uint32_t hlen,
                     const uint8_t *data, uint32_t len)
{
    struct bflb_i2c_msg_s msgs[2];

    if (i2c0_dev == NULL) {
        return -1;
    }
    if (hdr == NULL || hlen == 0 || hlen > 4 || len > 32) {
        return -2;
    }

    if (len == 0) {
        /* 只发寄存器地址（也当作 probe 用） */
        msgs[0].addr = addr7;
        msgs[0].flags = 0;
        msgs[0].buffer = (uint8_t *)hdr;
        msgs[0].length = (uint16_t)hlen;
        return bflb_i2c_transfer(i2c0_dev, msgs, 1);
    }

    /* 用 NOSTOP 让寄存器地址和后续数据之间不产生 STOP，
       和 SDK 的 ds3231 例子保持一致（该芯片要求一次连续写）。 */
    msgs[0].addr = addr7;
    msgs[0].flags = I2C_M_NOSTOP;
    msgs[0].buffer = (uint8_t *)hdr;
    msgs[0].length = (uint16_t)hlen;

    msgs[1].addr = addr7;
    msgs[1].flags = I2C_M_NOSTART;
    msgs[1].buffer = (uint8_t *)data;
    msgs[1].length = (uint16_t)len;

    return bflb_i2c_transfer(i2c0_dev, msgs, 2);
}

/* 一次读事务：先写 hdr（NOSTOP），再 repeated-START 读 data */
static int bus_read(uint8_t addr7, const uint8_t *hdr, uint32_t hlen,
                    uint8_t *data, uint32_t len)
{
    struct bflb_i2c_msg_s msgs[2];

    if (i2c0_dev == NULL) {
        return -1;
    }
    if (hdr == NULL || data == NULL || hlen == 0 || hlen > 4 || len == 0 || len > 32) {
        return -2;
    }

    msgs[0].addr = addr7;
    msgs[0].flags = I2C_M_NOSTOP;
    msgs[0].buffer = (uint8_t *)hdr;
    msgs[0].length = (uint16_t)hlen;

    msgs[1].addr = addr7;
    msgs[1].flags = I2C_M_READ | I2C_M_NOSTART;
    msgs[1].buffer = data;
    msgs[1].length = (uint16_t)len;

    return bflb_i2c_transfer(i2c0_dev, msgs, 2);
}

int i2c_bus_write_regs(uint8_t addr7, uint8_t reg, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[1] = { reg };
    int ret;

    if (len != 0 && data == NULL) {
        return -2;
    }
    i2c_bus_lock();
    ret = bus_write(addr7, hdr, 1, data, len);
    i2c_bus_unlock();
    return ret;
}

int i2c_bus_read_regs(uint8_t addr7, uint8_t reg, uint8_t *data, uint32_t len)
{
    uint8_t hdr[1] = { reg };
    int ret;

    i2c_bus_lock();
    ret = bus_read(addr7, hdr, 1, data, len);
    i2c_bus_unlock();
    return ret;
}

int i2c_bus_write_regs16(uint8_t addr7, uint16_t reg, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    int ret;

    if (len != 0 && data == NULL) {
        return -2;
    }
    i2c_bus_lock();
    ret = bus_write(addr7, hdr, 2, data, len);
    i2c_bus_unlock();
    return ret;
}

int i2c_bus_read_regs16(uint8_t addr7, uint16_t reg, uint8_t *data, uint32_t len)
{
    uint8_t hdr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    int ret;

    i2c_bus_lock();
    ret = bus_read(addr7, hdr, 2, data, len);
    i2c_bus_unlock();
    return ret;
}

int i2c_bus_write_raw(uint8_t addr7, const uint8_t *data, uint32_t len)
{
    struct bflb_i2c_msg_s msg;
    int ret;

    if (i2c0_dev == NULL) {
        return -1;
    }
    if (data == NULL || len == 0 || len > 32) {
        return -2;
    }

    msg.addr = addr7;
    msg.flags = 0;
    msg.buffer = (uint8_t *)data;
    msg.length = (uint16_t)len;

    i2c_bus_lock();
    ret = bflb_i2c_transfer(i2c0_dev, &msg, 1);
    i2c_bus_unlock();
    return ret;
}

int i2c_bus_probe(uint8_t addr7)
{
    /* 注意：读操作在本 SDK 里不检查 NACK，所以必须用「写」来探测 */
    return i2c_bus_write_regs(addr7, 0x00, NULL, 0);
}

int i2c_bus_set_freq(uint32_t freq_hz)
{
    if (i2c0_dev == NULL || freq_hz == 0) {
        return -1;
    }

    i2c_bus_lock();
    bflb_i2c_deinit(i2c0_dev);
    bflb_i2c_init(i2c0_dev, freq_hz);
    s_freq_hz = freq_hz;
    i2c_bus_unlock();
    return 0;
}
