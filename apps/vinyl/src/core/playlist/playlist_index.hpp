/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl playlists: the library database's index of the playlist files (P2c, 0.4.0; D26: the files are the truth, the tables
 * `playlists` and `playlist_items` of schema v1 are a cache, so no migration). Everything here runs on the LibraryWorker's
 * thread with its Library.
 *
 *   playlist_sync     rebuilds the index from the folder: a file added, changed (its mtime in ns) or deleted outside Vinyl is
 *                     seen; unchanged files are not written again (SD wear). Called when the Playlists page opens and when a
 *                     scan ends.
 *   playlist_reindex  one playlist, forced (after Vinyl's own edit)
 *   playlist_list     the rows the Playlists page shows (name, tracks, length), from the index
 *   playlist_rows     the entries of one playlist as the edit page shows them: read from the FILE, named from the library, with
 *                     whether each file is there
 * LVGL-free.
 */
#pragma once

#include "library/library.hpp"
#include "m3u.hpp"
#include "playlists.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vinyl {

struct PlaylistRow {
    int64_t id = 0;
    std::string name, path;
    int tracks = 0;
    uint64_t ms = 0;
};

struct PlaylistEntryRow {
    m3u::Entry entry;
    std::string title, artist; /* from the library; else the #EXTINF text; else the file name */
    uint64_t duration_ms = 0;
    int64_t track_id = 0;      /* the library's row, 0 when it does not know the file */
    bool exists = false;       /* the file is there (a regular file) */
    std::string label() const; /* "Artist, Title" or the title alone */
};

struct PlaylistStats {
    int tracks = 0, missing = 0;
    uint64_t ms = 0;
};

bool playlist_sync(Library &lib, PlaylistStore &store);
bool playlist_reindex(Library &lib, PlaylistStore &store, const std::string &name);
bool playlist_list(Library &lib, std::vector<PlaylistRow> &out);
bool playlist_rows(Library &lib, const PlaylistStore &store, const std::string &name, std::vector<PlaylistEntryRow> &out,
                   PlaylistStats *stats = nullptr);
/* the paths of a playlist that can be played (the files that are there), in order; `start_row` (an index into the entries) is
 * mapped to its place in the result, -1 when that entry is missing */
std::vector<std::string> playlist_playable(const std::vector<PlaylistEntryRow> &rows, int start_row, int *start_out);
/* the tracks of the library as playlist entries (path, #EXTINF text, seconds) */
std::vector<m3u::Entry> entries_from_tracks(const std::vector<TrackRow> &tracks);
/* paths that may or may not be in the library, as entries (with their #EXTINF when the library knows them) */
std::vector<m3u::Entry> entries_from_paths(Library &lib, const std::vector<std::string> &paths);

}  // namespace vinyl
