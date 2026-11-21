/*
 * PROTORIG ES M2 firmware - BLE radio/stack bring-up + advertising
 *
 * Merged from firmware_ble_probe/ble_probe_link.c (W1, 2026-10-28).
 * Sequence copied from SDK examples/btble/peripheral/main.c:
 *   rfparam_init() -> btble_controller_init() -> hci_driver_init() ->
 *   bt_enable(cb) -> register GATT -> set name -> start advertising.
 *
 * The OAD service is intentionally not used (CONFIG_BT_OAD_SERVER is off),
 * so the advertising start is NOT inside an OAD #if.
 */
#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include "task.h"

#include "bluetooth.h"
#include "bt_uuid.h"
#include "conn.h"
#include "hci_err.h"
#include "hci_core.h"
#include "hci_driver.h"
#include "btble_lib_api.h"
#include "rfparam_adapter.h"

#include "bl616_glb.h"       /* R4_usbclk: GLB_Set_USB_CLK_From_WIFIPLL */
#include "usb_console.h"     /* R4-usbdelay: late console init */
#include "ble_svc.h"
#include "ble_link.h"
#include "svc_device_id.h"   /* F2：广播名后缀 = 管理员编号（低 24 bit） */

/* 50524F54-4F52-4947-4553-000000000001, little-endian advertising bytes */
#define BLE_ADV_UUID128 \
    BT_UUID_128_ENCODE(0x50524F54, 0x4F52, 0x4947, 0x4553, 0x000000000001ULL)

/*
 * F2（2026-11-08）：广播名从固定 "PROTORIG ES" 改成运行时
 *   "PROTORIG ES <ADMIN_ID>"，例如 "PROTORIG ES 2BEA4E"（18 字符）。
 * bt_set_name()（GAP 名字）与 adv data 引用同一块 s_dev_name 缓冲，
 * 名字只在 bt_enable_cb 里拼一次（管理员编号是懒计算 + 缓存，值稳定）。
 *
 * Advertising packet（max 31 bytes）：
 *   flags            3
 *   16-bit UUIDs     6   (0x180F Battery, 0x180A Device Information)
 *   complete name   20   (1 长度 + 1 类型 + 18 字符名)
 *   ----------------------------------
 *   total           29   <= 31（扫描响应另算，不变）
 *
 * Scan response:
 *   128-bit UUID    18   (PROTORIG custom service)
 */
static char s_dev_name[BLE_DEVICE_NAME_MAX];

static struct bt_data ble_adv_data[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x0f, 0x18, 0x0a, 0x18),
    /* data_len 在 ble_link_name_build() 里按实际字符串长度填 */
    BT_DATA(BT_DATA_NAME_COMPLETE, s_dev_name, 0),
};

/* 编译期审计（2026-11-08）：管理员编号固定 6 位（%06X），所以名字 18 字符；
 *   flags 3 + UUID16 6 + name(1 长度 + 1 类型 + 18 字符) = 29 B <= 31 B。
 * 名字缓冲 32 B，snprintf 不会截断；扫描响应不变，另算。 */
#define BLE_ADV_NAME_LEN  ((int)sizeof(BLE_DEVICE_NAME_PREFIX) - 1 + 1 + 6)
#define BLE_ADV_TOTAL_LEN (3 + 6 + 1 + 1 + BLE_ADV_NAME_LEN)
_Static_assert(BLE_ADV_TOTAL_LEN <= 31, "ble advertising packet exceeds 31 bytes");

/* 拼运行时广播名（前缀 + 6 位大写十六进制管理员编号）。 */
static void ble_link_name_build(void)
{
    uint32_t admin = svc_device_id_admin();
    int n;

    n = snprintf(s_dev_name, sizeof(s_dev_name), "%s %06X",
                 BLE_DEVICE_NAME_PREFIX, (unsigned)(admin & SVC_DEVICE_ID_ADMIN_MASK));
    if (n < 0) {
        /* 不应发生；退回纯前缀，至少保留旧扫描端的匹配能力。 */
        memcpy(s_dev_name, BLE_DEVICE_NAME_PREFIX, sizeof(BLE_DEVICE_NAME_PREFIX));
    }
    ble_adv_data[2].data_len = (u8_t)strlen(s_dev_name);
}

static const struct bt_data ble_scan_rsp[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BLE_ADV_UUID128),
};

/* R2.1 power：广播/连接状态 */
static struct bt_conn *s_conn;
static int s_adv_on;
static volatile uint32_t s_last_rx_ms;   /* last GATT write from the peer */
static int s_bt_ready;
static int s_stopped;

static void ble_start_adv(void)
{
    struct bt_le_adv_param param = {
        .id = BT_ID_DEFAULT,
        .options = BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_ONE_TIME,
        .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
        .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
    };
    int err;

    if (s_stopped) {
        printf("[ble] adv start skipped (stopped)\r\n");
        return;
    }
    err = bt_le_adv_start(&param, ble_adv_data, ARRAY_SIZE(ble_adv_data),
                          ble_scan_rsp, ARRAY_SIZE(ble_scan_rsp));
    if (err) {
        printf("[ble] adv start failed: %d\r\n", err);
    } else {
        s_adv_on = 1;
        printf("[ble] advertising as \"%s\"\r\n", s_dev_name);
    }
}


static void ble_connected(struct bt_conn *conn, u8_t err)
{
    if (err) {
        printf("[ble] connect failed: %u\r\n", err);
        return;
    }
    printf("[ble] connected\r\n");
    s_conn = conn;
    s_adv_on = 0;
    s_last_rx_ms = (uint32_t)xTaskGetTickCount();
    ble_svc_connected(conn);
}

static void ble_disconnected(struct bt_conn *conn, u8_t reason)
{
    (void)conn;
    printf("[ble] disconnected, reason=0x%02x\r\n", reason);
    s_conn = NULL;
    s_adv_on = 0;
    ble_svc_disconnected();

    if (!s_stopped) {
        ble_start_adv();
    }
}

static struct bt_conn_cb ble_conn_callbacks = {
    .connected = ble_connected,
    .disconnected = ble_disconnected,
};

static void bt_enable_cb(int err)
{
    bt_addr_le_t bt_addr;

    if (err) {
        printf("[ble] bt_enable failed: %d\r\n", err);
        return;
    }

    s_bt_ready = 1;
    bt_get_local_public_address(&bt_addr);
    printf("[ble] BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x\r\n",
           bt_addr.a.val[5], bt_addr.a.val[4], bt_addr.a.val[3],
           bt_addr.a.val[2], bt_addr.a.val[1], bt_addr.a.val[0]);

    /* GAP device name.  The advertising name is set explicitly as well so a
     * scanner sees it without relying on the stack's name option.
     * F2：GAP 名与广播名共用同一块运行时缓冲（"PROTORIG ES <ADMIN_ID>"）。 */
    ble_link_name_build();
    printf("[ble] name = \"%s\" (%u B)\r\n", s_dev_name, (unsigned)strlen(s_dev_name));
    bt_set_name(s_dev_name);

    bt_conn_cb_register(&ble_conn_callbacks);
    ble_svc_init();
    ble_start_adv();
    printf("[usb] reclock before: switch USB clock to WIFIPLL\r\n");
    fflush(stdout);
    {
        BL_Err_Type rc = GLB_Set_USB_CLK_From_WIFIPLL(1);
        printf("[usb] reclock after: rc=%d\r\n", (int)rc);
        fflush(stdout);
    }
    usb_console_init();
    printf("[usb] delayed console init after BLE adv\r\n");
    fflush(stdout);
}

static void ble_link_task(void *pvParameters)
{
    (void)pvParameters;

    /* PHY calibration data comes from the board flash DTS. */
    if (0 != rfparam_init(0, NULL, 0)) {
        printf("[ble] PHY RF init failed!\r\n");
        vTaskDelete(NULL);
        return;
    }

    /* Controller priority must be <= configMAX_PRIORITIES - 1. */
    btble_controller_init(configMAX_PRIORITIES - 1);
    hci_driver_init();
    bt_enable(bt_enable_cb);

    vTaskDelete(NULL);
}

void ble_link_start(void)
{
    xTaskCreate(ble_link_task, "ble_link", 1024, NULL,
                configMAX_PRIORITIES - 2, NULL);
}

void ble_link_stop(void)
{
    s_stopped = 1;
    if (s_conn != NULL) {
        (void)bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        s_conn = NULL;
    }
    if (s_bt_ready) {
        (void)bt_le_adv_stop();
    }
    s_adv_on = 0;
    printf("[ble] stopped (adv/conn off)\r\n");
}

void ble_link_start_adv(void)
{
    s_stopped = 0;
    if (s_adv_on) {
        return;
    }
    if (s_bt_ready && s_conn == NULL) {
        ble_start_adv();
    } else {
        printf("[ble] start_adv deferred (ready=%d conn=%d)\r\n",
               s_bt_ready, s_conn != NULL);
    }
}

void ble_link_force_adv(void)
{
    if (s_stopped || !s_bt_ready || s_conn != NULL) {
        return;
    }
    (void)bt_le_adv_stop();
    s_adv_on = 0;
    ble_start_adv();
}

void ble_link_note_rx(void)
{
    s_last_rx_ms = (uint32_t)xTaskGetTickCount();
}

uint32_t ble_link_idle_ms(void)
{
    if (s_last_rx_ms == 0) {
        return 0;
    }
    return (uint32_t)xTaskGetTickCount() - s_last_rx_ms;
}

/* Force-drop a connection the peer already abandoned (Windows often keeps the
 * link object alive, so the watch thinks it is connected and stops advertising). */
void ble_link_drop_conn(void)
{
    if (s_conn != NULL) {
        (void)bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

int ble_link_adv_on(void)
{
    return s_adv_on;
}

int ble_link_is_active(void)
{
    return (s_conn != NULL) || s_adv_on;
}

int ble_link_is_connected(void)
{
    return s_conn != NULL;
}
