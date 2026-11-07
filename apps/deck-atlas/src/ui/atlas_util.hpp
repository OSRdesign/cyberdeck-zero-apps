/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: small helpers the UI files share: the colours (the launcher toolkit's palette), the log line, the
 * monotonic clock, a plain LVGL object, and the store listing (the engine's records turned into ArchiveItem /
 * OnlineItem; called on the worker, and once at the start).
 */
#pragma once

#include "atlas_app.hpp"

#include "lvgl/lvgl.h"

#include <cstdint>
#include <string>
#include <vector>

namespace atlasui {

namespace pal {
constexpr std::uint32_t kText = 0xFFFFFF;
constexpr std::uint32_t kMuted = 0x9A9A9A;   /* labels, notes (cp0_ui_list.h section grey) */
constexpr std::uint32_t kBar = 0x2B2B2B;     /* the active tab, borders */
constexpr std::uint32_t kStrip = 0x111111;   /* the tab strip */
constexpr std::uint32_t kPanel = 0x1C1C1C;   /* buttons */
constexpr std::uint32_t kPressed = 0x3A3A3A;
constexpr std::uint32_t kPlate = 0x1C1C1C;   /* the "No map here" plate */
constexpr std::uint32_t kAccent = 0x5AAAFF;  /* the active tab's mark, the progress bar */
constexpr std::uint32_t kOk = 0x66CC88;
constexpr std::uint32_t kWarn = 0xF0B400;
constexpr std::uint32_t kBad = 0xFF6666;
} // namespace pal

void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
std::int64_t mono_ms();
void plain(lv_obj_t *o);

/* The store's archives (with the engine's tile counts) and online sources. 0 or an engine error. */
int list_store(cp0_map_store_t *store, std::vector<ArchiveItem> &archives,
               std::vector<OnlineItem> &online);

/* A friendly text for an engine result of an import or a removal. */
std::string import_error_text(int rc);
/* The builtin fonts have no copyright sign: "(C)" instead, as the map widget draws attributions. */
std::string plain_text(const std::string &s);

} // namespace atlasui
