/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: the bound app (task T6c), built by app_spec (ui/app_spec.cpp) on the toolkit's screen: the application
 * controller on the real sound system (libasound through dlopen, the user's ~/Recordings, the real clock) or, with
 * CASSETTE_DEMO=1, on the scripted one (demo/demo_alsa.hpp: no hardware, a demo home); the UI (ui/cassette_ui.hpp);
 * and the Binder between them, polled by an LVGL timer every 33 ms.
 *
 * The controller starts before the first frame (its start() repairs interrupted takes and opens the card: tens of ms).
 * When the app ends (Esc at the root, the header's Esc, SIGTERM / SIGINT / SIGHUP from the launcher: the runner's
 * teardown releases the app) the queued commands run, then app_leave() and shutdown(): a take in progress is closed and
 * kept with its journal updated, a countdown cancelled, a save cancelled (its temporary file removed, the original
 * untouched). With no sound input the recorder shows the "no sound input" page and the controller keeps looking for a
 * card once a second (hot-plug).
 * LVGL thread only.
 */
#pragma once

#include "lvgl/lvgl.h"

#include <memory>

namespace cassette {
namespace bind {

/* The bound app is wanted: CASSETTE_DEMO unset, empty or "0" (the real sound system) or "1" (the scripted one). Any
 * other value names a state of the UI's own synthetic model (ui/demo.hpp, the UI scenes): false. */
bool bound_wanted();

/* Builds the bound app on `screen`; the returned object owns it (released by cdk::stop(), LVGL still up). */
std::shared_ptr<void> build_bound_app(lv_obj_t *screen);

} // namespace bind
} // namespace cassette
