/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: sort keys (report 060 3a, "Sorting and CJK"). SQLite has no ICU on the boards and Vinyl does not load
 * libicu (its versioned symbols make dlopen fragile), so the order of the Artists, Albums, Songs and Genres pages is
 * built in code: a key per name, stored next to it and compared byte by byte (SQLite's BINARY collation, which for UTF-8
 * is code-point order):
 *
 *   - case folded (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic) and Latin diacritics removed: "Beyoncé" and
 *     "BEYONCE" share a key; ß is "ss", æ "ae", œ "oe", þ "th"; combining marks (U+0300-U+036F, the decomposed names a
 *     Mac writes) are dropped; full-width ASCII is plain ASCII;
 *   - leading punctuation skipped ("\"Heroes\"" files under H) and runs of spaces made one;
 *   - a leading "The " moved behind the name ("The Beatles" sorts as "Beatles", just after a plain "Beatles");
 *   - numbers padded to 8 digits, so "Track 2" comes before "Track 10" (P1's natural_less in key form);
 *   - katakana folded to hiragana (カタカナ and かたかな share a key, between Latin and Han);
 *   - Hangul in code-point order, which is dictionary order; Han in code-point order, not pinyin (ICU is a P3 option);
 *   - an empty name ("unknown artist") gets kUnknownSortKey, after every other name.
 * LVGL-free, no allocation beyond the result.
 */
#pragma once

#include <string>

namespace vinyl {

/* U+FFFF: after every name of the Basic Multilingual Plane */
extern const char *const kUnknownSortKey;

/* The folding alone (case, diacritics, kana, full width, spaces), no article or number handling. */
std::string fold_text(const std::string &text);

/* The sort key of a name. */
std::string sort_key(const std::string &text);

/* The number of Unicode characters of `text` (an invalid byte counts as one). */
size_t utf8_length(const std::string &text);

/* The key range of names starting with `query` (the search's fallback for queries of 1-2 characters: FTS5's trigrams
 * match nothing that short): lo is the query's sort key, hi lo followed by 0xFF (above every UTF-8 continuation). Both
 * empty when the query folds to nothing. */
void prefix_range(const std::string &query, std::string &lo, std::string &hi);

}  // namespace vinyl
