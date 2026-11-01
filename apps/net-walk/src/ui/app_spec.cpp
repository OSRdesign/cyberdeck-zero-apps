/*
 * SPDX-License-Identifier: MIT
 *
 * What Net Walk gives the shared runners (_common/app.hpp): its id, title, minimum screen, minimum portrait canvas and UI.
 * min_w x min_h must equal X-MinSize and portrait_w x portrait_h X-Portrait in
 * root/usr/share/APPLaunch/applications/net-walk.desktop (tests/test_package.cpp checks both).
 */
#include "app.hpp"
#include "netwalk_app.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "net-walk";
        s.title = "Net Walk";
        s.min_w = nwui::kMinW;
        s.min_h = nwui::kMinH;
        s.portrait_w = nwui::kPortraitW;
        s.portrait_h = nwui::kPortraitH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<nwui::NetWalkApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
