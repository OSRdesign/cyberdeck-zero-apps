/*
 * SPDX-License-Identifier: MIT
 *
 * What RSS Reader gives the shared runners (apps/_common/app.hpp): its id, title, minimum screen and UI. min_w x min_h
 * must equal X-MinSize in root/usr/share/APPLaunch/applications/rss-reader.desktop (tests/unit/ui_tests.cpp checks it).
 */
#include "app.hpp"
#include "session.hpp"

namespace rssui {

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

} // namespace rssui

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "rss-reader";
        s.title = "RSS Reader";
        s.min_w = rssui::kScreenW;
        s.min_h = rssui::kScreenH;
        s.portrait_w = rssui::kPortraitW;
        s.portrait_h = rssui::kPortraitH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<rssui::Session>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
