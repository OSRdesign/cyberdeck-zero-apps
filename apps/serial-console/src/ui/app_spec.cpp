/*
 * SPDX-License-Identifier: MIT
 *
 * What the Serial Console gives the shared runners (_common/app.hpp): its id, title, minimum screen and UI.
 * min_w x min_h must equal X-MinSize in root/usr/share/APPLaunch/applications/serial-console.desktop
 * (tests/test_core.cpp checks it).
 */
#include "app.hpp"
#include "serial_app.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "serial-console";
        s.title = sc::kAppTitle;
        s.min_w = sc::kMinW;
        s.min_h = sc::kMinH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<sc::SerialApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
