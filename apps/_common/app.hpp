/*
 * SPDX-License-Identifier: MIT
 *
 * cdk (cyberdeck kit): what every native fullscreen app of this repository shares, on the deck and in the headless
 * harness (report 024 section 2). This header is the LVGL side that both runners use:
 *
 *   AppSpec       what an app gives the runner: id, title, the minimum screen (= X-MinSize of its .desktop file) and
 *                 the function that builds its UI on a screen.
 *   start/stop    the runner's two calls once the display exists: too small a screen -> the "needs WxH" notice; else
 *                 the on-screen keyboard policy (osk_policy.hpp), the keypad input group and the app's UI.
 *   request_quit  the app ends: a short Esc at the root page, the header's Esc button at the root page (report 024,
 *                 Controller decision 2026-10-13: a short Esc at an app's root quits). The 3 s Esc hold stays the
 *                 launcher's watchdog (SIGTERM to the app's process group), handled by the device runner.
 *
 * The device runner is platform.hpp (cp0_lvgl's display backend, keyboards, signals); the headless harness is
 * headless/ (a plain LVGL display at any size, scripted input, PNG shots, layout checks). LVGL thread only.
 */
#pragma once

#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>

namespace cdk {

struct AppSpec {
    const char *id = "";           /* package name / share_code, e.g. "cyberdeck-app-template" */
    const char *title = "";        /* the page title, also named in the notice */
    int min_w = 640;               /* the smallest logical screen the app supports: keep it equal to X-MinSize in */
    int min_h = 480;               /* the .desktop file (the template's tests compare them) */
    /* Builds the app on `screen` (the native display's active screen, black, empty). Called once on the LVGL thread
     * after the keypad group exists. The returned object owns the UI; it is released by stop(), LVGL still up. */
    std::function<std::shared_ptr<void>(lv_obj_t *screen)> build;
};

/* The spec of the app linked into this binary: each app defines it once (its ui/ code). The device main and the
 * headless harness both start it. */
const AppSpec &app_spec();

/* --------------------------------------------------------------------------------------------- pure (unit-tested) */

/* The screen is big enough: w >= min_w and h >= min_h (logical, landscape). */
bool screen_fits(const AppSpec &spec, int w, int h);
/* The notice for a screen that is too small (an older launcher ignores X-MinSize, APP-CONTRACT section 6). */
std::string min_size_notice(const AppSpec &spec, int w, int h);

/* What --version prints: the app's version, the launcher commit of the build and its pin state (build_info.hpp). */
std::string version_text(const AppSpec &spec);

/* --------------------------------------------------------------------------------------------- runtime */

/* The app ends with `code` (exit status). The runner sees it after the current LVGL event. Any thread. */
void request_quit(int code = 0);
bool quit_requested();
int quit_code();

/* Multitasking (the launcher's APP-CONTRACT section 13; the toolkit's cp0_app_lifecycle_lvgl.h does the work): with
 * X-Background=run|freeze in its .desktop file and a launcher that gives it a control socket, the app can be hidden and
 * shown again; the launcher's "close" quits it as request_quit(0) does. Without the socket nothing changes.
 * hidden(): true between hide and show. Nothing is drawn then; a run app's own timers keep going: skip the work that
 * only feeds the screen while it is true, and bring the screen up to date when it turns false. LVGL thread. */
bool hidden();
/* Leave to the launcher's home grid, the app staying open (the message "home"); false without a control socket. No
 * page uses it today: a short Esc at the root quits (owner decision D5). LVGL thread. */
bool leave_to_home();

/* After the display (and the keypad indev) exist, LVGL thread. Returns true when the app's UI was built, false when
 * the notice is shown instead (the app then quits on any key or a tap). */
bool start(const AppSpec &spec, lv_obj_t *screen);
/* Background commands stopped (async_run.hpp), the keyboard dismissed, the app's UI released. LVGL still up. */
void stop();

/* The input group every keypad indev drives (created by start()). Pages add their focus objects to it. */
lv_group_t *input_group();

/* The app's screen size as start() saw it (0 before). */
int screen_w();
int screen_h();

} // namespace cdk
