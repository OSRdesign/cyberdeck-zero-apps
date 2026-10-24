/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes: what the scripted tests may replace before the app starts (tests/flows/script_runner.cpp): the clock
 * (a fixed time: stable dates, ages and stamps in the shots, the daily note's name). The device build never sets it.
 */
#pragma once

#include <cstdint>
#include <functional>

namespace notesui {

struct Hooks {
    /* Epoch seconds. */
    std::function<std::int64_t()> now;
};

Hooks &hooks();

/* hooks().now, else the real time. */
std::int64_t now_epoch();

} // namespace notesui
