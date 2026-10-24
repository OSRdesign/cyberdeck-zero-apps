/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: Delete (report 037 section 2.4). The note moves to <vault>/.trash/, as Obsidian's own trash
 * does (the walk skips dot folders, so it leaves every list); the UI asks first. A name already in the trash gets
 * " 2", " 3"... before ".md". No restore page: the README says where the files are.
 */
#pragma once

#include <string>

namespace mdnotes {

constexpr const char *kTrashFolder = ".trash";

struct TrashResult {
    bool ok = false;
    std::string trashed;   /* the absolute path in the trash */
    int err = 0;
};

/* rel: the note's path relative to the vault. Same file system: a rename; across (a mount point inside the vault):
 * copy, fsync, then the original removed. */
TrashResult move_to_trash(const std::string &vault, const std::string &rel);

/* The free name in `trash_dir` for `file_name` ("a.md", then "a 2.md", "a 3.md"...). */
std::string free_trash_path(const std::string &trash_dir, const std::string &file_name);

} // namespace mdnotes
