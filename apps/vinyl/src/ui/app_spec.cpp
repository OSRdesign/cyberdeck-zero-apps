/*
 * SPDX-License-Identifier: MIT
 *
 * What Vinyl gives the shared runners (_common/app.hpp): its id, title, minimum screen (X-MinSize 480x320: the smallest
 * drafts), minimum portrait canvas (X-Portrait 320x480) and UI (ui/vinyl_app.hpp).
 */
#include "app.hpp"
#include "vinyl_app.hpp"

#include <memory>

namespace vinyl {
namespace ui {
constexpr int kMinW = 480, kMinH = 320;
constexpr int kPortraitW = 320, kPortraitH = 480;
} // namespace ui
} // namespace vinyl

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "vinyl";
        s.title = "Vinyl";
        s.min_w = vinyl::ui::kMinW;
        s.min_h = vinyl::ui::kMinH;
        s.portrait_w = vinyl::ui::kPortraitW;
        s.portrait_h = vinyl::ui::kPortraitH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> { return std::make_shared<vinyl::ui::VinylApp>(screen); };
        return s;
    }();
    return spec;
}

} // namespace cdk
