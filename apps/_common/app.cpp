/*
 * SPDX-License-Identifier: MIT
 *
 * cdk app start / stop, the quit request and the "needs WxH" notice (see app.hpp).
 */
#include "app.hpp"

#include "async_run.hpp"
#include "build_info.hpp"
#include "osk_policy.hpp"

#include "cp0_ui_metrics_lvgl.h"
#include "cp0_ui_widgets.hpp"
#include "keyboard_input.h"

#include <atomic>
#include <cstdio>

namespace cdk {
namespace {

std::atomic<bool> g_quit{false};
std::atomic<int> g_quit_code{0};
std::shared_ptr<void> g_app;
lv_group_t *g_group = nullptr;
lv_obj_t *g_notice_screen = nullptr;
int g_w = 0, g_h = 0;

/* Every keypad (and encoder) indev drives the app's group; a keypad created later is not expected (cp0_lvgl creates
 * its one keypad in init_input, the harness in its keyboard set-up). */
void attach_keypads(lv_group_t *group)
{
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        const lv_indev_type_t type = lv_indev_get_type(indev);
        if (type == LV_INDEV_TYPE_KEYPAD || type == LV_INDEV_TYPE_ENCODER) lv_indev_set_group(indev, group);
    }
}

void notice_key_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_KEY) {
        request_quit(0);
        return;
    }
    if (LV_EVENT_KEYBOARD != 0 && lv_event_get_code(event) == static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD)) {
        const auto *item = static_cast<const struct key_item *>(lv_event_get_param(event));
        if (item && item->key_state == KBD_KEY_PRESSED) request_quit(0);
    }
}

void notice_click_cb(lv_event_t *) { request_quit(0); }

/* The notice: one centred text in the BODY size, the whole screen a tap target. Only tokens, no geometry. */
void show_notice(const AppSpec &spec, lv_obj_t *screen, int w, int h)
{
    g_notice_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *area = lv_obj_create(screen);
    lv_obj_remove_style_all(area);
    lv_obj_set_size(area, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(area, notice_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(area, notice_key_cb, LV_EVENT_KEY, nullptr);
    lv_group_add_obj(g_group, area);
    lv_group_focus_obj(area);
    if (LV_EVENT_KEYBOARD != 0)
        lv_obj_add_event_cb(screen, notice_key_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), nullptr);

    const int margin = cp0_ui_tok(CP0_TOK_SPACE_L);
    lv_obj_t *label = lv_label_create(area);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, w - 2 * margin > 0 ? w - 2 * margin : w);
    lv_obj_set_style_text_font(label, cp0_ui_font(CP0_TEXT_BODY), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, min_size_notice(spec, w, h).c_str());
    lv_obj_center(label);
    std::fprintf(stderr, "[cdk] %s: screen %dx%d is under %dx%d: notice shown\n", spec.id, w, h, spec.min_w,
                 spec.min_h);
}

} // namespace

bool screen_fits(const AppSpec &spec, int w, int h)
{
    return w >= spec.min_w && h >= spec.min_h;
}

std::string min_size_notice(const AppSpec &spec, int w, int h)
{
    const std::string title = spec.title && spec.title[0] ? spec.title : "This app";
    return title + " needs a screen of at least " + std::to_string(spec.min_w) + "x" + std::to_string(spec.min_h) +
           ".\nThis screen is " + std::to_string(w) + "x" + std::to_string(h) +
           ".\n\nPress any key or tap the screen to close.";
}

std::string version_text(const AppSpec &spec)
{
    std::string text = std::string(spec.title) + " (" + spec.id + ") " + CDK_APP_VERSION + "\n";
    text += std::string("launcher ") + CDK_LAUNCHER_REF + " (cp0_lvgl " + CDK_LAUNCHER_PIN + "), " + CDK_BUILD_TARGET +
            " build " + CDK_BUILD_DATE + "\n";
    text += "needs a screen of at least " + std::to_string(spec.min_w) + "x" + std::to_string(spec.min_h) + "\n";
    return text;
}

void request_quit(int code)
{
    g_quit_code.store(code);
    g_quit.store(true);
}

bool quit_requested() { return g_quit.load(); }
int quit_code() { return g_quit_code.load(); }
lv_group_t *input_group() { return g_group; }
int screen_w() { return g_w; }
int screen_h() { return g_h; }

bool start(const AppSpec &spec, lv_obj_t *screen)
{
    if (!screen) return false;
    lv_display_t *display = lv_obj_get_display(screen);
    g_w = display ? static_cast<int>(lv_display_get_horizontal_resolution(display)) : 0;
    g_h = display ? static_cast<int>(lv_display_get_vertical_resolution(display)) : 0;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    if (!g_group) g_group = lv_group_create();
    attach_keypads(g_group);

    if (!screen_fits(spec, g_w, g_h)) {
        show_notice(spec, screen, g_w, g_h);
        return false;
    }
    osk_policy::install();
    /* under the LVGL lock: async_run workers post with lv_async_call from their threads (async_run.hpp) */
    lv_lock();
    if (spec.build) g_app = spec.build(screen);
    lv_unlock();
    return g_app != nullptr;
}

void stop()
{
    async_run::shutdown(); /* outside the LVGL lock: a worker may be waiting for it to post its last lines */
    lv_lock();
    cp0::ui::osk::dismiss();
    g_app.reset();
    if (g_notice_screen) {
        lv_obj_clean(g_notice_screen);
        g_notice_screen = nullptr;
    }
    if (g_group) {
        for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev))
            if (lv_indev_get_group(indev) == g_group) lv_indev_set_group(indev, nullptr);
        lv_group_delete(g_group);
        g_group = nullptr;
    }
    lv_unlock();
}

} // namespace cdk
