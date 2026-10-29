/*
 * SPDX-License-Identifier: MIT
 *
 * What Markdown Notes gives the shared runners (apps/_common/app.hpp): its id, title, minimum screens and UI. min_w x
 * min_h must equal X-MinSize and portrait_w x portrait_h X-Portrait in root/usr/share/APPLaunch/applications/markdown-notes.desktop (tests/unit/test_package.cpp
 * checks it). The title is the notice's ("Markdown Notes needs a screen of at least 640x480"); the Home page's header
 * says kHomeTitle ("Notes": the shared header cuts longer titles at 640x480).
 */
#include "app.hpp"
#include "session.hpp"

namespace notesui {

namespace {
Session *g_session = nullptr;
}

Session::Session(lv_obj_t *screen) : plat_(screen), app_(plat_)
{
    timer_ = lv_timer_create(tick_cb, kTickMs, this);
    g_session = this;
}

Session::~Session()
{
    if (g_session == this) g_session = nullptr;
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    plat_.close();
}

void Session::tick_cb(lv_timer_t *timer)
{
    static_cast<Session *>(lv_timer_get_user_data(timer))->app_.tick();
}

Session *running_session()
{
    return g_session;
}

} // namespace notesui

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "markdown-notes";
        s.title = notesui::kAppTitle;
        s.min_w = notesui::kMinW;
        s.min_h = notesui::kMinH;
        s.portrait_w = notesui::kPortraitW;
        s.portrait_h = notesui::kPortraitH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<notesui::Session>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
