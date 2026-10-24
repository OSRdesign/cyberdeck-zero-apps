/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: a note's bytes <-> the text the app works on, byte-identical both ways (report 037 section 2.4).
 *
 *   file bytes --decode_text()--> body (LF line ends, no BOM, real tabs) + TextFormat --encode_text()--> file bytes
 *   body --to_edit()--> edit text (tabs drawn as a glyph, LVGL draws nothing for '\t') --from_edit()--> body
 *
 * encode_text(decode_text(raw)) == raw for LF and CRLF files, with or without a BOM, tabs and any UTF-8. A file whose
 * line ends are mixed is the one exception: its body is LF and it is written back with the line end most of its lines
 * had (the app writes a note only after an edit, so an untouched mixed file is never rewritten). Lone CRs are kept as
 * they are. Offsets in the body (md4c's task offsets, the preview's) and in the edit text differ only by the tabs:
 * the line numbers are the same in both.
 */
#pragma once

#include <cstddef>
#include <string>

namespace mdnotes {

enum class Eol { Lf, Crlf, Mixed };

struct TextFormat {
    bool bom = false;            /* the file started with EF BB BF */
    Eol eol = Eol::Lf;
    bool write_crlf = false;     /* the line end encode_text() writes (Crlf: true; Mixed: the majority's) */
    bool map_tabs = true;        /* false when the text already holds the tab glyph: tabs then stay '\t' in the editor */
};

struct Decoded {
    std::string text;            /* the body */
    TextFormat fmt;
};

extern const char *const kTabGlyph;   /* U+21E5 RIGHTWARDS ARROW TO BAR, how the editor shows a tab */

Decoded decode_text(const std::string &raw);
std::string encode_text(const std::string &body, const TextFormat &fmt);

/* The editor's text of a body, and back (the identity when fmt.map_tabs is false). */
std::string to_edit(const std::string &body, const TextFormat &fmt);
std::string from_edit(const std::string &edit, const TextFormat &fmt);

/* Byte offset in the edit text of a byte offset in the body (each tab before it is the glyph's 3 bytes). */
std::size_t body_to_edit_offset(const std::string &body, std::size_t offset, const TextFormat &fmt);

} // namespace mdnotes
