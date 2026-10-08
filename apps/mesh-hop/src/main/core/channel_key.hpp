/*
 * SPDX-License-Identifier: MIT
 *
 * Private and community channels (H1): a channel is a name plus a 16-byte secret. The secret of a hashtag channel is derived
 * from its name (sha256.hpp); a private channel has a random key that is shared as 32 hex characters. No LVGL, no I/O.
 */

#pragma once

#include "util.hpp"

#include <array>
#include <functional>
#include <string>

namespace meshzero {

using ChannelSecret = std::array<uint8_t, 16>;

/* What a typed key may contain: hex digits, plus spaces / dashes / colons that are ignored. Returns "" when `text` holds
 * exactly 32 hex digits (key filled), else the reason ("27 of 32 hex characters", "not a hex character: g", "all zeros"). */
std::string parse_channel_key(const std::string &text, ChannelSecret &key);
/* The number of hex digits typed so far (separators ignored), for the live "n of 32" counter. */
int channel_key_digits(const std::string &text);
/* True when one more typed character may be added: a hex digit or a separator, and not above 32 digits. */
bool channel_key_char_ok(const std::string &current, const std::string &add);

/* 32 lower-case hex characters. */
std::string channel_key_hex(const ChannelSecret &key);
/* Four groups of eight for reading aloud or sharing: "8b3387e9 c5cdea6a c5e5edb5 ..." */
std::string channel_key_grouped(const ChannelSecret &key);

/* A random key (never all zeros). `fill` supplies random bytes; the default reads /dev/urandom (tests pass their own). */
using RandomFill = std::function<bool(uint8_t *out, size_t n)>;
bool random_channel_key(ChannelSecret &key, const RandomFill &fill = RandomFill());

/* The name of a private channel: 1..31 bytes, no control characters, must not start with '#' (that is a hashtag channel whose key
 * comes from its name). Empty when valid, else the reason. */
std::string validate_private_name(const std::string &name);

enum class ChannelKind { Public, Hashtag, Private };
const char *channel_kind_name(ChannelKind k);
/* Public: the well known key; Hashtag: name starts with '#' and the key is the hashtag key of the name; else Private. */
ChannelKind classify_channel(const std::string &name, const ChannelSecret &secret);

} // namespace meshzero
