/*
 * faces.c —— 表盘注册入口（76 §1：只留 0 默认 + 3 图片）
 *
 * 2026-11-08 用户拍板：face 1（极简）/ face 2（监控）是测试用的，删掉；
 * 源码/注册/CMake/切换逻辑一并去掉（见本地 research/76 §1，不发布）。
 */
#include "faces.h"

/* 每个 face_*.c 暴露一个注册函数（避免在头文件里塞一堆声明） */
void face_default_register(void);
void face_image_register(void);     /* B：media 分区里的 480x480 图片表盘 */

void faces_register(void)
{
    face_default_register();
    face_image_register();
}
