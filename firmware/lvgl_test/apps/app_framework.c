/*
 * app_framework.c —— App 注册表 + 生命周期实现（不是桩：页面/表盘切换的注册与分派都在这）
 */
#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app_framework.h"
#include "app_state.h"
#include "faces.h"
#include "pages.h"
#include "svc_settings.h"      /* 切表盘后持久化 face_id */

static app_desc_t s_apps[APP_FRAMEWORK_MAX];
static int s_app_count;
static int s_current = -1;
static int s_face_current = -1;

static int app_index_of(const char *id)
{
    int i;
    if (id == NULL) {
        return -1;
    }
    for (i = 0; i < s_app_count; i++) {
        if (s_apps[i].id != NULL && strcmp(s_apps[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

int app_register(const app_desc_t *app)
{
    app_desc_t *dst;

    if (app == NULL || app->id == NULL || app->id[0] == '\0' ||
        app->name == NULL || s_app_count >= APP_FRAMEWORK_MAX) {
        return -1;
    }
    if (app_index_of(app->id) >= 0) {
        return -1;              /* 重复 id */
    }

    dst = &s_apps[s_app_count];
    /* 修复 QF bug：先整体清零，再整体赋值（包含 power_off 全部指针）。
     * 不做逐字段拷贝，避免漏掉后来新增的回调。 */
    memset(dst, 0, sizeof(*dst));
    *dst = *app;
    s_app_count++;
    return 0;
}

void app_framework_init(void)
{
    s_app_count = 0;
    s_current = -1;
    memset(s_apps, 0, sizeof(s_apps));

    faces_register();           /* 2 个表盘描述符（0 默认 / 3 图片）；76 §1 删掉 1/2 */
    pages_register();           /* 8 个前台页面 App（page_id 0..7） */

    for (int i = 0; i < s_app_count; i++) {
        if (s_apps[i].init != NULL) {
            s_apps[i].init();
        }
    }

    /* 默认表盘在 app_framework_init() 之前已经由 main.c/eez_ui_glue 创建 */
    s_face_current = app_index_of("face_default");
    printf("[app] framework: %d app(s) registered\r\n", s_app_count);
}

int app_count(void)
{
    return s_app_count;
}

const app_desc_t *app_at(int idx)
{
    if (idx < 0 || idx >= s_app_count) {
        return NULL;
    }
    return &s_apps[idx];
}

const app_desc_t *app_find(const char *id)
{
    int i = app_index_of(id);
    return (i >= 0) ? &s_apps[i] : NULL;
}

const app_desc_t *app_current(void)
{
    return (s_current >= 0) ? &s_apps[s_current] : NULL;
}

int app_launch_index(int idx)
{
    const app_desc_t *next;

    if (idx < 0 || idx >= s_app_count) {
        return -1;
    }
    next = &s_apps[idx];
    if (idx == s_current) {
        return 0;
    }
    if (s_current >= 0 && s_apps[s_current].close != NULL) {
        s_apps[s_current].close();
    }
    s_current = idx;
    if (next->load != NULL) {
        next->load();
    }
    return 0;
}

int app_launch(const char *id)
{
    return app_launch_index(app_index_of(id));
}

int app_launch_page(int page)
{
    int i;

    if (page < 0 || page > APP_PAGE_MAX) {
        return -1;
    }
    for (i = 0; i < s_app_count; i++) {
        if (s_apps[i].has_gui && s_apps[i].page_id == page) {
            int r = app_launch_index(i);
            if (r == 0) {
                /* 唯一写入者：app_launch_page（UI 任务），page/face 都归本文件 */
                app_state_set_page((uint8_t)page);
            }
            return r;
        }
    }
    return -1;
}

int app_launch_face(int face_id)
{
    /* 76 §1（2026-11-08 用户拍板）：只保留 face 0（默认）与 face 3（图片）；
     * face 1（极简）/ face 2（监控）已删除，任何来源（shell / settings / BLE）
     * 传进来都直接拒绝，绝不把 UI 状态改成"不存在的表盘"。 */
    static const char *names[4] = { "face_default", NULL, NULL, "face_image" };
    int idx;

    if (face_id != 0 && face_id != 3) {
        return -1;
    }
    idx = app_index_of(names[face_id]);
    if (idx < 0) {
        return -1;
    }
    if (s_face_current >= 0 && s_face_current != idx &&
        s_apps[s_face_current].close != NULL) {
        s_apps[s_face_current].close();
    }
    s_face_current = idx;
    if (s_apps[idx].load != NULL) {
        s_apps[idx].load();
    }
    app_state_set_face((uint8_t)face_id);
    /* B：表盘选择落盘（SD3010 用户 RAM）。值没变时 settings_save_face_id()
     * 内部直接返回，所以开机恢复 face_id 的那次调用不会多写 I2C。 */
    (void)settings_save_face_id((uint8_t)face_id);
    return 0;
}


const app_desc_t *app_face_current(void)
{
    return (s_face_current >= 0) ? &s_apps[s_face_current] : NULL;
}

void app_event_dispatch(const event_t *ev)
{
    int i;

    if (ev == NULL) {
        return;
    }
    if (s_current >= 0 && s_apps[s_current].event != NULL) {
        s_apps[s_current].event(ev);
    }
    for (i = 0; i < s_app_count; i++) {
        if (i == s_current || !s_apps[i].allow_bg) {
            continue;
        }
        if (s_apps[i].event != NULL) {
            s_apps[i].event(ev);
        }
    }
}

void app_power_off_all(void)
{
    for (int i = 0; i < s_app_count; i++) {
        if (s_apps[i].power_off != NULL) {
            s_apps[i].power_off();
        }
    }
}
