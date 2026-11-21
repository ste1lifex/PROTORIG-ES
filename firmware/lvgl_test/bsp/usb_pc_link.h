/*
 * usb_pc_link.h —— USB 原始二进制上行（电脑模式像素透传的 RX 通道）
 *
 * 背景（见 research/56 §8）：
 *   USB 侧原来只能收 ASCII 行（shell），带宽不够推像素；BLE 只承载小数据。
 *   这个模块把 CDC OUT 端点的原始字节接过来，按 `A5 5A len payload` 组帧，
 *   交给 svc_pc_stage()（与 BLE 的 0x50 命令**同一个入口**，不另写解析）。
 *
 * 挂法：SDK 的 bsp/common/usb_app/usb_console.c 在 shell_exe_cmd() 之前调
 *   protorig_usb_pc_rx()；该函数由 sdk_patches/bouffalo_sdk_usb_pc_rx.patch
 *   在构建期注入（弱符号默认返回 0 = 原样交给 shell）。写路径和 USB 描述符
 *   都不动，所以不重新枚举、COM 号不变。
 *
 * 帧格式（小端）：
 *   A5 5A len  payload(len 字节)
 *   len ∈ 1..240（= svc_pc 的 PC_CHUNK_MAX），payload 就是 svc_pc 的帧
 *   （首字节是 op：0x01 HELLO / 0x10 TILE_BEGIN / …）。
 *
 * 返回约定：1 = 这一包已被吃掉，不要再交给 shell；0 = 原样交给 shell。
 *   —— 所以电脑模式下敲 `pcmode 0` 仍然有效（文本不以 0xA5 开头）。
 */
#ifndef USB_PC_LINK_H
#define USB_PC_LINK_H

#include <stdint.h>

/* 1 = 电脑模式占用了 CDC OUT 端点（只有 transport == USB 时置 1）。
 * 定义在 usb_pc_link.c；SDK 侧只引用 protorig_usb_pc_rx()，不引用这个变量。 */
extern int protorig_usb_pc_active;

/* SDK usb_console 的读字节钩子（构建期由 SDK 补丁声明弱默认） */
int protorig_usb_pc_rx(const uint8_t *buf, uint16_t len);

/* 进/出电脑模式时调用：置位并清掉半截帧；清零时把读取权还给 shell */
void usb_pc_link_set_active(int on);

/* 丢掉还没组完的半截帧（校验失败 / 切链路 / 重新握手时用） */
void usb_pc_link_reset(void);

#endif /* USB_PC_LINK_H */
