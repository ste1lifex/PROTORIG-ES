#ifndef PROTORIG_FLUSH_POLICY_H
#define PROTORIG_FLUSH_POLICY_H
#include "lvgl.h"

/*
 * 刷新窗口宽度取偶 —— 真机 bug「细笔画全花 / 指针拖痕」的**修复**。
 * （根因推导、真机 A/B 证据、以及未独立验证的边界，见
 *   docs/bug_thin_stroke_smearing.md 第 11 节。）
 *
 * 为什么必须挂在 LV_EVENT_INVALIDATE_AREA：
 *   此刻 LVGL **还没渲染**，改区域是安全的。绝不能在 disp_flush 里扩大
 *   CASET/RASET 却沿用已经紧扣的 px_map —— 那会读错像素甚至越界。
 *
 * 机制：
 *   LVGL 送出的脏区宽度可能是**奇数**。而 RGB565 下一个像素 2 字节、
 *   DBI 的 FIFO 一个 32 位字装两个像素；奇数宽度时窗口最后一个像素只能
 *   占半个字，st7802_push_pixels565() 补的那个"幽灵像素"落在**高半字**，
 *   硬件取的却是**低半字** —— 于是该窗口**最右一列像素全错**。
 *   表现：1~2px 细笔画被随机打坏（小字/指针/刻度/等高线），
 *         纯色和大面积看不出来，指针擦除窗口漏掉最右一列 = 拖痕。
 *
 * 修复：x1 向下取偶、x2 向上取奇 => 宽度 = 奇 − 偶 + 1 = **偶数**。
 *
 * 代价与验证：
 *   实测 ~29.5K px/flush，40 MHz QSPI 下带宽增加可忽略；
 *   主机侧 67 张输出画面 SHA-256 与原始模式完全相同 —— 视觉零变化。
 */
static inline void flush_policy_event(lv_event_t *e)
{
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    lv_display_t *d = (lv_display_t *)lv_event_get_target(e);
    const int32_t width = lv_display_get_horizontal_resolution(d);

    a->x1 &= ~1;      /* 起点向下取偶 */
    a->x2 |= 1;       /* 终点向上取奇 */
    if(a->x2 >= width) a->x2 = width - 1;
}
#endif
