/*
 * eez_ui_glue.c —— 圆屏终端的分页 / 手势层（表盘 + 原生 App 页）
 *
 * 文件名是历史遗留：页面已是原生布局（ui/device_pages.c、ui/app_pages.c），
 * firmware/eez_ui/ 的 EEZ Studio 工程只作参考、不参与编译。
 *
 * ===========================================================================
 * 2026-10-12 定版：**不做滑动动画，滑动即翻页（即时反馈）**
 * ===========================================================================
 * 走过的路（都留了数字，别再绕回去）：
 *   ① 老实现 lv_screen_load_anim(MOVE_LEFT, 140ms)：每帧把两屏一起移，
 *      每帧标脏整屏；一次翻页实测要推 1,612,800 像素 = 7 个整帧 = 161 ms，
 *      却塞进 140 ms 的动画里 —— 带宽上就不成立，必然掉帧。
 *   ② 跟手版：内容跟着手指走，每帧只重画进来那一页的可见区（1~8 ms/帧）。
 *      手感确实好，但**滑动这种转场每帧都要把平移后的内容整块重画**，
 *      一次翻页仍是 1.3~1.4M 像素；表盘那一页（地形 + 6 条抗锯齿大圆弧 +
 *      60 条刻度）一进画面就更重。真机打两轮后结论：这块板子的 CPU/带宽
 *      在"整页滑动"这件事上到顶了。
 *   ③ 现在：**手势判定 → 立刻整屏换页**。成本 = 一个整帧（230,400 px = 23 ms
 *      送屏 + 一次整屏渲染），而且新页不透明、旧页被遮挡判断剔掉，不重复画。
 *
 * 结构上仍然是"一个 screen 装多个页面容器"（见下；现在 PAGE_N=8）：只有当前页在 (0,0)、其余停在屏幕外，
 * 所以"换页"就是——旧页挪到屏幕外、新页挪回 (0,0)，再整屏标脏一次。
 *
 * 为什么不用 LVGL 的多屏（act_scr/prev_scr）：
 *   lv_indev.c 的 lv_indev_read() 里有一句
 *       if(indev->disp->prev_scr != NULL) return;   // input blocked while screen animation active
 *   只要 prev_scr 非空输入层就整个停摆，任何"两屏叠加"的方案都做不成。
 */
#include "eez_ui_glue.h"
#include "app_framework.h"   /* R3c A+B：页面入口统一走 app_launch_page() */
#include "device_pages.h"
#include "app_pages.h"       /* 2026-11-08：秒表/计时器/闹钟三个 App 页 */
#include "ui_widgets.h"
#include "face_lvgl.h"
#include "board_power.h"
#include "pc_monitor.h"
#include "svc_ble.h"
#include "svc_pc.h"
#include "ble_link.h"        /* P0-4：BLE 连上也能进 -1 屏电脑模式 */
#include "app_list.h"
#include "ui_theme.h"
#include "ui_gesture.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>

#define YELLOW 0xF2ED65

#define SCR_W 480
#define SCR_H 480
/* 0 表盘 / 1 协议网络 / 2 相遇记录（1/2 是占位页，App 列表 hidden=1 不显示）/
 * 3 终端设置 / 4 USB 监控 / 5 秒表 6 计时器 7 闹钟。
 * 必须与 app_framework.h 的 APP_PAGE_MAX、app_pages.h 的 APP_PAGE_* 对齐。 */
#define PAGE_N 8
#define MONITOR_PAGE 4

/* ---- P0-1 导航诊断（NAV_TRACE 改 0 即整体关闭）-------------------
 * 只在"状态变化"上打一行：每个 PRESSING 都打会把串口刷爆，还会让 UI 变慢，
 * 反而改变被测行为。 */
#define NAV_TRACE 1
#if NAV_TRACE
#define NAVLOG(...) printf("[nav] " __VA_ARGS__)
#else
#define NAVLOG(...) ((void)0)
#endif

/* ---- 手势参数（设备像素 / 毫秒）---- */
#define AXIS_MIN_PX 18      /* 判主方向前至少要划这么远（抗第一帧噪声） */
#define AXIS_X10    16      /* 主方向 > 1.6 × 次方向（×10 用整数比） */
#define NAV_PCT     9       /* 拖过屏宽/屏高的 9%（≈43px）就翻页 —— 真机要更灵敏 */
#define NAV_PX      32      /* 轻甩：只要划出这么远…… */
#define NAV_V       220     /* ……且速度 ≥0.22 px/ms 就翻页（×1000） */
#define TAP_TRAVEL  24      /* 轻点允许的累计曼哈顿位移（ui_gesture.h 里也用） */
#define TAP_MS      650

static int s_page, s_created;
static lv_obj_t *s_scr[PAGE_N];     /* 全部 PAGE_N 个页面容器（都是 root screen 的孩子） */
static lv_obj_t *s_input;           /* 整屏手势层（sys layer，吞掉所有报点） */

/* 每一笔触摸的序号：PRESSED 时 +1。App 列表用它判断"这一下按下是不是
 * 把列表打开的那一笔之后的新按"，从根上堵住"同一笔/分裂出来的一笔被当成
 * 点击列表项"的回弹路径。*/
static int s_touch_epoch;

int eez_ui_touch_epoch(void)
{
    return s_touch_epoch;
}

/* ---- 本次触摸 ---- */
static ui_gesture_t s_gesture;
static int s_tracking;
static int s_hold;              /* 这次触摸已经导航过 / 到头了，不再当轻点 */
static int s_nav_swallow;       /* PRESSING 已切列表：吞掉本笔剩余事件 */
static int s_list_down;         /* 列表分支消费了 PRESSED：只有这一笔能滑动/抬手 */
static int s_gest_page;         /* 这一笔按下时所在的页：手势只能改它自己起手的页 */
static int s_tapped_target;     /* 按下时命中的设置项 */
static uint32_t s_press_ms;
static int s_last_d, s_last_ms, s_vel;   /* 最近一次"真的动了"的采样：估速度 */

/* 诊断：上一笔手势收到几个采样、有没有触发翻页 */
static int s_list_press_x, s_list_press_y;
static uint32_t s_list_press_ms;
static int s_gest_samples, s_gest_nav;

void *eez_ui_page_obj(int i)
{
    if (i < 0 || i >= PAGE_N) return NULL;
    return s_scr[i];
}

/* ------------------------------------------------------------------ */
/* 页面搬运（记帐动作本身不该产生重绘）-------------------------------- */
/* ------------------------------------------------------------------ */

/* 抬层 / 挪到屏幕外这些动作不该重绘：被抬起来的页要么在屏幕外，要么已经被
 * 新页整屏盖住，肉眼看不出差别。 */
static void quiet_begin(void)
{
    lv_display_enable_invalidation(lv_display_get_default(), false);
}

static void quiet_end(void)
{
    lv_display_enable_invalidation(lv_display_get_default(), true);
}

/* 只改样式的话 coords 是懒更新的，会拖到下一次 layout 更新时才补标脏 ——
 * 那时候标脏开关已经打开，整块旧区域就又白重绘一遍。 */
static void page_place(int i, int x, int y)
{
    lv_obj_set_pos(s_scr[i], x, y);
    lv_obj_move_to(s_scr[i], x, y);
}

/* 换页：目标页占 (0,0) 且在最上面，其余页一律停到屏幕外，最后整屏标脏。
 * 整屏标脏一次 = 一个整帧，这就是"即时反馈"的全部代价。 */
static void goto_page_raw(int page)
{
    if (page < 0 || page >= PAGE_N || page == s_page) return;

    quiet_begin();
    for (int i = 0; i < PAGE_N; i++) {
        if (i == page) {
            lv_obj_move_foreground(s_scr[i]);
            page_place(i, 0, 0);
        } else if (i == s_page) {
            page_place(i, SCR_W, 0);
        }
    }
    quiet_end();

    s_page = page;
    /* 新页不透明、盖满整屏，旧页会被遮挡判断剔掉 —— 只画一次，不重复 */
    lv_obj_invalidate(s_scr[page]);
    printf("[ui] page=%d\r\n", s_page);
}

/* ------------------------------------------------------------------ */
/* 设置页 ------------------------------------------------------------- */
/* ------------------------------------------------------------------ */

/* Device pages own their layout and selection state. The shared gesture
 * layer keeps using the same press/cancel/release path as before. */
void eez_ui_refresh_settings(void)
{
    if (s_created) device_settings_open();
}

/* App 页（秒表/计时器/闹钟）：显示与触摸都转给 ui/app_pages.c，
 * 业务状态在 services/svc_stopwatch、svc_timer、svc_alarm。 */
static int is_app_page(int page)
{
    return app_page_is(page);
}

static int target_at(int x, int y)
{
    if (((s_page >= 1 && s_page <= 3) || is_app_page(s_page)) &&
        ui_rect_hit(&ui_back_rect, x, y)) {
        return 1;
    }
    if (s_page == 3) return device_settings_target(x, y);
    if (is_app_page(s_page)) return app_page_target(s_page, x, y);
    return 0;
}

static void feedback(int target, int pressed)
{
    if (s_page == 3) {
        device_settings_press(target, pressed);
    } else if (is_app_page(s_page)) {
        app_page_press(s_page, target, pressed);
    }
}

static void tap_dispatch(void)
{
    if (s_tapped_target == 1) {
        /* 设置子页先回总览；其余页面的返回键都回应用列表 */
        if (s_page == 3 && device_settings_back()) {
            return;
        }
        if (s_page >= 1) {
            app_list_show();
        }
    } else if (s_page == 3) {
        device_settings_activate(s_tapped_target);
    } else if (is_app_page(s_page)) {
        app_page_activate(s_page, s_tapped_target);
    }
}
/* ------------------------------------------------------------------ */
/* 输入 --------------------------------------------------------------- */
/* ------------------------------------------------------------------ */

/* 这一笔够不够"翻页"？够就立刻换页，不做动画。 */
static int nav_ready(int moved, int vel)
{
    int span = SCR_W;   /* 只用屏宽做比例：屏是方的 */

    if (moved >= span * NAV_PCT / 100) return 1;      /* 稳拖：12% ≈ 58px */
    if (moved >= NAV_PX && vel >= NAV_V) return 1;    /* 轻甩：40px 且 0.25px/ms */
    return 0;
}

/* P0-4：USB 监控回到"表盘右滑"的 -1 屏位置，但只有 USB 真的连上电脑才让进；
 * 没插 USB 时表盘右滑什么都不做（不是空页，也不闪一下旧页面）。
 * 左滑仍然是：表盘 -> App 列表；从列表打开的 App 右滑 -> 列表。 */
static void nav_horizontal(int left)
{
    int before = app_list_visible();
    int before_page = s_page;

    /* 开机 1.5 s 内忽略手势：触摸芯片上电会报一次假点，历史上它把设备
     * 带进过 App 列表、也带进过 -1 屏（[ui] page=4 -> page=0 闪一下）。 */
    if (lv_tick_get() < 1500u) {
        NAVLOG("NAV ignored (boot guard %lu ms)\r\n", (unsigned long)lv_tick_get());
        return;
    }

    if (!left && s_page == 3 && device_settings_back()) return;

    if (left) {
        if (s_page == 0) {
            app_list_show();
        } else if (s_page == MONITOR_PAGE) {
            /* USB 透传时输入层已经吞掉手势；这里再兜一层：不允许滑出，
             * 只能拔线 / 关 DSH 让固件自己退回表盘。BLE 模式仍可左滑退出。 */
            if (svc_pc_local_input_blocked()) {
                NAVLOG("NAV LEFT blocked (USB PC passthrough)\r\n");
                return;
            }
            /* P0-4：BLE 页用左滑退出，滑回 -1 屏重新握手 */
            app_launch_page(0);
        }
    } else if (s_page == 0) {
        /* 表盘右滑 -> -1 屏（电脑模式）。两种链路都能进：
         *   USB 已被主机配置（像素透传）；
         *   或者 BLE 已连接（电池供电时只有 BLE：本地界面 + 小数据/执行器）。
         * 都没有就什么都不做，不留空页、不闪旧页面。 */
        if (s_gest_page == 0) {
            /* 一律允许（和旧 firmware_m2 构建同一套修法）：要求"链路已连"才能进会死锁 ——
             * 广播/连接恰恰是 svc_pc_enter() 才开始做的。 */
            app_launch_page(MONITOR_PAGE);
        }
    } else if (s_page == MONITOR_PAGE) {
        /* 监控页右滑不再退出（退出改成左滑），这里什么都不做 */
    } else if (app_list_return_armed()) {
        app_list_show();
    }
    NAVLOG("NAV %s page=%d(->%d) armed=%d usb=%d list=%d->%d\r\n",
           left ? "LEFT" : "RIGHT", before_page, s_page, app_list_return_armed(),
           pc_monitor_usb_ready(), before, app_list_visible());
}

static void input_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    lv_event_code_t code;
    uint32_t now;
    int dx, dy, adx, ady, d, moved;

    if (!indev) return;
    lv_indev_get_point(indev, &p);
    code = lv_event_get_code(e);
    now = lv_tick_get();

    /* USB 电脑模式透传中：本地输入全部吞掉（触摸照常经 pc_touch_hook 转发给电脑）。
     * 否则画面冻结、手势却在本地页面上静默导航 —— 断线后会突然出现在设置页。 */
    if (svc_pc_local_input_blocked()) {
        s_tracking = 0;
        s_hold = 0;
        s_nav_swallow = 0;
        s_list_down = 0;
        return;
    }

    /* F1：列表是独立视图，输入先交给 app_list；不进入旧页面手势。 */
    if (app_list_visible()) {
        if (s_nav_swallow) {
            NAVLOG("L-swallow code=%d\r\n", (int)code);
            if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
                s_nav_swallow = 0;
                s_tracking = 0;
                s_hold = 0;
                s_list_down = 0;
            }
            return;
        }
        if (code == LV_EVENT_PRESSED) {
            s_touch_epoch++;
            s_list_press_x = p.x;
            s_list_press_y = p.y;
            s_list_press_ms = now;
            s_tracking = 1;
            s_hold = 0;
            s_list_down = 1;
            NAVLOG("L-PRESS (%d,%d) page=%d armed=%d epoch=%d\r\n",
                   (int)p.x, (int)p.y, s_page, app_list_return_armed(), s_touch_epoch);
            app_list_press(p.x, p.y);
            return;
        }
        /* 只有"列表分支自己消费过 PRESSED"的这一笔才允许算滑动/抬手。
         * 假抬手之后残留的 PRESSING 里 s_list_press_x 还是 0，算出来的 dx
         * 就是整个 x 坐标 —— 够判一次"右滑"把列表关掉，手指继续走的那一段
         * 又变成下一笔手势：设置页右滑串进 USB 页就是这么来的。 */
        if (!s_list_down) {
            return;
        }
        if (s_hold) {
            return;
        }
        if (code == LV_EVENT_PRESSING) {
            app_list_swipe(p.x - s_list_press_x, p.y - s_list_press_y);
            if (!app_list_visible()) {
                /* 这一笔把列表关掉了（右滑回表盘）：抬手时不要再让底层页面的兜底手势判一次方向 */
                s_hold = 1;
                s_tracking = 0;
                s_list_down = 0;
            }
        } else if (code == LV_EVENT_RELEASED) {
            s_list_down = 0;
            NAVLOG("L-REL dx=%d dy=%d ms=%d\r\n",
                   (int)(p.x - s_list_press_x), (int)(p.y - s_list_press_y),
                   (int)(now - s_list_press_ms));
            app_list_release(p.x - s_list_press_x, p.y - s_list_press_y,
                             now - s_list_press_ms);
            if (!app_list_visible()) {
                s_hold = 1;
                s_tracking = 0;
            }
        } else if (code == LV_EVENT_PRESS_LOST) {
            /* 丢触点不是"点了这一项"：只清候选，绝不启动 App */
            s_list_down = 0;
            NAVLOG("L-LOST -> cancel\r\n");
            app_list_cancel();
        }
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        s_touch_epoch++;
        s_gest_page = s_page;
        s_nav_swallow = 0;
        s_tracking = 1;
        s_hold = 0;
        s_press_ms = now;
        s_last_d = 0;
        s_last_ms = 0;
        s_vel = 0;
        s_gest_samples = 0;
        s_gest_nav = 0;
        ui_gesture_begin(&s_gesture, p.x, p.y, now);
        s_tapped_target = target_at(p.x, p.y);
        feedback(s_tapped_target, 1);
        NAVLOG("PRESS (%d,%d) page=%d list=%d armed=%d epoch=%d\r\n",
               (int)p.x, (int)p.y, s_page, app_list_visible(),
               app_list_return_armed(), s_touch_epoch);
        return;
    }

    if (code == LV_EVENT_PRESSING && s_tracking && !s_hold) {
        dx = p.x - s_gesture.x;
        dy = p.y - s_gesture.y;
        adx = abs(dx);
        ady = abs(dy);
        s_gest_samples++;
        ui_gesture_update(&s_gesture, p.x, p.y);

        d = (adx >= ady) ? dx : dy;
        if (d != s_last_d) {
            int prev_ms = s_last_ms ? s_last_ms : (int)s_press_ms;
            int dt = (int)(now - (uint32_t)prev_ms);
            if (dt > 0 && dt < 200) {
                int v = abs(d - s_last_d) * 1000 / dt;
                s_vel = (v > 6000) ? 6000 : v;
            }
            s_last_d = d;
            s_last_ms = (int)now;
        }
        if (s_last_ms && (int)(now - (uint32_t)s_last_ms) > 220) s_vel = 0;

        /* F1：只保留“表盘左滑进列表 / App 右滑回列表”，不再左右翻旧页。 */
        if (adx >= AXIS_MIN_PX && adx * 10 >= ady * AXIS_X10) {
            moved = adx;
            if (nav_ready(moved, s_vel)) {
                feedback(s_tapped_target, 0);
                s_tapped_target = 0;
                s_hold = 1;
                s_gest_nav = 1;
                NAVLOG("PRESSING nav dx=%d dy=%d vel=%d moved=%d hold=%d\r\n",
                       dx, dy, s_vel, moved, s_hold);
                nav_horizontal(dx < 0);
                if (app_list_visible()) {
                    s_nav_swallow = 1;
                    NAVLOG("swallow=1 (list just opened)\r\n");
                }

            }
        }
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_tracking) return;
        NAVLOG("REL code=%d dx=%d dy=%d ms=%d hold=%d page=%d list=%d\r\n",
               (int)code, (int)(p.x - s_gesture.x), (int)(p.y - s_gesture.y),
               (int)(now - s_press_ms), s_hold, s_page, app_list_visible());
        s_tracking = 0;
        feedback(s_tapped_target, 0);
        if (!s_hold && code == LV_EVENT_RELEASED) {
            /* 报点太稀时的兜底：抬手这一下才凑够距离，也认 */
            int a = ui_gesture_end(&s_gesture, p.x, p.y, now);
            NAVLOG("REL fallback dx=%d dy=%d ms=%d a=%d page=%d list=%d\r\n",
                   (int)(p.x - s_gesture.x), (int)(p.y - s_gesture.y),
                   (int)(now - s_press_ms), a, s_page, app_list_visible());
            if (a == UI_TAP) {
                if (target_at(p.x, p.y) == s_tapped_target) tap_dispatch();
            } else if (a == UI_LEFT) {
                s_gest_nav = 1;
                nav_horizontal(1);
            } else if (a == UI_RIGHT) {
                s_gest_nav = 1;
                nav_horizontal(0);
            }
        }
        s_tapped_target = 0;
        s_hold = 0;
    }
}


/* 手势层挂在 sys layer：它在所有页面之上，既不会被抬层打乱顺序，
 * 也不需要因为页面上下移动而重新提升（提升整屏对象 = 白送一次整屏重绘）。 */
static void overlay_create(void)
{
    lv_obj_t *top = lv_layer_sys();

    s_input = lv_obj_create(top);
    lv_obj_remove_style_all(s_input);
    lv_obj_set_pos(s_input, 0, 0);
    lv_obj_set_size(s_input, SCR_W, SCR_H);
    lv_obj_remove_flag(s_input, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_input, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_input, input_cb, LV_EVENT_ALL, NULL);
}

/* ------------------------------------------------------------------ */
/* 对外接口 ----------------------------------------------------------- */
/* ------------------------------------------------------------------ */

static void eez_ui_fast_tick_cb(lv_timer_t *t);

void eez_ui_init(void)
{
    if (!s_created) {
        for (int i = 1; i < MONITOR_PAGE; i++) s_scr[i] = device_page_create(lv_screen_active(), i);
        s_created = 1;
        s_scr[MONITOR_PAGE] = pc_monitor_create(lv_screen_active());
        /* 2026-11-08：秒表/计时器/闹钟三个 App 页（app_pages.c） */
        for (int i = APP_PAGE_FIRST; i <= APP_PAGE_LAST; i++) {
            s_scr[i] = app_page_create(lv_screen_active(), i);
        }
        s_scr[0] = face_lvgl_page();
        overlay_create();
        app_list_init();
        /* 秒表的百分秒用 33 ms 节拍（100 ms 的 eez_tick 会跳 10 个百分秒） */
        lv_timer_create(eez_ui_fast_tick_cb, 33, NULL);
    }
    /* root screen 只是所有页面的画布：不滚动、不吃点击、没有内边距。
     * 不滚动很重要 —— 页面会被停到屏幕外，可滚动的父对象会因此产生滚动条区域。 */
    {
        lv_obj_t *root = lv_screen_active();
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_all(root, 0, 0);
        lv_obj_set_style_bg_color(root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    }

    /* 直接落到表盘：quiet 窗口里把所有页摆好，最后整屏标一次 */
    s_page = 0;
    s_tracking = 0;
    s_hold = 0;
    s_tapped_target = 0;
    quiet_begin();
    for (int i = 0; i < PAGE_N; i++) {
        if (i == 0) {
            lv_obj_move_foreground(s_scr[i]);
            page_place(i, 0, 0);
        } else {
            page_place(i, SCR_W, 0);
        }
    }
    quiet_end();
    lv_obj_invalidate(s_scr[0]);
}

/* R3c A+B：App 的 load 只调这个"原始跳页"，不重复走 app_launch_page() */
void eez_ui_goto_raw(int page)
{
    goto_page_raw(page);
}

void eez_ui_enter(void)
{
    app_launch_page(1);
}

void eez_ui_leave(void)
{
    /* shell 的 face 命令会立刻重建表盘：必须同步切回去 */
    s_tracking = 0;
    s_hold = 0;
    s_tapped_target = 0;
    app_launch_page(0);
}

void eez_ui_show(int i)
{
    if (i >= 0 && i < 3) app_launch_page(i + 1);
}

void eez_ui_monitor(void) { app_launch_page(MONITOR_PAGE); }

int eez_ui_is_open(void)
{
    return s_page != 0;
}

int eez_ui_page(void)
{
    return s_page;
}

void eez_ui_tick(void)
{
    if (!s_created || app_list_visible()) {
        return;
    }
    if (s_page == 3) {
        device_settings_tick();
    } else if (is_app_page(s_page)) {
        app_page_tick(s_page);
    }
}

/* 33 ms 节拍：只有秒表页需要（百分秒）；其它页什么都不做。 */
static void eez_ui_fast_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (is_app_page(s_page) && !app_list_visible()) {
        app_page_tick_fast(s_page);
    }
}

void eez_ui_diag(void)
{
    printf("[ui] page=%d built=%d view=(%d,%d)  key=%d(12%%/40px-0.25pxms)\r\n",
           s_page, s_created,
           (int)lv_obj_get_x(s_scr[s_page]), (int)lv_obj_get_y(s_scr[s_page]),
           s_page);
    printf("[ui] 上一笔：采样 %d 次 / %s（采样数太少就说明触摸报点太稀）\r\n",
           s_gest_samples, s_gest_nav ? "触发了翻页" : "没翻页");
}
