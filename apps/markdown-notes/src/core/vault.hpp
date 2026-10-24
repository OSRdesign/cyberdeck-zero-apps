/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: the vault index (report 037 sections 2.4 and 2.5). One walk of the vault folder kept in memory:
 * the path, name, mtime and size of every note, the folders with their counts. No database, no content.
 *
 * The walk: ".md" files only (any case of the suffix); every name starting with a dot is skipped (".obsidian",
 * ".trash", ".git", ".stversions", our own ".<name>.~deck" temp files); symbolic links are skipped, folders and files
 * alike (a linked folder can loop or leave the vault, and an atomic save would replace a linked file by a copy);
 * caps: kWalkMaxNotes notes, folders at most kMaxFolderDepth below the root (deeper ones are not entered). Each
 * folder is read in name order, so the caps cut deterministically.
 */
#pragma once

#include "names.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mdnotes {

constexpr int kWalkMaxNotes = 5000;

struct WalkLimits {
    int max_notes = kWalkMaxNotes;
    int max_depth = kMaxFolderDepth;
};

struct NoteEntry {
    std::string rel;            /* "Projects/Mesh/Antenna ideas.md" (relative to the vault, '/'-separated) */
    std::string name;           /* "Antenna ideas" */
    std::string folder;         /* "Projects/Mesh"; "" at the root */
    std::string folded;         /* fold_case(name): the search and the links compare it */
    std::int64_t mtime_ns = 0;
    std::int64_t size = 0;
};

struct FolderEntry {
    std::string rel;            /* "Projects/Mesh"; "" = the vault root */
    std::string name;           /* "Mesh"; "" for the root */
    int depth = 0;              /* the root 0, "Projects" 1 */
    int notes = 0;              /* notes directly in it */
    int folders = 0;            /* folders directly in it */
    int total = 0;              /* notes in it and below */
};

struct VaultIndex {
    std::string root;                   /* absolute path of the vault */
    std::vector<NoteEntry> notes;       /* walk order: folder by folder, names in byte order */
    std::vector<FolderEntry> folders;   /* folders[0] is the root; then walk order */
    bool capped_notes = false;          /* stopped at max_notes: some notes are not listed */
    bool capped_depth = false;          /* some folder was deeper than max_depth */
    int skipped_links = 0;              /* symbolic links not followed */
    int unreadable = 0;                 /* folders that could not be read (permissions) */
    bool ok = false;                    /* the root itself could be read */

    int find_note(const std::string &rel) const;      /* exact rel, -1 when absent */
    int find_folder(const std::string &rel) const;    /* exact rel, -1 when absent */
    std::string abs_path(const std::string &rel) const { return rel.empty() ? root : root + "/" + rel; }
};

VaultIndex walk_vault(const std::string &root, const WalkLimits &limits = WalkLimits());

/* A folder page: its sub-folders by name (case-insensitive), its notes newest first (then by name). Indices into
 * index.folders / index.notes. */
struct FolderListing {
    std::vector<int> folders;
    std::vector<int> notes;
};
FolderListing list_folder(const VaultIndex &index, const std::string &rel);

/* The n most recently modified notes of the whole vault, newest first (indices into index.notes). */
std::vector<int> recent_notes(const VaultIndex &index, std::size_t n = 20);

} // namespace mdnotes
