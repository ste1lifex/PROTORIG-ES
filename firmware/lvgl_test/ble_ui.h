/* [历史/未编译] ble_ui.h —— 只被未编译的 ble_ui.c 使用；
 * 当前 BLE 的 UI 侧入口是 services/svc_ble.h。 */
/*
 * PROTORIG ES M2 firmware - BLE -> LVGL glue
 *
 * Only the LVGL task may call ble_ui_poll(): LVGL is configured with
 * LV_OS_NONE, so BLE callbacks must never call lv_* directly.
 */
#ifndef BLE_UI_H
#define BLE_UI_H

#include <stdint.h>

/* Drain BLE events (connected / command / CTS) and apply them to the UI.
 * Call from the lvgl_task main loop only.  Non-blocking. */
void ble_ui_poll(void);

/* Connection mirror updated by ble_ui_poll() (read-only elsewhere). */
int ble_ui_connected(void);
uint32_t ble_ui_cmd_count(void);
uint32_t ble_ui_cts_count(void);

#endif /* BLE_UI_H */
