/*
 * SPDX-License-Identifier: MIT
 *
 * What the Home Assistant app gives the shared runners (_common/app.hpp): its id, title, minimum screen, minimum
 * portrait canvas and UI. min_w x min_h must equal X-MinSize and portrait_w x portrait_h X-Portrait in
 * root/usr/share/APPLaunch/applications/home-assistant.desktop (tests/test_package.cpp checks both).
 */
#include "app.hpp"
#include "ha_app.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "home-assistant";
        s.title = "Home Assistant";
        s.min_w = haui::kMinW;
        s.min_h = haui::kMinH;
        s.portrait_w = haui::kPortraitW;
        s.portrait_h = haui::kPortraitH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<haui::HaApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
