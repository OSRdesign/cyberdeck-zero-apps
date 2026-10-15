/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: results saved as text (see netlog.hpp).
 */
#include "netlog.hpp"

#include "text.hpp"
#include "toolbox.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nt {
namespace {

bool make_dirs(const std::string &path, int *err)
{
    std::string cur;
    for (std::size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if ((path[i] == '/' && i > 0) || i + 1 == path.size()) {
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                *err = errno;
                return false;
            }
        }
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        *err = ENOTDIR;
        return false;
    }
    return true;
}

std::string local_time(std::time_t when, const char *format)
{
    struct tm tm;
    if (!localtime_r(&when, &tm)) return std::string();
    char buf[64];
    return std::strftime(buf, sizeof(buf), format, &tm) ? std::string(buf) : std::string();
}

const char *packet_mark(PacketState s)
{
    switch (s) {
    case PacketState::Ok: return "ok";
    case PacketState::Slow: return "slow";
    case PacketState::Lost: return "lost";
    default: return "waiting";
    }
}

} // namespace

std::string default_log_dir()
{
    const char *over = std::getenv("NET_TOOLBOX_LOG_DIR");
    if (over && over[0]) return over;
    const char *home = std::getenv("HOME");
    if (home && home[0]) return std::string(home) + "/net-logs";
    return std::string();
}

std::string log_file_name(const std::string &tool, std::time_t when)
{
    std::string name;
    for (const char c : lower(tool)) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') name += c;
        else if ((c == ' ' || c == '_') && !name.empty() && name.back() != '-') name += '-';
    }
    if (name.empty()) name = "result";
    if (name.size() > 32) name.resize(32);
    return name + "-" + local_time(when, "%Y%m%d-%H%M%S") + ".txt";
}

SaveResult save_log(const std::string &dir, const std::string &tool, const std::string &text, std::time_t when)
{
    SaveResult r;
    if (dir.empty()) {
        r.error = "No folder to save in (HOME is not set)";
        return r;
    }
    int err = 0;
    if (!make_dirs(dir, &err)) {
        r.error = "Cannot create " + dir + ": " + std::strerror(err);
        return r;
    }
    const std::string base = log_file_name(tool, when);
    const std::string stem = base.substr(0, base.size() - 4);
    std::string path;
    for (int n = 1; n < 100; ++n) {
        const std::string candidate = dir + "/" + (n == 1 ? base : stem + "-" + std::to_string(n) + ".txt");
        struct stat st;
        if (stat(candidate.c_str(), &st) != 0) {
            path = candidate;
            break;
        }
    }
    if (path.empty()) {
        r.error = "Too many logs in the same second";
        return r;
    }
    const std::string tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        r.error = "Cannot write in " + dir + ": " + std::strerror(errno);
        return r;
    }
    std::size_t done = 0;
    bool ok = true;
    while (done < text.size()) {
        const ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ok = false;
            err = errno;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    if (ok && fsync(fd) != 0) {
        ok = false;
        err = errno;
    }
    close(fd);
    if (ok && rename(tmp.c_str(), path.c_str()) != 0) {
        ok = false;
        err = errno;
    }
    if (!ok) {
        unlink(tmp.c_str());
        r.error = "Cannot write " + path + ": " + std::strerror(err ? err : EIO);
        return r;
    }
    r.ok = true;
    r.path = path;
    return r;
}

std::string log_heading(const std::string &version, const std::string &tool_title, std::time_t when)
{
    return std::string(kAppTitle) + " " + version + " - " + tool_title + " - " + local_time(when, "%Y-%m-%d %H:%M:%S");
}

std::string format_interfaces(const NetState &st)
{
    std::string out;
    for (const Iface &i : st.ifaces) {
        out += i.name + ": " + (i.connected() ? "connected" : i.up ? "up, no link" : "down");
        if (!i.operstate.empty()) out += " (" + i.operstate + ")";
        out += "\n";
        for (const std::string &a : i.ipv4) out += "  IPv4 " + a + "\n";
        for (const std::string &a : i.ipv6) out += "  IPv6 " + a + "\n";
        if (!i.mac.empty()) out += "  MAC " + i.mac + "\n";
        if (i.speed_mbps > 0) out += "  speed " + fmt_speed(i.speed_mbps) + "\n";
        if (i.mtu) out += "  MTU " + std::to_string(i.mtu) + "\n";
        if (i.link_valid && i.link.connected) {
            out += "  Wi-Fi " + printable(i.link.ssid);
            if (i.link.freq_mhz) out += ", " + std::to_string(i.link.freq_mhz) + " MHz";
            if (i.link.has_bitrate) out += ", " + fmt_mbit(i.link.tx_mbit);
            out += "\n";
        }
        if (i.has_level) out += "  signal " + std::to_string(static_cast<int>(i.level_dbm)) + " dBm\n";
    }
    out += "Gateway: " + (st.route.ok ? st.route.gateway + " via " + st.route.iface : std::string("none")) + "\n";
    out += "DNS servers:";
    if (st.dns.servers.empty()) out += " none";
    for (const std::string &s : st.dns.servers) out += " " + s;
    out += "\n";
    if (!st.dns.search.empty()) {
        out += "Search:";
        for (const std::string &s : st.dns.search) out += " " + s;
        out += "\n";
    }
    return out;
}

std::string format_ping(const PingSnapshot &s)
{
    std::string out = "Target: " + s.target;
    if (!s.address.empty() && s.address != s.target) out += " (" + s.address + ")";
    out += "\n";
    if (!s.resolved_by.empty()) out += "Resolved by: " + s.resolved_by + "\n";
    if (!s.error.empty()) out += "Error: " + s.error + "\n";
    const PingStats &st = s.stats;
    out += "Packets: " + std::to_string(st.transmitted) + " sent, " + std::to_string(st.received) + " received, " +
           std::to_string(st.lost) + " lost (" + fmt_pct(st.loss_pct()) + " loss)\n";
    if (st.received)
        out += "Round trip: min " + fmt_ms(st.min_ms) + ", avg " + fmt_ms(st.avg_ms()) + ", max " + fmt_ms(st.max_ms) +
               ", jitter " + fmt_ms(st.jitter_ms()) + "\n";
    if (!s.recent.empty()) out += "Last " + std::to_string(s.recent.size()) + " packets:\n";
    for (const PacketResult &p : s.recent) {
        out += "  " + std::to_string(p.number) + " " + packet_mark(p.state);
        if (p.state == PacketState::Ok || p.state == PacketState::Slow) out += " " + fmt_ms(p.rtt_ms);
        out += "\n";
    }
    return out;
}

std::string format_dns(const DnsSnapshot &s)
{
    std::string out = "Question: " + s.question + " " + dns::type_name(s.type) + "\n";
    if (!s.error.empty()) out += "Error: " + s.error + "\n";
    for (const DnsResult &r : s.results) {
        out += "Server " + r.server + ": ";
        if (!r.answered) {
            out += r.error + " after " + std::to_string(r.elapsed_ms) + " ms\n";
            continue;
        }
        out += r.rcode_name + " (" + r.rcode_words + ") in " + std::to_string(r.elapsed_ms) + " ms";
        if (r.via_tcp) out += ", over TCP";
        if (r.authoritative) out += ", authoritative";
        out += "\n";
        if (r.chain.size() > 1) {
            out += "  chain:";
            for (std::size_t i = 0; i < r.chain.size(); ++i) out += (i ? " -> " : " ") + r.chain[i];
            out += "\n";
        }
        for (const dns::Record &rec : r.answers)
            out += "  " + rec.name + " " + dns::type_name(rec.type) + " " + std::to_string(rec.ttl) + " " + rec.data + "\n";
        if (r.answers.empty())
            for (const dns::Record &rec : r.authority)
                out += "  (authority) " + rec.name + " " + dns::type_name(rec.type) + " " + rec.data + "\n";
    }
    return out;
}

std::string format_ports(const PortSnapshot &s)
{
    std::string out = "Host: " + s.host;
    if (!s.address.empty() && s.address != s.host) out += " (" + s.address + ")";
    out += "\n";
    if (!s.error.empty()) out += "Error: " + s.error + "\n";
    for (const PortResult &r : s.results) {
        out += "  " + std::to_string(r.port);
        const char *svc = port_service(r.port);
        if (svc[0]) out += std::string(" (") + svc + ")";
        out += std::string(": ") + port_state_name(r.state);
        if (r.state == PortState::Open || r.state == PortState::Closed) out += ", " + std::to_string(r.connect_ms) + " ms";
        else if (r.state != PortState::Pending && !r.detail.empty()) out += ", " + r.detail;
        out += "\n";
    }
    return out;
}

std::string format_diagnosis(const DiagReport &r)
{
    std::string out;
    for (const DiagStep &s : r.steps) {
        out += std::string(step_title(s.id)) + ": " + step_state_name(s.state);
        if (!s.detail.empty()) out += " (" + s.detail + ")";
        out += "\n";
    }
    if (!r.resolver.empty()) out += "Resolver: " + r.resolver + "\n";
    if (!r.explanation.empty()) out += r.explanation + "\n";
    for (const std::string &n : r.notes) out += "Note: " + n + "\n";
    return out;
}

} // namespace nt
