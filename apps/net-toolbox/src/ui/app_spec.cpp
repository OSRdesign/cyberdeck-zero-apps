/*
 * SPDX-License-Identifier: MIT
 *
 * What Net Toolbox gives the shared runners (_common/app.hpp): its id, title, minimum screen and UI.
 * min_w x min_h must equal X-MinSize in root/usr/share/APPLaunch/applications/net-toolbox.desktop
 * (tests/test_core.cpp checks it).
 */
#include "app.hpp"
#include "toolbox.hpp"
#include "toolbox_app.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = nt::kAppId;
        s.title = nt::kAppTitle;
        s.min_w = nt::kMinW;
        s.min_h = nt::kMinH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<nt::ToolboxApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
