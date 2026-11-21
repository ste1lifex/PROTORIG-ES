/*
 * device_pages.c —— 原生设备页布局：终端设置（page 3）+ 协议网络/相遇记录占位（page 1/2）
 *
 * 设置页两层：总览（亮度/息屏/连接模式）→ 子页（三档选项，ui_row_set 标选中）。
 * 协议网络/相遇记录只是静态占位布局——协议 v1 没接入，App 列表里 hidden=1，
 * 别把这两页当成已实现的功能；改文案/布局时同步 ui/app_pages.c 的圆屏安全区检查
 * （tests：python tools/device_ui_host.py）。
 */
#include <stdio.h>
#include "device_pages.h"
#include "ui_widgets.h"
#include "board_power.h"
#include "svc_ble.h"

static ui_row_t settings_rows[3];
static lv_obj_t *settings_title;
static int settings_section;
static int shown_brightness, shown_mode;
static uint32_t shown_idle;

static const char *mode_names[] = {"省电", "常连", "智能"};
static const char *section_names[] = {"终端设置", "屏幕亮度", "自动息屏", "连接模式"};
static const ui_icon_t section_icons[] = {UI_ICON_SUN, UI_ICON_TIMER, UI_ICON_LINK};

static void settings_refresh(void)
{
    int brightness = power_get_brightness();
    uint32_t idle = power_get_idle_ms();
    int mode = svc_ble_power_mode_get();
    char value[48];
    lv_label_set_text(settings_title, section_names[settings_section]);
    for (int i = 0; i < 3; i++)
        ui_icon_set_kind(settings_rows[i].icon, section_icons[settings_section ? settings_section - 1 : i]);
    if (settings_section == 0) {
        snprintf(value, sizeof(value), "%d%%", (brightness * 100 + 127) / 255);
        ui_row_set(&settings_rows[0], "屏幕亮度", value, 1, -1);
        snprintf(value, sizeof(value), "%lu 秒", (unsigned long)(idle / 1000));
        ui_row_set(&settings_rows[1], "自动息屏", value, 1, -1);
        ui_row_set(&settings_rows[2], "连接模式", mode >= 0 && mode < 3 ? mode_names[mode] : "未知", 1, -1);
    } else {
        const char *brightness_names[] = {"25%", "60%", "100%"};
        const char *brightness_hints[] = {"低亮度", "日常使用", "强光环境"};
        const char *idle_names[] = {"15 秒", "30 秒", "60 秒"};
        const char *idle_hints[] = {"较快息屏", "适中等待", "延长显示"};
        const char *mode_hints[] = {"优先降低待机功耗", "保持蓝牙可连接", "平衡连接与待机"};
        for (int i = 0; i < 3; i++) {
            if (settings_section == 1) {
                ui_row_set(&settings_rows[i], brightness_names[i], brightness_hints[i], 1,
                           brightness == power_brightness_levels[i]);
            } else if (settings_section == 2) {
                ui_row_set(&settings_rows[i], idle_names[i], idle_hints[i], 1,
                           idle == power_idle_levels[i]);
            } else {
                ui_row_set(&settings_rows[i], mode_names[i], mode_hints[i], 1, mode == i);
            }
        }
    }
    shown_brightness = brightness;
    shown_idle = idle;
    shown_mode = mode;
}

void device_settings_open(void)
{
    settings_section = 0;
    settings_refresh();
}

void device_settings_tick(void)
{
    if (shown_brightness != power_get_brightness() || shown_idle != power_get_idle_ms() ||
        shown_mode != svc_ble_power_mode_get()) settings_refresh();
}

int device_settings_back(void)
{
    if (!settings_section) return 0;
    settings_section = 0;
    settings_refresh();
    return 1;
}

int device_settings_target(int x, int y)
{
    for (int i = 0; i < 3; i++) {
        if (ui_rect_hit(&ui_row_rects[i], x, y)) return 10 + i;
    }
    return 0;
}

void device_settings_press(int target, int pressed)
{
    if (target >= 10 && target < 13) ui_row_press(&settings_rows[target - 10], pressed);
}

void device_settings_activate(int target)
{
    if (target < 10 || target >= 13) return;
    int index = target - 10;
    if (settings_section == 0) {
        settings_section = index + 1;
    } else if (settings_section == 1) {
        power_set_brightness(power_brightness_levels[index]);
        power_request_persist();
    } else if (settings_section == 2) {
        power_set_idle_ms(power_idle_levels[index]);
        power_request_persist();
    } else {
        (void)svc_ble_power_mode_set(index);
    }
    settings_refresh();
}

lv_obj_t *device_page_create(lv_obj_t *parent, int page)
{
    lv_obj_t *root = ui_page_create(parent);
    if (page == 3) {
        settings_title = ui_page_header(root, section_names[0]);
        for (int i = 0; i < 3; i++) ui_row_create(&settings_rows[i], root, i, section_icons[i]);
        device_settings_open();
    } else if (page == 1) {
        ui_page_header(root, "协议网络");
        ui_icon_create(root, UI_ICON_LINK, 204, 154, 72, UI_COLOR_ACCENT);
        ui_text(root, "链路待启用", UI_FONT_HEAD, UI_COLOR_TEXT, 100, 256, 280, LV_TEXT_ALIGN_CENTER);
        ui_text(root, "无线相遇功能尚未接入", UI_FONT_TITLE, UI_COLOR_MUTED, 88, 308, 304, LV_TEXT_ALIGN_CENTER);
        ui_text(root, "附近的容器将在这里出现", UI_FONT_BODY, UI_COLOR_MUTED, 120, 345, 240, LV_TEXT_ALIGN_CENTER);
    } else {
        ui_page_header(root, "相遇记录");
        ui_text(root, "00", UI_FONT_HUGE, UI_COLOR_TEXT, 140, 154, 200, LV_TEXT_ALIGN_CENTER);
        ui_text(root, "相遇次数", UI_FONT_BODY, UI_COLOR_MUTED, 140, 217, 200, LV_TEXT_ALIGN_CENTER);
        ui_text(root, "等待第一份相遇记录", UI_FONT_TITLE, UI_COLOR_TEXT, 100, 268, 280, LV_TEXT_ALIGN_CENTER);
        ui_text(root, "独一无二的地形", UI_FONT_TITLE, UI_COLOR_ACCENT, 100, 315, 280, LV_TEXT_ALIGN_CENTER);
        ui_text(root, "每一次相遇，留下新的轮廓", UI_FONT_BODY, UI_COLOR_MUTED, 112, 351, 256, LV_TEXT_ALIGN_CENTER);
    }
    return root;
}
