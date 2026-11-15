/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl playlists: reading and writing M3U / M3U8 (P2c, 0.4.0; report 060 2.4 and 3g, D26). Replaces P1's parse_m3u (which stays
 * as a thin wrapper in core/library/browse). LVGL-free, no database.
 *
 * Reading (parse):
 *   - UTF-8 with or without a byte-order mark; when the bytes are not valid UTF-8 they are taken as Windows-1252 (Latin-1 plus
 *     the printable 0x80-0x9F), the common .m3u of old players; CR LF, LF and a lone CR end lines
 *   - backslashes are separators; a Windows drive letter ("D:\Music\x.mp3") is resolved against the playlist's own folder and the
 *     known roots by dropping leading folders until a file is found; a `file://` URL (percent escapes decoded) is its path;
 *     `http(s)://` and every other URL is skipped
 *   - relative paths are resolved against the playlist's folder and made lexical (no "." or ".." left)
 *   - `#EXTINF:seconds,Artist - Title` is kept for the entry that follows; every other `#` line is ignored
 *   - a file that is not there is KEPT (the entry's path is the best guess, or the original text when nothing could be mapped):
 *     the pages draw it dimmed and playing skips it; only Remove takes it out
 * Writing (write): UTF-8, `#EXTM3U`, `#EXTINF:sec,text` for an entry that has one, LF line ends; an entry's path is relative to
 * the playlist's folder when both are on the same drive (`same_drive`), else absolute; an entry that could not be resolved is
 * written as it was read. LVGL-free.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vinyl {
namespace m3u {

struct Entry {
    std::string path;       /* absolute, lexical; or the text as read when `resolved` is false */
    std::string text;       /* the #EXTINF text ("Artist - Title"), "" when none */
    int seconds = -1;       /* the #EXTINF length, -1 unknown */
    bool resolved = true;   /* false: a path that could not be mapped to this machine (a Windows drive that is not here) */
    bool operator==(const Entry &o) const {
        return path == o.path && text == o.text && seconds == o.seconds && resolved == o.resolved;
    }
};

struct ParseOptions {
    std::string base_dir;                              /* the playlist's own folder ("" : relative paths stay relative to "/") */
    std::vector<std::string> roots;                    /* the folders music lives in (a drive-letter path is looked up under them) */
    std::function<bool(const std::string &)> exists;   /* is this a file: null means the real one (stat) */
    size_t max_entries = 5000;
};

/* The text as UTF-8: the byte-order mark dropped, Windows-1252 bytes converted when the whole is not valid UTF-8. */
std::string decode_text(const std::string &bytes, bool *was_latin1 = nullptr);
bool valid_utf8(const std::string &s);

std::vector<Entry> parse(const std::string &bytes, const ParseOptions &opt);

/* `path` with "." and ".." resolved lexically, doubled slashes dropped ("/a/b/../c" -> "/a/c"); the root stays "/" */
std::string normalize(const std::string &path);
/* `path` relative to the folder `dir` ("../x/y.mp3"); both absolute and normalized; "" when they share nothing but "/" */
std::string relative_to(const std::string &dir, const std::string &path);
/* the folder part of a path ("" when it has none) */
std::string dir_of(const std::string &path);

struct WriteOptions {
    std::string playlist_dir;                                              /* where the file will be */
    std::function<bool(const std::string &a, const std::string &b)> same_drive; /* null: always the same drive (relative) */
};
std::string write(const std::vector<Entry> &entries, const WriteOptions &opt);

/* "04:01"-style helper the pages share: the playlist's length from its entries is the library's job; this is just the sum of the
 * known #EXTINF seconds (-1 when none is known) */
int64_t total_seconds(const std::vector<Entry> &entries);

}  // namespace m3u
}  // namespace vinyl
