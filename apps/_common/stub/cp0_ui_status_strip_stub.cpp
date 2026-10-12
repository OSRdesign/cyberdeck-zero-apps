/*
 * SPDX-License-Identifier: MIT
 *
 * STAND-IN for cp0::ui::StatusStrip until task L1 moves the launcher's strip into cp0_lvgl (see
 * cp0_ui_status_strip_stub.hpp next to this file). Written against cp0_lvgl's public pieces only: the strip renderer
 * (cp0_statusbar.h, the same code the home grid and viz1090 draw with) and the layout service's shell numbers
 * (cp0_ui_metrics.h: status_w, status_top, status_pct, bar_h, title_x/y/px). The state comes from sysfs / procfs
 * (cp0_statusbar_read_state), which an app can read without the launcher's services.
 *
 * Compiled out as soon as cp0_lvgl provides the real header.
 */
#if !__has_include(<cp0_ui_status_strip.hpp>)

#include "cp0_ui_status_strip_stub.hpp"

#include "cp0_lvgl_app.h"
#include "cp0_statusbar.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <vector>

namespace cp0 {
namespace ui {
namespace {

constexpr uint32_t kTitleGold = 0xF0B400; /* the home grid's title colour */

struct Strip {
    lv_obj_t *canvas = nullptr;
    std::vector<uint32_t> pixels; /* ARGB8888, status_w x rows */
    int rows = 0;
    cp0_statusbar_state_t state{};
    cp0_statusbar_state_t shown{};
    bool drawn = false;
    unsigned ticks = 0;
    lv_timer_t *timer = nullptr;
};

cp0_statusbar_t *renderer()
{
    static cp0_statusbar_t *bar = cp0_statusbar_create(cp0_file_path_c("share/font/Montserrat-Medium.ttf"),
                                                       cp0_file_path_c("share/font/FontAwesome5-Solid+Brands+Regular.woff"));
    return bar;
}

int strip_rows(const cp0_ui_shell_t &s)
{
    return std::max(s.bar_h, CP0_STATUSBAR_HEIGHT) + 16; /* the renderer may touch 16 rows under the bar */
}

void redraw(Strip *strip)
{
    cp0_statusbar_t *bar = renderer();
    if (!bar || !strip->canvas) return;
    if (strip->drawn && std::memcmp(&strip->state, &strip->shown, sizeof(strip->state)) == 0) return;
    const cp0_ui_shell_t &s = cp0_ui_metrics_get()->shell;
    std::fill(strip->pixels.begin(), strip->pixels.end(), 0u);
    cp0_statusbar_render_scaled(bar, strip->pixels.data(), s.status_w, s.status_w, 0, s.status_top, 0, &strip->state,
                                s.status_pct);
    strip->shown = strip->state;
    strip->drawn = true;
    lv_obj_invalidate(strip->canvas);
}

void tick_cb(lv_timer_t *timer)
{
    auto *strip = static_cast<Strip *>(lv_timer_get_user_data(timer));
    if (!strip) return;
    if (strip->ticks++ % 2 == 0) {
        cp0_statusbar_read_state(&strip->state); /* Wi-Fi, Bluetooth and the clock: every 2 s */
    } else {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        std::strftime(strip->state.clock, sizeof(strip->state.clock), "%H:%M", &local);
    }
    redraw(strip);
}

void delete_cb(lv_event_t *event)
{
    auto *strip = static_cast<Strip *>(lv_event_get_user_data(event));
    if (!strip) return;
    if (strip->timer) lv_timer_delete(strip->timer);
    delete strip;
}

} // namespace

void StatusStrip::attach(lv_obj_t *parent)
{
    if (!parent) return;
    const cp0_ui_shell_t &s = cp0_ui_metrics_get()->shell;
    if (s.status_w <= 0 || s.bar_h <= 0) return;
    auto *strip = new Strip();
    strip->rows = strip_rows(s);
    strip->pixels.assign(static_cast<size_t>(s.status_w) * static_cast<size_t>(strip->rows), 0u);
    lv_obj_add_event_cb(parent, delete_cb, LV_EVENT_DELETE, strip);

    lv_display_t *display = lv_obj_get_display(parent);
    const int screen_w = display ? static_cast<int>(lv_display_get_horizontal_resolution(display)) : 0;
    strip->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(strip->canvas, strip->pixels.data(), s.status_w, s.bar_h, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(strip->canvas, screen_w - s.status_w, 0);
    lv_obj_remove_flag(strip->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(strip->canvas, LV_OBJ_FLAG_SCROLLABLE);
    strip->timer = lv_timer_create(tick_cb, 1000, strip);
    strip->ticks = 0;
    tick_cb(strip->timer); /* the state at once, so the first frame shows it */
}

int StatusStrip::left()
{
    static int cached = -1;
    if (cached >= 0) return cached;
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const cp0_ui_shell_t &s = m->shell;
    const int canvas_x = m->w - s.status_w;
    cp0_statusbar_t *bar = renderer();
    if (!bar || s.status_w <= 0) return canvas_x;
    /* the fullest state (Bluetooth glyph, four bars, a clock): its first drawn column is the strip's left edge */
    const int rows = strip_rows(s);
    std::vector<uint32_t> pixels(static_cast<size_t>(s.status_w) * static_cast<size_t>(rows), 0u);
    cp0_statusbar_state_t full{};
    full.wifi_up = 1;
    full.wifi_pct = 100;
    full.bt_on = 1;
    full.bt_connected = 1;
    std::strncpy(full.clock, "00:00", sizeof(full.clock) - 1);
    cp0_statusbar_render_scaled(bar, pixels.data(), s.status_w, s.status_w, 0, s.status_top, 0, &full, s.status_pct);
    int first = s.status_w;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < first; ++x)
            if (pixels[static_cast<size_t>(y) * static_cast<size_t>(s.status_w) + static_cast<size_t>(x)] >> 24) {
                first = x;
                break;
            }
    cached = canvas_x + (first < s.status_w ? first : 0);
    return cached;
}

lv_obj_t *StatusStrip::add_title(lv_obj_t *parent, const char *text)
{
    const cp0_ui_shell_t &s = cp0_ui_metrics_get()->shell;
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, text ? text : "");
    lv_obj_set_style_text_font(title, cp0_ui_font_px(s.title_px), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(kTitleGold), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, s.title_x, s.title_y);
    return title;
}

} // namespace ui
} // namespace cp0

#endif /* !__has_include(<cp0_ui_status_strip.hpp>) */
