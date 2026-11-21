/*
 * face_default.c —— 默认表盘（face_lvgl.c）的 App 描述符
 *
 * 切换由 app_framework.c 的 app_launch_face() 调 load/close；
 * 开机那一次仍由 main.c 先建（face_lvgl_create()），框架只登记描述符。
 */
#include <stddef.h>

#include "app_framework.h"
#include "faces.h"

#include "face_lvgl.h"
#include "eez_ui_glue.h"
#include "face_image.h"     /* 76：face 0 重建后要清 face_image 的旧对象指针 */

static void face_default_load(void)
{
    /* 与 `face` shell 命令相同的路径，保证切表盘时视觉一致 */
    eez_ui_leave();
    face_lvgl_create();
    eez_ui_init();
    /* face_lvgl_create() 的 lv_obj_clean() 已经把整个 page 子树删掉（face 3 的
     * 图片 root 也在里面），这里必须清它的指针，不能再访问旧对象。
     * 76 §1：原来这里调的是备用表盘 overlay 的重建钩子（face 1/2 已删除），
     * 现在直接调 face 3 的清理函数 —— 它才是挂在本页上的那个 App。 */
    face_image_on_face_recreated();
}


static void face_default_close(void)
{
    /* 切走时目前不用停/释放：main.c 仍持续把时间/电量喂给 face_lvgl，
     * 它自己按 eez_ui_page()==0 决定要不要做指针动画。 */
}

static const app_desc_t s_face_default = {
    .id = "face_default",
    .name = "默认",
    .has_gui = 1,
    .allow_bg = 0,
    .page_id = -1,
    .init = NULL,
    .load = face_default_load,
    .close = face_default_close,
    .kill = NULL,
    .power_off = NULL,
    .event = NULL,
};

void face_default_register(void)
{
    (void)app_register(&s_face_default);
}
