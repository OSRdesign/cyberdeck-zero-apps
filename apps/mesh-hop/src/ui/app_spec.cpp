/*
 * SPDX-License-Identifier: MIT
 *
 * What Mesh Hop gives the shared runners (apps/_common/app.hpp): its id, title, minimum screen and UI. min_w x min_h
 * must equal X-MinSize in root/usr/share/APPLaunch/applications/mesh-hop.desktop. The UI is a Session (session.hpp).
 */

#include "app.hpp"
#include "session.hpp"

namespace meshhop {

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
    plat_.close();                  // no key reaches the app while it goes (its destructor deletes the shell and the pages)
}

void Session::tick_cb(lv_timer_t *timer)
{
    static_cast<Session *>(lv_timer_get_user_data(timer))->app_.tick();
}

Session *running_session()
{
    return g_session;
}

} // namespace meshhop

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "mesh-hop";
        s.title = "Mesh Hop";
        s.min_w = meshhop::kScreenW;
        s.min_h = meshhop::kScreenH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<meshhop::Session>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
