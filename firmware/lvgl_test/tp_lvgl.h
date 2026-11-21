/*
 * tp_lvgl.h —— CST9220 -> LVGL 输入设备
 */
#ifndef _TP_LVGL_H_
#define _TP_LVGL_H_

#include <stdint.h>

/* 注册 LVGL indev（要求板级 tp_init() 已经跑过） */
void tp_lvgl_init(void);

/* 息屏状态下的轻量轮询：返回 1 表示有手指 */
int tp_lvgl_poll_raw(void);

/* 给 UI 层做"手指指示"用 */
void tp_lvgl_get_state(uint16_t *x, uint16_t *y, int *pressed, uint32_t *taps);

/* 手指是否正在按 / 刚抬起（交互时让开重活，比如暂停指针补间动画） */
int tp_lvgl_active_recently(void);

/* UI 反馈钩子：每次读触摸后立刻回调（在 LVGL 任务上下文里） */
typedef void (*tp_lvgl_hook_t)(int pressed, uint16_t x, uint16_t y, uint32_t taps);
void tp_lvgl_set_hook(tp_lvgl_hook_t h);

/* ---- INT 门控（省电实验，⚠️ 默认关闭）----
 * 实测 INT 脉冲很短（历史：当年 20 ms 轮询就漏掉大部分，见 watchface/README 6.7），
 * 再加"INT 高就跳过读"会丢真实触摸点。所以默认关，只做对照实验用。
 *   tpgate 1 / tpgate 0 现场切换 */
void tp_lvgl_set_int_gate(int on);
int  tp_lvgl_get_int_gate(void);
uint32_t tp_lvgl_get_gated(void);   /* 因 INT 为高而省掉的 I2C 读次数 */

#endif /* _TP_LVGL_H_ */
