/* Exercise the real launcher, device pages and gesture router with host I/O. */
#include "lvgl.h"
#include "eez_ui_glue.h"
#include "app_list.h"
#include "app_notify.h"
#include "app_pages.h"
#include "svc_alarm.h"
#include "svc_stopwatch.h"
#include "svc_timer.h"
#include "ui_widgets.h"
#include "misc/lv_text_private.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t pixels[480 * 480], drawbuf[480 * 64];
static int down, touch_x, touch_y;
static lv_obj_t *face;
static uint8_t brightness = 153;
static uint32_t idle_ms = 15000;
static int mode = 2, persist_count, mode_persist_count;
const uint8_t power_brightness_levels[] = {64, 153, 255};
const uint32_t power_idle_levels[] = {15000, 30000, 60000};

uint8_t power_get_brightness(void) { return brightness; }
void power_set_brightness(uint8_t value) { brightness = value; }
uint32_t power_get_idle_ms(void) { return idle_ms; }
void power_set_idle_ms(uint32_t value) { idle_ms = value; }
void power_request_persist(void) { persist_count++; }
int svc_ble_power_mode_get(void) { return mode; }
int svc_ble_power_mode_set(int value) { mode = value; mode_persist_count++; return 0; }
int pc_monitor_usb_ready(void) { return 0; }
int svc_pc_local_input_blocked(void) { return 0; }   /* host 测试里没有电脑模式 */
int ble_link_is_connected(void) { return 0; }
void *face_lvgl_page(void) { return face; }
lv_obj_t *pc_monitor_create(lv_obj_t *parent) { return ui_page_create(parent); }
int app_launch_page(int page)
{
    if (page == 3) eez_ui_refresh_settings();
    eez_ui_goto_raw(page);
    app_page_open(page);        /* 与 apps/pages.c 的 load 回调一致 */
    return 0;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    uint16_t *src = (uint16_t *)data;
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < 480 && area->y2 < 480);
    for (int y = area->y1; y <= area->y2; y++)
        for (int x = area->x1; x <= area->x2; x++) pixels[y * 480 + x] = *src++;
    lv_display_flush_ready(display);
}

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = touch_x;
    data->point.y = touch_y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void step(int ms)
{
    for (int i = 0; i < ms; i += 5) {
        lv_tick_inc(5);
        eez_ui_tick();
        app_notify_ui_tick(lv_tick_get());   /* main.c 主循环同款调用 */
        lv_timer_handler();
    }
}

static void press(int x, int y)
{
    touch_x = x; touch_y = y; down = 1; step(45);
}

static void release(void) { down = 0; step(350); }
static void tap(int x, int y) { press(x, y); release(); }
static void swipe(int x, int y, int to_x, int to_y)
{
    press(x, y); touch_x = to_x; touch_y = to_y; step(50); release();
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0)
        return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *match = find_label(lv_obj_get_child(obj, i), text);
        if (match) return match;
    }
    return NULL;
}

static void check_labels(lv_obj_t *obj)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *text = lv_label_get_text(obj);
        const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
        for (uint32_t i = 0; text[i];) {
            uint32_t cp = lv_text_encoded_next(text, &i);
            lv_font_glyph_dsc_t glyph;
            if (!lv_font_get_glyph_dsc(font, &glyph, cp, 0)) {
                fprintf(stderr, "missing glyph U+%04X in %s\n", (unsigned)cp, text);
                assert(0);
            }
        }
        lv_area_t area; lv_obj_get_coords(obj, &area);
        /* Text boxes, including their alignment padding, stay inside the safe circle. */
        int xs[] = {area.x1, area.x2}, ys[] = {area.y1, area.y2};
        for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) {
            int dx = xs[i] - 240, dy = ys[j] - 240;
            if (dx * dx + dy * dy > 207 * 207) {
                fprintf(stderr, "text bounds (%d,%d)-(%d,%d): %s\n",
                        (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2, text);
                assert(0);
            }
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++)
        check_labels(lv_obj_get_child(obj, i));
}

static void save(const char *name)
{
    lv_refr_now(NULL);
    lv_obj_t *visible = app_list_visible() ? lv_obj_get_child(lv_screen_active(), -1)
                                         : eez_ui_page_obj(eez_ui_page());
    check_labels(visible);
    char file[128]; snprintf(file, sizeof(file), "%s.ppm", name);
    FILE *out = fopen(file, "wb"); assert(out);
    fprintf(out, "P6\n480 480\n255\n");
    for (int i = 0; i < 480 * 480; i++) {
        uint16_t p = pixels[i];
        unsigned char rgb[] = {((p >> 11) & 31) * 255 / 31,
                               ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
}

/* 2026-11-08：通知卡（sys 层）圆屏适配检查。
 * 卡片本身没有对外句柄，按"尺寸 368x136 + 可见"在 sys 层里找。 */
static lv_obj_t *find_notify_card(void)
{
    lv_obj_t *sys = lv_layer_sys();
    for (unsigned i = 0; i < lv_obj_get_child_count(sys); i++) {
        lv_obj_t *o = lv_obj_get_child(sys, i);
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_get_width(o) == 368 && lv_obj_get_height(o) == 136) return o;
    }
    return NULL;
}

static void check_notify_card(void)
{
    lv_obj_t *card = find_notify_card();
    lv_area_t a;

    assert(card != NULL);
    lv_obj_get_coords(card, &a);
    assert(lv_obj_get_width(card) == 368 && lv_obj_get_height(card) == 136);
    /* 居中：中心 = (240,240) */
    assert((a.x1 + a.x2 + 1) / 2 == 240 && (a.y1 + a.y2 + 1) / 2 == 240);
    /* 四角都在安全半径 207 内 */
    {
        int xs[] = {a.x1, a.x2}, ys[] = {a.y1, a.y2};
        for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) {
            int dx = xs[i] - 240, dy = ys[j] - 240;
            assert(dx * dx + dy * dy <= 207 * 207);
        }
    }
    check_labels(card);          /* 标题/正文的字形与圆内边界 */
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    lv_init();
    lv_display_t *display = lv_display_create(480, 480);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, drawbuf, NULL, sizeof(drawbuf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_set_style_pad_all(lv_screen_active(), 0, 0);
    face = ui_page_create(lv_screen_active());
    /* 2026-11-08：App 页的服务状态先初始化（持久化/arming 回调在 host 上为 NULL） */
    svc_stopwatch_init();
    svc_timer_init();
    svc_alarm_init(7, 30, 0);
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    eez_ui_init();
    app_notify_init();
    app_notify_ui_init();
    step(2000);

    swipe(350, 240, 150, 240);
    assert(app_list_visible()); save("apps");
    /* A swipe that opens the list must not launch its first item on release. */
    assert(eez_ui_page() == 0);
    tap(240, 160);
    assert(!app_list_visible() && eez_ui_page() == 3); save("settings");
    tap(240, 160); save("brightness");
    tap(240, 160); assert(brightness == 64 && persist_count == 1);
    /* Large excursion and return must cancel a pending choice. */
    press(240, 250); touch_x = 240; touch_y = 300; step(50);
    touch_y = 250; release(); assert(brightness == 64 && persist_count == 1);
    tap(148, 88); assert(find_label(eez_ui_page_obj(3), "终端设置"));
    tap(240, 256); save("timeout");
    tap(240, 352); assert(idle_ms == 60000 && persist_count == 2);
    swipe(130, 240, 330, 240);
    assert(!app_list_visible() && find_label(eez_ui_page_obj(3), "终端设置"));
    tap(240, 352); save("connection");
    tap(240, 256); assert(mode == 1 && mode_persist_count == 1);
    /* Remote changes are reflected without re-entering the screen. */
    mode = 0; brightness = 255; step(30);
    lv_obj_t *selected = find_label(eez_ui_page_obj(3), "省电"); assert(selected);
    assert(lv_color_eq(lv_obj_get_style_text_color(selected, 0), lv_color_hex(UI_COLOR_ACCENT)));
    swipe(130, 240, 330, 240);
    swipe(130, 240, 330, 240);
    assert(app_list_visible()); step(500); assert(app_list_visible());
    /* 2026-11-15：协议网络 / 相遇记录 占位隐藏（页面还在，但列表里不出现）。
     * 注意只看列容器：其余页容器只是被挪到屏幕外，find_label(screen) 仍能找到。 */
    lv_obj_t *list = lv_obj_get_child(lv_screen_active(), -1);
    assert(!find_label(list, "协议网络"));
    assert(!find_label(list, "相遇记录"));
    assert(find_label(list, "秒表"));
    assert(find_label(list, "计时器"));
    assert(!find_label(list, "闹钟"));   /* 闹钟在第 2 页 */
    save("apps");
    /* 隐藏页仍能直接渲染（回归截图；入口不可达） */
    app_list_hide();
    eez_ui_goto_raw(1); save("network");
    eez_ui_goto_raw(2); save("encounters");
    eez_ui_goto_raw(0);
    app_list_show(); step(500);
    /* ---- 2026-11-08：第二批 App（秒表 / 计时器 / 闹钟）---- */
    tap(240, 256); assert(!app_list_visible() && eez_ui_page() == 5);
    save("stopwatch");
    assert(svc_stopwatch_state() == SVC_STOPWATCH_IDLE);
    tap(240, 256);                                   /* 开始 */
    assert(svc_stopwatch_state() == SVC_STOPWATCH_RUNNING);
    step(1200);
    assert(svc_stopwatch_elapsed_ms() >= 1000u);
    assert(find_label(eez_ui_page_obj(5), "暂停"));
    save("stopwatch_run");
    tap(240, 256);                                   /* 暂停 */
    assert(svc_stopwatch_state() == SVC_STOPWATCH_PAUSED);
    {
        uint32_t held = svc_stopwatch_elapsed_ms();
        step(500);
        assert(svc_stopwatch_elapsed_ms() == held);  /* 暂停后不再走 */
        assert(find_label(eez_ui_page_obj(5), "继续"));
    }
    /* 离开页面再回来：状态还在、计时继续（2026-11-08 用户要求） */
    tap(240, 256);                                   /* 继续 */
    assert(svc_stopwatch_state() == SVC_STOPWATCH_RUNNING);
    {
        uint32_t before = svc_stopwatch_elapsed_ms();
        swipe(130, 240, 330, 240); assert(app_list_visible());
        step(700);                                   /* 在列表里待 0.7 s */
        tap(240, 256); assert(eez_ui_page() == 5);
        assert(svc_stopwatch_state() == SVC_STOPWATCH_RUNNING);
        assert(svc_stopwatch_elapsed_ms() > before);
        assert(find_label(eez_ui_page_obj(5), "暂停"));
    }
    tap(240, 352);                                   /* 复位 */
    assert(svc_stopwatch_state() == SVC_STOPWATCH_IDLE);
    assert(svc_stopwatch_elapsed_ms() == 0u);
    /* 右滑回列表 → 计时器（列表里是第 3 行：设置 / 秒表 / 计时器） */
    swipe(130, 240, 330, 240); assert(app_list_visible());
    tap(240, 352); assert(eez_ui_page() == 6); save("timer");
    assert(svc_timer_preset_min() == 5u);
    tap(240, 216);                                   /* 预设值：5 → 下一档 */
    assert(svc_timer_preset_min() == 10u);
    tap(144, 216); assert(svc_timer_preset_min() == 9u);   /* - */
    tap(336, 216); assert(svc_timer_preset_min() == 10u);  /* + */
    tap(240, 300);                                   /* 开始 */
    assert(svc_timer_state() == SVC_TIMER_RUNNING);
    step(1500);
    assert(svc_timer_remaining_ms() < 10u * 60000u);
    assert(svc_timer_remaining_ms() > 9u * 60000u);
    save("timer_run");
    /* 到点：把 now 往后拨 11 分钟，服务报一次过期（main.c 的 tick 等价路径） */
    {
        uint32_t far = lv_tick_get() + 11u * 60000u;
        assert(svc_timer_tick(far) == 1);
        assert(svc_timer_state() == SVC_TIMER_DONE);
        assert(svc_timer_tick(far) == 0);            /* 只报一次 */
    }
    tap(240, 372);                                   /* 取消 */
    assert(svc_timer_state() == SVC_TIMER_IDLE);
    assert(svc_timer_remaining_ms() == 10u * 60000u);
    /* 右滑回列表 → 翻到第 2 页 → 闹钟 */
    swipe(130, 240, 330, 240); assert(app_list_visible());
    swipe(240, 340, 240, 180); save("apps_page2");
    tap(240, 160); assert(eez_ui_page() == 7); save("alarm");
    assert(!svc_alarm_enabled());
    assert(svc_alarm_hour() == 7u && svc_alarm_minute() == 30u);
    tap(348, 212); assert(svc_alarm_hour() == 8u);   /* 小时 + */
    tap(140, 212); assert(svc_alarm_hour() == 7u);   /* 小时 - */
    tap(348, 284); assert(svc_alarm_minute() == 31u);/* 分钟 + */
    tap(140, 284); assert(svc_alarm_minute() == 30u);/* 分钟 - */
    tap(240, 364);                                   /* 每天重复开关 */
    assert(svc_alarm_enabled());
    /* 2026-11-08 18:42 UI 文案改成"闹钟开关 · 已开启"（Lead/其它子代理改的） */
    assert(find_label(eez_ui_page_obj(7), "闹钟开关 · 已开启"));
    save("alarm_on");
    /* 回列表 → 回表盘 */
    swipe(130, 240, 330, 240); assert(app_list_visible());
    tap(148, 88); assert(!app_list_visible() && eez_ui_page() == 0);

    /* ---- 2026-11-08：通知卡圆形屏适配（用户反馈"矩形卡出界"）----
     * 正文故意用 >24 个字（都是固件里已有的字），验证 LONG_WRAP 会折行。 */
    app_notify_show("计时器", "倒计时结束，点按展开，点按关闭，倒计时结束，时间到");
    step(120);
    check_notify_card();
    assert(app_notify_visible());
    assert(find_label(lv_layer_sys(), "计时器"));
    assert(find_label(lv_layer_sys(),
                      "倒计时结束，点按展开，点按关闭，倒计时结束，时间到"));
    {
        /* 正文必须是折行（LONG_WRAP），不是单行省略 */
        lv_obj_t *body = find_label(lv_layer_sys(),
                                    "倒计时结束，点按展开，点按关闭，倒计时结束，时间到");
        assert(body != NULL);
        assert(lv_label_get_long_mode(body) == LV_LABEL_LONG_WRAP);
    }
    save("notify");
    /* 3 s 自动消失：队列里的第二条接上（真实闹钟文案） */
    app_notify_show("闹钟", "时间到");
    step(3200);
    assert(app_notify_visible());
    assert(find_label(lv_layer_sys(), "闹钟"));
    check_notify_card();
    save("notify_alarm");
    /* 点按卡片关闭：不应触发底层页面的手势/点击 */
    tap(240, 240);
    assert(!app_notify_visible());
    assert(find_notify_card() == NULL);
    assert(eez_ui_page() == 0);          /* 表盘没被这一下点走 */

    printf("[device-ui] native pages, settings, cancel, paging, stopwatch, timer, "
           "alarm, round notification card and return paths passed\n");
    return 0;
}
