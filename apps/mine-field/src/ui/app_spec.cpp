/*
 * SPDX-License-Identifier: MIT
 *
 * What Mine Field gives the shared runners (_common/app.hpp): its id, title, minimum screen, minimum portrait canvas and UI.
 * min_w x min_h must equal X-MinSize and portrait_w x portrait_h X-Portrait in
 * root/usr/share/APPLaunch/applications/mine-field.desktop (tests/test_core.cpp checks both).
 */
#include "app.hpp"
#include "minefield_app.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "mine-field";
        s.title = "Mine Field";
        s.min_w = mfui::kMinW;
        s.min_h = mfui::kMinH;
        s.portrait_w = mfui::kPortraitW;
        s.portrait_h = mfui::kPortraitH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<mfui::MineFieldApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
