/*
 * SPDX-License-Identifier: MIT
 *
 * Mesh Hop on the shared app platform (apps/_common): cdk::run on the boards (cp0_lvgl's display backend with the
 * rotation, the 16 / 32 bpp framebuffer and the touch of the launcher contract, every keyboard, the signals, the
 * "needs 640x480" notice), the headless harness on the PC. What the app itself needs from it is here:
 *
 *   the stage     the shell of the app (header, tab row, caption, hint line, dialogs) is built from the launcher's
 *                 widgets and follows the screen, and so do the pages of P3 (Contacts, Settings and their sub-pages,
 *                 the search). The body of Chats is still laid out for a 640-wide column: the app builds it in a
 *                 plain 640 px wide container (the stage) centred under the shell and gives it to the platform with
 *                 adopt_stage(). LVGL hit-tests touches in screen coordinates, so a tap reaches the control under the
 *                 finger at any size; the hold on a conversation name reads the raw point through to_stage().
 *   the keys      cp0_lvgl's key_item (LV_EVENT_KEYBOARD on the screen, the LVGL keypad path intercepted) is turned
 *                 into the app's KeyEvent by KeyBridge (key_bridge.hpp): the same KeyTranslator as before (US or FR
 *                 layout, Ctrl / Alt / AltGr / Caps, Tab and Shift+Tab, the arrows) on the evdev codes, the given text
 *                 for keys without a code or typed by the on-screen keyboard or a test.
 *   the rest      whether a finger is on the screen (no list is rebuilt under it) and whether a keyboard is there.
 */

#pragma once

#include "key_bridge.hpp"
#include "ui_logic.hpp"

#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>

namespace meshhop {

/* The smallest screen (X-MinSize) and the width of the page bodies: the stage. */
constexpr int kScreenW = 640;
constexpr int kScreenH = 480;

class Platform {
public:
    /* The keyboard bridge on `screen` (the active screen cdk::start gives the app). LVGL thread. */
    explicit Platform(lv_obj_t *screen);
    ~Platform();
    Platform(const Platform &) = delete;
    Platform &operator=(const Platform &) = delete;

    /* The screen the app builds on. */
    lv_obj_t *screen() const { return screen_; }
    /* The container the page bodies are built in (NULL until the app gives it). */
    lv_obj_t *stage() const { return stage_; }
    /* The app's stage container (a child of its page); the platform deletes it with close_stage(). */
    void adopt_stage(lv_obj_t *stage) { stage_ = stage; }
    /* Deletes the stage and everything in it, and stops the keys: the app's end, while the app object still exists
     * (its widgets point into it, e.g. the lists). Also done by the destructor. */
    void close_stage();
    /* A point of the screen (an indev point) in stage coordinates. */
    lv_point_t to_stage(lv_point_t p) const;

    /* Every key the app acts on (presses and repeats), already translated. */
    std::function<void(const KeyEvent &)> on_key;
    /* Straight to on_key, without the keyboard queue (the scripted tests' bench). */
    void inject_key(const KeyEvent &e)
    {
        if (on_key) on_key(e);
    }

    /* A finger is on the screen (a pointer indev is pressed): lists are not rebuilt under it. */
    bool touching() const;
    /* A keyboard is known to be there without looking at /proc/bus/input/devices (the app's keyboard_present()
     * decides otherwise): only the scripted harness says so (set_keyboard_assumed), as the old headless mode did. */
    bool keyboard_connected() const { return keyboard_assumed_; }
    static void set_keyboard_assumed(bool on) { keyboard_assumed_ = on; }

    /* The platform of the running app (NULL before it exists or after it ended). */
    static Platform *current() { return current_; }

private:
    static void keyboard_cb(lv_event_t *event);

    lv_obj_t *screen_ = nullptr;
    lv_obj_t *stage_ = nullptr;
    KeyBridge keys_;
    int saved_intercept_ = 0;
    static Platform *current_;
    static bool keyboard_assumed_;
};

/* A point of the screen in stage coordinates (the running app's stage; unchanged without one). */
lv_point_t stage_point(lv_point_t screen_point);

/* Milliseconds on a monotonic clock. */
uint64_t mono_ms();

} // namespace meshhop
