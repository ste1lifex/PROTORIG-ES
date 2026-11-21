/*
 * svc_ble.h —— BLE 服务桥接（R4 第一里程碑）
 *
 * 职责：
 *   - 启动 ble_link（RF/controller/host/广播）；
 *   - 在 UI 任务里 drain ble_svc 的事件队列，转成 event_t：
 *       CONNECTED/DISCONNECTED -> app_state.ble_connected + EV_BLE_*
 *       CTS write / SET_TIME   -> svc_time_set() + EV_TIME_SET
 *       NOTIFY                 -> EV_NOTIFY（UI 做小卡片+震动）
 *       FIND                   -> EV_FIND（UI 连震 3 次）
 *   - BAS notify 桥接：svc_ble_battery_update() 只在电量变化时发。
 *
 * 约束：本服务不 include lvgl、不碰 motor/face；BLE 回调只进 ble_svc 队列。
 */
#ifndef SVC_BLE_H
#define SVC_BLE_H

#include <stdint.h>

/* 启动 BLE 协议栈（在 app_state/event_queue/svc_time/svc_battery 之后调用一次）。
 * 注意：只起 controller/host + GATT 注册 + 广播名，**手表模式不广播**
 * （2026-11-14 起 svc_ble_init() 末尾会 ble_link_stop()）；进电脑模式（-1 屏）
 * 才由 svc_pc_enter() 重新开广播。 */
void svc_ble_init(void);

/* UI 主循环每轮调用：drain BLE 事件 + 1 s 同步 CTS/BAS 缓存。 */
void svc_ble_tick(void);

/* 电量变化时由 UI 调（只在 batt_seq 变化时），内部只在 pct 变化时 notify。 */
void svc_ble_battery_update(int pct);

/* ---- R2.1 power：连接模式 / PDS 策略 ---- */
typedef int (*svc_ble_mode_persist_t)(uint8_t mode);

void svc_ble_set_persist_cb(svc_ble_mode_persist_t cb);
int  svc_ble_power_mode_get(void);
int  svc_ble_power_mode_set(int mode);   /* 0=省电 1=常连 2=智能；会持久化 */
void svc_ble_power_mode_load(int mode);  /* 启动时从 settings 恢复，不写回 */
void svc_ble_start(void);                /* 重新广播（屏亮/省电唤醒） */
void svc_ble_stop(void);                 /* 手表模式省电屏灭：停广播/断连 */
int  svc_ble_is_active(void);            /* 1 = 已连接或正在广播 */
int  svc_ble_pds_allowed(void);          /* board_power PDS 回调 */

/* R2.1-BMP：BLE 写回调直接喂位图状态机（绕过 64 B 事件队列）。
 * cmd = 0x40/0x41/0x42/0x43；BEGIN/END 由本函数回 ACK。 */
void svc_ble_bmp_feed(uint8_t cmd, const uint8_t *payload, uint8_t len);

#endif /* SVC_BLE_H */
