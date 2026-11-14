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
 * brightness keys stay the launcher's. Transport keys of headsets and keyboards: media_keys.hpp. LVGL thread.
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

} // namespace ui
} // namespace vinyl
