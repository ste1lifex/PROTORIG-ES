/*
 * svc_pc.h —— 电脑连接模式（"拓展界面" / -1 屏）
 *
 * 设计（2026-11-06）：
 *   手表模式：表盘 / App 列表 / App 页面 —— 全部本地渲染，与现在一致。
 *   电脑模式：滑到 -1 屏（page 4）即进入，整屏交给电脑做"透传显示"：
 *     · 传输：USB 已配置优先，否则 BLE；都不在 → 显示"连接丢失"
 *     · 亮度 / 震动 / 整屏像素由电脑经帧协议控制
 *     · 进入时亮屏，常亮和息屏时间由电脑设置；息屏期间保留电脑模式和链路
 *
 * 除下面三个跨任务入口外，所有函数都只在 LVGL/UI 任务里调用（与 pc_monitor 一致）：
 *   svc_pc_stage()/svc_pc_stage_wait() —— BLE/USB 任务上下文，只进记录环；
 *   svc_pc_request()                 —— 任意任务（BLE 回调）只置一个待处理标志。
 */
#ifndef SVC_PC_H
#define SVC_PC_H
#include <stdint.h>

/* 传输类型 */
enum { SVC_PC_LINK_NONE = 0, SVC_PC_LINK_USB = 1, SVC_PC_LINK_BLE = 2 };

void svc_pc_init(void);

/* 进入 / 离开电脑模式（page 4 的 load / close 调用） */
void svc_pc_enter(void);
void svc_pc_leave(void);
int  svc_pc_active(void);

/* USB 像素透传期间本地输入必须静默（触摸仍转发给电脑）。
 * 返回 1 = 当前的本地手势/点击应该被 UI 层丢弃。 */
int  svc_pc_local_input_blocked(void);

/* 当前可用传输（每 tick 刷新） */
int  svc_pc_transport(void);
int  svc_pc_link_up(void);

/* UI 任务周期调用：刷新链路、收发状态、掉线切换 */
void svc_pc_tick(void);

/* BLE 任务上下文调用：把一段 ≤240 B 的帧片塞进记录环，UI 任务再处理。
 * 绝不在这里碰 LVGL / 面板；环满时直接丢（不阻塞协议栈）。 */
void svc_pc_stage(const uint8_t *p, uint16_t len);

/* USB 任务上下文调用：与 svc_pc_stage 同一个入口，但环满时会让出 CPU 等
 * UI 任务抽片，最多等 timeout_ms（实测整带 65 片连发时 8 片环会丢）。
 * 返回 0 = 已入队，-1 = 超时丢弃（计入 ring_drop）。 */
int  svc_pc_stage_wait(const uint8_t *p, uint16_t len, uint32_t timeout_ms);

/* shell：pcmode 0|1 / pcmode（查询） */
int  svc_pc_cmd(int argc, char **argv);

/* 任意任务（BLE 协议栈回调）请求进/出电脑模式：只置一个待处理标志，
 * 真正的切换在 UI 任务的 svc_pc_tick() 里做（要切页面、冻结 LVGL）。
 * on = 1 进 -1 屏，0 退出回表盘。 */
void svc_pc_request(int on);

/* ------------------------------------------------------------------ */
/* 2026-11-08：PC 模式下的本地弹窗（闹钟/计时器到点）                  */
/*                                                                    */
/* 用户要求：PC 模式（USB 透传 / BLE 远程）下到点也要弹出本地圆屏通知卡，*/
/* 但**不能退出 PC 模式 / 不能拆 BLE session**。做法：                  */
/*   suspend=1：保持 s_active / 链路 / 0x91 状态与心跳不变，            */
/*              丢弃 0x10/0x11/0x12/0x13 像素帧，临时解冻 LVGL          */
/*              把本地页 + 通知卡画到屏幕上（s_passthrough 保持 1，     */
/*              不向电脑发 MODE 0）；弹窗期间触摸只走本地、不转发 PC；  */
/*   suspend=0：重新冻结 LVGL、恢复收帧，并给电脑发一次 0x92            */
/*              REDRAW_REQ（reason=1），请主机整屏重发。                */
/* 只在 UI 任务调用。不在 PC 模式时两个方向都直接返回（行为完全不变）。  */
void svc_pc_suspend_frames(int on);
int  svc_pc_frames_suspended(void);

#endif /* SVC_PC_H */
