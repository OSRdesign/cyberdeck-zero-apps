/*
 * SPDX-License-Identifier: MIT
 *
 * Headless harness: a plain LVGL display at W x H in memory, the layout service's metrics for it, the keyboard bridge,
 * a scripted pointer and the services the widgets ask for (font and resource paths). No framebuffer, no evdev, no
 * launcher: the app's own code and cp0_lvgl's widgets run unchanged on top.
 */
#include "headless.hpp"

#include "cp0_lvgl_app.h"
#include "cp0_ui_metrics.h"
#include "hal_lvgl_bsp.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <time.h>
#include <unordered_map>

namespace headless {
namespace {

Platform g_platform;
std::string g_resources;
struct {
    bool pressed = false;
    int x = 0, y = 0;
} g_touch;
std::vector<uint8_t> g_draw_buf;
cp0_ui_metrics_t g_metrics;

uint32_t tick_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    Platform &p = g_platform;
    const int aw = lv_area_get_width(area);
    const auto *src = reinterpret_cast<const uint32_t *>(px_map);
    for (int y = area->y1; y <= area->y2; ++y) {
        if (y < 0 || y >= p.h) continue;
        for (int x = area->x1; x <= area->x2; ++x)
            if (x >= 0 && x < p.w)
                p.shadow[static_cast<size_t>(y) * p.w + x] = src[static_cast<size_t>(y - area->y1) * aw + (x - area->x1)];
    }
    lv_display_flush_ready(disp);
}

void pointer_read(lv_indev_t *, lv_indev_data_t *data)
{
    data->point.x = g_touch.x;
    data->point.y = g_touch.y;
    data->state = g_touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* What /usr/share/APPLaunch/<path> is in the launcher tree: fonts by name (Montserrat-Bold.ttf) and share/... paths. */
std::string resource_path(const std::string &name)
{
    if (name.empty() || name[0] == '/') return name;
    const std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : "";
    if (name.find('/') == std::string::npos && (ext == ".ttf" || ext == ".otf" || ext == "woff"))
        return g_resources + "/share/font/" + name;
    return g_resources + "/" + name;
}

} // namespace

Platform &platform() { return g_platform; }

bool platform_init(int w, int h, int ppmm_x100, const std::string &resource_root)
{
    g_resources = resource_root;
    Platform &p = g_platform;
    p.w = w;
    p.h = h;
    p.shadow.assign(static_cast<size_t>(w) * h, 0);

    lv_init();
    lv_tick_set_cb(tick_cb);

    /* fonts and resources: the filesystem service's Path command, as cp0_lvgl answers it on a board */
    cp0_signal_filesystem_api.append([](std::list<std::string> args, std::function<void(int, std::string)> reply) {
        if (args.size() == 2 && args.front() == "Path") {
            if (reply) reply(0, resource_path(args.back()));
        } else if (reply) {
            reply(-1, "headless: no such command");
        }
    });

    /* metrics: the deck size has no physical size (default density); the others report mm like a driver */
    int mm_w = 0, mm_h = 0;
    if (ppmm_x100 > 0 && !(w == 640 && h == 480)) {
        mm_w = (w * 100 + ppmm_x100 / 2) / ppmm_x100;
        mm_h = (h * 100 + ppmm_x100 / 2) / ppmm_x100;
    }
    const cp0_ui_density_t density = cp0_ui_density_resolve(w, h, 0, nullptr, mm_w, mm_h);
    if (cp0_ui_metrics_compute(w, h, 0, density.ppmm_x100, density.src, 0, &g_metrics) != 0) return false;
    cp0_ui_metrics_set_current(&g_metrics);

    p.display = lv_display_create(w, h);
    if (!p.display) return false;
    lv_display_set_color_format(p.display, LV_COLOR_FORMAT_XRGB8888);
    g_draw_buf.assign(static_cast<size_t>(w) * h * 4, 0);
    lv_display_set_buffers(p.display, g_draw_buf.data(), nullptr, static_cast<uint32_t>(g_draw_buf.size()),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(p.display, flush_cb);
    lv_display_set_default(p.display);

    headless_keyboard_init();
    p.pointer = lv_indev_create();
    lv_indev_set_type(p.pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(p.pointer, pointer_read);
    lv_indev_set_display(p.pointer, p.display);

    char desc[1024];
    if (cp0_ui_metrics_describe(&g_metrics, desc, sizeof(desc)) > 0) std::fprintf(stderr, "[headless] %s\n", desc);
    return true;
}

void platform_touch(bool pressed, int x, int y)
{
    g_touch.pressed = pressed;
    g_touch.x = x;
    g_touch.y = y;
}

void run_for(int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do {
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    } while (std::chrono::steady_clock::now() < end);
}

Image screenshot()
{
    lv_refr_now(g_platform.display);
    Image img;
    img.w = g_platform.w;
    img.h = g_platform.h;
    img.px.resize(g_platform.shadow.size());
    for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = g_platform.shadow[i] & 0xFFFFFFu;
    return img;
}

} // namespace headless

/* ---- what the device's filesystem service and the cp0 runtime would provide (not linked here) ---- */

extern "C" const char *cp0_file_path_c(const char *file)
{
    static std::unordered_map<std::string, std::string> paths;
    const std::string key = file ? file : "";
    auto it = paths.find(key);
    if (it == paths.end()) {
        std::string resolved;
        cp0_signal_filesystem_api({"Path", key}, [&](int code, std::string data) {
            if (code == 0) resolved = std::move(data);
        });
        it = paths.emplace(key, resolved).first;
    }
    return it->second.c_str();
}

std::string cp0_file_path(std::string file)
{
    return cp0_file_path_c(file.c_str());
}
