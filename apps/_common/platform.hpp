/*
 * SPDX-License-Identifier: MIT
 *
 * The device runner of a native fullscreen app (X-Fullscreen=true) built on cp0_lvgl (report 024 section 1).
 *
 * The app inherits the launcher's environment (APPLAUNCH_DISPLAY=dpi-scaled, the board profile, the contract
 * variables) and runs cp0_lvgl's own display backend, keyboard thread and widgets in its own process. The glue the
 * study found missing, all here:
 *   1. native display: the backend starts on its 320x170 compat display; after cp0_lvgl_init() the runner switches to
 *      cp0_display_set_mode(CP0_DISPLAY_MODE_NATIVE), so the app draws the whole panel 1:1 (rotation, 16/32 bpp and
 *      touch come with the backend);
 *   2. every keyboard: cp0_keyboard_set_read_all_keyboards(1) before init (the inherited LV_LINUX_KEYBOARD_DEVICE is
 *      only the Bluetooth keyboard); APPLAUNCH_KEYBOARD_SCAN=0 turns it off, as for the launcher;
 *   3. status strip and title: cp0::ui::StatusStrip / add_title (cp0_ui_widgets.hpp) are used directly, nothing here;
 *   4. root exit: app.hpp request_quit() (short Esc / header Esc at the root page), SIGTERM / SIGINT / SIGHUP (the
 *      launcher's 3 s hold-Esc watchdog signals the app's process group) end the run loop cleanly; a screen under
 *      AppSpec min_w x min_h shows the notice (app.hpp) instead of the app;
 *   5. multitasking: when the launcher gives a control socket (X-Background=run|freeze in the .desktop file, a
 *      multitasking launcher), the toolkit's cp0_app_lifecycle_device_start() answers hide / show / close from the run
 *      loop (APP-CONTRACT section 13); close = request_quit(0). Without the socket nothing changes.
 * Started by hand (ssh, no launcher environment): APPLAUNCH_DISPLAY is set to dpi-scaled if missing, so the same
 * backend probes the framebuffer (APP-CONTRACT section 7); a board profile can be loaded first with
 *   set -a; . /etc/applaunch/board.conf; set +a
 *
 * Command line: --version (app version, pinned launcher commit, build date), --help.
 */
#pragma once

#include "app.hpp"

#include <string>

namespace cdk {

/* The app's main(): return cdk::run(cdk::app_spec(), argc, argv); */
int run(const AppSpec &spec, int argc, char **argv);

} // namespace cdk
