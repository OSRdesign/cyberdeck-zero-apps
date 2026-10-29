/*
 * SPDX-License-Identifier: MIT
 *
 * What Klondike Solitaire gives the shared runners (_common/app.hpp): its id, title, minimum screen, minimum portrait
 * canvas and UI. min_w x min_h must equal X-MinSize and portrait_w x portrait_h X-Portrait in
 * root/usr/share/APPLaunch/applications/klondike-solitaire.desktop (tests/test_core.cpp checks both).
 */
#include "app.hpp"
#include "klondike_app.hpp"
#include "rules.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "klondike-solitaire";
        s.title = "Klondike Solitaire";
        s.min_w = klondike::kMinW;
        s.min_h = klondike::kMinH;
        s.portrait_w = klondike::kPortraitW;
        s.portrait_h = klondike::kPortraitH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<kui::KlondikeApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
