/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: small text helpers shared by every module (no LVGL, no I/O).
 * Everything here is total: bad input gives a harmless result, never a crash.
 */
#pragma once

#include <cstdint>
#include <string>

namespace rssreader {

constexpr const char *kVersion = "0.1.0";

std::string trim(const std::string &s);                 /* ASCII whitespace at both ends */
std::string lower(std::string s);                       /* ASCII only */
bool starts_with(const std::string &s, const std::string &prefix);
bool ends_with(const std::string &s, const std::string &suffix);
bool iequals(const std::string &a, const std::string &b);
bool icontains(const std::string &hay, const std::string &needle);

void append_utf8(std::string &out, std::uint32_t cp);   /* invalid code points become U+FFFD */
std::string utf8_sanitize(const std::string &s);        /* invalid / overlong sequences become U+FFFD */
std::string truncate_utf8(const std::string &s, std::size_t max_bytes); /* cut on a character boundary */

/* ISO-8859-1 / windows-1252 (and ISO-8859-15 when latin9) to UTF-8. */
std::string latin1_to_utf8(const std::string &s, bool cp1252, bool latin9 = false);

/* HTML / XML character references: &amp; &#8217; &#x2019; &eacute; ... ; an unknown or malformed reference stays as written.
 * Numeric references 128..159 are read as windows-1252, as browsers do. */
std::string decode_entities(const std::string &s);
std::string html_escape(const std::string &s);          /* & < > " */

/* Whitespace runs (including NBSP and zero-width characters) become one space; the ends are trimmed. */
std::string collapse_ws(const std::string &s);

/* Lower case with the common Latin accents removed ("Elysee" for "Élysée"), for searching. */
std::string fold_ascii(const std::string &s);

/* 16 lower-case hex digits of FNV-1a 64 of s: stable ids and file names. */
std::string hash16(const std::string &s);

/* A string made of [A-Za-z0-9._-] only (a file name): other bytes become '_'; never empty, never starts with '.'. */
std::string safe_name(const std::string &s);

} // namespace rssreader
