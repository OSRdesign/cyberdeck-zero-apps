/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the one clock every stage uses (CLOCK_MONOTONIC, the clock uvcvideo stamps its buffers with).
 */
#pragma once

#include <cstdint>
#include <ctime>

namespace vhs {

inline std::int64_t mono_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

} // namespace vhs
