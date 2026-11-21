/*
 * PROTORIG ES M2 firmware - BLE GATT service layer
 *
 * Merged from firmware_ble_probe/ble_probe_svc.* in W1 (2026-10-28) and
 * renamed ble_*.  The GATT services are unchanged:
 *
 *   0x180A  Device Information (Manufacturer / Model / Firmware revision)
 *   0x180F  Battery Service  (Battery Level 0x2A19, read + notify)
 *   0x1805  Current Time Service (0x2A2B, write + read back, 10-byte BLE format)
 *   50524F54-4F52-4947-4553-000000000001  PROTORIG custom service
 *       ...0002  Command  (write / write-without-response)
 *       ...0003  Event    (notify, responses come out here)
 *
 * The custom command frame is deliberately tiny:
 *     request : A5 5A <cmd> <len> <payload[len]>
 *     response: A5 5A <cmd|80> <len> <payload[len]>
 *
 * Implemented commands:
 *     0x01 PING      -> "PONG"
 *     0x02 GET_INFO  -> proto(1) fw_major fw_minor fw_patch battery
 *                      （fw_* = 固件版本，唯一来源 fw_version.h；旧固件曾固定 0.4.0）
 *                      caps(u16 LE) uptime_seconds(u32 LE) power_mode
 *                      admin_id(u32 LE)                             = 16 bytes
 *                      （F4：admin_id 是 24 bit 管理员编号，高字节 0）
 *
 * Error responses carry a one-byte status:
 *     0x01 bad frame / wrong header / length mismatch
 *     0x02 unknown command
 *     0x03 payload too long for this firmware
 *
 * ---- LVGL decoupling -------------------------------------------------------
 * LVGL is configured with LV_OS_NONE, so BLE callbacks (host task) must never
 * call into LVGL.  Every callback that the UI cares about is pushed into a
 * FreeRTOS queue (ble_evt_t); services/svc_ble.c pops it with
 * ble_svc_evt_pop() from svc_ble_tick() in the UI task.
 */
#ifndef BLE_SVC_H
#define BLE_SVC_H

#include <stdint.h>
#include "bluetooth.h"

struct bt_conn;

#define BLE_PROTO_MAGIC0          0xA5u
#define BLE_PROTO_MAGIC1          0x5Au
#define BLE_PROTO_RSP_FLAG        0x80u

#define BLE_CMD_PING              0x01u
#define BLE_CMD_GET_INFO          0x02u
#define BLE_CMD_SET_TIME          0x03u
#define BLE_CMD_NOTIFY            0x20u
#define BLE_CMD_FIND              0x22u
#define BLE_CMD_SET_POWER_MODE    0x30u
#define BLE_CMD_BMP_BEGIN          0x40u
#define BLE_CMD_BMP_DATA           0x41u
#define BLE_CMD_BMP_END            0x42u
#define BLE_CMD_BMP_ABORT          0x43u
/* 电脑模式（透传显示）：payload = [op][args...]，见 services/svc_pc.h */
#define BLE_CMD_PC_FRAME           0x50u
/* 电脑模式进/出：payload = [on]（1=进 -1 屏电脑模式，0=退出回表盘）。
 * 走普通命令队列，由 UI 任务执行 —— 会切页面/冻结 LVGL，不能在协议栈回调里做。 */
#define BLE_CMD_PC_MODE            0x51u
/* BLE 数字通道：payload =
 *   cpu u16, gpu u16, ram u16, vram u16, brand u8, rx u32, tx u32 （共 15 B）
 * 单位十分之一百分比；用于无 USB 时让手表本地画三环。 */
#define BLE_CMD_METRICS            0x52u

#define BLE_STATUS_BAD_FRAME      0x01u
#define BLE_STATUS_UNKNOWN_CMD    0x02u
#define BLE_STATUS_TOO_LONG       0x03u

/* BLE 服务自身的版本（GET_INFO 回帧用），不是整机固件版本：
 * 固件版本看 main.c 的 PROTORIG_FW_VERSION / shell `ver`（当前 1.6.0+构建时间）。 */
/* 协议版本（不是固件版本；固件版本见 fw_version.h）。 */
#define BLE_PROTO_VERSION         1u

#define BLE_CAP_PING              (1u << 0)
#define BLE_CAP_GET_INFO          (1u << 1)
#define BLE_CAP_SET_TIME          (1u << 2)
#define BLE_CAP_NOTIFY            (1u << 3)
#define BLE_CAP_FIND              (1u << 4)
#define BLE_CAP_SET_POWER_MODE    (1u << 5)
#define BLE_CAP_BMP_NOTIFY        (1u << 6)
#define BLE_CAP_PC_MODE           (1u << 7)
#define BLE_CAP_METRICS           (1u << 8)
#define BLE_CAPABILITIES          (BLE_CAP_PING | BLE_CAP_GET_INFO | BLE_CAP_SET_TIME | BLE_CAP_NOTIFY | BLE_CAP_FIND | BLE_CAP_SET_POWER_MODE | BLE_CAP_BMP_NOTIFY | BLE_CAP_PC_MODE | BLE_CAP_METRICS)

/* ------------------------------------------------------------------ */
/* BLE -> LVGL event queue                                            */
/* ------------------------------------------------------------------ */

#define BLE_EVT_PAYLOAD_MAX       64

typedef enum {
    BLE_EVT_CONNECTED    = 1,
    BLE_EVT_DISCONNECTED = 2,
    BLE_EVT_COMMAND      = 3,   /* valid frame: cmd + payload (truncated) */
    BLE_EVT_CTS_WRITE    = 4,   /* phone wrote Current Time (10-byte value) */
} ble_evt_type_t;

typedef struct {
    uint8_t type;                        /* ble_evt_type_t */
    uint8_t cmd;                         /* BLE_EVT_COMMAND only */
    uint8_t len;                         /* bytes used in data[] */
    uint8_t data[BLE_EVT_PAYLOAD_MAX];
} ble_evt_t;

/* Register all M2 GATT services. Call once, after bt_enable() succeeded.
 * Also creates the event queue. */
int ble_svc_init(void);

/* Connection bookkeeping (used for notifications). Called from BLE task. */
void ble_svc_connected(struct bt_conn *conn);
void ble_svc_disconnected(void);

/* Notifies the current Battery Level if a peer subscribed. */
void ble_svc_battery_tick(void);

/* Feed a real battery percentage (main.c ui_batt_apply() via
 * svc_ble_battery_update); also notifies when a peer has BAS notify enabled. */
void ble_svc_battery_update(uint8_t level);

/* Pop one UI event (non-blocking).  Only call from the LVGL task.
 * @return 1 = event stored in *out, 0 = queue empty / not initialised. */
int ble_svc_evt_pop(ble_evt_t *out);

/* Send a custom-service response frame: A5 5A <cmd> <len> <payload>.
 * Must be called from the BLE/host task (or with the stack idle). */
int ble_svc_send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len);

/* Send an ACK/status for a command (cmd|0x80, len=1, status). */
void ble_svc_send_ack(uint8_t cmd, uint8_t status);

/* Update the cached CTS value (e.g. from RTC) so reads return real time. */
void ble_svc_cts_cache_set(const uint8_t value[10]);

/* R2.1：当前连接模式，GET_INFO 返回；0=省电 1=常连 2=智能 */
void ble_svc_set_power_mode(uint8_t mode);
uint8_t ble_svc_get_power_mode(void);

#endif /* BLE_SVC_H */
