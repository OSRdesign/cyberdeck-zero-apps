/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: what a note may be called (report 037 section 2.4). Spaces and any Unicode are allowed. Refused:
 * the characters / \ : * ? " < > | # ^ [ ] (they break links or a Windows peer of the sync), control characters, a
 * leading dot (a hidden file the walk skips), a trailing space or dot (Windows drops them), the Windows device names
 * (CON, PRN, AUX, NUL, COM1-9, LPT1-9, also with an extension) and more than kNameMaxBytes bytes. A note name never
 * carries ".md": the file is name + ".md". The UI turns a NameError into words (part 2).
 */
#pragma once

#include <cstddef>
#include <string>

namespace mdnotes {

constexpr std::size_t kNameMaxBytes = 120;   /* the name without ".md" */
constexpr int kMaxFolderDepth = 12;          /* = the walk's depth cap (vault.hpp) */

enum class NameError {
    None,
    Empty,
    TooLong,
    Forbidden,           /* NameCheck::bad holds the character */
    ControlChar,
    LeadingDot,
    TrailingSpaceOrDot,
    Reserved,            /* a Windows device name */
    NotUtf8,
    TooDeep,             /* a path with more than kMaxFolderDepth folders */
    EmptyFolder          /* a path with an empty part ("a//b", "/b") or a "." / ".." part */
};

struct NameCheck {
    NameError error = NameError::None;
    std::string bad;     /* Forbidden: the character; a path: the part that failed */
    bool ok() const { return error == NameError::None; }
};

/* What the user typed, as a name: spaces around it removed, one ".md" at the end (any case) removed. */
std::string clean_typed_name(const std::string &typed);
/* One name (a note's without ".md", or a folder's). */
NameCheck check_name(const std::string &name);
/* A note path relative to the vault without ".md": "Projects/Mesh/Antenna ideas"; each part checked, at most
 * max_depth folders. */
NameCheck check_note_path(const std::string &rel, int max_depth = kMaxFolderDepth);

/* "Antenna ideas" -> "Antenna ideas.md" */
std::string note_file_name(const std::string &name);
/* The file name ends with ".md" (any case) and has a name before it. */
bool is_note_file(const std::string &file_name);
/* "Antenna ideas.md" -> "Antenna ideas" (the suffix as written removed); other names unchanged. */
std::string strip_md(const std::string &file_name);

/* A collision: the entry of `dir` whose name equals file_name ignoring case (utf8.hpp folding), "" when none. The
 * folder is read now (not the index, which may be older). */
std::string find_existing_ci(const std::string &dir, const std::string &file_name);

} // namespace mdnotes
