/*
 * SPDX-License-Identifier: MIT
 *
 * What the viz1090 intro gives the shared runners (_common/app.hpp): its id, title, minimum screen and UI.
 * min_w x min_h must equal X-MinSize in root/usr/share/APPLaunch/applications/viz1090.desktop (tests/test_core.cpp).
 */
#include "app.hpp"
#include "viz_app.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = viz::kAppId;
        s.title = viz::kAppTitle;
        s.min_w = viz::kMinW;
        s.min_h = viz::kMinH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<viz::VizApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
