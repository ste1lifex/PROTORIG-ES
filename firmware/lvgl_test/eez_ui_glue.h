/*
 * eez_ui_glue.h —— 表盘 / 原生 App 页 / 电脑模式的导航
 */
#ifndef _EEZ_UI_GLUE_H_
#define _EEZ_UI_GLUE_H_

/* 在表盘上装入口（在 face_lvgl_create() 之后调用） */
void eez_ui_init(void);

void eez_ui_enter(void);
void eez_ui_leave(void);
int  eez_ui_is_open(void);

/* 当前页号：0 = 表盘，1 = 协议网络，2 = 相遇记录（1/2 是占位页，App 列表不显示），
 * 3 = 终端设置，4 = USB 监控，5 = 秒表，6 = 计时器，7 = 闹钟
 * （2026-11-08 APP 第一批，见 ui/app_pages.h） */
int  eez_ui_page(void);

/* P0-1：触摸笔序号（PRESSED 时 +1）。App 列表用它区分"开列表的那一笔"和
 * "之后的新按"，防止同一笔手势分裂后误点列表项。 */
int eez_ui_touch_epoch(void);

/* 自检 / host 预览用：第 i 页的容器对象（不要拿去当业务接口） */
void *eez_ui_page_obj(int i);

/* R3c A+B：App 的 load 回调里用的原始跳页（不做 App 生命周期） */
void eez_ui_goto_raw(int page);

/* 切到第 i 页：i=0..2 → 页 1..3；越界忽略，不循环（当前无调用者，只保留接口） */
void eez_ui_show(int i);

/* 每个 LVGL 周期调一次，按需同步当前设置页的值 */
void eez_ui_tick(void);
void eez_ui_monitor(void); /* 表盘右滑进入的第 4 页（USB 监控），不是 EEZ 页面 */

/* 设置 App 的 load 用，回到设置总览并读回真实配置 */
void eez_ui_refresh_settings(void);

/* F1（2026-11）：App 列表改成圆形视图后，底部旧分页点已删除，本文件不再画点。 */

/* 诊断：打印分页/手势层的 LVGL 状态（shell 的 uidiag 命令） */
void eez_ui_diag(void);

#endif /* _EEZ_UI_GLUE_H_ */
