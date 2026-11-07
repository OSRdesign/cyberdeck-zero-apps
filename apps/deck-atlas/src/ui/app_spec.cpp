/*
 * SPDX-License-Identifier: MIT
 *
 * What Deck Atlas gives the shared runners (_common/app.hpp): its id, title, minimum screen, minimum portrait canvas
 * and UI. min_w x min_h must equal X-MinSize and portrait_w x portrait_h X-Portrait in
 * root/usr/share/APPLaunch/applications/deck-atlas.desktop (tests/test_package.cpp checks both).
 */
#include "app.hpp"
#include "atlas_app.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "deck-atlas";
        s.title = "Deck Atlas";
        s.min_w = atlasui::kMinW;
        s.min_h = atlasui::kMinH;
        s.portrait_w = atlasui::kPortraitW;
        s.portrait_h = atlasui::kPortraitH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<atlasui::AtlasApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
