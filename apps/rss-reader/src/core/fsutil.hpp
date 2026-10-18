/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: the few file operations the store needs. Every function reports failure by its return value.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rssreader {

bool read_file(const std::string &path, std::string &out, std::size_t max_bytes = 64u * 1024 * 1024);
/* Writes to "<path>.tmp", flushes and fsyncs it, then renames it over `path`: a reader sees the old or the new file,
 * never half of one; on failure the old file is untouched and the temp file is removed. */
bool write_file_atomic(const std::string &path, const std::string &data);
bool make_dirs(const std::string &path);                       /* mkdir -p */
bool path_exists(const std::string &path);
bool is_directory(const std::string &path);
std::int64_t file_size(const std::string &path);               /* -1 when missing */
std::vector<std::string> list_dir(const std::string &path);    /* names, sorted, without . and .. */
bool remove_tree(const std::string &path);                     /* a file, or a directory with everything in it (symlinks are removed, not followed) */
std::uint64_t tree_size(const std::string &path);              /* bytes of all regular files below */
bool rename_path(const std::string &from, const std::string &to);

} // namespace rssreader
