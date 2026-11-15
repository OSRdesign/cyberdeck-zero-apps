/*
 * SPDX-License-Identifier: MIT
 *
 * What VHS gives the shared runners (_common/app.hpp): its id, title, no minimum screen (no X-MinSize, D27), the
 * portrait canvas 320x480 (X-Portrait=320x480) and its UI (ui/vhs_app.hpp).
 */
#include "app.hpp"
#include "vhs_app.hpp"

#include <memory>

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "vhs";
        s.title = "VHS";
        s.portrait_w = 320;
        s.portrait_h = 480;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<vhs::ui::VhsApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
