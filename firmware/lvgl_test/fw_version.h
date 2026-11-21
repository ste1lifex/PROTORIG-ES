/*
 * fw_version.h —— 固件版本号的唯一来源
 *
 * 版本号规则：
 *   语义化三段 + **构建时间**。同一版本号靠构建时间区分不同 bin。
 *
 * 各处用法：
 *   - shell `adminid`：`fw=PROTORIG_FW_VERSION_STR build=__DATE__ __TIME__`
 *     （插件 USB 探测时读它，概览显示 `1.6.0 · <构建时间>`）；
 *   - BLE DIS 的 Firmware Revision 特征：PROTORIG_FW_VERSION_STR；
 *   - BLE GET_INFO 的 3 个 fw 字节：MAJOR / MINOR / PATCH；
 *   - 屏幕/日志不单独写版本号。
 *
 * 改版本只改下面四行，字符串和三段数字必须保持一致。
 * 注意：BLE_PROTO_VERSION（ble_svc.h）是**协议版本**，不等于固件版本，不要混。
 */
#ifndef FW_VERSION_H
#define FW_VERSION_H

#define PROTORIG_FW_VERSION_STR    "1.6.0"
#define PROTORIG_FW_VERSION_MAJOR  1u
#define PROTORIG_FW_VERSION_MINOR  6u
#define PROTORIG_FW_VERSION_PATCH  0u

#endif /* FW_VERSION_H */
