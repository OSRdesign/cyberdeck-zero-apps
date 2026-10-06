/*
 * SPDX-License-Identifier: MIT
 *
 * Esc rule: a short Esc is always Back and never quits the app (the launcher quits on a 3 s hold by itself).
 * At a top-level screen it only shows a hint. Pure function, host-tested (src/tests/test_esc.cpp).
 */

#pragma once

namespace wifisurvey {

enum class View { Networks, Channels, Detail };

struct EscResult {
    bool hint;      // true: stay and show "Hold Esc 3 s to exit"
    View next;      // screen to show when hint is false
};

// tab: the top-level screen Detail was opened from
constexpr EscResult esc_short_press(View view, View tab)
{
    return view == View::Detail ? EscResult{false, tab} : EscResult{true, view};
}

constexpr const char *kEscHint = "Hold Esc 3 s to exit";

} // namespace wifisurvey
