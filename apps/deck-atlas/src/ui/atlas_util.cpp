/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: shared UI helpers (see atlas_util.hpp).
 */
#include "atlas_util.hpp"

#include "record.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace atlasui {

void log_line(const char *format, ...)
{
    std::fputs("[deck-atlas] ", stderr);
    va_list ap;
    va_start(ap, format);
    std::vfprintf(stderr, format, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

std::int64_t mono_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

int list_store(cp0_map_store_t *store, const std::string &store_dir, std::vector<ArchiveItem> &archives,
               std::vector<OnlineItem> &online)
{
    archives.clear();
    online.clear();
    if (!store) return CP0_MAP_E_INVALID;
    int n = cp0_map_store_sources(store, nullptr, 0);
    if (n < 0) return n;
    std::vector<cp0_map_source_info_t> all(static_cast<std::size_t>(n) + 4);
    n = cp0_map_store_sources(store, all.data(), static_cast<int>(all.size()));
    if (n < 0) return n;
    n = std::min(n, static_cast<int>(all.size()));
    for (int i = 0; i < n; ++i) {
        const cp0_map_source_info_t &s = all[static_cast<std::size_t>(i)];
        if (s.kind == CP0_MAP_ENTRY_ARCHIVE) {
            ArchiveItem a;
            a.id = s.id;
            a.title = s.title[0] ? s.title : s.id;
            a.path = s.path;
            a.attribution = s.attribution;
            a.state = s.state;
            a.mode = static_cast<int>(s.mode);
            a.minzoom = s.minzoom;
            a.maxzoom = s.maxzoom;
            a.tile_px = s.tile_px;
            a.has_bounds = s.has_bounds != 0;
            a.bounds = {s.bounds.west, s.bounds.south, s.bounds.east, s.bounds.north};
            a.bytes = s.bytes;
            std::string rec;
            if (atlas::read_small_file(store_dir + "/archives/" + a.id + ".conf", rec)) {
                const atlas::ArchiveRecord r = atlas::parse_archive_record(rec);
                a.tiles = r.tiles;
                a.imported = r.imported;
                a.source_path = r.source_path;
            }
            archives.push_back(std::move(a));
        } else if (s.kind == CP0_MAP_ENTRY_SOURCE) {
            OnlineItem o;
            o.facts.id = s.id;
            o.facts.title = s.title[0] ? s.title : s.id;
            o.facts.enabled = s.enabled != 0;
            o.facts.needs_key = s.policy.needs_key != 0;
            o.facts.has_key = s.has_key != 0;
            o.facts.state = s.state;
            o.facts.until = s.until;
            o.facts.fetches_today = s.fetches_today;
            o.facts.daily_cap = s.policy.daily_cap;
            o.facts.cache_tiles = s.cache_tiles;
            o.licence = s.licence;
            o.note = s.note;
            o.attribution = s.attribution;
            o.shipped = s.shipped != 0;
            online.push_back(std::move(o));
        }
    }
    return CP0_MAP_OK;
}

std::string plain_text(const std::string &s)
{
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA9) {
            o += "(C)";
            ++i;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string import_error_text(int rc)
{
    switch (rc) {
    case CP0_MAP_E_EXISTS: return "This map is already imported (remove it first to import it again)";
    case CP0_MAP_E_NOT_FOUND: return "The file or folder is gone (a USB stick pulled out?)";
    case CP0_MAP_E_UNSUPPORTED:
        return "Not supported: vector (pbf) and WebP maps cannot be shown yet; use PNG or JPEG raster tiles";
    case CP0_MAP_E_FORMAT: return "Not an MBTiles or PMTiles file, or not PNG / JPEG tiles";
    case CP0_MAP_E_NO_TILE: return "No tile found in this folder (expected <zoom>/<x>/<y>.png)";
    case CP0_MAP_E_SQLITE_MISSING: return "SQLite library missing: install libsqlite3-0";
    case CP0_MAP_E_IO: return "A read or write failed (the card full, or the stick pulled out?)";
    case CP0_MAP_E_READ_ONLY: return "Another app holds the map folder: try again when it is closed";
    default: return cp0_map_strerror(rc);
    }
}

} // namespace atlasui
