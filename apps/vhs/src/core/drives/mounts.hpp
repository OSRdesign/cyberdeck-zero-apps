/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Vinyl 0.3.0 (apps/vinyl at origin/main fdad829) for VHS (brief 3.1, 9.3); VHS changes marked VHS.
 *
 * The lines of /proc/self/mounts. VHS: Vinyl keeps these two functions in library/roots.cpp (with the library's roots and
 * its database); VHS has no library, so only MountEntry, parse_mounts and read_mounts come over, unchanged, and drives.cpp
 * includes this file where Vinyl's includes roots.hpp. LVGL-free.
 */
#pragma once

#include <string>
#include <vector>

namespace vhs {

struct MountEntry {
    std::string dev, dir, type;
};

/* The lines of /proc/self/mounts (octal escapes such as \040 undone). */
std::vector<MountEntry> parse_mounts(const std::string &text);
/* The text of /proc/self/mounts ("" when unreadable). */
std::string read_mounts();

}  // namespace vhs
