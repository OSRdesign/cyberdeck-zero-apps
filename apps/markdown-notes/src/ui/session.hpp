/*
 * SPDX-License-Identifier: MIT
 *
 * What the shared runners build for Markdown Notes (app_spec.cpp; cdk::start on the board and in the headless harness):
 * the keyboard bridge (stage.hpp), the app (notes_app.hpp) and the LVGL timer that runs App::tick every kTickMs (the
 * autosave, the 3 s hold, the notices, the redraws). RSS Reader's design.
 */
#pragma once

#include "notes_app.hpp"
#include "stage.hpp"

#include "lvgl/lvgl.h"

namespace notesui {

constexpr uint32_t kTickMs = 20;

class Session {
public:
    explicit Session(lv_obj_t *screen);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    Platform &platform() { return plat_; }
    App &app() { return app_; }

private:
    static void tick_cb(lv_timer_t *timer);

    Platform plat_;                   // first: the keys reach the app through it
    App app_;
    lv_timer_t *timer_ = nullptr;
};

/* The running session (NULL before cdk::start built it or after cdk::stop): the scripted harness reads the app here. */
Session *running_session();

} // namespace notesui
