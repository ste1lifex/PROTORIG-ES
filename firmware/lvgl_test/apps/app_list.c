/* Circular launcher. Touch geometry is shared with the native device pages. */
#include <stdio.h>
#include "lvgl.h"
#include "ui_widgets.h"
#include "app_framework.h"
#include "app_pages.h"
#include "eez_ui_glue.h"
#include "app_list.h"

#define LIST_W 480
#define ITEMS_PER_PAGE 3
#define LIST_ARM_MS 200

typedef struct {
    const char *name, *sub, *app_id;
    int page, enabled;
    /* 2026-11-15：还没做完的入口先"占位但不上架"。
     * 协议网络 / 相遇记录 的页面、注册、布局都在（apps/pages.c、ui/device_pages.c），
     * 只是这里 hidden=1 不进列表 —— 发布前不发半成品，接好协议 v1 后把 1 改回 0 即可。 */
    int hidden;
    ui_icon_t icon;
} list_item_t;

/* 2026-11-08 用户定：第一批 APP = 秒表 / 计时器 / 闹钟（页号见 app_pages.h） */
static const list_item_t s_items[] = {
    {"设置", "显示 · 息屏 · 连接", "settings", 3, 1, 0, UI_ICON_SETTINGS},
    {"协议网络", "服务与连接状态", "network", 1, 1, 1, UI_ICON_LINK},
    {"相遇记录", "查看网络相遇记录", "meets", 2, 1, 1, UI_ICON_ARCHIVE},
    {"秒表", "计时 · 暂停 · 复位", "stopwatch", APP_PAGE_STOPWATCH, 1, 0, UI_ICON_STOPWATCH},
    {"计时器", "倒计时提醒", "timer", APP_PAGE_TIMER, 1, 0, UI_ICON_TIMER},
    {"闹钟", "每天重复", "alarm", APP_PAGE_ALARM, 1, 0, UI_ICON_ALARM},
};
#define ITEM_N ((int)(sizeof(s_items) / sizeof(s_items[0])))

/* 可见项索引表：列表的翻页 / 点击 / 启动全部按这张表算，
 * 这样 hidden 只影响"显示顺序"，不会在页面里留下空洞。 */
static int s_vis[ITEM_N];
static int s_vis_n;
static int s_page_max = 1;

static void build_visible(void)
{
    s_vis_n = 0;
    for (int i = 0; i < ITEM_N; i++) {
        if (!s_items[i].hidden) s_vis[s_vis_n++] = i;
    }
    s_page_max = (s_vis_n + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    if (s_page_max < 1) s_page_max = 1;
}

/* 显示槽位（第几页第几行）→ s_items 下标；无此槽位返回 -1 */
static int slot_item(int slot)
{
    if (slot < 0 || slot >= s_vis_n) return -1;
    return s_vis[slot];
}

static lv_obj_t *s_list, *s_page_lbl, *s_prev, *s_next;
static ui_row_t s_row[ITEMS_PER_PAGE];
static const ui_rect_t s_prev_rect = {168, 400, 48, 32};
static const ui_rect_t s_next_rect = {264, 400, 48, 32};
static int s_inited, s_visible, s_page_idx, s_press_target, s_swipe_used, s_return_armed;
/* Only a new stroke after the list has appeared may launch an app. */
static uint32_t s_show_ms;
static int s_show_epoch, s_press_epoch;

static void list_clear_press(void)
{
    for (int i = 0; i < ITEMS_PER_PAGE; i++) ui_row_press(&s_row[i], 0);
}

static void list_refresh(void)
{
    for (int i = 0; i < ITEMS_PER_PAGE; i++) {
        int item = slot_item(s_page_idx * ITEMS_PER_PAGE + i);
        lv_obj_set_flag(s_row[i].root, LV_OBJ_FLAG_HIDDEN, item < 0);
        if (item < 0) continue;
        const list_item_t *it = &s_items[item];
        ui_icon_set_kind(s_row[i].icon, it->icon);
        ui_row_set(&s_row[i], it->name, it->sub, it->enabled, -1);
    }
    lv_label_set_text_fmt(s_page_lbl, "%d / %d", s_page_idx + 1, s_page_max);
    lv_obj_set_style_text_color(s_prev, lv_color_hex(s_page_idx > 0 ? UI_COLOR_TEXT : UI_COLOR_LINE), 0);
    lv_obj_set_style_text_color(s_next, lv_color_hex(s_page_idx + 1 < s_page_max ? UI_COLOR_TEXT : UI_COLOR_LINE), 0);
}

void app_list_init(void)
{
    if (s_inited) return;
    s_inited = 1;
    s_list = ui_page_create(lv_screen_active());
    lv_obj_set_pos(s_list, LIST_W, 0);
    ui_page_header(s_list, "应用");
    for (int i = 0; i < ITEMS_PER_PAGE; i++) ui_row_create(&s_row[i], s_list, i, UI_ICON_SETTINGS);
    s_prev = ui_icon_create(s_list, UI_ICON_UP, 180, 404, 24, UI_COLOR_TEXT);
    s_next = ui_icon_create(s_list, UI_ICON_DOWN, 276, 404, 24, UI_COLOR_TEXT);
    s_page_lbl = ui_text(s_list, "", UI_FONT_BODY, UI_COLOR_MUTED, 216, 410, 48, LV_TEXT_ALIGN_CENTER);
    build_visible();          /* 先按 hidden 建可见索引，再画第一页 */
    list_refresh();
}

int app_list_visible(void) { return s_visible; }
int app_list_return_armed(void) { return s_return_armed; }

void app_list_show(void)
{
    if (!s_inited) app_list_init();
    if (s_visible) return;
    s_visible = 1;
    s_show_ms = lv_tick_get();
    s_show_epoch = eez_ui_touch_epoch();
    s_press_target = 0;
    s_press_epoch = 0;
    s_swipe_used = 0;
    printf("[app_list] show page=%d armed=%d epoch=%d\r\n",
           s_page_idx, s_return_armed, s_show_epoch);
    lv_obj_set_pos(s_list, 0, 0);
    lv_obj_move_foreground(s_list);
    list_refresh();
    lv_obj_invalidate(s_list);
}

void app_list_hide(void)
{
    if (!s_visible) return;
    s_visible = 0;
    list_clear_press();
    printf("[app_list] hide page=%d armed=%d\r\n", s_page_idx, s_return_armed);
    lv_obj_set_pos(s_list, LIST_W, 0);
}

static void close_to_face(void)
{
    app_list_hide();
    s_return_armed = 0;
    app_launch_page(0);
}

static void launch_index(int idx, int base)
{
    int slot = base + idx;
    int i = slot_item(slot);
    if (i < 0 || !s_items[i].enabled || s_items[i].app_id == NULL || s_items[i].page < 0) {
        return;     /* 空槽位/占位项不启动，不产生空入口 */
    }
    printf("[app_list] launch %s page=%d\r\n", s_items[i].app_id, s_items[i].page);
    app_list_hide();
    s_return_armed = 1;
    app_launch_page(s_items[i].page);
}

void app_list_press(int x, int y)
{
    if (!s_visible) return;
    s_swipe_used = 0;
    s_press_target = 0;
    s_press_epoch = eez_ui_touch_epoch();

    if (ui_rect_hit(&ui_back_rect, x, y)) {
        s_press_target = 100;
        return;
    }
    if (ui_rect_hit(&s_prev_rect, x, y)) {
        s_press_target = 101;
        return;
    }
    if (ui_rect_hit(&s_next_rect, x, y)) {
        s_press_target = 102;
        return;
    }
    for (int i = 0; i < ITEMS_PER_PAGE; i++) {
        int item = slot_item(s_page_idx * ITEMS_PER_PAGE + i);
        if (item >= 0 && s_items[item].enabled && ui_rect_hit(&ui_row_rects[i], x, y)) {
            s_press_target = i + 1;
            ui_row_press(&s_row[i], 1);
            return;
        }
    }
    printf("[app_list] press (%d,%d) -> none\r\n", x, y);
}

/* P0-1：丢触点/被吞掉的触摸只取消候选，不产生点击 */
void app_list_cancel(void)
{
    list_clear_press();
    if (s_press_target != 0) {
        printf("[app_list] cancel target=%d\r\n", s_press_target);
    }
    s_press_target = 0;
    s_swipe_used = 1;
}

void app_list_swipe(int dx, int dy)
{
    if (!s_visible || s_swipe_used) return;
    /* 和 app_list_release 的第二道闸门一致：列表刚出现这一小段时间里不接受
     * 滑动。开列表那一笔被芯片断点切成"假抬手 + 新按下"之后，手指继续走的
     * 那一段否则会被当成列表右滑，把列表关掉、再顺势滑到下一屏去。 */
    if ((uint32_t)(lv_tick_get() - s_show_ms) < LIST_ARM_MS) {
        return;
    }
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    if (adx >= 42 && adx * 10 > ady * 15) {
        list_clear_press();
        if (dx > 0) {
            s_swipe_used = 1;
            close_to_face();
        } else {
            s_swipe_used = 1;
        }
        return;
    }
    if (ady >= 52 && ady * 10 > adx * 15) {
        list_clear_press();
        s_swipe_used = 1;
        if (dy < 0 && s_page_idx < s_page_max - 1) {
            s_page_idx++;
            list_refresh();
            lv_obj_invalidate(s_list);
        } else if (dy > 0 && s_page_idx > 0) {
            s_page_idx--;
            list_refresh();
            lv_obj_invalidate(s_list);
        }
    }
}

void app_list_release(int dx, int dy, uint32_t press_ms)
{
    list_clear_press();
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    if (!s_visible || s_swipe_used) {
        return;
    }
    if (adx > 24 || ady > 24 || press_ms > 650) {
        return;
    }
    int t = s_press_target;
    s_press_target = 0;

    /* 闸门 1：这一笔按下必须晚于"列表被打开"那一笔 */
    if (s_press_epoch != 0 && s_press_epoch <= s_show_epoch) {
        printf("[app_list] release target=%d ignored (same gesture as show: %d<=%d)\r\n",
               t, s_press_epoch, s_show_epoch);
        return;
    }
    /* 闸门 2：列表刚出现的一瞬间不接受点击（手指分裂/回弹） */
    if ((uint32_t)(lv_tick_get() - s_show_ms) < LIST_ARM_MS) {
        printf("[app_list] release target=%d ignored (list young: %lums)\r\n",
               t, (unsigned long)(lv_tick_get() - s_show_ms));
        return;
    }
    if (t == 100) {
        close_to_face();
    } else if (t == 101) {
        if (s_page_idx > 0) { s_page_idx--; list_refresh(); lv_obj_invalidate(s_list); }
    } else if (t == 102) {
        if (s_page_idx < s_page_max - 1) { s_page_idx++; list_refresh(); lv_obj_invalidate(s_list); }
    } else if (t >= 1 && t <= ITEMS_PER_PAGE) {
        launch_index(t - 1, s_page_idx * ITEMS_PER_PAGE);
    }
}
