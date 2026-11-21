/*
 * faces.h —— 内置表盘注册
 */
#ifndef FACES_H
#define FACES_H

/* 注册 face 0（默认）/ face 3（图片）两个表盘描述符（76 §1 起 1/2 已删除）。
 * 只登记、不显示；切换由 app_framework.c 的 app_launch_face() 调 load/close。 */
void faces_register(void);

#endif /* FACES_H */
