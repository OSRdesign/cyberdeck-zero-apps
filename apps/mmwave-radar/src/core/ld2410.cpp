/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the LD2410 stub driver (see ld2410.hpp).
 */
#include "ld2410.hpp"

namespace mmw {

namespace {

const std::uint8_t kHead[4] = {0xF4, 0xF3, 0xF2, 0xF1};

SniffResult sniff(const std::uint8_t *s, std::size_t n)
{
    bool maybe = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (s[i] != 0xF4) continue;
        if (ld2410_frame_at(s + i, n - i)) return SniffResult::Match;
        /* the window may end inside a frame that is valid so far */
        const std::size_t avail = n - i;
        std::size_t m = 0;
        while (m < avail && m < 4 && s[i + m] == kHead[m]) ++m;
        if (m < avail && m < 4) continue; /* a wrong header byte */
        if (avail < 6) {
            maybe = true;
            continue;
        }
        const std::size_t len = static_cast<std::size_t>(s[i + 4] | (s[i + 5] << 8));
        if (len < 4 || len > 100 || avail >= 10 + len) continue;
        /* the data type and the head byte, once they are in the window, must fit an LD2410 data frame */
        if (avail >= 7 && s[i + 6] != 0x01 && s[i + 6] != 0x02) continue;
        if (avail >= 8 && s[i + 7] != 0xAA) continue;
        maybe = true;
    }
    return maybe ? SniffResult::Maybe : SniffResult::No;
}

} // namespace

bool ld2410_frame_at(const std::uint8_t *p, std::size_t avail)
{
    if (avail < 10 || p[0] != 0xF4 || p[1] != 0xF3 || p[2] != 0xF2 || p[3] != 0xF1) return false;
    const std::size_t len = static_cast<std::size_t>(p[4] | (p[5] << 8));
    if (len < 4 || len > 100 || 10 + len > avail) return false;
    if ((p[6] != 0x01 && p[6] != 0x02) || p[7] != 0xAA) return false; /* data type, head */
    const std::size_t f = 6 + len;
    if (p[f - 2] != 0x55 || p[f - 1] != 0x00) return false;           /* tail */
    return p[f] == 0xF8 && p[f + 1] == 0xF7 && p[f + 2] == 0xF6 && p[f + 3] == 0xF5;
}

const SensorDriver kDriverLd2410 = {
    "ld2410",
    "HLK-LD2410",
    "LD2410 presence radar (not supported yet)",
    "",
    "LD2410 presence radar (not supported yet)",
    nullptr,
    0,
    &sniff,
    nullptr,
    {false, true},
    ViewKind::Presence,
};

} // namespace mmw
