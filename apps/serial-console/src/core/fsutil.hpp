/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: small file helpers on POSIX calls (no exceptions, no std::filesystem: the boards' toolchain links
 * the same way as the launcher's).
 */
#pragma once

#include <string>
#include <vector>

namespace sc {
namespace fs {

bool exists(const std::string &path);                  /* stat() follows links */
std::string read_line(const std::string &path);        /* the first line, without its end; "" when unreadable */
std::string read_all(const std::string &path, bool *ok = nullptr); /* the whole file (up to 1 MB) */
std::string read_link(const std::string &path);        /* a link's target as written, "" when not a link */
std::string real_path(const std::string &path);        /* "" when it does not resolve */
std::string base_name(const std::string &path);        /* "a/b/c" -> "c" */
std::string parent(const std::string &path);           /* "a/b/c" -> "a/b" */
std::vector<std::string> list_dir(const std::string &path); /* sorted names without . and .. */

bool make_dirs(const std::string &path, unsigned mode = 0755);
/* Writes path through a temporary file and a rename (never a half-written file). */
bool write_atomic(const std::string &path, const std::string &text);
bool write_file(const std::string &path, const std::string &text);
bool make_link(const std::string &target, const std::string &path); /* replaces an existing link */
bool remove_tree(const std::string &path);              /* rm -rf, links not followed */

} // namespace fs
} // namespace sc
