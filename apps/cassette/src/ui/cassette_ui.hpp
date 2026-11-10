/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI (task T6b): the whole user interface on the launcher toolkit's page (header, Esc, status strip and clock,
 * the narrow portrait shell: cp0::ui::Page, never drawn by hand), driven by a ViewModel and reporting through
 * UiActions (view_model.hpp). The controller (task T6a) owns everything else.
 *
 *   CassetteUi ui(screen, actions);  ui.update(vm);  ...  ui.update(vm);
 *
 * Pages: Recorder (the root), Settings, Recordings (list), Trim; the save and confirm boxes over them (shown while the
 * model says so); the record countdown and the scan badge are states of the recorder page.
 *
 * Keys (a Bluetooth or USB keyboard; docs/KEYS.md is the same map). Esc is back everywhere (the recorder's Esc ends
 * the app as in every app, during the countdown it cancels it; a picker, a box or a text field closes first); the
 * header's Esc button does what Esc does.
 *   Recorder    R REC, Space PLAY/PAUSE, X STOP, comma REW, period FWD (the press and the release reach the deck:
 *               holding REW / FWD scans), L the recordings, S the settings
 *   Recordings  Up / Down move the selection, Enter PREVIEW (again on the previewing row: stop), L LOAD (the take goes
 *               into the deck, the recorder shows; dimmed while recording), N RENAME, T TRIM, Delete DELETE (the
 *               controller asks in the confirm box), S the settings, Esc the recorder (the preview stops)
 *   Trim        Left / Right move the marker 0.1 s (Shift 1 s), Up zoom in, Down zoom out, I SET IN, O SET OUT,
 *               Enter SPLIT (REMOVE while a flag is selected), Tab select the next flag, Space PLAY, S SAVE, F waveform TRUE / FIT, Esc
 *               back to the list (nothing saved)
 *   Boxes       Left / Right or Tab move between the buttons (a white outline from the first move), Enter chooses
 *               (the save box: SAVE AS NEW FILES; the confirm box: CANCEL, when nothing was moved), Esc cancels
 *   Settings    Up / Down move, Enter or Right change / open (Left / Right step a slider row), Esc back (a picker
 *               first), L the recordings
 *
 * Text entry (the File prefix setting, RENAME): the toolkit's Dialog with a TextField, so the keyboard policy applies
 * as everywhere (a physical keyboard first; the toolkit's fallback keyboard only when none is connected).
 *
 * CASSETTE_LAYOUT_CHECK=1 (the scenes): every 250 ms the visible page is checked (no label text cut or overlapping
 * another, every touch target at least 44 px but the two 40 x 44 header-side buttons, everything inside the screen);
 * a failure is logged and shows "LAYOUT FAIL" at the top left. CASSETTE_TIMING=1 logs the render time every 5 s.
 * LVGL thread only.
 */
#pragma once

#include "cassette_window.hpp"
#include "view_model.hpp"

#include "lvgl/lvgl.h"

#include <memory>

struct key_item;

namespace cassette {
namespace ui {

constexpr int kMinW = 480, kMinH = 320;          /* X-MinSize */
constexpr int kPortraitW = 320, kPortraitH = 480; /* X-Portrait */

class CassetteUi {
public:
    CassetteUi(lv_obj_t *screen, UiActions actions, AnimOptions anim = anim_options_from_env());
    ~CassetteUi();
    CassetteUi(const CassetteUi &) = delete;
    CassetteUi &operator=(const CassetteUi &) = delete;

    void update(const ViewModel &vm);
    /* Shows a page (as the user's navigation does; UiActions::open is called). */
    void go(Screen s);
    Screen screen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace ui
} // namespace cassette
