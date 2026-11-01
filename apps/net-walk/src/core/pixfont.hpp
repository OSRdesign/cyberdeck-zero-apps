/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: the bold bitmap font of the Palm look. Own glyphs, drawn for this app on a 5 x 8 grid (capitals 7 rows,
 * lower case with a descender row), made bold by lighting each pixel and the one to its right: a glyph is 6 pixels wide
 * and the advance is 7. Nothing is taken from another font; no font file is shipped. Covers the characters the screens
 * use (letters, digits, a few marks); any other character draws as a question mark. Pure data and arithmetic: the UI
 * draws the pixels.
 */
#pragma once

#include <string>

namespace netwalk {

constexpr int kGlyphRows = 8;
constexpr int kGlyphCols = 5;
constexpr int kAdvance = 7;
constexpr int kTextH = 8; /* rows a line of text occupies (capitals use the top 7) */

/* row r (0 = top) of the glyph for `c`: bits 4..0 = columns left to right, before the bold smear */
unsigned glyph_row(char c, int r);
/* the width of `text` in logical pixels */
int text_width(const std::string &text);
/* the table is well formed: every glyph has 8 rows of 5 characters, no character twice */
bool font_table_ok();
/* is every character of `text` a real glyph (not the question mark) */
bool has_glyphs(const std::string &text);

} // namespace netwalk
