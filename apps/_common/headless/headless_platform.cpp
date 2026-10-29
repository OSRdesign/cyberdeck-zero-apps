/*
 * SPDX-License-Identifier: MIT
 *
 * Headless harness: a plain LVGL display at W x H in memory, the layout service's metrics for it, the keyboard bridge,
 * a scripted pointer and the services the widgets ask for (font and resource paths). No framebuffer, no evdev, no
 * launcher: the app's own code and cp0_lvgl's widgets run unchanged on top. For the multitasking scenes the harness
 * plays the launcher's side of the lifecycle socket and gives the toolkit's helper its port (lifecycle_begin). For the
 * PDA writing zone (report 044) it plays the display backend run with APPLAUNCH_PDA=1: the band's own display under the
 * app's, the touch router, the band's pointer (band_begin).
 */
#include "headless.hpp"

#include "app.hpp"
#include "cp0_app_lifecycle_lvgl.h"
#include "cp0_display.h"
#include "cp0_lvgl_app.h"
#include "cp0_ui_metrics.h"
#include "cp0_zone.h"
#include "cp0_zone_ui.h"
#include "hal_lvgl_bsp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <unordered_map>

namespace headless {
namespace {

Platform g_platform;
std::string g_resources;
struct {
    bool pressed = false;
    int x = 0, y = 0;
    int32_t id = 0; /* the contact's tracking id: a new one per press (the router's) */
} g_touch;
std::vector<uint8_t> g_draw_buf;
cp0_ui_metrics_t g_metrics;

/* The writing zone's band (report 044), as cp0_lvgl_dpi_scaled.c builds it with APPLAUNCH_PDA=1 on a portrait canvas:
 * its own display (the panel's rows from strip_y), drawn into its own shadow and laid over the app's rows while it
 * shows; the router decides who owns a touch by where it starts (app area, strip, writing zone). */
struct {
    bool on = false;
    bool shown = false;
    cp0_zone_layout_t l{};
    cp0_zone_router_t router{};
    lv_display_t *display = nullptr;
    lv_indev_t *pointer = nullptr;
    std::vector<uint8_t> draw_buf;
    std::vector<uint32_t> shadow;
} g_band;

/* the lifecycle (P-e): the helper's port, the coverage of the flushes since the output came back, the launcher's end */
OutputStats g_out;
std::vector<uint8_t> g_cover;
bool g_input_on = true;
int g_launcher_fd = -1;
uint32_t g_lc_n = 0;
std::vector<std::string> g_heard;

uint32_t tick_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    Platform &p = g_platform;
    if (!g_out.on) { /* hidden: the board's display writes nothing either (cp0_display_suspend) */
        ++g_out.flushes_while_off;
        lv_display_flush_ready(disp);
        return;
    }
    const int aw = lv_area_get_width(area);
    const auto *src = reinterpret_cast<const uint32_t *>(px_map);
    for (int y = area->y1; y <= area->y2; ++y) {
        if (y < 0 || y >= p.h) continue;
        for (int x = area->x1; x <= area->x2; ++x)
            if (x >= 0 && x < p.w) {
                const size_t i = static_cast<size_t>(y) * p.w + x;
                p.shadow[i] = src[static_cast<size_t>(y - area->y1) * aw + (x - area->x1)];
                if (!g_cover.empty() && !g_cover[i]) {
                    g_cover[i] = 1;
                    ++g_out.pixels_since_on;
                }
            }
    }
    lv_display_flush_ready(disp);
}

/* the touch in progress is the app's (always without the band): it started in the app area */
bool app_touch()
{
    return !g_band.on || g_band.router.owner == CP0_ZONE_OWNER_APP;
}

void pointer_read(lv_indev_t *, lv_indev_data_t *data)
{
    data->point.x = g_touch.x;
    data->point.y = g_touch.y;
    data->state = g_touch.pressed && g_input_on && app_touch() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---- the band (band_begin) */

int64_t band_clock_us(void)
{
    return static_cast<int64_t>(lv_tick_get()) * 1000; /* the backend's clock without a monotonic touch device */
}

void band_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (!g_out.on) { /* hidden: the backend's suspend covers the band too */
        ++g_out.flushes_while_off;
        lv_display_flush_ready(disp);
        return;
    }
    const int w = g_platform.w, h = g_band.l.band_h;
    const int aw = lv_area_get_width(area);
    const auto *src = reinterpret_cast<const uint32_t *>(px_map);
    for (int y = area->y1; y <= area->y2; ++y) {
        if (y < 0 || y >= h) continue;
        for (int x = area->x1; x <= area->x2; ++x)
            if (x >= 0 && x < w)
                g_band.shadow[static_cast<size_t>(y) * w + x] =
                    src[static_cast<size_t>(y - area->y1) * aw + (x - area->x1)];
    }
    lv_display_flush_ready(disp);
}

/* the band's pointer: a touch that started on the strip, in the band display's coordinates */
void band_pointer_read(lv_indev_t *, lv_indev_data_t *data)
{
    data->point.x = g_touch.x;
    data->point.y = g_touch.y - g_band.l.strip_y;
    const bool mine = g_band.on && g_band.router.owner == CP0_ZONE_OWNER_BAND;
    data->state = g_touch.pressed && g_input_on && mine ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* shown or hidden (cp0_zone_ui_set_vis_cb, the display manager's hook): the band repaints when it comes */
void band_vis_cb(int shown)
{
    g_band.shown = shown != 0;
    if (g_band.shown && g_band.display) lv_obj_invalidate(lv_display_get_screen_active(g_band.display));
}

/* one frame of the touch screen to the router (one contact or none), its pen events to the recogniser */
void band_route()
{
    if (!g_band.on) return;
    cp0_touch_frame_t f{};
    f.t_us = band_clock_us();
    if (g_touch.pressed && g_input_on) {
        f.n = 1;
        f.c[0].id = g_touch.id;
        f.c[0].x = g_touch.x;
        f.c[0].y = g_touch.y;
        f.c[0].major = -1;
        f.c[0].slot = 0;
    }
    cp0_zone_pen_t pen;
    (void)cp0_zone_route(&g_band.router, &g_band.l, g_band.shown ? 1 : 0, 0, &f, &pen);
    if (pen.kind != CP0_ZONE_PEN_NONE) cp0_zone_ui_pen(&pen);
}

/* the harness's port of the helper: output = the flush above, input = the scripted pointer */
void port_output(int on)
{
    g_out.on = on != 0;
    if (on) {
        std::fill(g_cover.begin(), g_cover.end(), 0);
        g_out.pixels_since_on = 0;
        if (g_band.on) { /* the backend's resume: the band repainted, a stroke or a held strip key dropped */
            lv_obj_invalidate(lv_display_get_screen_active(g_band.display));
            cp0_zone_router_reset(&g_band.router, 0);
            cp0_zone_ui_reset_input();
        }
    }
}

void port_input(int on)
{
    g_input_on = on != 0;
    if (on) g_touch.pressed = false; /* what was touched while hidden is gone (the board reads its contacts back) */
}

const cp0_app_lifecycle_port_t kPort = {port_output, port_input};

/* the app's messages besides the answers go to g_heard; the answer "ack <n>" is reported */
bool read_launcher_end(uint32_t want_ack)
{
    bool acked = false;
    char buf[128];
    for (;;) {
        const ssize_t r = recv(g_launcher_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (r <= 0) break;
        buf[r] = '\0';
        const std::string msg(buf);
        if (msg == "ack " + std::to_string(want_ack)) acked = true;
        else g_heard.push_back(msg);
    }
    return acked;
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

bool platform_init(int w, int h, int ppmm_x100, const std::string &resource_root, bool force_density)
{
    g_resources = resource_root;
    Platform &p = g_platform;
    p.w = w;
    p.h = h;
    p.shadow.assign(static_cast<size_t>(w) * h, 0);
    g_cover.assign(static_cast<size_t>(w) * h, 0);

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
    if (ppmm_x100 > 0 && (force_density || !(w == 640 && h == 480))) {
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
    if (pressed && !g_touch.pressed) ++g_touch.id;
    g_touch.pressed = pressed;
    g_touch.x = x;
    g_touch.y = y;
    band_route(); /* the frame to the writing zone's router first, as the backend reads it */
}

bool band_begin(const std::string &strokes_dir)
{
    if (g_band.on) return true;
    Platform &p = g_platform;
    if (cp0_zone_layout(&g_metrics, &g_band.l) != 0) return false; /* landscape, or no room: no band, as on a board */
    setenv("APPLAUNCH_PDA", "1", 1);
    setenv("APPLAUNCH_STROKES_DIR", strokes_dir.c_str(), 1);
    setenv("APPLAUNCH_STROKES_USER", "", 1); /* never the user's own trained glyphs */
    unsetenv("APPLAUNCH_STROKES_LOG");
    g_band.shadow.assign(static_cast<size_t>(p.w) * g_band.l.band_h, 0);
    g_band.display = lv_display_create(p.w, g_band.l.band_h);
    if (!g_band.display) return false;
    lv_display_set_color_format(g_band.display, LV_COLOR_FORMAT_XRGB8888);
    g_band.draw_buf.assign(static_cast<size_t>(p.w) * g_band.l.band_h * 4, 0);
    lv_display_set_buffers(g_band.display, g_band.draw_buf.data(), nullptr,
                           static_cast<uint32_t>(g_band.draw_buf.size()), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(g_band.display, band_flush_cb);
    lv_display_set_default(p.display); /* the app draws on its own display */
    g_band.pointer = lv_indev_create();
    lv_indev_set_type(g_band.pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_band.pointer, band_pointer_read);
    lv_indev_set_display(g_band.pointer, g_band.display);
    if (cp0_zone_ui_create(g_band.display, &g_band.l, band_clock_us) != 0) return false;
    lv_display_set_default(p.display);
    int heal_ms = 0, heal_px = 0;
    cp0_zone_ui_heal(&heal_ms, &heal_px);
    cp0_zone_router_init(&g_band.router, heal_ms, heal_px);
    cp0_zone_ui_set_vis_cb(band_vis_cb);
    g_band.on = true;
    std::fprintf(stderr, "[headless] writing zone: %s\n", cp0_zone_ui_describe());
    return true;
}

bool band_on() { return g_band.on; }
bool band_shown() { return g_band.on && g_band.shown; }
int band_strip_y() { return g_band.on ? g_band.l.strip_y : -1; }

bool band_rect(const std::string &name, int *x, int *y, int *w, int *h)
{
    if (!g_band.on) return false;
    static const char *const keys[CP0_ZONE_KEY__COUNT] = {"Keyb", "Tab",  "Ctrl",  "Alt",  "Left",
                                                         "Up",   "Down", "Right", "Enter"};
    const cp0_zone_rect_t *r = nullptr;
    for (int i = 0; i < CP0_ZONE_KEY__COUNT; ++i)
        if (name == keys[i]) r = &g_band.l.strip_key[i];
    if (name == "letters") r = &g_band.l.letters;
    if (name == "digits") r = &g_band.l.digits;
    if (!r) return false;
    *x = r->x, *y = r->y, *w = r->w, *h = r->h;
    return true;
}

bool band_write(bool digits, const std::string &glyph)
{
    const strokes_table *table = cp0_zone_ui_table();
    int bx = 0, by = 0, bw = 0, bh = 0;
    if (!table || !band_rect(digits ? "digits" : "letters", &bx, &by, &bw, &bh)) return false;
    /* the glyph's first template (the chart's stroke) in the box's set, else a gesture of both boxes */
    float xy[2 * 256];
    int n = strokes_table_template_points(table, glyph.c_str(), digits ? STROKES_SET_DIGITS : STROKES_SET_LETTERS, 0,
                                          xy, 256);
    if (n < 0) n = strokes_table_template_points(table, glyph.c_str(), STROKES_SET_ANY, 0, xy, 256);
    if (n < 0 || n > 256) return false;
    std::vector<std::pair<int, int>> pts;
    if (n == 0) { /* a tap glyph */
        pts.emplace_back(bx + bw / 2, by + bh / 2);
    } else {
        /* drawn 12 mm on its longer side (as a hand writes in the box), centred in the box */
        const double ppmm = g_metrics.ppmm_x100 / 100.0;
        const double scale = std::min(12.0 * ppmm, 0.8 * std::min(bw, bh)) / 100.0;
        float minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
        for (int i = 1; i < n; ++i) {
            minx = std::min(minx, xy[2 * i]), maxx = std::max(maxx, xy[2 * i]);
            miny = std::min(miny, xy[2 * i + 1]), maxy = std::max(maxy, xy[2 * i + 1]);
        }
        const double ox = bx + (bw - (maxx - minx) * scale) / 2 - minx * scale;
        const double oy = by + (bh - (maxy - miny) * scale) / 2 - miny * scale;
        for (int i = 0; i < n; ++i)
            pts.emplace_back(static_cast<int>(std::lround(ox + xy[2 * i] * scale)),
                             static_cast<int>(std::lround(oy + xy[2 * i + 1] * scale)));
    }
    band_stroke(pts);
    return true;
}

void band_stroke(const std::vector<std::pair<int, int>> &pts)
{
    if (pts.empty()) return;
    /* a frame every 1 mm, 10 ms apart (about 100 mm/s), the router and the recogniser on the LVGL tick */
    const double step = std::max(1.0, g_metrics.ppmm_x100 / 100.0);
    platform_touch(true, pts[0].first, pts[0].second);
    run_for(10);
    for (size_t i = 1; i < pts.size(); ++i) {
        const double dx = pts[i].first - pts[i - 1].first, dy = pts[i].second - pts[i - 1].second;
        const int steps = std::max(1, static_cast<int>(std::ceil(std::hypot(dx, dy) / step)));
        for (int k = 1; k <= steps; ++k) {
            platform_touch(true, pts[i - 1].first + static_cast<int>(std::lround(dx * k / steps)),
                           pts[i - 1].second + static_cast<int>(std::lround(dy * k / steps)));
            run_for(10);
        }
    }
    platform_touch(false, pts.back().first, pts.back().second);
    run_for(400); /* the heal window, the result typed, the ink's fade */
}

void run_for(int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do {
        lv_timer_handler();
        cp0_app_lifecycle_lvgl_service(); /* the device runner's loop hook (nothing without a lifecycle) */
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    } while (std::chrono::steady_clock::now() < end);
}

bool lifecycle_begin()
{
    if (g_launcher_fd >= 0) return true;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return false;
    setenv("APPLAUNCH_LIFECYCLE", "1", 1);
    setenv("APPLAUNCH_CONTROL_FD", std::to_string(sv[1]).c_str(), 1);
    if (!cp0_app_lifecycle_lvgl_start(&kPort, [] { cdk::request_quit(0); })) {
        close(sv[0]);
        close(sv[1]);
        return false;
    }
    g_launcher_fd = sv[0];
    return true;
}

int lifecycle_send(const std::string &word, int deadline_ms)
{
    if (g_launcher_fd < 0) return -1;
    const uint32_t n = ++g_lc_n;
    const std::string msg = word + " " + std::to_string(n);
    if (send(g_launcher_fd, msg.data(), msg.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(msg.size())) return -1;
    const bool close_msg = word == "close";
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    };
    while (elapsed() <= deadline_ms) {
        cp0_app_lifecycle_lvgl_service();
        if (read_launcher_end(n) && !close_msg) return elapsed();
        if (close_msg && cdk::quit_requested()) return elapsed();
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return -1;
}

std::vector<std::string> lifecycle_heard()
{
    if (g_launcher_fd >= 0) (void)read_launcher_end(0xFFFFFFFFu);
    std::vector<std::string> out;
    out.swap(g_heard);
    return out;
}

const OutputStats &output_stats() { return g_out; }

Image screenshot()
{
    lv_refr_now(g_platform.display);
    if (g_band.on) lv_refr_now(g_band.display);
    Image img;
    img.w = g_platform.w;
    img.h = g_platform.h;
    img.px.resize(g_platform.shadow.size());
    for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = g_platform.shadow[i] & 0xFFFFFFu;
    if (band_shown()) { /* the band covers the app's bottom rows while it shows, as the backend's clipped flush */
        const size_t from = static_cast<size_t>(g_band.l.strip_y) * img.w;
        for (size_t i = 0; i < g_band.shadow.size() && from + i < img.px.size(); ++i)
            img.px[from + i] = g_band.shadow[i] & 0xFFFFFFu;
    }
    return img;
}

} // namespace headless

/* ---- what cp0_lvgl's display backend answers about the writing zone (cp0_display.h; the backend is not linked here):
 * the band exists when the scene asked for it (band_begin) at a size that has one ---- */

extern "C" int cp0_display_available(void)
{
    return headless::band_on() ? 1 : 0;
}

extern "C" int cp0_display_zone_active(void)
{
    return headless::band_on() ? 1 : 0;
}

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
