/*
 * Stubs for host unit tests of bsp/board_power.c power timing logic.
 * The hardware drivers are not linked; only the symbols used by the
 * power state machine are provided.
 */
#include <stdint.h>

int st7802_write_cmd(uint8_t cmd, const uint8_t *data, uint32_t len)
{
    (void)cmd;
    (void)data;
    (void)len;
    return 0;
}
