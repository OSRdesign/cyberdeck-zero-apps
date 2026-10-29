/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the few file operations the store needs (POSIX; no LVGL). Every function reports failure by its
 * return value and never throws.
 *
 *   read_file          the whole file, up to max_bytes (false when missing, unreadable or larger).
 *   write_file_atomic  writes "<path>.tmp", flushes and fsyncs it, renames it over `path` and fsyncs the folder: a
 *                      reader sees the old or the new file, never half of one (a power cut during a save keeps the
 *                      previous file); on failure the old file is untouched and the temp file is removed.
 *   make_dirs          mkdir -p (mode 0700 for the folders it creates).
 */
#pragma once

#include <cstddef>
#include <string>

namespace klondike {

bool read_file(const std::string &path, std::string &out, std::size_t max_bytes = 4u * 1024 * 1024);
bool write_file_atomic(const std::string &path, const std::string &data);
bool make_dirs(const std::string &path);
bool path_exists(const std::string &path);
bool rename_path(const std::string &from, const std::string &to);
bool remove_file(const std::string &path);

} // namespace klondike
