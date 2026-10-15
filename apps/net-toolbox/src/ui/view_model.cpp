/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: what the pages say (see view_model.hpp).
 */
#include "view_model.hpp"

#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace nt {
namespace ui {
namespace {

RowDef item(std::string label, std::string value, Tone tone = Tone::Normal, int id = 0)
{
    RowDef r;
    r.label = std::move(label);
    r.value = std::move(value);
    r.tone = tone;
    r.id = id;
    return r;
}

RowDef heading(std::string title)
{
    RowDef r;
    r.section = true;
    r.label = std::move(title);
    return r;
}

std::string ms_text(int ms) { return std::to_string(ms) + " ms"; }

std::string capital(std::string s)
{
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

} // namespace

/* ---------------------------------------------------------------- Home, Interfaces */

Band home_band(const NetState &st)
{
    Band b;
    const Iface *main = st.main();
    if (!main || !main->connected()) {
        b.text = "No network";
        b.tone = Tone::Warn;
        return b;
    }
    const std::string ip = main->first_ipv4();
    if (ip.empty()) {
        b.text = main->name + ": no address yet";
        b.tone = Tone::Warn;
        return b;
    }
    b.text = main->name + ": " + ip;
    b.tone = Tone::Ok;
    return b;
}

const char *signal_word(double dbm)
{
    if (dbm >= -50) return "Excellent";
    if (dbm >= -60) return "Good";
    if (dbm >= -70) return "Fair";
    return "Weak";
}

Tone signal_tone(double dbm)
{
    if (dbm >= -60) return Tone::Ok;
    if (dbm >= -70) return Tone::Warn;
    return Tone::Bad;
}

std::string signal_text(double dbm)
{
    return std::to_string(static_cast<int>(std::lround(dbm))) + " dBm, " + signal_word(dbm);
}

std::vector<RowDef> interface_rows(const NetState &st)
{
    std::vector<RowDef> rows;
    for (const Iface &i : st.ifaces) {
        rows.push_back(heading(i.name));
        if (i.connected()) rows.push_back(item("State", "Connected", Tone::Ok));
        else if (i.loopback) rows.push_back(item("State", i.up ? "Up" : "Down"));
        else if (i.up) rows.push_back(item("State", i.wireless ? "Not connected" : "No link", Tone::Warn));
        else rows.push_back(item("State", "Down"));
        if (i.ipv4.empty()) rows.push_back(item("IPv4", "none"));
        for (const std::string &a : i.ipv4) rows.push_back(item("IPv4", a));
        if (i.ipv6.empty()) rows.push_back(item("IPv6", "none"));
        for (const std::string &a : i.ipv6) rows.push_back(item("IPv6", a));
        if (i.loopback) continue;
        if (!i.mac.empty()) rows.push_back(item("MAC", i.mac));
        if (!i.wireless) {
            const std::string speed = fmt_speed(i.speed_mbps);
            rows.push_back(item("Speed", speed.empty() ? "unknown" : speed));
            continue;
        }
        if (i.link_valid) {
            if (i.link.connected) rows.push_back(item("Network", printable(i.link.ssid)));
            else rows.push_back(item("Network", "Not connected", Tone::Warn));
        }
        const bool associated = !i.link_valid || i.link.connected;
        if (associated) {
            if (i.link_valid && i.link.has_signal)
                rows.push_back(item("Signal", signal_text(i.link.signal_dbm), signal_tone(i.link.signal_dbm)));
            else if (i.has_level)
                rows.push_back(item("Signal", signal_text(i.level_dbm), signal_tone(i.level_dbm)));
        }
        if (i.link_valid && i.link.connected && i.link.has_bitrate)
            rows.push_back(item("Bit rate", fmt_mbit(i.link.tx_mbit)));
    }
    rows.push_back(heading("Network"));
    if (st.route.ok) rows.push_back(item("Gateway", st.route.gateway));
    else rows.push_back(item("Gateway", "none", Tone::Warn));
    if (st.dns.servers.empty()) rows.push_back(item("DNS server", "none", Tone::Warn));
    for (std::size_t k = 0; k < st.dns.servers.size(); ++k)
        rows.push_back(item("DNS server " + std::to_string(k + 1), st.dns.servers[k] + (k == 0 ? " (asked first)" : "")));
    if (!st.dns.search.empty()) {
        std::string joined;
        for (const std::string &s : st.dns.search) joined += (joined.empty() ? "" : " ") + s;
        rows.push_back(item("Search", joined));
    }
    return rows;
}

/* ---------------------------------------------------------------- Diagnosis */

std::string step_text(const DiagStep &step)
{
    switch (step.state) {
    case StepState::Pending: return "waiting";
    case StepState::Running: return "checking...";
    case StepState::Skipped: return "skipped";
    case StepState::Pass: return step.detail.empty() ? "ok" : step.detail;
    case StepState::Fail: return step.detail.empty() ? "failed" : step.detail;
    }
    return std::string();
}

Tone step_tone(StepState state)
{
    switch (state) {
    case StepState::Pass: return Tone::Ok;
    case StepState::Fail: return Tone::Bad;
    case StepState::Running: return Tone::Warn;
    default: return Tone::Normal;
    }
}

DiagMessage diag_message(const DiagReport &r)
{
    DiagMessage m;
    switch (r.state) {
    case DiagReport::State::Idle:
    case DiagReport::State::Running:
        m.head = "Checking your connection...";
        break;
    case DiagReport::State::Passed: {
        m.head = "Everything works";
        m.tone = Tone::Ok;
        for (const std::string &n : r.notes) m.body += (m.body.empty() ? "" : "\n") + std::string("- ") + n;
        if (m.body.empty()) m.body = r.explanation;
        break;
    }
    case DiagReport::State::Failed:
        m.head = r.failed >= 0 && r.failed < kStepCount
                     ? std::string(step_title(static_cast<Step>(r.failed))) + " failed"
                     : std::string("Something is wrong");
        m.tone = Tone::Bad;
        m.body = r.explanation;
        break;
    case DiagReport::State::Cancelled:
        m.head = "Stopped";
        break;
    }
    return m;
}

/* ---------------------------------------------------------------- Ping */

double nice_top_ms(double max_ms)
{
    const double want = std::max(10.0, max_ms * 1.1);
    double decade = 10.0;
    for (int guard = 0; guard < 12; ++guard, decade *= 10.0)
        for (const double m : {1.0, 1.5, 2.0, 3.0, 5.0, 7.5})
            if (decade * m >= want) return decade * m;
    return decade;
}

double max_rtt_ms(const std::vector<PacketResult> &packets)
{
    double m = 0;
    for (const PacketResult &p : packets)
        if (p.state == PacketState::Ok || p.state == PacketState::Slow) m = std::max(m, p.rtt_ms);
    return m;
}

int ring_slot(std::size_t i, std::size_t n)
{
    const std::size_t cap = kRingSize;
    const std::size_t shown = std::min(n, cap);
    const std::size_t first = n - shown; /* the oldest packet that fits */
    if (i < first) return -1;
    return static_cast<int>(cap - shown + (i - first));
}

Cell packet_cell(PacketState state)
{
    Cell c;
    c.state = state;
    switch (state) {
    case PacketState::Ok: c.height_pct = 45; break;
    case PacketState::Slow: c.height_pct = 70; break;
    case PacketState::Lost: c.height_pct = 100; break;
    default: c.height_pct = 25; break;
    }
    return c;
}

const char *packet_word(PacketState state)
{
    switch (state) {
    case PacketState::Ok: return "ok";
    case PacketState::Slow: return "slow";
    case PacketState::Lost: return "lost";
    default: return "waiting";
    }
}

Band ping_head(const PingSnapshot &s)
{
    Band b;
    for (auto it = s.recent.rbegin(); it != s.recent.rend(); ++it) {
        if (it->state == PacketState::Pending) continue;
        if (it->state == PacketState::Ok) {
            b.text = "now " + fmt_ms(it->rtt_ms);
            b.tone = Tone::Ok;
        } else if (it->state == PacketState::Slow) {
            b.text = "now " + fmt_ms(it->rtt_ms) + " (slow)";
            b.tone = Tone::Warn;
        } else {
            b.text = "last packet lost";
            b.tone = Tone::Bad;
        }
        return b;
    }
    if (s.running()) b.text = "waiting for the first reply";
    return b;
}

std::string ping_scale(double top_ms, std::size_t packets)
{
    std::string t = "0-" + std::to_string(static_cast<long>(std::lround(top_ms))) + " ms";
    if (packets > 0) t += ", last " + std::to_string(std::min(packets, kRingSize)) + " packets";
    return t;
}

std::string ping_stats(const PingStats &st)
{
    if (st.transmitted == 0) return std::string();
    std::string t = "sent " + std::to_string(st.transmitted) + "  lost " + std::to_string(st.lost) + " (" +
                    fmt_pct(st.loss_pct()) + ")";
    if (st.received)
        t += "  min " + fmt_ms(st.min_ms) + "  avg " + fmt_ms(st.avg_ms()) + "  max " + fmt_ms(st.max_ms) + "  jitter " +
             fmt_ms(st.jitter_ms());
    return t;
}

Band ping_band(const PingSnapshot &s)
{
    Band b;
    const std::string where = s.address.empty() || s.address == s.target ? s.target : s.target + " (" + s.address + ")";
    switch (s.state) {
    case PingSnapshot::State::Idle:
        b.text = "Press Start to ping";
        break;
    case PingSnapshot::State::Resolving:
        b.text = "Finding " + s.target + "...";
        break;
    case PingSnapshot::State::Running:
        if (!s.error.empty()) {
            b.text = s.error;
            b.tone = Tone::Warn;
        } else {
            b.text = "Pinging " + where;
            b.tone = Tone::Ok;
        }
        break;
    case PingSnapshot::State::Done:
        b.text = "Done, " + std::to_string(s.stats.transmitted) + " packets sent to " + where;
        break;
    case PingSnapshot::State::Failed:
        b.text = s.error.empty() ? std::string("Ping failed") : s.error;
        b.tone = Tone::Bad;
        break;
    case PingSnapshot::State::Cancelled:
        b.text = "Stopped, " + std::to_string(s.stats.transmitted) + " packets sent to " + where;
        break;
    }
    return b;
}

/* ---------------------------------------------------------------- DNS lookup */

std::string resolver_label(const std::string &server, const std::string &system_server)
{
    for (const ResolverPreset &p : resolver_presets())
        if (p.address[0] && server == p.address) return std::string(p.label) + " " + server;
    if (!system_server.empty() && server == system_server) return "System " + server;
    return server;
}

Band dns_status(const DnsSnapshot &s)
{
    Band b;
    switch (s.state) {
    case DnsSnapshot::State::Idle:
        b.text = "Type a name and press Look up";
        return b;
    case DnsSnapshot::State::Failed:
        b.text = s.error.empty() ? std::string("The lookup failed") : s.error;
        b.tone = Tone::Bad;
        return b;
    case DnsSnapshot::State::Cancelled:
        b.text = "Stopped";
        return b;
    default: break;
    }
    const bool running = s.state == DnsSnapshot::State::Running;
    if (s.compare()) {
        int best = -1;
        std::string who;
        for (const DnsResult &r : s.results)
            if (r.ok() && (best < 0 || r.elapsed_ms < best)) {
                best = r.elapsed_ms;
                who = r.server;
            }
        if (running) {
            b.text = "Asking resolver " + std::to_string(s.results.size() + 1) + " of " + std::to_string(s.servers.size()) + "...";
            return b;
        }
        if (best < 0) {
            b.text = "No resolver answered";
            b.tone = Tone::Bad;
        } else {
            b.text = "Compared " + std::to_string(s.servers.size()) + " resolvers, fastest " + who + " " + ms_text(best);
            b.tone = Tone::Ok;
        }
        return b;
    }
    if (s.results.empty()) {
        b.text = "Asking " + (s.servers.empty() ? std::string("the resolver") : s.servers[0]) + "...";
        return b;
    }
    const DnsResult &r = s.results[0];
    if (!r.answered) {
        b.text = "No answer: " + r.error + " after " + ms_text(r.elapsed_ms) + ", via " + r.server;
        b.tone = Tone::Bad;
        return b;
    }
    b.text = capital(r.rcode_words) + " (" + r.rcode_name + "), " + ms_text(r.elapsed_ms) + ", via " + r.server;
    if (r.via_tcp) b.text += ", TCP";
    b.tone = r.rcode == dns::kNoError ? Tone::Ok : Tone::Warn;
    return b;
}

std::vector<RowDef> dns_rows(const DnsSnapshot &s, const std::string &system_server)
{
    std::vector<RowDef> rows;
    if (s.state == DnsSnapshot::State::Idle || s.state == DnsSnapshot::State::Failed) return rows;
    const std::string what = s.question + "  " + dns::type_name(s.type);
    if (s.compare()) {
        rows.push_back(heading("Compare: " + what));
        for (std::size_t k = 0; k < s.servers.size(); ++k) {
            const std::string label = resolver_label(s.servers[k], system_server);
            if (k >= s.results.size()) {
                const bool asking = k == s.results.size() && s.state == DnsSnapshot::State::Running;
                rows.push_back(item(label, asking ? "asking..." : "waiting"));
                continue;
            }
            const DnsResult &r = s.results[k];
            if (!r.answered)
                rows.push_back(item(label, "no answer, " + ms_text(r.elapsed_ms), Tone::Bad));
            else if (r.rcode != dns::kNoError)
                rows.push_back(item(label, r.rcode_name + ", " + ms_text(r.elapsed_ms), Tone::Warn));
            else
                rows.push_back(item(label, ms_text(r.elapsed_ms) + ", " + std::to_string(r.answers.size()) +
                                               (r.answers.size() == 1 ? " answer" : " answers"),
                                    Tone::Ok));
        }
        return rows;
    }
    rows.push_back(heading("Answers: " + what));
    if (s.results.empty()) return rows;
    const DnsResult &r = s.results[0];
    if (!r.answered) {
        rows.push_back(item("No answer", r.error, Tone::Bad));
        return rows;
    }
    for (const dns::Record &rec : r.answers) {
        const std::string label = dns::names_equal(rec.name, s.question) ? rec.data : rec.name + "  " + rec.data;
        rows.push_back(item(label, dns::type_name(rec.type) + "  " + std::to_string(rec.ttl) + " s"));
    }
    if (r.answers.empty())
        rows.push_back(item(r.rcode == dns::kNoError ? "No records of this type" : "No records", r.rcode_name, Tone::Warn));
    return rows;
}

/* ---------------------------------------------------------------- Port check */

const char *port_word(PortState state)
{
    switch (state) {
    case PortState::Open: return "Open";
    case PortState::Closed: return "Closed";
    case PortState::Filtered: return "No answer";
    case PortState::Unreachable: return "Unreachable";
    case PortState::Error: return "Error";
    default: return "Checking...";
    }
}

namespace {

Tone port_tone(PortState state)
{
    switch (state) {
    case PortState::Open: return Tone::Ok;
    case PortState::Filtered: return Tone::Warn;
    case PortState::Pending: return Tone::Normal;
    default: return Tone::Bad;
    }
}

Mark port_mark(PortState state)
{
    switch (state) {
    case PortState::Open: return Mark::Check;
    case PortState::Filtered: return Mark::Dash;
    case PortState::Unreachable: return Mark::Bang;
    case PortState::Pending: return Mark::None;
    default: return Mark::Cross;
    }
}

std::string port_name(std::uint16_t port)
{
    std::string t = std::to_string(port);
    const char *svc = port_service(port);
    if (svc && svc[0]) t += std::string(" (") + svc + ")";
    return t;
}

} // namespace

PortSummary port_summary(const PortSnapshot &s)
{
    PortSummary p;
    p.host = s.host;
    if (!s.address.empty() && s.address != s.host) p.host += " (" + s.address + ")";
    switch (s.state) {
    case PortSnapshot::State::Idle:
        p.detail = "Choose a host and a port, then press Run";
        return p;
    case PortSnapshot::State::Resolving:
        p.word = "Finding host...";
        return p;
    case PortSnapshot::State::Failed:
        p.word = "Not checked";
        p.detail = s.error;
        p.tone = Tone::Bad;
        p.mark = Mark::Cross;
        return p;
    default: break;
    }
    if (s.results.empty()) return p;
    bool same = true;
    for (const PortResult &r : s.results) same = same && r.state == s.results[0].state;
    if (s.results.size() == 1 || same) {
        const PortResult &r = s.results[0];
        p.word = port_word(r.state);
        p.tone = port_tone(r.state);
        p.mark = port_mark(r.state);
    } else {
        p.word = "Mixed";
        p.tone = Tone::Warn;
        p.mark = Mark::Bang;
    }
    if (s.results.size() == 1) {
        const PortResult &r = s.results[0];
        const std::string port = port_name(r.port);
        switch (r.state) {
        case PortState::Open: p.detail = port + ": connected in " + ms_text(r.connect_ms); break;
        case PortState::Closed:
            p.detail = port + ": refused after " + ms_text(r.connect_ms) + ". The host is there, nothing listens on it";
            break;
        case PortState::Filtered:
            p.detail = port + ": no answer" + (r.detail.empty() ? "" : " (" + r.detail + ")") + ". A firewall may drop it";
            break;
        case PortState::Unreachable: p.detail = port + ": " + (r.detail.empty() ? "the network says no route" : r.detail); break;
        case PortState::Error: p.detail = port + ": " + (r.detail.empty() ? "error" : r.detail); break;
        default: p.detail = port + ": checking..."; break;
        }
    } else {
        for (const PortResult &r : s.results) {
            p.detail += (p.detail.empty() ? "" : "   ") + std::to_string(r.port) + " " + port_word(r.state);
            if (r.state == PortState::Open || r.state == PortState::Closed) p.detail += " " + ms_text(r.connect_ms);
        }
    }
    return p;
}

std::string port_choice_text(const std::string &preset, int port)
{
    if (!preset.empty())
        for (const PortPreset &p : port_presets())
            if (preset == p.label) {
                std::string t = p.label;
                for (std::size_t i = 0; i < p.ports.size(); ++i) t += (i ? ", " : " ") + std::to_string(p.ports[i]);
                return t;
            }
    return port_name(static_cast<std::uint16_t>(port));
}

/* ---------------------------------------------------------------- Save log */

Band save_band(bool ok, const std::string &path_or_error)
{
    Band b;
    if (!ok) {
        b.text = path_or_error;
        b.tone = Tone::Bad;
        return b;
    }
    std::string p = path_or_error;
    const char *home = std::getenv("HOME");
    if (home && home[0] && p.rfind(std::string(home) + "/", 0) == 0) p = "~" + p.substr(std::string(home).size());
    b.text = "Saved " + p;
    b.tone = Tone::Ok;
    return b;
}

} // namespace ui
} // namespace nt
