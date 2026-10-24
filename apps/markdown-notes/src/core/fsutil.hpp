/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: file operations. Every function reports failure by its return value (and errno through `err`
 * where it says so); nothing throws, nothing logs.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mdnotes {

struct FileStat {
    bool exists = false;
    bool is_dir = false;
    bool is_reg = false;
    bool is_link = false;       /* only with follow = false */
    std::int64_t size = 0;
    std::int64_t mtime_ns = 0;  /* st_mtim in nanoseconds */
    unsigned mode = 0;          /* permission bits */
};
FileStat stat_path(const std::string &path, bool follow = true);

enum class ReadStatus { Ok, Missing, TooBig, Error };
/* The whole file, at most max_bytes (more: TooBig, out empty). Missing: ENOENT / ENOTDIR. */
ReadStatus read_file(const std::string &path, std::string &out, std::size_t max_bytes, int *err = nullptr);

/* The hidden temp file of an atomic save: "<dir>/.<name>.~deck" (sync tools and Obsidian skip dot files). */
std::string temp_path_for(const std::string &path);
/* Atomic replace (report 037 section 2.4): the temp file in the same folder, every byte written, the old file's mode
 * kept (0644 for a new file), fsync, rename over `path`, fsync of the folder. A reader sees the old file or the new
 * one, never half of one; on any failure before the rename the old file is untouched and the temp file removed. */
bool write_file_atomic(const std::string &path, const std::string &data, int *err = nullptr);
/* A new file that must not exist yet (O_EXCL), written and fsynced, the folder fsynced. EEXIST when it exists. */
bool create_file_exclusive(const std::string &path, const std::string &data, int *err = nullptr);

bool make_dirs(const std::string &path, int *err = nullptr);   /* mkdir -p, 0755 */
bool fsync_dir(const std::string &dir);

struct DirEntry {
    std::string name;
    bool is_dir = false;
    bool is_reg = false;
    bool is_link = false;   /* a symbolic link (not followed: is_dir / is_reg are false then) */
    std::int64_t size = 0;
    std::int64_t mtime_ns = 0;
};
/* Names sorted (bytewise), without . and ..; lstat for each. False when the folder cannot be read. */
bool list_dir(const std::string &path, std::vector<DirEntry> &out, int *err = nullptr);

bool remove_tree(const std::string &path);   /* tests: a file, or a folder with everything in it (links not followed) */

std::string path_join(const std::string &a, const std::string &b);   /* "a/b"; either side empty: the other */
std::string dir_name(const std::string &path);    /* "a/b/c.md" -> "a/b"; "c.md" -> "" */
std::string base_name(const std::string &path);   /* "a/b/c.md" -> "c.md" */
std::string home_dir();                           /* $HOME, else the passwd entry, else "/" */
std::string current_dir();

} // namespace mdnotes
