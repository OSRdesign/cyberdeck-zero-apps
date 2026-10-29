/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader on the shared app platform (apps/_common): cdk::run on the boards, the headless harness on the PC. What the
 * app needs from it is here (Mesh Hop's stage.hpp, the same design):
 *
 *   the keys      cp0_lvgl's key_item (LV_EVENT_KEYBOARD on the screen, the LVGL keypad path intercepted) is turned into
 *                 the app's KeyEvent by KeyBridge (key_bridge.hpp).
 *   the touch     whether a finger is on the screen.
 */
#pragma once

#include "key_bridge.hpp"

#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>

namespace rssui {

/* The smallest screen (X-MinSize of the .desktop file). */
constexpr int kScreenW = 640;
constexpr int kScreenH = 480;
/* The smallest portrait canvas (X-Portrait of the .desktop file, PDA mode). */
constexpr int kPortraitW = 480;
constexpr int kPortraitH = 640;

class Platform {
public:
    explicit Platform(lv_obj_t *screen);
    ~Platform();
    Platform(const Platform &) = delete;
    Platform &operator=(const Platform &) = delete;

    lv_obj_t *screen() const { return screen_; }
    /* Stops the keys (the app's end). Also done by the destructor. */
    void close();

    /* Every key the app acts on (presses and repeats), already translated. */
    std::function<void(const KeyEvent &)> on_key;
    void inject_key(const KeyEvent &e)
    {
        if (on_key) on_key(e);
    }
    /* A finger is on the screen (a pointer indev is pressed). */
    bool touching() const;

private:
    static void keyboard_cb(lv_event_t *event);

    lv_obj_t *screen_ = nullptr;
    KeyBridge keys_;
    int saved_intercept_ = 0;
};

/* Milliseconds on a monotonic clock. */
std::uint64_t mono_ms();

} // namespace rssui
