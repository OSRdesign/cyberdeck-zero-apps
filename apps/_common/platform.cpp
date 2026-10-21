/*
 * SPDX-License-Identifier: MIT
 *
 * The device runner (see platform.hpp). Device builds only: it needs cp0_lvgl's display backend and keyboard thread.
 */
#include "platform.hpp"

#include "build_info.hpp"

#include "cp0_app_contract.h"
#include "cp0_app_lifecycle_lvgl.h"
#include "cp0_display.h"
#include "cp0_lvgl_app_runner.hpp"
#include "keyboard_input.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cdk {
namespace {

volatile std::sig_atomic_t g_signal = 0;

void on_signal(int sig)
{
    g_signal = sig; /* the run loop checks it after every LVGL pass (indev timers: about every 33 ms) */
}

void install_signals()
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal; /* no SA_RESTART: a blocking wait returns EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    struct sigaction ign;
    std::memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, nullptr);
}

void log_start(const AppSpec &spec)
{
    cp0_app_env_t env;
    const bool contract = cp0_app_env_read(&env) == 0;
    if (contract) {
        std::fprintf(stderr, "[cdk] %s %s: launcher contract %d, mode %s, canvas %dx%d, state dir %s\n", spec.id,
                     CDK_APP_VERSION, env.contract, cp0_app_mode_name(env.mode), env.canvas_w, env.canvas_h,
                     env.state_dir[0] ? env.state_dir : "(default)");
        if (env.mode == CP0_APP_MODE_COMPAT)
            std::fprintf(stderr, "[cdk] %s: started as a compat app (320x170): its .desktop file needs "
                                 "X-Fullscreen=true\n", spec.id);
    } else {
        std::fprintf(stderr, "[cdk] %s %s: no launcher contract (started by hand or by an older launcher): the "
                             "display backend probes the framebuffer\n", spec.id, CDK_APP_VERSION);
    }
}

} // namespace

int run(const AppSpec &spec, int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::fputs(version_text(spec).c_str(), stdout);
            return 0;
        }
        std::fprintf(stderr, "usage: %s [--version]\n  started by the launcher (X-Fullscreen=true), or by hand on the "
                             "board's console\n", argv[0]);
        return std::strcmp(argv[i], "--help") == 0 ? 0 : 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0); /* line by line: the lines reach the journal at once */
    install_signals();
    log_start(spec);

    /* started by hand: the launcher's own backend (rotation, 16 / 32 bpp, touch) rather than plain fbdev */
    if (!std::getenv("APPLAUNCH_DISPLAY")) setenv("APPLAUNCH_DISPLAY", "dpi-scaled", 1);
    /* only the launcher describes the panel (APP-CONTRACT section 2): the backend skips its screen.state publication
     * when APPLAUNCH_APP_MODE is set, which a start by hand lacks (the contract itself stays absent: no
     * APPLAUNCH_CONTRACT, window or canvas) */
    if (!std::getenv("APPLAUNCH_APP_MODE")) setenv("APPLAUNCH_APP_MODE", "fullscreen", 1);
    /* glue 2: every keyboard, not only the inherited LV_LINUX_KEYBOARD_DEVICE (the launcher's own switch honoured) */
    const char *scan = std::getenv("APPLAUNCH_KEYBOARD_SCAN");
    cp0_keyboard_set_read_all_keyboards(scan && scan[0] == '0' ? 0 : 1);

    Cp0LvglRunOptions options;
    options.after_resource_init = [] {
        /* glue 1: the backend starts on its compat display; the app draws the whole panel */
        if (cp0_display_available()) cp0_display_set_mode(CP0_DISPLAY_MODE_NATIVE);
    };
    options.setup = [&spec] {
        lv_display_t *display = cp0_display_available() ? cp0_display_native() : lv_display_get_default();
        if (!display) return false;
        lv_obj_t *screen = lv_display_get_screen_active(display);
        std::fprintf(stderr, "[cdk] %s: display %dx%d (%s backend)\n", spec.id,
                     static_cast<int>(lv_display_get_horizontal_resolution(display)),
                     static_cast<int>(lv_display_get_vertical_resolution(display)),
                     cp0_display_available() ? "dpi-scaled" : "fbdev");
        start(spec, screen);
        /* glue 5, multitasking (APP-CONTRACT section 13): only when the launcher gave a control socket (X-Background in
         * the .desktop file, a multitasking launcher); its "close" quits as a short Esc at the root does */
        if (cp0_app_lifecycle_device_start([] { request_quit(0); }))
            std::fprintf(stderr, "[cdk] %s: can stay in the background (lifecycle on)\n", spec.id);
        return true;
    };
    options.should_quit = [] { return g_signal != 0 || quit_requested(); };
    options.teardown = [] {
        cp0_app_lifecycle_device_stop();
        stop();
    };
    const int rc = cp0_lvgl_run(std::move(options));
    if (g_signal) std::fprintf(stderr, "[cdk] %s: signal %d, ended cleanly\n", spec.id, static_cast<int>(g_signal));
    return rc != 0 ? rc : quit_code();
}

} // namespace cdk
