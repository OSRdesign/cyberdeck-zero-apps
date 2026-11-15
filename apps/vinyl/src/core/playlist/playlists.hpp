/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl playlists: the files (P2c, 0.4.0; D26: "Playlists are M3U8 files in ~/Music/Playlists"; the library database only
 * indexes them, core/playlist/playlist_index). One PlaylistStore is one folder of `<name>.m3u8` files:
 *
 *   names      letters, digits, space, - and _ (the letters of every alphabet count), 1..24 characters, as Cassette's prefix
 *              rules; a typed name is checked (name_problem), a name made from a file name or a default is cleaned
 *              (clean_name); a name that is taken (compared without case) is refused
 *   writes     atomic: the new text goes to a hidden temporary file in the same folder, is flushed, then renamed over the old
 *              file, so an interrupted write leaves the old file whole (and a leftover temporary file, swept at the next list)
 *   edits      append, move one entry, remove one entry, rename, delete: each reads the file, changes the entry list and writes
 *              it again; an entry that is not on disk any more stays (core/playlist/m3u: missing entries are kept)
 *   import     a .m3u/.m3u8 file from anywhere becomes a playlist of this folder (paths relative or absolute as the writer
 *              rules say); export writes a playlist to a folder, relative paths when the tracks are on that drive
 * No database and no LVGL here: tests/test_playlists.cpp.
 */
#pragma once

#include "m3u.hpp"

#include "library/media_guard.hpp"

#include <functional>
#include <string>
#include <vector>

namespace vinyl {

enum class PlResult { Ok, Taken, NotFound, Invalid, Io, Range };
const char *pl_result_text(PlResult r);

enum class NameProblem { None, Empty, TooLong, BadChar };

class PlaylistStore {
public:
    static constexpr int kMaxName = 24;

    /* roots: the music folders (a drive-letter path of an imported file is looked up under them) */
    explicit PlaylistStore(std::string dir, std::vector<std::string> roots = {}, std::vector<DriveBase> bases = default_drive_bases());
    const std::string &dir() const { return dir_; }
    void set_roots(std::vector<std::string> roots) { roots_ = std::move(roots); }
    const std::vector<std::string> &roots() const { return roots_; }

    /* ---- names */
    /* the number of characters (code points) of a name */
    static int name_length(const std::string &name);
    /* trims the ends; why a name is refused ("" none): empty, longer than 24 characters, a character that is not allowed */
    static NameProblem name_problem(const std::string &name);
    static std::string trim_name(const std::string &name);
    /* a name made from any text: bad characters become spaces, runs of spaces one, cut to 24 characters, "Playlist" when empty */
    static std::string clean_name(const std::string &raw);
    /* "Queue" when free, else "Queue 2", "Queue 3" ... (cut so the whole stays within 24 characters) */
    std::string free_name(const std::string &base) const;

    std::string path_of(const std::string &name) const;
    /* a playlist of this name (compared without case): its own spelling, "" when none */
    std::string find(const std::string &name) const;
    bool exists(const std::string &name) const { return !find(name).empty(); }
    /* the playlists, A-Z without case (natural order); sweeps leftover temporary files */
    std::vector<std::string> names();

    /* ---- reading and writing */
    PlResult read(const std::string &name, std::vector<m3u::Entry> &out) const;
    /* a new playlist (Taken when the name is in use, Invalid when it is not a name) */
    PlResult create(const std::string &name, const std::vector<m3u::Entry> &entries = {});
    /* replaces the file (made when missing), atomically */
    PlResult write(const std::string &name, const std::vector<m3u::Entry> &entries);
    PlResult append(const std::string &name, const std::vector<m3u::Entry> &add);
    /* moves the entry at `index` by `delta` places (Range when it would leave the list); `new_index` is where it went */
    PlResult move(const std::string &name, int index, int delta, int *new_index = nullptr);
    PlResult remove_at(const std::string &name, int index);
    PlResult rename(const std::string &from, const std::string &to);
    PlResult remove(const std::string &name);

    /* a .m3u / .m3u8 anywhere becomes the playlist `name` (Taken when the name is in use) */
    PlResult import_file(const std::string &src_path, const std::string &name);
    /* the file as `<dest_dir>/<name>.m3u8` (replaced when there; relative paths for the tracks on that drive); the path written */
    PlResult export_to(const std::string &name, const std::string &dest_dir, std::string *out_path = nullptr);

    /* the parse options for a file in `dir` (its own folder, the roots) */
    m3u::ParseOptions parse_options(const std::string &file_dir) const;
    /* the text a playlist file in `dir` would have */
    std::string text_for(const std::vector<m3u::Entry> &entries, const std::string &file_dir) const;

    /* tests: called with "written" (the temporary file is complete) before the rename; false stops the write there, as a power
     * cut would */
    std::function<bool(const char *stage)> test_hook;

    bool same_drive(const std::string &a, const std::string &b) const;

private:
    PlResult write_file_atomic(const std::string &path, const std::string &bytes) const;
    bool ensure_dir() const;

    std::string dir_;
    std::vector<std::string> roots_;
    std::vector<DriveBase> bases_;
};

/* a file's name without its folder and extension ("/a/Road trip.m3u8" -> "Road trip") */
std::string playlist_stem(const std::string &path);
bool is_playlist_file_name(const std::string &name); /* ends in .m3u or .m3u8, without case */

}  // namespace vinyl
