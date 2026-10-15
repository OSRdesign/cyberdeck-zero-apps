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
    if (!r.iface.empty()) out += "Interface: " + r.iface + " (chosen)\n";
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

std::string format_trace(const TraceSnapshot &s)
{
    std::string out = "Target: " + s.target;
    if (!s.address.empty() && s.address != s.target) out += " (" + s.address + ")";
    out += "\n";
    if (!s.resolved_by.empty()) out += "Resolved by: " + s.resolved_by + "\n";
    if (!s.method.empty()) out += "Probes: " + s.method + (s.method_note.empty() ? "" : " (" + s.method_note + ")") + "\n";
    if (!s.error.empty()) out += "Error: " + s.error + "\n";
    out += "Rounds: " + std::to_string(s.rounds) + "\n";
    if (s.reached) out += "Destination reached at hop " + std::to_string(s.dest_hop) + "\n";
    else if (s.unreachable && s.dest_hop > 0 && s.dest_hop <= static_cast<int>(s.hops.size()))
        out += "Unreachable at hop " + std::to_string(s.dest_hop) + " (" + s.hops[s.dest_hop - 1].unreachable + ")\n";
    else if (s.state != TraceSnapshot::State::Failed)
        out += "Destination not reached (" + std::to_string(s.max_hops) + " hops at most)\n";
    if (s.shown) out += "Hop  Address  Loss  Sent  Last  Avg  Best  Worst  Jitter\n";
    for (std::size_t i = 0; i < s.shown && i < s.hops.size(); ++i) {
        const TraceHop &h = s.hops[i];
        out += (h.ttl < 10 ? " " : "") + std::to_string(h.ttl) + "  ";
        if (h.addresses.empty()) {
            out += h.silent ? "no answer (silent hop)\n" : "waiting\n";
            continue;
        }
        out += h.addresses[0].address;
        if (!h.addresses[0].name.empty()) out += " (" + printable(h.addresses[0].name) + ")";
        const PingStats &st = h.stats;
        out += "  " + fmt_pct(st.loss_pct()) + "  " + std::to_string(st.transmitted);
        if (st.received)
            out += "  " + fmt_ms(st.last_ms) + "  " + fmt_ms(st.avg_ms()) + "  " + fmt_ms(st.min_ms) + "  " +
                   fmt_ms(st.max_ms) + "  " + fmt_ms(st.jitter_ms());
        if (h.destination) out += "  destination";
        if (!h.unreachable.empty()) out += "  " + h.unreachable;
        out += "\n";
        if (h.loss_only_here) out += "    loss only at this hop: likely ICMP rate limiting, not a real problem\n";
        for (std::size_t k = 1; k < h.addresses.size(); ++k) {
            out += "    also " + h.addresses[k].address;
            if (!h.addresses[k].name.empty()) out += " (" + printable(h.addresses[k].name) + ")";
            out += ", " + std::to_string(h.addresses[k].count) + " answers\n";
        }
        if (h.other_answers) out += "    and " + std::to_string(h.other_answers) + " answers from other addresses\n";
    }
    if (!s.reached && !s.unreachable && s.shown < s.hops.size())
        out += "Hops " + std::to_string(s.shown + 1) + " to " + std::to_string(s.hops.size()) + " did not answer\n";
    return out;
}

std::string format_httpcheck(const HttpCheckResult &r)
{
    std::string out = "URL: " + r.url + (r.head ? " (HEAD)" : "") + (r.insecure ? " (certificate errors ignored)" : "") +
                      "\n";
    if (r.answered) {
        out += "Status: " + std::to_string(r.status) + (r.reason.empty() ? "" : " " + r.reason);
        if (!r.http_version.empty()) out += ", " + r.http_version;
        out += "\n";
    }
    if (!r.final_url.empty() && r.final_url != r.url && r.final_url != r.url + "/")
        out += "Final URL: " + r.final_url + "\n";
    if (r.redirects) out += "Redirects: " + std::to_string(r.redirects) + "\n";
    if (!r.redirect_url.empty()) out += "Redirects to: " + r.redirect_url + " (not followed)\n";
    if (!r.remote_ip.empty()) out += "Server: " + r.remote_ip + " port " + std::to_string(r.remote_port) + "\n";
    if (r.answered && !r.head) out += "Size: " + std::to_string(r.size_bytes) + " bytes\n";
    const HttpTimings &t = r.timings;
    if (t.valid) {
        out += "Timing: DNS " + fmt_ms(t.dns_ms);
        if (t.connected) out += ", connect " + fmt_ms(t.connect_ms);
        if (t.tls) out += ", TLS " + fmt_ms(t.tls_ms);
        if (t.first_byte) out += ", wait " + fmt_ms(t.wait_ms) + ", transfer " + fmt_ms(t.transfer_ms);
        if (t.redirect_ms > 0) out += ", redirects " + fmt_ms(t.redirect_ms);
        out += ", total " + fmt_ms(t.total_ms) + "\n";
    }
    const CertInfo &c = r.cert;
    if (c.present) {
        out += "Certificate: " + printable(c.subject) + "\n";
        if (!c.issuer.empty()) out += "  issued by " + printable(c.issuer) + "\n";
        if (!c.not_before.empty() || !c.not_after.empty())
            out += "  valid from " + c.not_before + " to " + c.not_after + "\n";
        if (c.has_days) {
            if (c.expired) out += "  expired " + std::to_string(-c.days_left) + " days ago\n";
            else out += "  " + std::to_string(c.days_left) + " days left\n";
        }
        if (c.not_yet_valid) out += "  not valid yet\n";
        out += "  (read by " + c.source + ")\n";
    }
    if (r.problem != HttpProblem::None) out += "Problem: " + r.problem_words + "\n";
    if (!r.curl_message.empty() && r.problem != HttpProblem::None) out += "curl: " + printable(r.curl_message) + "\n";
    if (!r.cert_note.empty()) out += "Note: " + r.cert_note + "\n";
    return out;
}

std::string format_publicip(const PublicIpResult &r, const std::string &interface_ip)
{
    std::string out;
    const auto family = [&](const char *label, const PublicIpFamily &f) {
        out += label;
        if (!f.tried) out += ": not asked\n";
        else if (!f.ok) out += ": " + f.error + "\n";
        else {
            out += ": " + f.trace.ip + " (" + ip_class_name(f.cls) + ")";
            if (!f.trace.loc.empty()) out += ", country " + f.trace.loc;
            if (!f.trace.colo.empty()) out += ", Cloudflare site " + f.trace.colo;
            out += "\n";
        }
    };
    family("IPv4", r.v4);
    family("IPv6", r.v6);
    if (!interface_ip.empty()) {
        out += "Board address: " + interface_ip;
        switch (nat_kind(interface_ip, r.v4.ok ? r.v4.trace.ip : std::string())) {
        case NatKind::Direct: out += " (the public address itself)\n"; break;
        case NatKind::Nat: out += " (behind a router: NAT)\n"; break;
        case NatKind::Cgnat: out += " (behind carrier-grade NAT: the provider shares the public address)\n"; break;
        case NatKind::Unknown: out += "\n"; break;
        }
    }
    return out;
}

std::string format_whois(const WhoisResult &r)
{
    std::string out = "Query: " + r.object + (r.is_ip ? " (address)" : " (domain)") + "\n";
    const WhoisSummary &s = r.summary;
    const auto field = [&](const char *label, const std::string &v) {
        if (!v.empty()) out += std::string(label) + ": " + v + "\n";
    };
    const auto list = [&](const char *label, const std::vector<std::string> &v) {
        if (v.empty()) return;
        out += std::string(label) + ":";
        for (const std::string &x : v) out += " " + x;
        out += "\n";
    };
    field("Registrar", s.registrar);
    field("Created", s.created);
    field("Updated", s.updated);
    field("Expires", s.expires);
    list("Status", s.status);
    list("Name servers", s.name_servers);
    field("Network", s.netname);
    field("Organisation", s.organisation);
    field("Country", s.country);
    field("Range", s.cidr);
    field("AS", s.as_number);
    field("Abuse contact", s.abuse);
    if (!r.note.empty()) out += "Note: " + r.note + "\n";
    for (const WhoisAnswer &a : r.answers) {
        out += "--- " + a.server + " (asked \"" + a.query + "\")";
        if (!a.ok) out += ": " + a.error;
        else if (a.truncated) out += ", cut at 64 kB";
        out += "\n";
        for (const std::string &line : a.lines) out += line + "\n";
    }
    if (r.lines_truncated) out += "--- (cut at " + std::to_string(kWhoisMaxLines) + " lines)\n";
    return out;
}

/* ---------------------------------------------------------------- 0.3 */

namespace {

std::string fmt_mb(std::uint64_t bytes)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / 1e6);
    return buf;
}

std::string fmt_s(double s)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f s", s);
    return buf;
}

std::string latency_line(const LatencyStats &l)
{
    if (!l.ok) return "no reply (" + std::to_string(l.lost) + " lost)";
    return "median " + fmt_ms(l.median_ms) + ", min " + fmt_ms(l.min_ms) + ", max " + fmt_ms(l.max_ms) + ", jitter " +
           fmt_ms(l.jitter_ms) + ", " + std::to_string(l.received) + " replies, " + fmt_pct(l.loss_pct()) + " loss";
}

void direction_text(std::string &out, const char *title, const SpeedDirection &d, bool upload)
{
    if (!d.tried) return;
    out += std::string(title) + ": ";
    if (!d.measured) {
        out += d.error.empty() ? std::string("not measured\n") : d.error + "\n";
    } else {
        out += fmt_mbit(d.mbps) + " (" + fmt_mb(d.bytes) + " in " + fmt_s(d.seconds) + (d.capped ? ", every byte allowed moved" : "") + ")\n";
        if (upload && d.has_server_bytes) out += "  received by the server: " + fmt_mb(d.server_bytes) + "\n";
        if (!d.note.empty()) out += "  note: " + d.note + "\n";
    }
    if (d.loaded.sent) out += "  latency under load: " + latency_line(d.loaded) + "\n";
    if (d.has_increase) out += "  increase: " + fmt_ms(d.increase_ms) + ", grade " + grade_letter(d.grade) + "\n";
    else if (d.grade != BloatGrade::Unknown) out += "  grade " + std::string(grade_letter(d.grade)) + "\n";
    if (!d.grade_note.empty()) out += "  " + d.grade_note + "\n";
    if (!d.history.empty()) {
        out += "  samples (s, Mbit/s):";
        for (const SpeedSample &s : d.history) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), " %.1f/%.1f", s.t_s, s.mbps);
            out += buf;
        }
        out += "\n";
    }
}

const char *wol_state_name(WolSnapshot::State s)
{
    switch (s) {
    case WolSnapshot::State::Sending: return "sending";
    case WolSnapshot::State::Watching: return "watching";
    case WolSnapshot::State::Done: return "done";
    case WolSnapshot::State::Failed: return "failed";
    case WolSnapshot::State::Cancelled: return "stopped";
    case WolSnapshot::State::Idle: break;
    }
    return "idle";
}

} // namespace

std::string format_speedtest(const SpeedSnapshot &s)
{
    const SpeedResult &r = s.result;
    std::string out = std::string("Server: ") + kSpeedHost;
    if (!r.colo.empty()) out += " (Cloudflare site " + r.colo + ")";
    out += "\n";
    if (s.state == SpeedSnapshot::State::Cancelled) out += "Stopped before the end.\n";
    if (!s.error.empty()) out += "Error: " + s.error + "\n";
    if (r.latency_ok)
        out += "Latency: " + fmt_ms(r.latency_ms) + " (median of " + std::to_string(r.latency_samples) + "), jitter " +
               fmt_ms(r.jitter_ms) + ", min " + fmt_ms(r.latency_min_ms) + ", max " + fmt_ms(r.latency_max_ms) +
               "; connection setup " + fmt_ms(r.setup_ms) + "\n";
    if (!r.ping_target.empty() && r.ping_target != "off") {
        out += "Ping target: " + r.ping_target;
        if (!r.ping_address.empty() && r.ping_address != r.ping_target) out += " (" + r.ping_address + ")";
        out += "\n";
    }
    if (r.idle.sent) out += "Latency idle (ping): " + latency_line(r.idle) + "\n";
    direction_text(out, "Download", r.down, false);
    direction_text(out, "Upload", r.up, true);
    if (r.grade != BloatGrade::Unknown)
        out += "Bufferbloat grade: " + std::string(grade_letter(r.grade)) + " - " + grade_words(r.grade) + "\n";
    if (!r.ping_note.empty()) out += "Note: " + r.ping_note + "\n";
    if (!s.detail.empty()) out += "curl: " + s.detail + "\n";
    return out;
}

std::string format_wol(const WolSnapshot &s)
{
    std::string out = "Machine: " + printable(s.machine);
    if (!s.mac.empty()) out += " (" + s.mac + ")";
    out += "\nState: " + std::string(wol_state_name(s.state)) + "\n";
    if (!s.error.empty()) out += "Error: " + s.error + "\n";
    if (!s.send.summary.empty()) out += "Sent: " + s.send.summary + "\n";
    for (const WolPacket &p : s.send.packets) out += "  " + p.dest.label() + ": " + p.words + "\n";
    if (s.watch) {
        out += "Watch: " + s.watch_target;
        if (!s.watch_address.empty() && s.watch_address != s.watch_target) out += " (" + s.watch_address + ")";
        out += ", " + std::to_string(s.attempts) + " pings: " + s.watch_words + "\n";
    }
    if (!s.hint.empty()) out += "Note: " + s.hint + "\n";
    return out;
}

std::string format_ble(const BleSnapshot &s)
{
    std::string out;
    if (!s.adapter.empty()) out += "Adapter: " + s.adapter + "\n";
    if (!s.error.empty()) out += "Error: " + s.error + "\n";
    out += "Scan: " + fmt_s(s.elapsed_s) + " of " + std::to_string(s.seconds) + " s; " + std::to_string(s.live) +
           " devices in range, " + std::to_string(s.seen) + " seen in all\n";
    if (!s.stop_note.empty()) out += s.stop_note + "\n";
    if (s.other_discovery) out += "Note: another program was scanning too.\n";
    if (!s.devices.empty()) out += "Address  Type  RSSI (smoothed)  Distance  TX  Vendor  Name\n";
    for (const BleDevice &d : s.devices) {
        char rssi[48];
        std::snprintf(rssi, sizeof(rssi), "%d dBm (%.0f)", d.rssi_dbm, d.smooth_dbm);
        out += d.address + "  " + (d.address_type.empty() ? "-" : d.address_type) + "  " + rssi + "  " +
               distance_words(d.distance) + "  " + (d.has_tx ? std::to_string(d.tx_dbm) + " dBm" : "-") + "  " +
               (d.vendor.empty() ? "-" : d.vendor) + "  " + (d.name.empty() ? "-" : printable(d.name)) + "\n";
    }
    out += std::string("Distance: ") + kBleDistanceCaveat + "\n";
    return out;
}

} // namespace nt
