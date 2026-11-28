#ifndef PC_MONITOR_H
#define PC_MONITOR_H
#include "lvgl.h"
#include "monitor_protocol.h"
/* All UI and power operations run only in the LVGL task. */
lv_obj_t *pc_monitor_create(lv_obj_t *parent);
void pc_monitor_update(const pm_link_t *state, bool visible);

/* BLE 数字通道（无 USB 时用）：把 CPU/GPU/RAM 等数值直接喂给本地监控页，
 * 由 LVGL 本地渲染（30 fps），不传像素。3 s 收不到新值自动退回 pcmon 数据。
 * 数值单位：cpu/gpu/ram 为十分之一百分比(0..1000)，vram 同(0xFFFF=未知)，
 * brand: 0 默认 / 1 NVIDIA / 2 AMD / 3 Intel。 */
void pc_monitor_set_metrics(int cpu, int gpu, int ram, int vram, int brand,
                            uint32_t rx, uint32_t tx);

/* ------------------------------------------------------------------ */
/* BLE 卡片层（P1..P4，无像素链路时由 PC 用 0x53/0x54/0x55 驱动）     */
/*                                                                    */
/* 三环继续由本地 LVGL 画；卡片只占下半屏。文本字段带 utf8 兜底，     */
/* 真正的中文标题/题目走 1-bit 位图（0x57）——固件字库只有 ~800 个常用 */
/* 汉字，装不下任意会话标题。所有本层 API 只能在 LVGL 任务里调用。     */
/* ------------------------------------------------------------------ */
#define PC_CARD_OFF       0
#define PC_CARD_SESSION   1
#define PC_CARD_APPROVAL  2
#define PC_CARD_QUESTION  3

#define PC_CARD_TEXT_MAX  96
#define PC_CARD_BMP_SLOTS 2
#define PC_CARD_BMP_W     300
#define PC_CARD_BMP_H     44      /* 位图槽高：2 行×17px（长题干/选项要能读完，2026-11-21） */

typedef struct {
    int kind;                       /* PC_CARD_*；PC_CARD_OFF = 隐藏卡片 */
    int state;                      /* PM_STATE_* */
    int steps;
    int ctx;                        /* -1 = 未知，否则 0..100 */
    char tool[12];                  /* ASCII 工具名（超长截断） */
    char title[PC_CARD_TEXT_MAX];   /* 会话标题（文本兜底） */
    char footer[PC_CARD_TEXT_MAX];  /* 会话卡底部一行：电量 97% · ¥91.38 */
    char cmd[PC_CARD_TEXT_MAX];     /* 审批命令 / 问答题目（文本兜底） */
    char choice[PC_CARD_TEXT_MAX];  /* 问答当前选项（文本兜底） */
    int can_allow, computer;        /* 审批：是否允许在表上答 / 已移交电脑 */
    int countdown;                  /* 审批：剩余秒数（0 = 不显示倒计时） */
    int q_index, q_count, q_multi;  /* 问答：第几题 / 总题数 / 是否多选 */
    int q_page;                     /* 问答：当前显示的选项序号 */
    int q_optcount;                 /* 问答：本页/本组的选项总数（标题行 x/y 页） */
    uint16_t q_sel;                 /* 问答：已选掩码（bit = 选项序号） */
} pm_card_t;

/* 覆盖当前卡片（kind=PC_CARD_OFF 等同 pc_monitor_card_clear）。 */
void pc_monitor_set_card(const pm_card_t *card);
/* 清卡片，回归"无卡片"状态。 */
void pc_monitor_card_clear(void);
/* 卡片位图分片：slot 0/1，行优先 MSB-first，w 必须是 8 的倍数。
 * bpp = 1（黑白）或 2（4 级灰，值 0/85/170/255 —— 标题/题目用它抗锯齿）。
 * index 从 0 计，收满 total 片后生效（PC 侧按 MTU 切片）。
 * w=h=total=0 表示清槽。返回 1=整张生效 / 0=已收一片 / -1=丢弃（需重发）。 */
int pc_monitor_card_bitmap(int slot, int w, int h, int index, int total, int bpp,
                           const uint8_t *data, int len);
/* 任意卡片帧都算"链路还活着"：避免 BLE 下三环数值偶尔停 3 s 就掉回离线页。 */
void pc_monitor_note_link(void);
/* 卡片上的触摸（按下/抬起 + 坐标）：只做"按下高亮"反馈，判定仍在插件里。 */
void pc_monitor_card_touch(int pressed, int x, int y);

/* R3c C：app_monitor 的 load/close 调用，控制监控会话是否可见 */
void pc_monitor_page_enter(void);
void pc_monitor_page_exit(void);
int  pc_monitor_page_active(void);

/* 最近一次"主机→手表"BLE 数据帧（metrics 0x52 / 卡片帧）距今多久，毫秒。
 * 0xFFFFFFFF = 本次上电还没收到过任何 BLE 入帧。
 * 给 svc_pc 的 BLE 主机存活看门狗用：BLE 连接本身不会因为主机卡死而断开。 */
uint32_t pc_monitor_link_age_ms(void);

/* P0-4：USB 监控是表盘的 -1 屏，只有 USB 真的被主机配置起来才让右滑进去。
 * 状态由 main.c 的监控服务每个周期写一次（usb_device_is_configured(0)），
 * 读写都在 LVGL 任务里，不需要锁。 */
void pc_monitor_set_usb_ready(int on);
int  pc_monitor_usb_ready(void);
#endif
