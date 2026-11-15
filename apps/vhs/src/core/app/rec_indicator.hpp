/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the REC tag's start indicator (M4, the owner's camcorder idea, decided by the Controller 2026-11-15): from the REC
 * press (or the self-timer's zero) until the first frame is written the red dot blinks, 500 ms on and 500 ms off,
 * starting lit; the tag reads "REC 00:00:00", the Sound-on lamp is hidden and "Photo now" is dimmed. Once the first
 * frame is written the dot is solid and the timer runs. Pure: the time is passed in (the UI's frame timer passes the
 * monotonic clock), unit-tested. LVGL-free.
 */
#pragma once

#include <cstdint>

namespace vhs {

constexpr std::int64_t kRecBlinkMs = 500;

/* The dot at now_ms of a start that began at since_ms: lit during the even half-seconds. */
inline bool rec_dot_lit(std::int64_t now_ms, std::int64_t since_ms)
{
    const std::int64_t d = now_ms - since_ms;
    return d < 0 || (d / kRecBlinkMs) % 2 == 0;
}

/* The REC tag's dot: solid once recording (the first frame written), blinking while starting. */
inline bool rec_dot_shown(bool starting, std::int64_t now_ms, std::int64_t since_ms)
{
    return !starting || rec_dot_lit(now_ms, since_ms);
}

} // namespace vhs
