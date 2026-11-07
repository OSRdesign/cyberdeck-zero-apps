/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the import page's file browser. It starts at the places maps come from (the home folder, USB sticks
 * and SD cards mounted under /media, /run/media and /mnt) and lists folders and the files it can import: .mbtiles and
 * .pmtiles archives, and z/x/y tile folders (a folder of zoom folders of column folders of tile files, MapTileDl's and
 * most tile downloaders' layout), which the engine converts into one MBTiles. Other files are not shown; hidden ones
 * (a leading dot) neither. The listing goes through a Lister (POSIX here, a fake in the tests) and runs on the app's
 * worker thread (a USB stick can be slow). Pure apart from PosixLister and folder_bytes().
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace atlas {

struct DirEntry {
    std::string name;
    bool dir = false;
    std::uint64_t size = 0;
};

class Lister {
public:
    virtual ~Lister() = default;
    /* the entries of a folder (no "." / ".."), false when it cannot be read (errno-style reason in why) */
    virtual bool list(const std::string &path, std::vector<DirEntry> &out, std::string *why) = 0;
};

class PosixLister : public Lister {
public:
    bool list(const std::string &path, std::vector<DirEntry> &out, std::string *why) override;
};

enum class RowKind { Root, Up, Folder, Archive, ImportFolder, Note };

struct BrowserRow {
    RowKind kind = RowKind::Note;
    std::string label, value, path;
};

constexpr int kMaxEntries = 1000;       /* a folder's rows shown at most (the rest: a note) */

std::string path_join(const std::string &dir, const std::string &name);
std::string path_parent(const std::string &path); /* "/" stays "/" */
std::string path_name(const std::string &path);   /* the last part */
/* "~/Maps" for a path under home, else the path. */
std::string path_display(const std::string &path, const std::string &home);

/* .mbtiles / .pmtiles, any case */
bool is_archive_name(const std::string &name);
/* "12", "0": a zoom, column or row folder or file stem (digits only, at most 9) */
bool is_number(const std::string &s);
/* A tile file name: digits, then nothing or an image extension (.png .jpg .jpeg .webp .pbf .mvt, also "*.png.tile"). */
bool is_tile_file_name(const std::string &name);
/* The folder is a z/x/y tile folder: one of its number folders holds number folders holding tile files (checked on
 * the first few of each, so a 10,000-tile folder costs a handful of listings). */
bool looks_like_tile_folder(Lister &lister, const std::string &path);

/* The places to start from: Home, then each mounted stick or card (/media/<user>/<label>, /media/<label>,
 * /run/media/<user>/<label>, /mnt/<label>), each with what it is ("Home folder", "USB stick or card"). */
std::vector<BrowserRow> browser_roots(Lister &lister, const std::string &home, const std::string &user);
/* The rows of one folder: "Import this folder" first when it is a tile folder (and then nothing else of it), else
 * "Up one level", the folders, then the archives with their size; a note when nothing can be imported here. */
std::vector<BrowserRow> browser_rows(Lister &lister, const std::string &path, bool at_top, std::string *error);

/* The bytes and files under a tile folder (z/x/y, three levels; stops early when cancel turns true). */
std::uint64_t folder_bytes(const std::string &path, std::uint64_t *files, const std::function<bool()> &cancel);
/* Free bytes for an unprivileged user on the file system of path (statvfs f_bavail); 0 when unknown. */
std::uint64_t free_bytes(const std::string &path);
/* The size of a file, 0 when it is not a regular file. */
std::uint64_t file_size(const std::string &path);

} // namespace atlas
