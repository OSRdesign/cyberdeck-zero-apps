/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the diagnostic log line (see diag.hpp).
 */
#include "diag.hpp"

#include <cstdio>

namespace mmw {

std::string diag_line(const Link &link, Ms now)
{
    char b[640];
    const LinkStats s = link.stats(now);
    int n = std::snprintf(b, sizeof(b), "t=%lld state=%s port=%s baud=%d frames=%u bad=%u answers=%u bytes=%llu fps=%.1f age=%lld",
                          static_cast<long long>(now), link_state_name(link.state()),
                          link.state() == LinkState::Found || link.state() == LinkState::Lost ? link.port().tty.c_str() : "-",
                          link.baud(), s.frames, s.bad_frames, s.answers, static_cast<unsigned long long>(s.bytes), s.fps,
                          static_cast<long long>(s.last_frame_age_ms));
    for (int i = 0; i < 3 && n > 0 && n < static_cast<int>(sizeof(b)); ++i) {
        const SlotState &t = link.tracker().slot(i);
        n += std::snprintf(b + n, sizeof(b) - static_cast<std::size_t>(n),
                           " T%d[act=%d now=%d x=%d y=%d v=%d id=%u]", i + 1, t.active ? 1 : 0, t.present_now ? 1 : 0,
                           t.last.x_mm, t.last.y_mm, t.last.speed_cm_s, t.id);
    }
    return b;
}

std::string diag_extra(const Link &link, unsigned &sent)
{
    std::string out;
    if (!(sent & 1u) && !link.tty_settings().empty()) {
        out += "termios " + link.tty_settings() + "\n";
        sent |= 1u;
    }
    const std::vector<std::string> &f = link.first_target_frames();
    for (unsigned i = 0; i < f.size() && i < 3; ++i) {
        if (sent & (2u << i)) continue;
        out += "target-frame " + std::to_string(i + 1) + " " + f[i] + "\n";
        sent |= 2u << i;
    }
    return out;
}

} // namespace mmw
