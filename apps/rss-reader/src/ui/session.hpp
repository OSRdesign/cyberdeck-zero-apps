/*
 * SPDX-License-Identifier: MIT
 *
 * What the shared runners build for RSS Reader (app_spec.cpp; cdk::start on the board and in the headless harness): the
 * keyboard bridge (stage.hpp), the app (rss_app.hpp) and the LVGL timer that runs App::tick every kTickMs (the work the
 * worker threads posted, the 3 s hold, the ages that move on, the redraws).
 */
#pragma once

#include "rss_app.hpp"
#include "stage.hpp"

#include "lvgl/lvgl.h"

namespace rssui {

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

} // namespace rssui
