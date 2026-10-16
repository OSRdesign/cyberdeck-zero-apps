/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the saved text of the Wi-Fi networks and of the walk test (see netlog_wifi.hpp).
 */
#include "netlog_wifi.hpp"

#include "text.hpp"

#include <cmath>
#include <cstdio>

namespace nt {
namespace {

/* "12", "0.5" */
std::string num(double s)
{
    char buf[32];
    if (std::fabs(s - std::round(s)) < 1e-6) std::snprintf(buf, sizeof(buf), "%.0f", s);
    else std::snprintf(buf, sizeof(buf), "%.1f", s);
    return buf;
}

/* "12 s", "0.5 s" */
std::string secs(double s)
{
    return num(s) + " s";
}

std::string at_ms(std::int64_t ms)
{
    return secs(static_cast<double>(ms) / 1000.0);
}

std::string dbm(int v)
{
    return std::to_string(v) + " dBm";
}

std::string signal_text(const WifiNetwork &n)
{
    return std::to_string(n.signal_pct) + "% (about " + dbm(n.dbm) + ")";
}

const char *wifi_state_words(WifiSnapshot::State s)
{
    switch (s) {
    case WifiSnapshot::State::Idle: return "not started";
    case WifiSnapshot::State::Scanning: return "scanning";
    case WifiSnapshot::State::Ready: return "ready";
    case WifiSnapshot::State::Failed: return "failed";
    case WifiSnapshot::State::Stopped: return "stopped";
    }
    return "";
}

const char *walk_state_words(WalkSnapshot::State s)
{
    switch (s) {
    case WalkSnapshot::State::Idle: return "not started";
    case WalkSnapshot::State::Running: return "running";
    case WalkSnapshot::State::Stopped: return "stopped";
    case WalkSnapshot::State::Failed: return "failed";
    }
    return "";
}

std::string band_line(const BandChannels &b)
{
    std::string out = std::string("  ") + band_words(b.band) + ": ";
    if (b.networks == 0 && b.band != WifiBand::G24) return out + "none seen\n";
    out += std::to_string(b.networks) + (b.networks == 1 ? " network" : " networks");
    bool any = false;
    for (const ChannelLoad &c : b.channels) {
        if (c.networks == 0) continue;
        out += any ? ", " : "; ";
        out += "channel " + std::to_string(c.channel) + ": " + std::to_string(c.networks);
        any = true;
    }
    if (b.recommended) out += "; least crowded: channel " + std::to_string(b.recommended);
    return out + "\n";
}

} // namespace

std::string format_wifi_networks(const WifiSnapshot &s)
{
    std::string out = "Interface: " + (s.iface.empty() ? std::string("none") : printable(s.iface)) + "\n";
    out += std::string("State: ") + wifi_state_words(s.state);
    if (s.state == WifiSnapshot::State::Ready || s.state == WifiSnapshot::State::Stopped)
        out += ", " + std::to_string(s.networks.size()) + (s.networks.size() == 1 ? " network" : " networks");
    out += "\n";
    if (!s.error.empty()) out += "Error: " + s.error + "\n";
    if (s.cached) out += "Note: NetworkManager's last results (it did not allow a new scan then).\n";
    if (!s.networks.empty()) out += "Name  BSSID  Signal  Channel  Band  Bit rate  Security  Vendor  Notes\n";
    for (const WifiNetwork &n : s.networks) {
        std::string notes;
        if (n.in_use) notes = "in use";
        if (n.adhoc) notes += notes.empty() ? "ad-hoc" : ", ad-hoc";
        if (n.local_bssid) notes += notes.empty() ? "local address" : ", local address";
        out += printable(n.name()) + "  " + n.bssid + "  " + signal_text(n) + "  " + std::to_string(n.channel) + "  " +
               band_words(n.band) + "  " + std::to_string(n.rate_mbit) + " Mbit/s  " + n.security_text + "  " +
               (n.vendor.empty() ? "-" : n.vendor) + "  " + (notes.empty() ? "-" : notes) + "\n";
    }
    if (!s.networks.empty()) {
        out += "Channels:\n";
        out += band_line(s.channels.g24);
        out += band_line(s.channels.g5);
        out += band_line(s.channels.g6);
    }
    return out;
}

std::string format_wifi_network(const WifiNetwork &n, bool gone)
{
    std::string out = "Network: " + printable(n.name()) + (gone ? " (gone: not in the last scan; its last values)" : "") + "\n";
    out += "BSSID: " + n.bssid + (n.local_bssid ? " (locally administered)" : "") + "\n";
    out += "Vendor: " + (n.vendor.empty() ? std::string("unknown") : n.vendor) + "\n";
    out += "Signal: " + signal_text(n) + "\n";
    out += "Channel: " + std::to_string(n.channel) + " (" + std::to_string(n.freq_mhz) + " MHz, " + band_words(n.band) + ")\n";
    out += "Security: " + n.security_text;
    if (!n.security_raw.empty() && n.security_raw != n.security_text) out += " (" + n.security_raw + ")";
    out += "\n";
    out += "Bit rate: " + std::to_string(n.rate_mbit) + " Mbit/s\n";
    out += std::string("Mode: ") + (n.adhoc ? "ad-hoc" : "access point") + "\n";
    out += std::string("In use: ") + (n.in_use ? "yes" : "no") + "\n";
    return out;
}

std::string format_walktest(const WalkSnapshot &s)
{
    const WalkSummary &m = s.summary;
    std::string out = "Interface: " + (s.iface.empty() ? std::string("none") : printable(s.iface)) + "\n";
    out += std::string("State: ") + walk_state_words(s.state) + "\n";
    if (!s.error.empty()) out += "Error: " + s.error + "\n";
    out += "Weak zone: below " + dbm(s.weak_dbm) + " or no link (a dip ends after " + at_ms(s.hysteresis_ms) +
           " back above)\n";
    out += "Duration: " + secs(m.duration_s) + ", " + std::to_string(m.samples) + " samples, one every " +
           at_ms(s.interval_ms) + "\n";
    if (m.has_signal) {
        char avg[32];
        std::snprintf(avg, sizeof(avg), "%.1f dBm", m.average_dbm);
        out += std::string("Signal: average ") + avg + ", strongest " + dbm(m.strongest_dbm) + " at " +
               secs(m.strongest_at_s) + ", weakest " + dbm(m.weakest_dbm) + " at " + secs(m.weakest_at_s) + "\n";
    } else {
        out += "Signal: no reading\n";
    }
    out += "Weak zone: " + secs(m.weak_s) + " (" + fmt_pct(m.weak_pct) + ") in " + std::to_string(m.dips) +
           (m.dips == 1 ? " dip" : " dips");
    if (m.nolink_samples) out += "; no link " + secs(m.nolink_s);
    out += "\n";
    if (!s.dips.empty()) out += "Dips:\n";
    for (const WalkDip &d : s.dips) {
        out += "  " + at_ms(d.start_ms) + " to " + (d.end_ms < 0 ? std::string("the end") : at_ms(d.end_ms));
        if (d.has_dbm) out += ", lowest " + dbm(d.lowest_dbm);
        if (d.lost_link) out += ", link lost";
        out += "\n";
    }
    if (m.dips > static_cast<int>(s.dips.size()))
        out += "  (and " + std::to_string(m.dips - static_cast<int>(s.dips.size())) + " more)\n";
    if (!m.roaming_known) {
        out += std::string("Access point changes: not known") + (s.iw_missing ? " (iw is not installed)" : "") + "\n";
    } else {
        out += "Access point changes: " + std::to_string(m.roams) + "\n";
        for (const WalkRoam &r : s.roams) {
            out += "  at " + at_ms(r.t_ms) + ": " + r.from + " -> " + r.to;
            if (!r.ssid.empty()) out += " (" + printable(r.ssid) + ")";
            if (r.has_dbm) out += ", " + dbm(r.dbm);
            out += "\n";
        }
        if (m.roams > static_cast<int>(s.roams.size()))
            out += "  (and " + std::to_string(m.roams - static_cast<int>(s.roams.size())) + " more)\n";
    }
    if (!s.samples.empty()) {
        out += "Samples (s dBm, - = no link";
        if (s.dropped) out += "; the last " + std::to_string(s.samples.size());
        out += "):\n";
        int on_line = 0;
        for (const WalkSample &x : s.samples) {
            out += on_line ? ", " : "  ";
            out += num(static_cast<double>(x.t_ms) / 1000.0) + " " + (x.linked ? std::to_string(x.dbm) : std::string("-"));
            if (++on_line == 10) {
                out += "\n";
                on_line = 0;
            }
        }
        if (on_line) out += "\n";
    }
    return out;
}

} // namespace nt
