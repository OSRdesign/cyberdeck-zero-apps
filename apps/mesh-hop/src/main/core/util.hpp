/*
 * SPDX-License-Identifier: MIT
 *
 * Small helpers shared by the protocol, model and store layers (no LVGL, no I/O).
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace meshzero {

using Bytes = std::vector<uint8_t>;
using PubKey = std::array<uint8_t, 32>;
using KeyPrefix = std::array<uint8_t, 6>;

std::string to_hex(const uint8_t *data, size_t len);
inline std::string to_hex(const Bytes &b) { return to_hex(b.data(), b.size()); }
inline std::string to_hex(const PubKey &k) { return to_hex(k.data(), k.size()); }
inline std::string to_hex(const KeyPrefix &k) { return to_hex(k.data(), k.size()); }
/* Parses hex into out (exactly out_len bytes). False on a bad character or a wrong length. */
bool from_hex(const std::string &hex, uint8_t *out, size_t out_len);

/* Replaces invalid UTF-8 sequences and control characters (except \n) by '?', cuts NUL bytes. */
std::string sanitize_utf8(const std::string &in);
/* Longest prefix of s that is at most max_bytes long and does not cut a UTF-8 sequence. */
std::string truncate_utf8(const std::string &s, size_t max_bytes);
size_t utf8_length(const std::string &s);

/* Wall clock seconds (UNIX time). Replaced in tests through set_time_source(). */
/* Time of the app: the deck's clock plus the offset that makes it equal to the board's clock when the board is the time
 * source (set by the clock policy). deck_unix() is the deck's own clock. */
uint32_t now_unix();
uint32_t deck_unix();
void set_time_offset(int64_t seconds);
int64_t time_offset();
void set_time_source(uint32_t (*fn)());

/* "5 s", "12 min", "3 h", "2 d" for an age in seconds. */
std::string format_age(int64_t seconds);

} // namespace meshzero
