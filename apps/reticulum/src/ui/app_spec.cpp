/*
 * SPDX-License-Identifier: MIT
 *
 * What the Reticulum app gives the shared runners (_common/app.hpp): its id, title, minimum screen and UI.
 * No minimum screen (min 0 x 0, no X-MinSize in the .desktop file): the lists and forms scroll on any size.
 * It also has a portrait layout (the same pages on a taller canvas): X-Portrait=480x640 in the .desktop file and the same
 * numbers here (tests/test_core.cpp compares them). Tested at 480x640, 480x800 and 800x1280, with the writing band.
 */
#include "app.hpp"
#include "ret_app.hpp"

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "reticulum";
        s.title = "Reticulum";
        s.portrait_w = 480;
        s.portrait_h = 640;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<retui::App>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
