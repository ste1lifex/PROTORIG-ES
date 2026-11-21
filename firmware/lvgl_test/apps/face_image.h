/*
 * face_image.h —— 图片表盘 App（face id 3，表盘可替换，2026-11-08）
 *
 * 数据来自 services/svc_face_store（media 分区里的 480x480 I8 图 + 叠层参数）；
 * 本模块只负责把 XIP 指针包装成 lv_image_dsc_t、叠时间/日期/电量。
 * 契约见本地 research/70_表盘可替换_实现方案.md §3.1（local/，不发布）。
 */
#ifndef FACE_IMAGE_APP_H
#define FACE_IMAGE_APP_H

/* 注册 app_desc（id="face_image", page_id=-1, has_gui=1），由 faces_register() 调用 */
void face_image_register(void);

/* 创建/显示（root 挂在 face_lvgl_page() 上，480x480，顶层）。
 * 无有效图片时显示"未安装表盘"提示页 —— 仍然允许切到 face 3，方便调试。 */
void face_image_show(void);

/* 隐藏（切换到其它表盘时由 app_framework 的 close 回调调用） */
void face_image_hide(void);

/* 主循环/500 ms 定时器调用：仅在可见时按 app_state 刷新时间/日期/电量 */
void face_image_tick(void);

/* face_lvgl_create() 的 lv_obj_clean() 会删掉本 App 的对象，之后必须清指针 */
void face_image_on_face_recreated(void);

/* 1 = media 分区里有校验通过的 header（不校验 231 KB 图片 CRC） */
int face_image_available(void);

#endif /* FACE_IMAGE_APP_H */
