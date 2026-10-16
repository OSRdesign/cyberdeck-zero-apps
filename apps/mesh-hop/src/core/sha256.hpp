/*
 * SPDX-License-Identifier: MIT
 *
 * SHA-256 (FIPS 180-4), used to derive the key of a hashtag channel exactly as meshcore_py's set_channel does:
 * secret = sha256(name as typed, including the leading '#')[0:16]  (companion_protocol.md, "Hashtag Channels").
 */

#pragma once

#include "util.hpp"

namespace meshzero {

std::array<uint8_t, 32> sha256(const uint8_t *data, size_t len);
inline std::array<uint8_t, 32> sha256(const std::string &s)
{
    return sha256(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

/* The 16 byte secret of the hashtag channel called name (which includes the '#'): the first 16 bytes of its SHA-256. */
std::array<uint8_t, 16> hashtag_secret(const std::string &name);
/* Empty when name is usable as a hashtag channel ("#" + 1..30 visible characters without spaces), else the reason. */
std::string validate_hashtag(const std::string &name);

} // namespace meshzero
