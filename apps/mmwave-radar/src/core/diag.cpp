/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the diagnostic log line (see diag.hpp).
 */
#include "diag.hpp"

#include "settings.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

std::string diag_frames(Link &link, Ms now, std::string *hex_line)
{
    DiagFrameCounts &c = link.diag_counts();
    char b[200];
    std::snprintf(b, sizeof(b), " empty=%u nonempty=%u same_as_prev=%u total_empty=%llu total_nonempty=%llu total_same_as_prev=%llu",
                  c.empty, c.nonempty, c.same, static_cast<unsigned long long>(c.total_empty),
                  static_cast<unsigned long long>(c.total_nonempty), static_cast<unsigned long long>(c.total_same));
    c.empty = c.nonempty = c.same = 0;
    if (hex_line) hex_line->clear();
    if (now - c.hex_since >= 10000) {
        if (c.latest_new && hex_line) {
            static const char *hex = "0123456789abcdef";
            *hex_line = "last-target-frame ";
            for (std::uint8_t v : c.latest) {
                *hex_line += hex[v >> 4];
                *hex_line += hex[v & 15];
            }
            *hex_line += '\n';
        }
        c.latest_new = false;
        c.hex_since = now;
    }
    return b;
}

std::string diag_log_path(const char *env_log, const std::string &config_dir, bool *from_marker)
{
    if (from_marker) *from_marker = false;
    if (env_log && *env_log) return env_log;
    if (config_dir.empty()) return std::string();
    struct stat st;
    if (::stat((config_dir + "/diag").c_str(), &st) != 0) return std::string();
    if (from_marker) *from_marker = true;
    return config_dir + "/mmwave.log";
}

bool diag_trim(const std::string &path, std::size_t keep_bytes)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    const std::size_t size = static_cast<std::size_t>(st.st_size);
    if (size <= keep_bytes) return false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::string tail(keep_bytes, '\0');
    std::size_t got = 0;
    const off_t from = static_cast<off_t>(size - keep_bytes);
    while (got < keep_bytes) {
        const ssize_t n = ::pread(fd, &tail[got], keep_bytes - got, from + static_cast<off_t>(got));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    tail.resize(got);
    const std::size_t nl = tail.find('\n');
    tail.erase(0, nl == std::string::npos ? tail.size() : nl + 1); /* no half line at the top */
    return write_file_atomic(path, tail);
}

} // namespace mmw
