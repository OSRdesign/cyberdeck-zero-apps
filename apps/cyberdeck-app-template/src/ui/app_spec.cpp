/*
 * SPDX-License-Identifier: MIT
 *
 * What the template gives the shared runners (_common/app.hpp): its id, title, minimum screen and UI.
 * min_w x min_h must equal X-MinSize in root/usr/share/APPLaunch/applications/cyberdeck-app-template.desktop
 * (tests/test_core.cpp checks it).
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
