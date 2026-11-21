/*
 * face_lvgl.h —— 「协议源石容器」表盘的 LVGL 版接口
 */
#ifndef _FACE_LVGL_H_
#define _FACE_LVGL_H_

#include <stdbool.h>

/* 在当前活动屏幕上创建表盘页面（会先清空该容器） */
void face_lvgl_create(void);

/* 推时间进来（由 main.c 的 RTC 轮询调用）。只重画真正变了的那几块区域。 */
void face_lvgl_set_time(int year, int mon, int day, int week,
                        int hh, int mm, int ss);

/*
 * 诊断（deliverable 2，整分取证）：hh:mm 刚变化时置 1，值为 hh*100+mm。
 * 由 main.c 的 flush 钩子在"分钟变化那一刻的第一次 flush"消费并清零。
 * 不改任何渲染语义，正常 UI 下没人读它。
 */
extern volatile int s_time_minute_changed;
extern volatile int s_time_minute;

/* 触摸唤醒后调用：只刷新时间/日期几块小区域（面板 GRAM 保留了上一帧） */
void face_lvgl_wake_refresh(void);

/* 表盘内容所在的页面容器（分页层拿它当第 0 页） */
void *face_lvgl_page(void);

/* 表盘所在的 screen 对象（所有页面共用一个 screen，切换由 eez_ui_glue.c 的
 * 页面容器负责，见那边的说明） */
void *face_lvgl_screen(void);

/* 更新表盘上的电量读数（真实 ADC 采样；mv < 0 表示读失败）。
 * 只有变化超过阈值才会真正重绘标签，可以放心周期调用。 */
void face_lvgl_set_battery(int mv, int percent);

/* 充电状态（U5 的 CHRG 飞线到 IO21，2026-10-17）。1 = 正在充电。
 * 会在电量标签后面加 " CHG" 并换成亮金色。 */
void face_lvgl_set_charging(int on);

/* 低电告警等级（2026-10-18）：
 *   0 = 正常；1 = 低电（标签变橙）；2 = 极低（标签变红 + 后缀 "LOW"）。
 * 充电中按 0 显示。 */
void face_lvgl_set_power_warn(int level);

/* host 预览自检用：第 k 根表圈指针的载体对象（不要拿去当业务接口） */
void *face_lvgl_hand_obj(int k);

/* 自检用：第 k 根指针当前角度（顺时针十分之一度，12 点为 0）。
 * 载体已改成 480x480 全屏对象、指针位置由角度算出，
 * 所以自检要查**角度**，不能再靠对象坐标推位置。 */
int face_lvgl_hand_angle(int k);

/* 诊断：表圈绘制的全局偏移（会叠加到 d_line/d_rect/bezel_arc 的每一笔上）。
 * 正常应为 (0,0)；非 0 就说明"画的位置"和"标脏的位置"会错开。 */
void face_lvgl_get_draw_offset(int *x, int *y);

#endif /* _FACE_LVGL_H_ */
