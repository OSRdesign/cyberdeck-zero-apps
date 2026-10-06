/*
 * SPDX-License-Identifier: MIT
 *
 * Esc rule: a short Esc is always Back and never quits the app (the launcher quits on a 3 s hold by itself).
 * At a top-level screen it only shows a hint. Pure function, host-tested (src/tests/test_esc.cpp).
 */

#pragma once

namespace lanscan {

enum class View { Hosts, Ports };

struct EscResult {
    bool hint;      // true: stay and show "Hold Esc 3 s to exit"
    View next;      // screen to show when hint is false
};

constexpr EscResult esc_short_press(View view)
{
    return view == View::Ports ? EscResult{false, View::Hosts} : EscResult{true, View::Hosts};
}

constexpr const char *kEscHint = "Hold Esc 3 s to exit";

} // namespace lanscan
