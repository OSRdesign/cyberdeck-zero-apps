/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: links between notes (report 037 section 3, wikilinks). Obsidian-compatible as far as the
 * vault index allows:
 *
 *   [[Name]]                 the note "Name" anywhere in the vault, ignoring case; several: the one in the linking
 *                            note's folder, else the shortest path, else the first by path
 *   [[Folder/Name]]          a path: from the vault root, else from the linking note's folder, else any note whose
 *                            path ends with it; "/Name" only from the vault root, "./Name" "../Name" only from the
 *                            linking note's folder
 *   [[Name#Heading]]         the note, and the heading to show (the UI scrolls to it; an empty name = this note)
 *   [[Name|Shown text]]      the alias is only what is shown
 *   [Text](Other%20note.md)  a relative Markdown link to a note (URL-decoded, from the linking note's folder; "/" first:
 *                            from the vault root); other targets: an external address (a scheme, "www.", an e-mail)
 *                            or a local file that is not a note
 *
 * A missing note gives the path to create: a bare name at the vault root (Obsidian's default location for new notes)
 * or in the linking note's folder (NewNoteFolder), a path as written from the vault root. A name the rules refuse
 * (names.hpp), or a path that leaves the vault, is `invalid`.
 */
#pragma once

#include "vault.hpp"

#include <cstddef>
#include <string>

namespace mdnotes {

enum class LinkKind {
    Wiki,       /* [[...]] */
    Note,       /* a relative link to a .md file */
    External,   /* http:, https:, mailto:, any scheme; www.; an e-mail address */
    File        /* a local path that is not a note (an image, a PDF): shown, not opened, in N1 */
};

struct WikiTarget {
    std::string note;      /* "Folder/Name" as written (".md" kept if written), "" = this note */
    std::string heading;   /* after the first '#' */
    std::string alias;     /* after the first '|' */
};
WikiTarget parse_wikilink(const std::string &inner);   /* the text between [[ and ]] */

LinkKind classify_href(const std::string &href);
std::string url_decode(const std::string &s);         /* %XX and nothing else ('+' stays) */

enum class NewNoteFolder { VaultRoot, SameFolder };

struct Resolved {
    bool found = false;      /* rel is an existing note of the index */
    bool invalid = false;    /* not a note we can open or create */
    bool same_note = false;  /* [[#Heading]]: the linking note itself */
    std::string rel;         /* found: the note; missing: the path to create (with ".md") */
    std::string heading;
};

/* target: the wikilink's inner text ("a#h|b" or md4c's target "a#h"); from_rel: the linking note's rel path. */
Resolved resolve_wikilink(const VaultIndex &index, const std::string &from_rel, const std::string &target,
                          NewNoteFolder where = NewNoteFolder::VaultRoot);
/* href: a Markdown link's destination as written (still URL-encoded). Only for LinkKind::Note hrefs. */
Resolved resolve_note_href(const VaultIndex &index, const std::string &from_rel, const std::string &href);

/* "a/./b/../c" -> "a/c"; false when ".." climbs above the start. */
bool normalize_rel(const std::string &path, std::string &out);

/* The link around a byte of the editor's text (Ctrl+Enter follows it): [[...]] first, then [text](href), on the line of
 * `offset`, brackets included. */
struct LinkAt {
    bool found = false;
    bool wiki = false;
    std::string target;    /* the inner text of [[...]], or the href */
    std::size_t begin = 0, end = 0;   /* the whole link, [begin, end) */
};
LinkAt link_at(const std::string &text, std::size_t offset);

} // namespace mdnotes
