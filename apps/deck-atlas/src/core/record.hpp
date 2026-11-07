/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the few fields of an imported archive's record (<store>/archives/<id>.conf, key=value lines written by
 * the engine, cp0_map_store.h "RECORD") that the engine's listing does not carry: the tile count, the import date and
 * what was imported. Readers ignore unknown keys; a missing key leaves its default.
 */
#pragma once

#include <cstdint>
#include <string>

namespace atlas {

struct ArchiveRecord {
    std::uint64_t tiles = 0;         /* 0 = not known */
    std::string imported;            /* UTC, "2026-11-07T10:20:30Z" */
    std::string source_path;         /* the file or folder that was imported */
};

ArchiveRecord parse_archive_record(const std::string &text);

} // namespace atlas
