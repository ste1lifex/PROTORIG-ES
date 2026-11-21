/*
 * tp_lvgl.c —— 把 CST9220 触摸接成 LVGL 的输入设备（lv_indev）
 *
 * 用法上这一层很薄：LVGL 输入定时器每 5 ms 回调一次 read_cb（周期在
 * tp_lvgl_init() 里设，实际受 lv_timer_handler 调度影响），
 * 我们读一次触摸芯片，填 state/point 就完事。好处是：
 *   · 点击 / 长按 / 滑动 / 拖拽 全部由 LVGL 的控件自动处理，
 *     不用再像 watchface/main.c 那样自己写「按下沿 + 800ms 抬手 + 1.2s 冷却」
 *   · 坐标系一致（触摸 480x480，屏也是 480x480），不需要缩放
 *
 * 判定规则沿用 V1.4 踩过的坑（见 watchface/README 6.7）：
 *   status==6 才是「真新报点」，status==0 是「位置没变的回显」。
 *   所以只认 6，连续 100ms 没有新报点才算抬手（与渲染帧数无关）——
 *   否则按住不动会被误判成抬手，一次按压被切成好几段。
 */
#include "tp_lvgl.h"

#include "tp.h"
#include "board_power.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define TP_RELEASE_MS 100


static uint16_t s_last_x;
static uint16_t s_last_y;

static bool     s_pressed;
static bool     s_wake_contact; /* raw wake stroke is never forwarded to widgets */
static uint32_t s_wake_last_ms;
static uint32_t s_taps;
static uint32_t s_last_press_ms;    /* 最近一次有效报点的时间 */
static tp_lvgl_hook_t s_hook;       /* UI 反馈钩子（表盘拿它画手指圆环） */

/* ------------------------------------------------------------------ */
/* INT 门控 —— ⚠️ 默认关闭，别随便开                                    */
/*                                                                    */
/* 想法是：没手指时跳过 I2C 读，省电。**但实测数据不支持这么做**：      */
/* watchface/README 6.7 (3) 那张 77 帧原始报文表里，20 次真正的 status=6 */
/* 报点中有 **14 次 INT 是高电平** —— INT 脉冲很短，当时 20 ms 轮询本来  */
/* 就会漏掉大部分。再加一层"INT 高就跳过读"会把真实触摸点也一起漏掉。   */
/*                                                                    */
/* 而且收益也有限：跳过 I2C 读并不能让触摸芯片少扫描（它自己照扫），   */
/* 真正的耗电大头是芯片自身的 1.4 mA 动态电流（见                      */
/* docs/cst9220_lowpower_investigation.md）。省的是主机侧那 5% 的       */
/* I2C 占空比，不值得冒丢点的风险。                                     */
/*                                                                    */
/* 保留这个开关只为做对照实验（量电流时 `tpgate 1` / `tpgate 0`）。    */
/* ------------------------------------------------------------------ */
static int      s_int_gate = 0;     /* ← 默认关！理由见上 */
static uint32_t s_gated;

void tp_lvgl_set_int_gate(int on)
{
    s_int_gate = on ? 1 : 0;
    printf("[tp] int gate = %s\r\n", s_int_gate ? "ON" : "OFF");
}

int tp_lvgl_get_int_gate(void)
{
    return s_int_gate;
}

uint32_t tp_lvgl_get_gated(void)
{
    return s_gated;
}

/* ------------------------------------------------------------------ */
/* LVGL 回调：tp_lvgl_init() 把读周期设成 5 ms                        */
/* ------------------------------------------------------------------ */
static void tp_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x = 0, y = 0;
    int hit;
    int int_low;

    (void)indev;

    int_low = tp_int_asserted();

    /* 没手指 且 INT 为高 → 芯片没报"有事"，一个 I2C 事务都不发 */
    if (s_int_gate && !int_low && !s_pressed) {
        s_gated++;
        data->point.x = s_last_x;
        data->point.y = s_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        if (s_hook) {
            s_hook(0, s_last_x, s_last_y, s_taps);
        }
        return;
    }

    hit = tp_cst9220_read_points(TP_ADDR_DEFAULT, &x, &y);

    /* Consume the complete wake stroke, not the next arbitrary tap. */
    if (s_wake_contact) {
        if (hit == 1) { s_wake_last_ms = lv_tick_get(); power_note_touch(); }
        else if ((uint32_t)(lv_tick_get() - s_wake_last_ms) >= TP_RELEASE_MS) s_wake_contact = false;
        s_pressed = false;
        data->point.x = s_last_x;
        data->point.y = s_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (hit == 1) {

        s_last_x = x;
        s_last_y = y;
        s_last_press_ms = lv_tick_get();

        if (!s_pressed) {
            s_pressed = true;
            s_taps++;
            /* No USB printf on the interactive input path. */
        }

        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;

        /* 有手指 → 刷新"用户在场"计时（同时也负责从息屏里唤醒） */
        power_note_touch();
    } else {

        data->point.x = s_last_x;
        data->point.y = s_last_y;

        if (!s_pressed || (uint32_t)(lv_tick_get() - s_last_press_ms) >= TP_RELEASE_MS) {
            if (s_pressed) {
                s_pressed = false;
                
            }
            data->state = LV_INDEV_STATE_RELEASED;
        } else {
            /* 还没到抬手阈值：保持按下，继续沿用上一个坐标（防抖） */
            data->state = LV_INDEV_STATE_PRESSED;
        }
    }

    /* 立刻回调 UI —— 不再另开一个定时器，省掉那层延迟。
     * 这样从"芯片报点"到"屏上圆环动"最多只有一个 LVGL 刷新周期。 */
    if (s_hook) {
        s_hook(s_pressed, s_last_x, s_last_y, s_taps);
    }
}

/* ------------------------------------------------------------------ */
/* 息屏时的轻量轮询：只要"有没有手指"，不进 LVGL                        */
/* ------------------------------------------------------------------ */
int tp_lvgl_poll_raw(void)
{
    uint16_t x, y;

    /* 息屏时同样用 INT 门控 —— 这是"息屏待机"下最频繁的一条 I2C 读取 */
    if (s_int_gate && !tp_int_asserted()) {
        s_gated++;
        return 0;
    }
    if (tp_cst9220_read_points(TP_ADDR_DEFAULT, &x, &y) == 1) {
        s_wake_contact = true;
        s_wake_last_ms = lv_tick_get();
        s_last_x = x; s_last_y = y;
        return 1;
    }
    return 0;
}

/* 手指刚离开也保持一小段"活跃"，避免刚抬手就去做重活 */
int tp_lvgl_active_recently(void)
{
    if (s_pressed) {
        return 1;
    }
    if (s_last_press_ms && (lv_tick_get() - s_last_press_ms) < 600) {
        return 1;
    }
    return 0;
}

void tp_lvgl_set_hook(tp_lvgl_hook_t h)
{
    s_hook = h;
}

void tp_lvgl_get_state(uint16_t *x, uint16_t *y, int *pressed, uint32_t *taps)
{
    if (x) { *x = s_last_x; }
    if (y) { *y = s_last_y; }
    if (pressed) { *pressed = s_pressed ? 1 : 0; }
    if (taps) { *taps = s_taps; }
}

void tp_lvgl_init(void)
{
    lv_indev_t *indev;

    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, tp_read_cb);
    lv_indev_set_display(indev, lv_display_get_default());
    lv_timer_set_period(lv_indev_get_read_timer(indev), 5);

    printf("[tp] indev ready (poll 5ms, release=%dms)\r\n", TP_RELEASE_MS);
}
