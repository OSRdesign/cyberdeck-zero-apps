/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: the settings (no LVGL; tests/test_store.cpp). store.hpp keeps them in settings.json.
 *
 *   preset                  the difficulty of the next game; its board comes from the screen (Session::dims_for).
 *   fair                    lay boards the solver can finish without guessing.
 *   animations              On / Reduced / Off.
 *   flag_mode               the UI starts in "tap flags" mode.
 *   show_timer              the clock is drawn (it always runs).
 *   first_tap_opens         always true: the first dug cell and its neighbours are free of mines.
 *   JSON: {"version":1,"preset":"beginner","fair":false,
 *   "animations":"on","flag_mode":false,"show_timer":true}. Missing members keep their defaults; numbers are clamped.
 */
#pragma once

#include "board.hpp"

#include <string>

namespace minefield {

enum class Animations { On, Reduced, Off };

struct Settings {
    Preset preset = Preset::Beginner;
    bool fair = false;
    Animations animations = Animations::On;
    bool flag_mode = false;
    bool show_timer = true;
    static constexpr bool first_tap_opens = true;

    bool operator==(const Settings &o) const;

    std::string to_json() const;
    /* false: not JSON, wrong type, or a version this build does not know (0 or newer than 1) */
    static bool from_json(const std::string &text, Settings &out, std::string *why = nullptr);
};

const char *animations_name(Animations a);
bool parse_animations(const std::string &name, Animations &out);

} // namespace minefield
