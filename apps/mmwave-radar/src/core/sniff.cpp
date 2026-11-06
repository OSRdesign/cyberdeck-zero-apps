/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: what else talks on a port (see sniff.hpp).
 */
#include "sniff.hpp"

#include "ld2410.hpp"

namespace mmw {

namespace {

constexpr std::size_t kWin = 1024;

int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

unsigned char u(char c) { return static_cast<unsigned char>(c); }

/* A complete NMEA sentence at s[i] == '$'; returns its first talker letter via talker, true when the checksum fits. */
bool nmea_at(const std::string &s, std::size_t i, char &talker)
{
    const std::size_t end = s.size();
    std::size_t k = i + 1;
    unsigned x = 0;
    while (k < end && s[k] != '*' && k - i < 100) {
        if (u(s[k]) < 0x20 || u(s[k]) > 0x7E) return false;
        x ^= u(s[k]);
        ++k;
    }
    if (k + 2 >= end || s[k] != '*') return false;
    const int h1 = hexval(s[k + 1]), h2 = hexval(s[k + 2]);
    if (h1 < 0 || h2 < 0 || static_cast<unsigned>(h1 * 16 + h2) != x) return false;
    if (k - i < 6) return false; /* "$XXXXX" at least */
    talker = s[i + 1];
    return true;
}

} // namespace

void Sniffer::reset()
{
    win_.clear();
    total_ = zeros_ = printable_ = 0;
    kind_ = Sniffed::None;
}

void Sniffer::feed(const std::uint8_t *data, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t b = data[i];
        ++total_;
        if (b == 0) ++zeros_;
        if ((b >= 0x20 && b < 0x7F) || b == '\r' || b == '\n' || b == '\t') ++printable_;
    }
    if (kind_ != Sniffed::None) return;
    win_.append(reinterpret_cast<const char *>(data), n);
    if (win_.size() > kWin) win_.erase(0, win_.size() - kWin);
    scan();
}

void Sniffer::scan()
{
    const std::string &s = win_;
    const std::size_t n = s.size();
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = u(s[i]);
        if (c == '$') {
            char talker = 0;
            if (nmea_at(s, i, talker)) {
                kind_ = talker == 'G' ? Sniffed::Gps : Sniffed::Nmea;
                return;
            }
        } else if (c == 0xB5 && i + 8 <= n && u(s[i + 1]) == 0x62) {
            const std::size_t len = static_cast<std::size_t>(u(s[i + 4]) | (u(s[i + 5]) << 8));
            if (len <= 600 && i + 8 + len <= n) {
                unsigned a = 0, b = 0;
                for (std::size_t k = i + 2; k < i + 6 + len; ++k) {
                    a = (a + u(s[k])) & 0xFF;
                    b = (b + a) & 0xFF;
                }
                if (a == u(s[i + 6 + len]) && b == u(s[i + 7 + len])) {
                    kind_ = Sniffed::Ubx;
                    return;
                }
            }
        } else if (stubs_ && c == 0xF4 &&
                   ld2410_frame_at(reinterpret_cast<const std::uint8_t *>(s.data()) + i, n - i)) {
            kind_ = Sniffed::Ld2410; /* the stub driver's frame (ld2410.hpp) */
            return;
        }
    }
}

std::string Sniffer::describe() const
{
    switch (kind_) {
    case Sniffed::Gps: return "GPS receiver (NMEA)";
    case Sniffed::Nmea: return "NMEA device";
    case Sniffed::Ubx: return "GPS receiver (u-blox UBX)";
    case Sniffed::Ld2410: return kDriverLd2410.stub_text;
    case Sniffed::None: break;
    }
    return std::string();
}

std::string Sniffer::describe_unknown() const
{
    if (total_ == 0) return "no data";
    if (zeros_ * 100 >= total_ * 95) return "only zeros";
    if (total_ >= 24 && printable_ * 100 >= total_ * 85) return "text output (a console or a log)";
    return "unknown data";
}

} // namespace mmw
