/*
 * SPDX-License-Identifier: MIT
 *
 * The full-screen platform of Mesh Hop: LVGL on the 640x480 framebuffer (/dev/fb0), the touch screen and the Bluetooth
 * keyboard read straight from evdev, the same way the viz1090 bridge does (the launcher stops drawing and reading input while
 * a full-screen app runs; it only watches Esc to end the app after 3 s). A headless mode renders into memory and takes input
 * from a script: it is how the app is tested on a PC and in screenshots.
 */

#pragma once

#include "ui_logic.hpp"

#include "lvgl/lvgl.h"

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace meshhop {

constexpr int kScreenW = 640;
constexpr int kScreenH = 480;

class Platform {
public:
    Platform();
    ~Platform();
    Platform(const Platform &) = delete;
    Platform &operator=(const Platform &) = delete;

    /* lv_init, the display and the touch input. headless: no framebuffer and no evdev. False (and a message on stderr) when
     * the framebuffer cannot be opened. */
    bool init(bool headless);
    bool headless() const { return headless_; }

    /* Reads the keyboard and the touch screen (non-blocking) and calls on_key. */
    void poll_input();
    /* Sleeps up to ms, less when input arrives. */
    void wait(int ms);

    std::function<void(const KeyEvent &)> on_key;

    /* A finger is on the screen (or its samples are not read yet): lists are not rebuilt under it. */
    bool touching() const { return last_pressed_ || p_pressed_ || !samples_.empty(); }
    /* A keyboard is attached and awake (always true headless). */
    bool keyboard_connected() const { return headless_ || kbd_fd_ >= 0; }

    // ---- headless injection and screenshots
    void inject_touch(bool pressed, int x, int y);
    void inject_key(const KeyEvent &e);
    bool screenshot(const std::string &path) const;

    // used by the LVGL callbacks
    void flush(const lv_area_t *area, const uint8_t *px);
    void read_pointer(lv_indev_data_t *data);

private:
    struct Sample {
        bool pressed;
        int x, y;
    };
    bool open_fb();
    void open_touch();
    void open_keyboard();
    void read_touch();
    void read_keyboard();
    void push_sample(bool pressed, int x, int y);

    bool headless_ = false;
    lv_display_t *disp_ = nullptr;
    lv_indev_t *indev_ = nullptr;
    uint8_t *buf1_ = nullptr, *buf2_ = nullptr;

    // framebuffer
    int fb_fd_ = -1;
    uint8_t *fb_map_ = nullptr;
    size_t fb_size_ = 0;
    int fb_stride_ = 0, fb_bytes_ = 4, fb_w_ = 0, fb_h_ = 0;
    std::vector<uint32_t> shadow_;      // headless: the whole screen

    // touch
    int touch_fd_ = -1;
    long touch_retry_ = 0;
    int min_x_ = 0, max_x_ = 0, min_y_ = 0, max_y_ = 0;
    bool swap_xy_ = true, inv_x_ = false, inv_y_ = true;
    int slot_ = 0;
    bool t_active_ = false;
    int t_x_ = 0, t_y_ = 0;
    bool last_pressed_ = false;          // what LVGL has read
    bool p_pressed_ = false;             // what was queued last
    int p_x_ = 0, p_y_ = 0;
    int last_x_ = 0, last_y_ = 0;
    std::deque<Sample> samples_;

    // keyboard
    int kbd_fd_ = -1;
    long kbd_retry_ = 0;
    KeyTranslator keys_;
};

/* Milliseconds on a monotonic clock. */
uint64_t mono_ms();

} // namespace meshhop
