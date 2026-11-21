/* [历史/未编译] ble_ui.c —— M2 W1 的 BLE→LVGL 事件消费桩；CMake 不编译它。
 * 当前 BLE 事件由 services/svc_ble.c 的 svc_ble_tick() 在 UI 任务里 drain。 */
/*
 * PROTORIG ES M2 firmware - BLE -> LVGL glue (W1 stub)
 *
 * LVGL is built with LV_OS_NONE: lv_* calls are only legal from the LVGL
 * task.  The BLE stack runs in its own host task, so ble_svc.c pushes
 * events (connected / command / CTS write) into a FreeRTOS queue and this
 * module drains it from lvgl_task's main loop.
 *
 * W1 scope: consume the queue and log.  The real UI actions (notification
 * popup + motor, find-watch, CTS -> SD3010, battery label) land in W2/W3.
 */
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "shell.h"

#include "ble_ui.h"
#include "ble_svc.h"

#define BLE_UI_MAX_EVENTS_PER_POLL 8

static volatile int s_connected;
static volatile uint32_t s_cmd_count;
static volatile uint32_t s_cts_count;

void ble_ui_poll(void)
{
    ble_evt_t ev;
    int budget = BLE_UI_MAX_EVENTS_PER_POLL;

    while (budget-- > 0 && ble_svc_evt_pop(&ev)) {
        switch (ev.type) {
        case BLE_EVT_CONNECTED:
            s_connected = 1;
            printf("[ble_ui] phone connected\r\n");
            break;

        case BLE_EVT_DISCONNECTED:
            s_connected = 0;
            printf("[ble_ui] phone disconnected\r\n");
            break;

        case BLE_EVT_COMMAND:
            s_cmd_count++;
            printf("[ble_ui] cmd 0x%02x len=%u (stub)\r\n", ev.cmd, ev.len);
            /* TODO(W3): 0x03 NOTIFY -> popup + motor_pulse();
             *           0x04 FIND   -> motor_pulse(long);
             *           0x05 SET_TIME -> sd3010 + face redraw.
             * Keep LVGL work here (LVGL task), never in ble_svc.c. */
            break;

        case BLE_EVT_CTS_WRITE:
            s_cts_count++;
            printf("[ble_ui] CTS %04u-%02u-%02u %02u:%02u:%02u (TODO: SD3010)\r\n",
                   ev.data[0] | (ev.data[1] << 8), ev.data[2], ev.data[3],
                   ev.data[4], ev.data[5], ev.data[6]);
            break;

        default:
            break;
        }
    }
}

int ble_ui_connected(void)
{
    return s_connected;
}

uint32_t ble_ui_cmd_count(void)
{
    return s_cmd_count;
}

uint32_t ble_ui_cts_count(void)
{
    return s_cts_count;
}

/* shell: blestat -- BLE event mirror (LVGL task consumes the queue, so this
 * is a cheap read-only view; safe to call from the shell task). */
static int cmd_blestat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("[ble] connected=%d  cmd=%lu  cts=%lu\r\n",
           ble_ui_connected(),
           (unsigned long)ble_ui_cmd_count(),
           (unsigned long)ble_ui_cts_count());
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_blestat, blestat, BLE link + event queue status);
