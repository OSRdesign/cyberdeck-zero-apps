/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: loading and saving one note (report 037 section 2.4).
 *
 *   load_note()   reads the file (at most kReadCap), checks UTF-8, decodes it (text_codec.hpp) and remembers what it read
 *                 (DiskStamp). Over kEditCap, or not valid UTF-8: read-only. Over kReadCap: refused.
 *   check_disk()  has the file changed since we read or wrote it? A changed size says yes; otherwise the content hash
 *                 decides (a touch, or a sync that rewrote the same bytes, is not a change; a same-size edit within the
 *                 file system's mtime granularity still is). Gone: the file was deleted or moved away.
 *   save_note()   check_disk() first: Changed -> Conflict, nothing written (the UI asks Keep both / Reload); otherwise the
 *                 atomic write (fsutil.hpp) and a new stamp. Gone -> the note is written again (re-created).
 *   conflict_copy_path()  "Keep both": a free "<name> (deck 2026-10-24 1530).md" beside the note, for our text.
 *   create_note()  a new note that must not exist (O_EXCL), with its folders.
 */
#pragma once

#include "text_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

namespace mdnotes {

constexpr std::size_t kEditCap = 128 * 1024;    /* bytes: larger notes open read-only */
constexpr std::size_t kReadCap = 1024 * 1024;   /* bytes: larger files are refused */

std::uint64_t content_hash(const std::string &bytes);   /* FNV-1a 64 */

struct DiskStamp {
    bool exists = false;
    std::int64_t mtime_ns = 0;
    std::int64_t size = -1;
    std::uint64_t hash = 0;   /* of the file's bytes as read or written */
};

enum class LoadStatus {
    Ok,               /* editable */
    ReadOnlyLarge,    /* over kEditCap: preview only */
    ReadOnlyNotUtf8,  /* not valid UTF-8: preview only (the bytes are never rewritten) */
    TooBig,           /* over kReadCap: not opened */
    Missing,
    Error             /* err holds errno */
};

struct LoadedNote {
    LoadStatus status = LoadStatus::Missing;
    Decoded body;      /* empty unless Ok / ReadOnly* */
    DiskStamp stamp;
    int err = 0;
    bool readable() const { return status == LoadStatus::Ok || status == LoadStatus::ReadOnlyLarge || status == LoadStatus::ReadOnlyNotUtf8; }
    bool editable() const { return status == LoadStatus::Ok; }
};

LoadedNote load_note(const std::string &path);

enum class DiskState { Same, Changed, Gone };
/* `stamp` is what we read or wrote last; Same refreshes its mtime (a touch). */
DiskState check_disk(const std::string &path, DiskStamp &stamp);

enum class SaveStatus { Ok, Conflict, Error };
struct SaveResult {
    SaveStatus status = SaveStatus::Error;
    int err = 0;
    bool recreated = false;   /* the file was gone and was written again */
};
/* body: LF text with real tabs (from_edit() of the editor's text). force: write even over a changed file (Reload
 * declined, or after Keep both wrote the copy elsewhere: never used for that). */
SaveResult save_note(const std::string &path, const std::string &body, const TextFormat &fmt, DiskStamp &stamp,
                     bool force = false);

/* "<dir>/<name> (deck YYYY-MM-DD HHMM).md", then " 2", " 3"... until the name is free (local time). */
std::string conflict_copy_path(const std::string &path, std::time_t now);

/* A new note: its folders created, then the file with O_EXCL (EEXIST when the name is taken). stamp is set. */
bool create_note(const std::string &path, const std::string &body, const TextFormat &fmt, DiskStamp *stamp, int *err);

} // namespace mdnotes
