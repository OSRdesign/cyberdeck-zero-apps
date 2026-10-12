/*
 * SPDX-License-Identifier: MIT
 *
 * THE swap point of the status strip (see status_strip.hpp). Every app reaches cp0::ui::StatusStrip through this
 * file only.
 *
 * Today (L1 not merged): cp0_lvgl has no StatusStrip, so stub/cp0_ui_status_strip_stub.hpp declares the API L1 is expected
 * to publish and stub/cp0_ui_status_strip_stub.cpp implements it with cp0_lvgl's own strip renderer
 * (cp0_statusbar.h). When L1 lands: delete stub/, include L1's header below and adapt the three calls to its real
 * names. Nothing else in the apps changes.
 */
#include "status_strip.hpp"

#if __has_include(<cp0_ui_status_strip.hpp>)
#include <cp0_ui_status_strip.hpp> /* L1: cp0_lvgl publishes it (name guessed: adapt when it lands) */
#else
#include "stub/cp0_ui_status_strip_stub.hpp"
#endif

namespace cdk {
namespace status_strip {

void attach(lv_obj_t *header)
{
    cp0::ui::StatusStrip::attach(header);
}

int left()
{
    return cp0::ui::StatusStrip::left();
}

cp0::ui::Page::TitleFactory title_factory()
{
    return [](lv_obj_t *parent, const char *text) { return cp0::ui::StatusStrip::add_title(parent, text); };
}

} // namespace status_strip
} // namespace cdk
