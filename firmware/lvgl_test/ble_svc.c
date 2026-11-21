/*
 * PROTORIG ES M2 firmware - BLE GATT services (DIS / BAS / CTS / custom)
 *
 * Merged from firmware_ble_probe/ble_probe_svc.c (W1, 2026-10-28).
 * Written against the Bouffalo SDK "blestack" Zephyr-style API, in the same
 * style as components/.../examples/btble/peripheral/ble_tp_svc.c.
 *
 * The battery level RAM byte starts at 88% (placeholder) and is replaced by the
 * real value: services/svc_battery.c samples the ADC, main.c ui_batt_apply() calls
 * svc_ble_battery_update() -> ble_svc_battery_update().
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "bluetooth.h"
#include "conn.h"
#include "gatt.h"
#include "bt_uuid.h"

#include "ble_svc.h"
#include "ble_link.h"     /* ble_link_note_rx()：BLE 收包活性计数 */
#include "fw_version.h"     /* DIS Firmware Revision + GET_INFO 的 fw 三段 */
#include "svc_ble.h"
#include "svc_pc.h"
#include "svc_device_id.h"   /* F4：GET_INFO 末尾追加 admin_id */

/* ------------------------------------------------------------------------- */
/* custom UUIDs                                                              */
/* ------------------------------------------------------------------------- */

/* 50524F54-4F52-4947-4553-000000000001 : "PROTORIGES" + 000000000001 */
#define BLE_UUID_SVC \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x50524F54, 0x4F52, 0x4947, 0x4553, 0x000000000001ULL))
#define BLE_UUID_CHR_CMD \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x50524F54, 0x4F52, 0x4947, 0x4553, 0x000000000002ULL))
#define BLE_UUID_CHR_EVT \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x50524F54, 0x4F52, 0x4947, 0x4553, 0x000000000003ULL))

#define BLE_UUID128_STR "50524F54-4F52-4947-4553-000000000001"

/* ------------------------------------------------------------------------- */
/* shared state                                                              */
/* ------------------------------------------------------------------------- */

static struct bt_conn *ble_conn;
static bool bas_notify_enabled;
static bool evt_notify_enabled;
static bool services_registered;

/*
 * BLE -> UI event queue.
 *
 * BLE callbacks run in the blestack host task.  LVGL was built with
 * LV_OS_NONE, so touching lv_* from here is not allowed.  Every callback
 * that the UI may care about is pushed into this queue and drained by
 * services/svc_ble.c's svc_ble_tick() (UI task), which turns them into
 * app_state updates + event_queue events.
 *
 * Queue full = drop the event (never block the BLE host for the UI).
 */
static QueueHandle_t ble_evt_q;

static void ble_evt_push(uint8_t type, uint8_t cmd, const uint8_t *data, uint8_t len)
{
    ble_evt_t ev;

    if (ble_evt_q == NULL) {
        return;
    }
    if (len > BLE_EVT_PAYLOAD_MAX) {
        len = BLE_EVT_PAYLOAD_MAX;
    }
    ev.type = type;
    ev.cmd = cmd;
    ev.len = len;
    memset(ev.data, 0, sizeof(ev.data));
    if (data != NULL && len > 0) {
        memcpy(ev.data, data, len);
    }
    (void)xQueueSend(ble_evt_q, &ev, 0);
}

int ble_svc_evt_pop(ble_evt_t *out)
{
    if (ble_evt_q == NULL || out == NULL) {
        return 0;
    }
    return (xQueueReceive(ble_evt_q, out, 0) == pdTRUE) ? 1 : 0;
}

/* Value attribute of the custom Event characteristic.  Filled in by
 * ble_svc_init(): the attribute array itself is declared further down,
 * so keep only a pointer here. */
static const struct bt_gatt_attr *ble_evt_attr;

/*
 * RAM mirror of the battery percentage. The initial 88 is only a placeholder
 * until the first real sample arrives from services/svc_battery.c (IO20 /
 * ADC_CH0) through ble_svc_battery_update(). Kept as a RAM byte on purpose so
 * notifications can read it without touching the ADC.
 */
static uint8_t ble_battery_level = 88;
static uint8_t ble_power_mode = 2;

/*
 * Current Time, 10-byte BLE layout:
 *   year(u16 LE) month day hour minute second weekday(1=Mon..7=Sun)
 *   fractions(1/256 s) reason
 * Initial value is a compile-time placeholder (2026-10-28 20:00:08, Wed).
 * A phone writing CTS overwrites it in RAM; this cache is NOT persisted to flash.
 * The write event also goes to services/svc_ble.c -> svc_time_set(), which
 * programs the SD3010 RTC.
 */
static uint8_t ble_cts_value[10] = {
    0xEA, 0x07,  /* 2026 */
    0x0A,        /* October */
    0x1C,        /* 28 */
    0x14,        /* 20 h */
    0x00,        /* 00 min */
    0x08,        /* 08 s */
    0x03,        /* Wednesday */
    0x00,        /* no fractions */
    0x00,        /* reason: manual time update */
};

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static ssize_t ble_read_string(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 void *buf, u16_t len, u16_t offset)
{
    const char *str = (const char *)attr->user_data;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, str, (u16_t)strlen(str));
}

static void ble_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------- */
/* Device Information Service 0x180A                                         */
/* ------------------------------------------------------------------------- */

static struct bt_gatt_attr dis_attrs[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DIS),

    BT_GATT_CHARACTERISTIC(BT_UUID_DIS_MANUFACTURER_NAME,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ,
                           ble_read_string, NULL, "PROTORIG"),

    BT_GATT_CHARACTERISTIC(BT_UUID_DIS_MODEL_NUMBER,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ,
                           ble_read_string, NULL, "ES-V1.6"),

    BT_GATT_CHARACTERISTIC(BT_UUID_DIS_FIRMWARE_REVISION,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ,
                           ble_read_string, NULL, PROTORIG_FW_VERSION_STR),
};
static struct bt_gatt_service dis_svc = BT_GATT_SERVICE(dis_attrs);

/* ------------------------------------------------------------------------- */
/* Battery Service 0x180F                                                    */
/* ------------------------------------------------------------------------- */

static ssize_t ble_read_battery(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  void *buf, u16_t len, u16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &ble_battery_level, sizeof(ble_battery_level));
}

static void ble_bas_ccc_changed(const struct bt_gatt_attr *attr, u16_t value)
{
    ARG_UNUSED(attr);

    bas_notify_enabled = (value & BT_GATT_CCC_NOTIFY) != 0;
    ble_printf("[ble] BAS notify %s\r\n", bas_notify_enabled ? "on" : "off");

    if (bas_notify_enabled) {
        ble_svc_battery_tick();
    }
}

static struct bt_gatt_attr bas_attrs[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_BAS),

    BT_GATT_CHARACTERISTIC(BT_UUID_BAS_BATTERY_LEVEL,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           ble_read_battery, NULL, &ble_battery_level),

    BT_GATT_CCC(ble_bas_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};
static struct bt_gatt_service bas_svc = BT_GATT_SERVICE(bas_attrs);

/* ------------------------------------------------------------------------- */
/* Current Time Service 0x1805                                               */
/* ------------------------------------------------------------------------- */

static ssize_t ble_read_cts(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, u16_t len, u16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             ble_cts_value, sizeof(ble_cts_value));
}

static ssize_t ble_write_cts(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, u16_t len, u16_t offset, u8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0 || len != sizeof(ble_cts_value)) {
        ble_printf("[ble] CTS write rejected: offset=%u len=%u\r\n", offset, len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    memcpy(ble_cts_value, buf, sizeof(ble_cts_value));
    ble_printf("[ble] CTS <- %04u-%02u-%02u %02u:%02u:%02u wd=%u\r\n",
                     ble_cts_value[0] | (ble_cts_value[1] << 8),
                     ble_cts_value[2], ble_cts_value[3],
                     ble_cts_value[4], ble_cts_value[5], ble_cts_value[6],
                     ble_cts_value[7]);

    /* Queue for the UI task: svc_ble.c's svc_ble_tick() drains this, calls
     * svc_time_set() (SD3010 + app_state) and redraws the face clock. */
    ble_evt_push(BLE_EVT_CTS_WRITE, 0, ble_cts_value, sizeof(ble_cts_value));

    /* Write it back to the caller so a phone can verify the round-trip. */
    return len;
}

static struct bt_gatt_attr cts_attrs[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_CTS),

    BT_GATT_CHARACTERISTIC(BT_UUID_CTS_CURRENT_TIME,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                           ble_read_cts, ble_write_cts, NULL),
};
static struct bt_gatt_service cts_svc = BT_GATT_SERVICE(cts_attrs);

/* ------------------------------------------------------------------------- */
/* custom PROTORIG probe service                                             */
/* ------------------------------------------------------------------------- */

static void ble_evt_ccc_changed(const struct bt_gatt_attr *attr, u16_t value)
{
    ARG_UNUSED(attr);

    evt_notify_enabled = (value & BT_GATT_CCC_NOTIFY) != 0;
    ble_printf("[ble] EVT notify %s\r\n", evt_notify_enabled ? "on" : "off");
}

static int ble_send_event(const uint8_t *data, u16_t len)
{
    int err;

    if (ble_conn == NULL) {
        return -1;
    }

    if (!evt_notify_enabled) {
        ble_printf("[ble] event dropped: CCC notify not enabled\r\n");
        return -1;
    }

    if (ble_evt_attr == NULL) {
        return -1;
    }

    err = bt_gatt_notify(ble_conn, ble_evt_attr, data, len);
    if (err) {
        ble_printf("[ble] notify failed: %d\r\n", err);
    }
    return err;
}

static void ble_send_status(uint8_t cmd, uint8_t status);

int ble_svc_send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[4 + 64];

    if (len > 64) {
        return -1;
    }
    frame[0] = BLE_PROTO_MAGIC0;
    frame[1] = BLE_PROTO_MAGIC1;
    frame[2] = cmd;
    frame[3] = (uint8_t)len;
    if (payload != NULL && len > 0) {
        memcpy(&frame[4], payload, len);
    }
    return ble_send_event(frame, (uint16_t)(4u + len));
}

void ble_svc_send_ack(uint8_t cmd, uint8_t status)
{
    ble_send_status(cmd, status);
}

void ble_svc_cts_cache_set(const uint8_t value[10])
{
    if (value != NULL) {
        memcpy(ble_cts_value, value, sizeof(ble_cts_value));
    }
}

static void ble_send_status(uint8_t cmd, uint8_t status)
{
    uint8_t frame[5];

    frame[0] = BLE_PROTO_MAGIC0;
    frame[1] = BLE_PROTO_MAGIC1;
    frame[2] = (uint8_t)(cmd | BLE_PROTO_RSP_FLAG);
    frame[3] = 1;
    frame[4] = status;
    (void)ble_send_event(frame, sizeof(frame));
}

static void ble_handle_ping(void)
{
    uint8_t frame[8];

    frame[0] = BLE_PROTO_MAGIC0;
    frame[1] = BLE_PROTO_MAGIC1;
    frame[2] = (uint8_t)(BLE_CMD_PING | BLE_PROTO_RSP_FLAG);
    frame[3] = 4;
    memcpy(&frame[4], "PONG", 4);
    (void)ble_send_event(frame, 8);
}

static void ble_handle_get_info(void)
{
    /* 4-byte header + 16-byte payload:
     *   proto, fw_major, fw_minor, fw_patch, battery, caps(u16 LE),
     *   uptime_seconds(u32 LE), power_mode,
     *   admin_id(u32 LE，F4 追加；有效 24 bit，高字节保留 0)
     * 旧字段偏移 0..15 不变，旧客户端忽略末尾多出的 4 B。 */
    uint8_t frame[20];
    uint32_t uptime_s = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
    uint32_t admin_id = svc_device_id_admin() & SVC_DEVICE_ID_ADMIN_MASK;

    frame[0] = BLE_PROTO_MAGIC0;
    frame[1] = BLE_PROTO_MAGIC1;
    frame[2] = (uint8_t)(BLE_CMD_GET_INFO | BLE_PROTO_RSP_FLAG);
    frame[3] = 16;

    frame[4]  = BLE_PROTO_VERSION;
    frame[5]  = (uint8_t)PROTORIG_FW_VERSION_MAJOR;
    frame[6]  = (uint8_t)PROTORIG_FW_VERSION_MINOR;
    frame[7]  = (uint8_t)PROTORIG_FW_VERSION_PATCH;
    frame[8]  = ble_battery_level;
    frame[9]  = (uint8_t)(BLE_CAPABILITIES & 0xFF);
    frame[10] = (uint8_t)((BLE_CAPABILITIES >> 8) & 0xFF);
    frame[11] = (uint8_t)(uptime_s & 0xFF);
    frame[12] = (uint8_t)((uptime_s >> 8) & 0xFF);
    frame[13] = (uint8_t)((uptime_s >> 16) & 0xFF);
    frame[14] = (uint8_t)((uptime_s >> 24) & 0xFF);
    frame[15] = ble_power_mode;
    frame[16] = (uint8_t)(admin_id & 0xFF);
    frame[17] = (uint8_t)((admin_id >> 8) & 0xFF);
    frame[18] = (uint8_t)((admin_id >> 16) & 0xFF);
    frame[19] = (uint8_t)((admin_id >> 24) & 0xFF);

    (void)ble_send_event(frame, sizeof(frame));
}

static ssize_t ble_write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, u16_t len, u16_t offset, u8_t flags)
{
    ble_link_note_rx();
    const uint8_t *req = (const uint8_t *)buf;
    uint8_t cmd;
    uint8_t payload_len;

    ARG_UNUSED(conn);
    ARG_UNUSED(attr);

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    /* Write without response is allowed; a response is still emitted via the
     * Event characteristic if the peer subscribed. */
    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    }

    if (len < 4 || req[0] != BLE_PROTO_MAGIC0 || req[1] != BLE_PROTO_MAGIC1) {
        ble_printf("[ble] bad command frame (len=%u)\r\n", len);
        ble_send_status(0, BLE_STATUS_BAD_FRAME);
        return len;
    }

    cmd = req[2];
    payload_len = req[3];

    if ((uint16_t)(4u + payload_len) != len) {
        ble_printf("[ble] frame length mismatch: header=%u att=%u\r\n",
                   (unsigned)(4u + payload_len), len);
        ble_send_status(cmd, BLE_STATUS_BAD_FRAME);
        return len;
    }

    /* R2.1-BMP：BMP DATA 最大 203 B，直接进 svc_ble 状态机，绕过
     * 64 B 事件队列，避免分片丢失。 */
    if (cmd == BLE_CMD_BMP_BEGIN || cmd == BLE_CMD_BMP_DATA ||
        cmd == BLE_CMD_BMP_END || cmd == BLE_CMD_BMP_ABORT) {
        if (cmd == BLE_CMD_BMP_DATA && payload_len > 203) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        svc_ble_bmp_feed(cmd, req + 4, payload_len);
        return len;
    }

    /* 电脑模式（透传显示）：整帧直送 svc_pc 记录环，绕开 64 B 事件队列；
     * 也不回 status —— 每帧都 ACK 会拖垮透传速率。 */
    if (cmd == BLE_CMD_PC_FRAME) {
        if (payload_len > 240) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        svc_pc_stage(req + 4, payload_len);
        return len;
    }

    /* 电脑模式进/出：协议栈回调里只置一个待处理标志，由 UI 任务的
     * svc_pc_tick() 真正切页面（这里碰 LVGL 会破坏 LV_OS_NONE 的前提）。 */
    if (cmd == BLE_CMD_PC_MODE) {
        if (payload_len < 1) {
            ble_send_status(cmd, BLE_STATUS_BAD_FRAME);
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        svc_pc_request(req[4] != 0);
        return len;
    }

    if (payload_len > 64) {
        ble_send_status(cmd, BLE_STATUS_TOO_LONG);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    /* Hand the command to the LVGL task (non-blocking; never touch LVGL
     * from the BLE host task).  The reply itself is sent below from here. */
    ble_evt_push(BLE_EVT_COMMAND, cmd, req + 4, payload_len);

    switch (cmd) {
    case BLE_CMD_PING:
        ble_handle_ping();
        break;

    case BLE_CMD_GET_INFO:
        ble_handle_get_info();
        break;

    case BLE_CMD_SET_TIME:
    case BLE_CMD_NOTIFY:
    case BLE_CMD_FIND:
        /* 语义处理在 services/svc_ble.c（UI 任务侧会 drain 这条 BLE_EVT_COMMAND）。
         * 这里先回 ACK，手机不必等 UI。 */
        ble_send_status(cmd, 0);
        break;

    case BLE_CMD_METRICS:
        /* handled in svc_ble (UI task); no reply */
        break;

    default:
        ble_printf("[ble] unknown cmd 0x%02x\r\n", cmd);
        ble_send_status(cmd, BLE_STATUS_UNKNOWN_CMD);
        break;
    }

    return len;
}

static struct bt_gatt_attr ble_custom_attrs[] = {
    BT_GATT_PRIMARY_SERVICE(BLE_UUID_SVC),

    /* Command: phone -> watch */
    BT_GATT_CHARACTERISTIC(BLE_UUID_CHR_CMD,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE,
                           NULL, ble_write_cmd, NULL),

    /* Event: watch -> phone, responses/telemetry */
    BT_GATT_CHARACTERISTIC(BLE_UUID_CHR_EVT,
                           BT_GATT_CHRC_NOTIFY,
                           0,
                           NULL, NULL, NULL),
    BT_GATT_CCC(ble_evt_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};
static struct bt_gatt_service ble_custom_svc = BT_GATT_SERVICE(ble_custom_attrs);

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

int ble_svc_init(void)
{
    int err = 0;
    int ret;

    if (services_registered) {
        return 0;
    }

    /* Event queue for the LVGL task (BLE_EVT_CONNECTED / COMMAND / CTS ...). */
    ble_evt_q = xQueueCreate(8, sizeof(ble_evt_t));
    if (ble_evt_q == NULL) {
        ble_printf("[ble] event queue create FAILED\r\n");
    }

    ble_evt_attr = &ble_custom_attrs[4];

    ret = bt_gatt_service_register(&dis_svc);
    ble_printf("[ble] register DIS   : %d\r\n", ret);
    err |= ret;

    ret = bt_gatt_service_register(&bas_svc);
    ble_printf("[ble] register BAS   : %d\r\n", ret);
    err |= ret;

    ret = bt_gatt_service_register(&cts_svc);
    ble_printf("[ble] register CTS   : %d\r\n", ret);
    err |= ret;

    ret = bt_gatt_service_register(&ble_custom_svc);
    ble_printf("[ble] register custom: %d\r\n", ret);
    err |= ret;

    services_registered = true;
    ble_printf("[ble] GATT ready, service uuid %s\r\n", BLE_UUID128_STR);
    return err;
}

void ble_svc_connected(struct bt_conn *conn)
{
    ble_conn = conn;
    bas_notify_enabled = false;
    evt_notify_enabled = false;
    ble_evt_push(BLE_EVT_CONNECTED, 0, NULL, 0);
    ble_printf("[ble] LE connected, mtu=%u\r\n", bt_gatt_get_mtu(conn));
}

void ble_svc_disconnected(void)
{
    ble_conn = NULL;
    bas_notify_enabled = false;
    evt_notify_enabled = false;
    ble_evt_push(BLE_EVT_DISCONNECTED, 0, NULL, 0);
    ble_printf("[ble] LE disconnected\r\n");
}

void ble_svc_battery_tick(void)
{
    int err;

    if (ble_conn == NULL || !bas_notify_enabled || !services_registered) {
        return;
    }

    err = bt_gatt_notify(ble_conn, &bas_attrs[2],
                         &ble_battery_level, sizeof(ble_battery_level));
    if (err) {
        ble_printf("[ble] BAS notify failed: %d\r\n", err);
    }
}

void ble_svc_battery_update(uint8_t level)
{
    ble_battery_level = (level > 100) ? 100 : level;
    ble_svc_battery_tick();
}

void ble_svc_set_power_mode(uint8_t mode)
{
    if (mode <= 2) {
        ble_power_mode = mode;
    }
}

uint8_t ble_svc_get_power_mode(void)
{
    return ble_power_mode;
}
