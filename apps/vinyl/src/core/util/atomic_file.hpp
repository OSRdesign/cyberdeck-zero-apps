/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette takes: small-file helpers on the FileOps seam. atomic_write_file() writes <path>.tmp, fsyncs it, renames it
 * over <path> and fsyncs the directory, so a crash or a failure at any step leaves the old file or the new one, never a
 * mix (a failed write removes the temp file). LVGL-free.
 */
#pragma once

#include "../fileops.hpp"

#include <string>

namespace vinyl {

/* 0 or -errno. */
int atomic_write_file(FileOps &ops, const std::string &path, const std::string &body);
/* Reads a whole small file (at most max_bytes, else -EFBIG). 0 or -errno. */
int read_small_file(FileOps &ops, const std::string &path, std::string &out, size_t max_bytes = 1u << 20);
/* Creates every missing directory of path (like mkdir -p). 0 or -errno. */
int make_dirs(FileOps &ops, const std::string &path);
/* "a/b/c.wav" -> "a/b"; "c.wav" -> "." */
std::string dir_name(const std::string &path);
/* "a/b/c.wav" -> "c.wav" */
std::string base_name(const std::string &path);
/* Does the path exist (stat succeeds)? */
bool path_exists(FileOps &ops, const std::string &path);

}  // namespace vinyl
