/*
 * SPDX-License-Identifier: MIT
 *
 * What the System Monitor gives the shared runners (_common/app.hpp): its id, title, minimum screen and UI.
 * min_w x min_h must equal X-MinSize in root/usr/share/APPLaunch/applications/system-monitor.desktop
 * (tests/test_core.cpp checks it).
 */
#include "app.hpp"
#include "monitor_app.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = sm::kAppId;
        s.title = sm::kAppTitle;
        s.min_w = sm::kMinW;
        s.min_h = sm::kMinH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<sm::MonitorApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
