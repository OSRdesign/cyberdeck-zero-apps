/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the controller. It owns the engine, the queue, the browser's folder, the background tag reader, the output
 * model, the launcher link and the media-key reader, turns them into the pages' views every 100 ms, and turns the
 * pages' actions and the keys into commands.
 *
 * Pages: the Browser is the root (a short Esc there ends the app, owner decision 2026-10-13); Now playing opens over it
 * (from a track, the mini player, or at start when a queue is resumed); Queue and Output open from Now playing (or the
 * Browser) and Esc goes back to the page that opened them. P2a: the Menu (the Menu key, M) and its pages (Sleep timer,
 * Playback with its ReplayGain pick list, Output, Keys, About); Esc goes back to the Menu, and from the Menu to the page
 * it was opened from. The controller owns the settings (core/settings), the sleep timer (core/sleep), the positions in
 * long files (core/state/positions) and the write policy of the state files (core/state/write_policy).
 * P2b: the Library (core/library/library_ctl: the database, the scanner, the hub and its pages). Browse opens the hub when the
 * database opened (else the folder list), Folders opens the Browser, Esc steps back one place and at the hub ends the app.
 *
 * Keys: core/menu letter_command (the table the Keys page shows, README.md "Keys and touch"); the volume, mute and
 * brightness keys stay the launcher's. Transport keys of headsets and keyboards: media_keys.hpp. Knock-knock (2026-11-16,
 * core/screen/knock.hpp): three quick taps on the cover switch the screen off through the launcher; while it is off every
 * touch and every key but the media and volume keys is ignored, three quick taps anywhere wake it, and every way out
 * (exit, hidden, the setting Off, a crash the guard catches) gives the light back. LVGL thread.
 */
#pragma once

#include "lvgl/lvgl.h"

#include <memory>

namespace vinyl {
namespace ui {

class VinylApp {
public:
    explicit VinylApp(lv_obj_t *screen);
    ~VinylApp();
    VinylApp(const VinylApp &) = delete;
    VinylApp &operator=(const VinylApp &) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

/* Knock-knock (2026-11-16): the launcher's backlight request, cp0_app_backlight (cp0_app_client.h: 0 done, 1 refused, -1
 * no service; it blocks up to 200 ms, so Vinyl calls it on a worker thread). The device main gives it before the app
 * starts; without it (the headless harness) the setting is not offered, except the scenes' fake (VINYL_DEMO_SCREEN). */
void set_backlight_call(int (*call)(int on));

} // namespace ui
} // namespace vinyl
