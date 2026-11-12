/*
 * SPDX-License-Identifier: MIT
 *
 * What the template gives the shared runners (_common/app.hpp): its id, title, minimum screen and UI.
 * The template has NO minimum screen (min 0 x 0, no X-MinSize line in its .desktop file): it fits every screen.
 * Declare a minimum only when your UI genuinely cannot fit a smaller screen: set tpl::kMinW / kMinH (src/core/
 * template_core.hpp) to the smallest size the layouts are tested at, e.g. 640 x 480, and add the same line
 * X-MinSize=640x480 to root/usr/share/APPLaunch/applications/cyberdeck-app-template.desktop. min_w x min_h must
 * always equal X-MinSize there (tests/test_core.cpp checks it, also "no X-MinSize == 0 x 0").
 */
#include "app.hpp"
#include "template_app.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "cyberdeck-app-template";
        s.title = "Template";
        s.min_w = tpl::kMinW;
        s.min_h = tpl::kMinH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<tpl::TemplateApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
