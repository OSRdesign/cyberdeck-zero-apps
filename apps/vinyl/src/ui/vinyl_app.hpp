/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the controller. It owns the engine, the queue, the browser's folder, the background tag reader, the output
 * model, the launcher link and the media-key reader, turns them into the pages' views every 100 ms, and turns the
 * pages' actions and the keys into commands.
 *
 * Pages: the Browser is the root (a short Esc there ends the app, owner decision 2026-10-13); Now playing opens over it
 * (from a track, the mini player, or at start when a queue is resumed); Queue and Output open from Now playing (or the
 * Browser) and Esc goes back to the page that opened them.
 *
 * Keys (also in README.md): Space play / pause, Left / Right seek 10 s, comma / period previous / next, S shuffle,
 * R repeat, Q queue, O output, B browse, minus / equals the in-app volume (only for an output the system volume does not
 * reach, decision D11), Up / Down / Enter in the lists, U / D / Delete / C on the queue page, Esc back. The volume,
 * mute and brightness keys stay the launcher's. Transport keys of headsets and keyboards: media_keys.hpp. LVGL thread.
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
