/*
 * app_framework.h —— App 注册表 + 生命周期（已编入；main.c 用它做页面/表盘切换）
 *
 * 设计参考 QF ZERO V2 的 app_manage（纯 C 链表/数组注册表、has_gui 区分后台），
 * 但修掉它 app_install() 漏拷 app_power_off 的 bug：
 * 这里用 **整个 struct 一次赋值**（先 memset 再 *dst = *src），不做逐字段拷贝。
 *
 * 约束：
 *   - main.c / eez_ui_glue.c 只在 UI 任务里调 app_framework_init()/app_launch_page()/app_launch_face()；
 *   - load/close/kill 等回调由 App 实现，只允许在 UI 任务里被调用；
 *   - id/name 必须是静态生命周期的字符串（注册表不深拷贝字符串）。
 */
#ifndef APP_FRAMEWORK_H
#define APP_FRAMEWORK_H

#include <stdint.h>

#include "event_queue.h"

#define APP_FRAMEWORK_MAX 16

/* 前台页号上限（与 eez_ui_glue.c 的 PAGE_N 一致）：
 *   0 表盘 / 1 协议网络 / 2 相遇记录 / 3 终端设置 / 4 USB 监控
 *   5 秒表 / 6 计时器 / 7 闹钟      ← 2026-11-08 APP 第一批
 * page_id = -1 表示不是页面 App（表盘注册在 face 域）。 */
#define APP_PAGE_MAX 7

typedef struct app_desc {
    const char *id;             /* 唯一 id，静态字符串 */
    const char *name;           /* 显示名 */
    uint8_t has_gui;            /* 1 = 有前台界面 */
    uint8_t allow_bg;           /* 1 = 退到后台仍接收事件（BLE/雷达类） */
    int page_id;                /* 前台页号 0..APP_PAGE_MAX；-1 = 不是页面 App（表盘等） */

    void (*init)(void);         /* 注册后一次（框架 init 时调用） */
    void (*load)(void);         /* 进前台：创建/显示 UI，仅 UI 任务 */
    void (*close)(void);        /* 退后台：隐藏 UI，保留状态 */
    void (*kill)(void);         /* 释放资源（息屏/内存紧张） */
    void (*power_off)(void);    /* 关机前存档（QF 漏拷的就是这个） */
    void (*event)(const event_t *ev);   /* 事件分发（仅 UI 任务） */
} app_desc_t;

/* 清空注册表并调用 faces_register()（默认/图片两个表盘；76 §1 起 1/2 已删）。
 * 在 lvgl_task 初始化末期、eez_ui_init() 之后调用一次。 */
void app_framework_init(void);

/* 注册（深拷贝 struct；id 重复 / 表满返回 -1）。 */
int app_register(const app_desc_t *app);

int app_count(void);
const app_desc_t *app_at(int idx);
const app_desc_t *app_find(const char *id);
const app_desc_t *app_current(void);

/* 生命周期：页面切换（app_list -> app_launch_page）与表盘切换（app_launch_face）都走这里。 */
int app_launch(const char *id);
int app_launch_index(int idx);

/* 按 page_id(0..APP_PAGE_MAX) 启动页面 App；成功后写 app_state.page。
 * 只允许在 UI 任务调用（页面 App 的 load 会调 eez_ui_goto_raw）。 */
int app_launch_page(int page);

/* 76 §1：切换表盘（0=face_default 默认 / 3=face_image 图片表盘）。
 * face 1（极简）/ face 2（监控）已删除：传 1/2 或其它值一律返回 -1，不改任何状态。
 * 成功后持久化 face_id（settings_save_face_id）。
 * 只改 face 域，不动 app_current（页面 App）。仅 UI 任务调用。 */
int app_launch_face(int face_id);
const app_desc_t *app_face_current(void);
void app_event_dispatch(const event_t *ev);
void app_power_off_all(void);

#endif /* APP_FRAMEWORK_H */
