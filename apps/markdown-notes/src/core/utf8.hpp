/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: the few UTF-8 helpers the names, the search and the links need. No locale, no LVGL.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mdnotes {

/* Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF, no truncated sequence. */
bool utf8_valid(const std::string &s);
/* The code point at s[i], i moved past it; a malformed byte gives U+FFFD and moves one byte. i < s.size(). */
std::uint32_t utf8_next(const std::string &s, std::size_t &i);
void utf8_append(std::string &out, std::uint32_t cp);
std::size_t utf8_length(const std::string &s);   /* code points (a malformed byte counts as one) */

/* Simple case folding for comparisons: ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic letters to lower case;
 * every other code point (and a malformed byte) unchanged. No normalisation (an NFD name stays NFD). */
std::uint32_t fold_cp(std::uint32_t cp);
std::string fold_case(const std::string &s);
bool equal_ci(const std::string &a, const std::string &b);

} // namespace mdnotes
