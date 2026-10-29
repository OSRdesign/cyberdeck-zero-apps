/*
 * SPDX-License-Identifier: MIT
 *
 * What Net Toolbox gives the shared runners (_common/app.hpp): its id, title, minimum screen, minimum portrait canvas
 * and UI. min_w x min_h must equal X-MinSize in root/usr/share/APPLaunch/applications/net-toolbox.desktop, portrait_w x
 * portrait_h its X-Portrait (tests/test_core.cpp checks both).
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
        s.portrait_w = nt::kPortraitW; /* PDA mode: upright on a portrait canvas of at least 480x640 (0.5.0) */
        s.portrait_h = nt::kPortraitH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<nt::ToolboxApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
