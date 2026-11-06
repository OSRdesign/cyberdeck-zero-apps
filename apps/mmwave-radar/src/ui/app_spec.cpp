/*
 * SPDX-License-Identifier: MIT
 *
 * What mmWave Radar gives the shared runners (_common/app.hpp): its id, title, minimum screen, minimum portrait canvas
 * and UI. min_w x min_h must equal X-MinSize and portrait_w x portrait_h X-Portrait in
 * root/usr/share/APPLaunch/applications/mmwave-radar.desktop (tests/test_package.cpp checks both).
 */
#include "app.hpp"
#include "radar_app.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "mmwave-radar";
        s.title = "mmWave Radar";
        s.min_w = mmwui::kMinW;
        s.min_h = mmwui::kMinH;
        s.portrait_w = mmwui::kPortraitW;
        s.portrait_h = mmwui::kPortraitH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<mmwui::RadarApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
