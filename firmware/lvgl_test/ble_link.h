/*
 * PROTORIG ES M2 firmware - BLE radio/stack bring-up + advertising
 *
 * Merged from firmware_ble_probe/ble_probe_link.* in W1 (2026-10-28).
 * One call to ble_link_start() creates a task that initializes
 *   rfparam_init -> btble_controller_init -> hci_driver_init -> bt_enable
 *   -> GATT registration (ble_svc.c) -> connectable advertising
 * then deletes itself.
 *
 * Call it from the lvgl_task init sequence (after board_init/peripherals);
 * in this firmware it is reached via svc_ble_init() from main.c's lvgl_task.
 * The BLE task runs at configMAX_PRIORITIES - 2, i.e. above the LVGL task,
 * so stack bring-up finishes before UI rendering resumes.
 *
 * Requires: CONFIG_BLUETOOTH=y, CONFIG_RF=y,
 *           CONFIG_BTBLECONTROLLER_LIB=ble1m0s1bredr0 (BL616 BLE-only).
 */
#ifndef BLE_LINK_H
#define BLE_LINK_H

/*
 * 广播名前缀（F2，2026-11-08）：完整名字是 "<前缀> <ADMIN_ID>"，
 * 例如 "PROTORIG ES 2BEA4E"（18 字符）。运行时在 ble_link.c 里拼，
 * 与 bt_set_name() 用同一块缓冲。
 * 前缀不能改：Android 扫描端用 contains("PROTORIG ES")、DSH 插件 BLE 桥
 * 用 startswith()，两边都靠这个前缀兼容。
 */
#define BLE_DEVICE_NAME_PREFIX "PROTORIG ES"
/* 运行时名字缓冲（"PROTORIG ES 2BEA4E" = 18 字符）。
 * 取 32 而不是刚刚好 18：SDK 的 bt_set_name() 内部按
 * CONFIG_BT_DEVICE_NAME_MAX + 1（本 SDK 29+1=30）整块 memcpy 复制名字，
 * 缓冲给足才不会让协议栈读到数组外。 */
#define BLE_DEVICE_NAME_MAX    32

/*
 * Create the BLE bring-up task.  Safe to call before vTaskStartScheduler();
 * the actual controller/host init then runs from the task context.
 */
void ble_link_start(void);

/* R2.1 power：停/启广播（模式 0 屏灭用）；不会反初始化 controller/host。 */
void ble_link_stop(void);
void ble_link_start_adv(void);
int  ble_link_is_active(void);      /* 1 = 已连接或正在广播 */
int  ble_link_is_connected(void);
int  ble_link_adv_on(void);
void ble_link_force_adv(void);
void ble_link_note_rx(void);
uint32_t ble_link_idle_ms(void);
void ble_link_drop_conn(void);

#endif /* BLE_LINK_H */
