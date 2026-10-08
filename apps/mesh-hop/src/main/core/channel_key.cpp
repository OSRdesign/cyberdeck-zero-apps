/*
 * SPDX-License-Identifier: MIT
 */

#include "channel_key.hpp"

#include "protocol.hpp"
#include "sha256.hpp"

#include <cstdio>

namespace meshzero {

namespace {
bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_sep(char c)
{
    return c == ' ' || c == '-' || c == ':';
}
} // namespace

int channel_key_digits(const std::string &text)
{
    int n = 0;
    for (char c : text)
        if (is_hex(c)) ++n;
    return n;
}

std::string parse_channel_key(const std::string &text, ChannelSecret &key)
{
    std::string hex;
    for (char c : text) {
        if (is_sep(c)) continue;
        if (!is_hex(c)) return std::string("not a hex character: ") + (c >= 0x20 && c < 0x7F ? std::string(1, c) : std::string("?"));
        hex.push_back(c);
    }
    if (hex.size() != 32) return std::to_string(hex.size()) + " of 32 hex characters";
    ChannelSecret k{};
    if (!from_hex(hex, k.data(), 16)) return "not a hex key";
    bool zero = true;
    for (uint8_t b : k)
        if (b) zero = false;
    if (zero) return "the key cannot be all zeros";
    key = k;
    return "";
}

bool channel_key_char_ok(const std::string &current, const std::string &add)
{
    if (add.size() != 1) return false;
    const char c = add[0];
    if (is_sep(c)) return true;
    if (!is_hex(c)) return false;
    return channel_key_digits(current) < 32;
}

std::string channel_key_hex(const ChannelSecret &key)
{
    return to_hex(key.data(), key.size());
}

std::string channel_key_grouped(const ChannelSecret &key)
{
    const std::string h = channel_key_hex(key);
    std::string out;
    for (size_t i = 0; i < h.size(); i += 8) {
        if (!out.empty()) out.push_back(' ');
        out += h.substr(i, 8);
    }
    return out;
}

bool random_channel_key(ChannelSecret &key, const RandomFill &fill)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        ChannelSecret k{};
        bool ok = false;
        if (fill) {
            ok = fill(k.data(), k.size());
        } else {
            if (FILE *f = std::fopen("/dev/urandom", "rb")) {
                ok = std::fread(k.data(), 1, k.size(), f) == k.size();
                std::fclose(f);
            }
        }
        if (!ok) return false;
        bool zero = true;
        for (uint8_t b : k)
            if (b) zero = false;
        if (zero) continue;
        key = k;
        return true;
    }
    return false;
}

std::string validate_private_name(const std::string &name)
{
    if (name.empty()) return "The channel needs a name";
    if (name.size() > kMaxNameBytes) return "The name is limited to 31 bytes";
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7F) return "The name has a control character";
    if (name[0] == '#') return "A name starting with # is a hashtag channel";
    return "";
}

const char *channel_kind_name(ChannelKind k)
{
    switch (k) {
    case ChannelKind::Public: return "public";
    case ChannelKind::Hashtag: return "hashtag";
    case ChannelKind::Private: return "private";
    }
    return "";
}

ChannelKind classify_channel(const std::string &name, const ChannelSecret &secret)
{
    if (secret == kPublicChannelSecret) return ChannelKind::Public;
    if (!name.empty() && name[0] == '#' && secret == hashtag_secret(name)) return ChannelKind::Hashtag;
    return ChannelKind::Private;
}

} // namespace meshzero
