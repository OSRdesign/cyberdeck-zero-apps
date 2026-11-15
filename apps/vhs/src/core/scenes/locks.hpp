/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the locked files (brief 8, 9.2): <folder>/.locked holds one file name per line, replaced atomically; a missing
 * file means nothing is locked. A locked file cannot be deleted. LVGL-free.
 */
#pragma once

#include <set>
#include <string>

namespace vhs {

class FileOps;

std::set<std::string> read_locks(FileOps &ops, const std::string &folder);
/* Atomic; an empty set removes the file. 0 or -errno. */
int write_locks(FileOps &ops, const std::string &folder, const std::set<std::string> &names);
/* Lock or unlock one name: 0 or -errno. */
int set_locked(FileOps &ops, const std::string &folder, const std::string &name, bool locked);

} // namespace vhs
