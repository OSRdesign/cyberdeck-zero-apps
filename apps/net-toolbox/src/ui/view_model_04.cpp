/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the words and numbers of the Local devices, Wi-Fi networks and Wi-Fi walk test pages (see view_model_04.hpp).
 */
#include "view_model_04.hpp"

#include "netaddr.hpp"
#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace nt {
namespace ui {

namespace {

std::string fmt(const char *format, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    return buf;
}

std::string plural(int n, const char *one, const char *many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

bool has(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

RowDef row(const std::string &label, const std::string &value, Tone tone = Tone::Normal)
{
    RowDef r;
    r.label = label;
    r.value = value;
    r.tone = tone;
    return r;
}

int ceil10(double v) { return static_cast<int>(std::ceil(v / 10.0)) * 10; }
int floor10(double v) { return static_cast<int>(std::floor(v / 10.0)) * 10; }

/* a text cut to `max` bytes at a character boundary */
std::string cut_utf8(std::string s, std::size_t max)
{
    if (s.size() <= max) return s;
    std::size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.erase(n);
    return trim(s);
}

} // namespace

/* ================================================================ Local devices */

std::string rtt_text(double ms)
{
    if (ms < 1.0) return "<1 ms";
    return fmt("%.0f ms", ms);
}

std::string device_title(const Device &d) { return d.name.empty() ? d.ip : d.name; }

std::string device_vendor_text(const Device &d)
{
    if (d.vendor_private) return kPrivateAddressWords;
    if (!d.vendor.empty()) return d.vendor;
    if (!d.mac.empty()) return "Vendor unknown";
    return "";
}

DeviceRowTexts device_row_texts(const Device &d)
{
    DeviceRowTexts t;
    t.title = device_title(d);
    t.ip = d.name.empty() ? std::string() : d.ip;
    const std::string vendor = device_vendor_text(d);
    t.detail = vendor;
    t.vendor = vendor.empty() ? d.mac : vendor;
    if (!d.mac.empty()) t.detail += (t.detail.empty() ? "" : "  ") + d.mac;
    if (d.has_rtt) t.rtt = rtt_text(d.rtt_ms);
    t.self = d.is_self;
    t.router = d.is_gateway && !d.is_self;
    t.tag = t.self ? "this device" : t.router ? "router" : "";
    return t;
}

Band devices_band(const DevicesSnapshot &s)
{
    Band b;
    switch (s.state) {
    case DevicesSnapshot::State::Idle: b.text = "Press Scan to look for devices on your network"; break;
    case DevicesSnapshot::State::Scanning:
        if (s.total > 0 && s.probed >= s.total && s.naming)
            b.text = "Looking up names: " + plural(s.found, "device", "devices") + " on " + s.subnet;
        else
            b.text = "Scanning " + s.subnet + ": " + std::to_string(s.probed) + " of " + std::to_string(s.total) + ", " +
                     std::to_string(s.found) + " found";
        break;
    case DevicesSnapshot::State::Done: b.text = "Done: " + plural(s.found, "device", "devices") + " on " + s.subnet; break;
    case DevicesSnapshot::State::Cancelled:
        b.text = "Stopped: " + plural(s.found, "device", "devices") + " on " + s.subnet;
        b.tone = Tone::Warn;
        break;
    case DevicesSnapshot::State::Failed:
        b.text = devices_headline(s.error);
        b.tone = s.error == "No network" ? Tone::Warn : Tone::Bad;
        break;
    }
    return b;
}

std::string devices_headline(const std::string &error)
{
    if (error.empty()) return "The scan could not run";
    if (has(error, "not a private address")) return "Only private networks can be scanned";
    if (has(error, "no other addresses")) return "Nothing to scan on this network";
    return error.size() <= 48 ? error : std::string("The scan could not run");
}

std::string devices_fix(const std::string &error)
{
    if (error == "No network") return "Connect to a network first, or choose another one under Network.";
    if (has(error, "not a private address")) return "Connect to your home network, or choose another one under Network.";
    if (has(error, "no other addresses")) return "Choose another network under Network.";
    return "";
}

DevicesEmpty devices_empty(const DevicesSnapshot &s)
{
    DevicesEmpty e;
    switch (s.state) {
    case DevicesSnapshot::State::Idle: e.text = kDevicesHint; break;
    case DevicesSnapshot::State::Scanning: e.text = "No device found yet."; break;
    case DevicesSnapshot::State::Failed:
        e.text = s.error.empty() ? std::string("The scan could not run") : s.error;
        e.tone = s.error == "No network" ? Tone::Warn : Tone::Bad;
        e.fix = devices_fix(s.error);
        break;
    default: e.text = "No device answered. Some devices ignore scans."; break;
    }
    return e;
}

std::string device_role(const Device &d)
{
    if (d.is_self && d.is_gateway) return "This device, the gateway";
    if (d.is_self) return "This device";
    if (d.is_gateway) return "Router: the gateway of this network";
    return "Another device";
}

std::vector<RowDef> device_info_rows(const Device &d)
{
    std::vector<RowDef> rows;
    rows.push_back(row("IP address", d.ip));
    std::string name = "none found";
    if (!d.name.empty()) {
        name = d.name;
        const char *src = name_source_label(d.name_source);
        if (src[0]) name += std::string("  (from ") + src + ")";
    }
    rows.push_back(row("Name", name));
    rows.push_back(row("MAC address", d.mac.empty() ? std::string("not known") : d.mac));
    std::string vendor = device_vendor_text(d);
    if (d.vendor_private) vendor += ": the vendor cannot be known";
    else if (vendor.empty()) vendor = "not known";
    rows.push_back(row("Vendor", vendor));
    rows.push_back(row("Round trip", d.has_rtt ? rtt_text(d.rtt_ms) : std::string(d.is_self ? "this device" : "no answer to ping")));
    rows.push_back(row("Role", device_role(d)));
    return rows;
}

std::string device_band_text(const Device &d)
{
    const DeviceRowTexts t = device_row_texts(d);
    return t.tag.empty() ? d.ip : d.ip + ", " + t.tag;
}

Band portscan_band(const PortScanSnapshot &s)
{
    Band b;
    switch (s.state) {
    case PortScanSnapshot::State::Idle: break;
    case PortScanSnapshot::State::Scanning:
        b.text = "Scanning " + std::to_string(s.scanned) + " of " + std::to_string(s.total) + ": " + std::to_string(s.open_count) + " open";
        break;
    case PortScanSnapshot::State::Done:
        if (s.open_count > 0) b.text = "Done: " + plural(s.open_count, "open port", "open ports") + " among " + std::to_string(s.total);
        else b.text = "Done: no open port among " + std::to_string(s.total);
        break;
    case PortScanSnapshot::State::Cancelled:
        b.text = "Stopped at " + std::to_string(s.scanned) + " of " + std::to_string(s.total) + ": " + std::to_string(s.open_count) + " open";
        b.tone = Tone::Warn;
        break;
    case PortScanSnapshot::State::Failed:
        b.text = s.error.empty() ? std::string("The port scan could not run") : s.error;
        b.tone = Tone::Bad;
        break;
    }
    return b;
}

std::string portscan_empty_text(const PortScanSnapshot &s)
{
    if (s.state != PortScanSnapshot::State::Done || s.open_count > 0) return "";
    return s.full_range ? "No open port found in ports 1-1024" : "No open port found among the common ones";
}

RowDef open_port_row(const OpenPort &p)
{
    return row("Port " + std::to_string(p.port) + (p.service.empty() ? std::string() : "  " + p.service), "open", Tone::Ok);
}

bool wol_machine_of(const Device &d, const Settings &settings, WolMachine &out, std::string *why)
{
    if (d.mac.empty()) {
        if (why) *why = "This device has no MAC address to save.";
        return false;
    }
    std::string base;
    for (const char ch : device_title(d)) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F || ch == '|') continue;
        base += ch;
    }
    base = cut_utf8(trim(base), kWolNameMax);
    if (base.empty()) base = d.ip;
    std::string name = base;
    for (int n = 2; settings.find_machine(name) && n < 100; ++n)
        name = cut_utf8(base, kWolNameMax - 3) + " " + std::to_string(n);
    out = WolMachine();
    out.name = name;
    out.mac = d.mac;
    out.host = d.ip;
    return true;
}

std::string wol_save_text(const std::string &name) { return "Saved " + name + " for Wake-on-LAN"; }

std::string wol_machine_with_mac(const Settings &settings, const std::string &mac)
{
    for (const WolMachine &m : settings.wol_machines)
        if (lower(m.mac) == lower(mac)) return m.name;
    return "";
}

/* ================================================================ Wi-Fi networks */

int signal_bars(double dbm)
{
    if (dbm >= -50.0) return 4;
    if (dbm >= -60.0) return 3;
    if (dbm >= -70.0) return 2;
    return 1;
}

WifiRowTexts wifi_row_texts(const WifiNetwork &n)
{
    WifiRowTexts t;
    t.title = n.name();
    t.dbm = fmt("%d dBm", n.dbm);
    t.bars = signal_bars(n.dbm);
    t.in_use = n.in_use;
    t.connected = n.in_use ? "connected" : "";
    if (n.channel > 0) t.caption = "ch " + std::to_string(n.channel);
    const std::string band = band_words(n.band);
    if (!band.empty()) t.caption += (t.caption.empty() ? "" : ", ") + band;
    if (n.adhoc) t.caption += (t.caption.empty() ? "" : ", ") + std::string("ad hoc");
    t.security = n.security_text;
    t.weak_security = security_weak(n.security);
    return t;
}

std::string scanned_ago(std::int64_t now_ms, std::int64_t list_at_ms)
{
    const std::int64_t s = std::max<std::int64_t>(0, now_ms - list_at_ms) / 1000;
    if (s < 2) return "scanned just now";
    if (s < 60) return "scanned " + std::to_string(s) + " s ago";
    return "scanned " + std::to_string(s / 60) + " min ago";
}

WifiProblem wifi_problem(const std::string &error, const std::string &wifi_hint)
{
    WifiProblem p;
    p.words = error;
    if (has(error, "No Wi-Fi adapter on this board")) {
        p.headline = "No Wi-Fi adapter on this board";
    } else if (has(error, "No Wi-Fi adapter on")) {
        const std::string choose = wifi_hint.empty() ? std::string("choose a Wi-Fi adapter under Network") : "choose " + wifi_hint + " under Network";
        p.headline = error + ": " + choose;
        p.fix = "Choose a Wi-Fi adapter under Network on the first page.";
    } else if (has(error, "nmcli is missing")) {
        p.headline = "nmcli is missing: the scan needs NetworkManager";
        p.fix = "Install the package network-manager.";
    } else if (has(error, "NetworkManager is not running")) {
        p.headline = "NetworkManager is not running";
        p.fix = "Start it with: sudo systemctl start NetworkManager";
    } else if (has(error, "switched off")) {
        p.headline = "Wi-Fi is switched off";
        p.fix = "Turn Wi-Fi on, then try again.";
    } else if (has(error, "did not answer") || has(error, "did not allow")) {
        p.headline = "NetworkManager did not answer";
        p.fix = "The scan tries again by itself.";
    } else {
        p.headline = "The Wi-Fi scan could not run";
    }
    return p;
}

Band wifi_band(const WifiSnapshot &s, std::int64_t now_ms, const std::string &wifi_hint)
{
    Band b;
    const std::string iface = s.iface.empty() ? std::string("this board") : s.iface;
    switch (s.state) {
    case WifiSnapshot::State::Idle: b.text = "Looking for Wi-Fi networks"; break;
    case WifiSnapshot::State::Scanning: b.text = "Scanning " + iface; break;
    case WifiSnapshot::State::Ready:
        b.text = s.networks.empty() ? "No network found on " + iface : plural(static_cast<int>(s.networks.size()), "network", "networks") + " on " + iface;
        if (s.busy) b.text += ", scanning again";
        else b.text += ", " + scanned_ago(now_ms, s.list_at_ms);
        if (!s.error.empty() && !s.busy) b.tone = Tone::Warn;
        break;
    case WifiSnapshot::State::Failed:
        b.text = wifi_problem(s.error, wifi_hint).headline;
        b.tone = has(s.error, "No Wi-Fi adapter") ? Tone::Warn : Tone::Bad;
        break;
    case WifiSnapshot::State::Stopped:
        b.text = plural(static_cast<int>(s.networks.size()), "network", "networks") + " on " + iface + ", stopped";
        b.tone = Tone::Warn;
        break;
    }
    return b;
}

std::string security_meaning(WifiSecurity s)
{
    switch (s) {
    case WifiSecurity::Open: return "Open: anyone near can read your traffic.";
    case WifiSecurity::EnhancedOpen: return "Enhanced Open: encrypted without a password.";
    case WifiSecurity::Wep: return "WEP: broken, it protects almost nothing.";
    case WifiSecurity::Wpa: return "WPA: old and weak, prefer WPA2 or WPA3.";
    case WifiSecurity::Wpa2: return "WPA2: encrypted, a password is needed.";
    case WifiSecurity::Wpa3: return "WPA3: the newest and strongest.";
    case WifiSecurity::Enterprise: return "Enterprise: each user signs in with an account.";
    }
    return "";
}

std::vector<RowDef> wifi_detail_rows(const WifiNetwork &n, bool gone)
{
    std::vector<RowDef> rows;
    if (gone) rows.push_back(row("Status", "Gone: not in the last scan", Tone::Warn));
    else if (n.in_use) rows.push_back(row("Status", "Connected", Tone::Ok));
    else rows.push_back(row("Status", "In range"));
    rows.push_back(row("Network name", n.name()));
    rows.push_back(row("BSSID", n.bssid));
    std::string vendor = n.vendor;
    if (n.local_bssid) vendor = std::string(kPrivateAddressWords) + ": the vendor cannot be known";
    else if (vendor.empty()) vendor = "not known";
    rows.push_back(row("Vendor", vendor));
    rows.push_back(row("Signal", signal_text(n.dbm), signal_tone(n.dbm)));
    if (n.channel > 0) rows.push_back(row("Channel", std::to_string(n.channel)));
    if (n.freq_mhz > 0) rows.push_back(row("Frequency", std::to_string(n.freq_mhz) + " MHz"));
    const std::string band = band_words(n.band);
    if (!band.empty()) rows.push_back(row("Band", band));
    if (n.rate_mbit > 0) rows.push_back(row("Bit rate", std::to_string(n.rate_mbit) + " Mbit/s"));
    rows.push_back(row("Security", n.security_text, security_weak(n.security) ? Tone::Warn : Tone::Normal));
    return rows;
}

Band wifi_detail_band(const WifiNetwork &n, bool gone, const WifiSnapshot &s, std::int64_t now_ms, const std::string &wifi_hint)
{
    Band b;
    if (s.state == WifiSnapshot::State::Failed) return wifi_band(s, now_ms, wifi_hint);
    if (gone) {
        b.text = "Gone: not in the last scan, last values shown";
        b.tone = Tone::Warn;
        return b;
    }
    b.text = signal_text(n.dbm) + ", " + scanned_ago(now_ms, s.list_at_ms);
    return b;
}

ChannelsView channels_view(const BandChannels &b, WifiBand band)
{
    ChannelsView v;
    v.band = band;
    v.title = band_words(band);
    if (band == WifiBand::G24) {
        const int last = [&] {
            for (const ChannelLoad &c : b.channels)
                if (c.channel == 14 && c.networks > 0) return 14;
            return 13;
        }();
        for (int ch = 1; ch <= last; ++ch) {
            ChannelBar bar;
            bar.channel = ch;
            for (const ChannelLoad &c : b.channels)
                if (c.channel == ch) {
                    bar.networks = c.networks;
                    bar.weight = c.weight;
                }
            v.bars.push_back(bar);
        }
    } else {
        for (const ChannelLoad &c : b.channels) {
            ChannelBar bar;
            bar.channel = c.channel;
            bar.networks = c.networks;
            bar.weight = c.weight;
            v.bars.push_back(bar);
        }
    }
    int best_networks = 0;
    for (const ChannelBar &bar : v.bars) v.top = std::max(v.top, bar.weight);
    for (const ChannelBar &bar : v.bars) best_networks = std::max(best_networks, bar.networks);
    bool marked = false;
    for (ChannelBar &bar : v.bars) {
        bar.best = b.recommended != 0 && bar.channel == b.recommended;
        if (!marked && best_networks > 0 && bar.networks == best_networks) {
            bar.busiest = true;
            marked = true;
            v.busiest = "Busiest: channel " + std::to_string(bar.channel) + ", " + plural(bar.networks, "network", "networks");
        }
    }
    if (b.networks == 0) {
        if (band == WifiBand::G24) v.empty = "No network seen on 2.4 GHz";
        else v.empty = std::string("No ") + band_words(band) + " network seen" + (band == WifiBand::G5 ? ": this adapter may be 2.4 GHz only" : "");
    }
    return v;
}

std::string channels_sentence(const ChannelSummary &c)
{
    std::string out;
    const auto add = [&](const BandChannels &b, const char *name) {
        if (b.recommended == 0) return;
        out += (out.empty() ? "" : ", ") + std::string("channel ") + std::to_string(b.recommended) + " (" + name + ")";
    };
    add(c.g24, "2.4 GHz");
    add(c.g5, "5 GHz");
    add(c.g6, "6 GHz");
    if (out.empty()) return "Not enough networks seen yet to name a channel";
    return "Least crowded: " + out;
}

bool channels_have_149(const ChannelsView &v)
{
    if (v.band != WifiBand::G5) return false;
    for (const ChannelBar &b : v.bars)
        if (b.channel >= 149 && b.channel <= 165) return true;
    return false;
}

ChannelsLayout channels_layout(int w, int lh, int slh, int bars_h, int pad, bool first_data, bool second_data, bool portrait)
{
    ChannelsLayout L;
    w = std::max(1, w);
    const auto part_h = [&](bool data) { return data ? lh + bars_h + 2 * slh + pad : lh + 2 * slh + pad; };
    L.side = !portrait && first_data && second_data && w >= 640;
    if (L.side) {
        const int half = (w - pad) / 2;
        const int h = part_h(true);
        L.first = {0, 0, half, h};
        L.second = {half + pad, 0, w - half - pad, h};
        L.total_h = h;
    } else {
        L.first = {0, 0, w, part_h(first_data)};
        L.second = {0, L.first.h, w, part_h(second_data)};
        L.total_h = L.second.y + L.second.h;
    }
    return L;
}

/* ================================================================ Wi-Fi walk test */

WalkLayout walk_layout(int w, int h, int lh, int slh, int big_h, int pad, bool portrait)
{
    WalkLayout L;
    w = std::max(1, w);
    h = std::max(1, h);
    const int legend_h = 2 * slh;
    L.side = !portrait && w * 100 >= h * 170;
    if (L.side) {
        const int rc = std::min(250, std::max(150, w * 30 / 100));
        L.reading = {0, 0, rc, h};
        const int cx = rc + pad;
        L.chart = {cx, 0, std::max(1, w - cx), std::max(1, h - legend_h - pad)};
        L.legend = {cx, std::max(0, h - legend_h), std::max(1, w - cx), legend_h};
    } else {
        const int rh = std::max(big_h + slh, 3 * lh) + pad;
        L.reading = {0, 0, w, rh};
        const int cy = rh + pad;
        L.chart = {0, cy, w, std::max(1, h - cy - legend_h - pad)};
        L.legend = {0, std::max(0, h - legend_h), w, legend_h};
    }
    return L;
}

std::string mmss(double seconds)
{
    const long total = static_cast<long>(std::floor(std::max(0.0, seconds)));
    const long h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h > 0) return fmt("%ld:%02ld:%02ld", h, m, s);
    return fmt("%02ld:%02ld", m, s);
}

std::string weak_value(int dbm) { return fmt("%d dBm", dbm); }

int next_weak_dbm(int dbm)
{
    if (dbm == -70) return -75;
    if (dbm == -75) return -80;
    if (dbm == -80) return -70;
    return -75;
}

std::string walk_start_label(const WalkSnapshot &s) { return s.running() ? "Stop" : "Start"; }

Band walk_band(const WalkSnapshot &s, const std::string &wifi_hint)
{
    Band b;
    switch (s.state) {
    case WalkSnapshot::State::Idle: b.text = "Press Start, then walk around"; break;
    case WalkSnapshot::State::Running:
        b.text = "Walking: " + mmss(s.elapsed_s);
        if (!s.linked && !s.samples.empty()) {
            b.text += ", no signal at the moment";
            b.tone = Tone::Warn;
        } else {
            if (s.in_dip) {
                b.text += ", in a weak spot";
                b.tone = Tone::Warn;
            }
            if (s.summary.has_signal) b.text += ", weakest " + weak_value(s.summary.weakest_dbm);
        }
        break;
    case WalkSnapshot::State::Stopped:
        if (s.samples.empty() && s.summary.samples == 0) {
            b.text = "Stopped before the first reading";
            b.tone = Tone::Warn;
        } else {
            b.text = "Stopped: " + mmss(s.summary.duration_s);
            if (s.summary.has_signal) b.text += ", weakest " + weak_value(s.summary.weakest_dbm);
        }
        break;
    case WalkSnapshot::State::Failed:
        b.text = wifi_problem(s.error, wifi_hint).headline;
        b.tone = has(s.error, "No Wi-Fi adapter") ? Tone::Warn : Tone::Bad;
        break;
    }
    return b;
}

WalkLive walk_live(const WalkSnapshot &s)
{
    WalkLive l;
    l.elapsed = mmss(s.state == WalkSnapshot::State::Stopped ? s.summary.duration_s : s.elapsed_s);
    if (s.state == WalkSnapshot::State::Idle || s.state == WalkSnapshot::State::Failed) {
        l.big = "--";
        l.word = s.state == WalkSnapshot::State::Idle ? "Ready" : "No signal";
        l.tone = s.state == WalkSnapshot::State::Idle ? Tone::Normal : Tone::Bad;
        l.mark = s.state == WalkSnapshot::State::Idle ? Mark::None : Mark::Cross;
        return l;
    }
    if (!s.linked) {
        l.big = "--";
        l.word = "No signal";
        l.tone = Tone::Bad;
        l.mark = Mark::Cross;
    } else {
        l.big = std::to_string(s.dbm);
        l.unit = "dBm";
        l.word = signal_word(s.dbm);
        l.tone = signal_tone(s.dbm);
        l.mark = s.dbm >= -60 ? Mark::Check : s.dbm >= -70 ? Mark::Bang : Mark::Cross;
        if (s.dbm < s.weak_dbm) l.zone = "Below " + weak_value(s.weak_dbm) + ": the weak zone";
    }
    if (!s.ssid.empty() && !s.iface.empty()) l.where = s.ssid + " on " + s.iface;
    else if (!s.iface.empty()) l.where = s.iface;
    return l;
}

WalkWindow walk_window(const WalkSnapshot &s)
{
    WalkWindow w;
    if (s.samples.empty()) return w;
    const double first = static_cast<double>(s.samples.front().t_ms) / 1000.0;
    const double last = static_cast<double>(s.samples.back().t_ms) / 1000.0;
    if (s.running()) {
        w.t1_s = std::max(30.0, last);
        w.t0_s = std::max(0.0, w.t1_s - kWalkWindowS);
    } else {
        w.t0_s = first;
        w.t1_s = std::max(first + 30.0, last);
    }
    return w;
}

WalkYScale walk_y_scale(const std::vector<WalkSample> &samples, int weak_dbm)
{
    double hi = weak_dbm + 5.0, lo = weak_dbm - 5.0;
    for (const WalkSample &s : samples) {
        if (!s.linked) continue;
        hi = std::max(hi, static_cast<double>(s.dbm));
        lo = std::min(lo, static_cast<double>(s.dbm));
    }
    WalkYScale y;
    y.top = std::min(-20, ceil10(hi + 3.0));
    y.bottom = std::max(-100, floor10(lo - 3.0));
    while (y.top - y.bottom < 30) {
        if (y.bottom > -100) y.bottom -= 10;
        else y.top += 10;
        if (y.top > -20) {
            y.top = -20;
            y.bottom = -50;
            break;
        }
    }
    return y;
}

std::string walk_legend(const WalkSnapshot &s)
{
    std::string t = "Dashed line: " + weak_value(s.weak_dbm) + ". Squares: weak.";
    if (s.iw_missing) t += " Access point changes need iw.";
    else t += " Ticks: access point changes.";
    return t;
}

std::string walk_nolink_note(const WalkSnapshot &s)
{
    if (s.summary.nolink_samples <= 0) return "";
    return fmt("No link for %.0f s", s.summary.nolink_s);
}

std::vector<RowDef> walk_summary_rows(const WalkSnapshot &s)
{
    std::vector<RowDef> rows;
    const WalkSummary &m = s.summary;
    rows.push_back(row("Duration", mmss(m.duration_s)));
    rows.push_back(row("Readings", std::to_string(m.samples)));
    if (!m.has_signal) {
        rows.push_back(row("Signal", "no reading: the interface had no link", Tone::Warn));
        return rows;
    }
    rows.push_back(row("Weakest", weak_value(m.weakest_dbm) + " at " + mmss(m.weakest_at_s), signal_tone(m.weakest_dbm)));
    rows.push_back(row("Average", fmt("%d dBm", static_cast<int>(std::lround(m.average_dbm)))));
    rows.push_back(row("Strongest", weak_value(m.strongest_dbm) + " at " + mmss(m.strongest_at_s)));
    rows.push_back(row("Time in the weak zone", fmt("%.0f s (%.0f %%)", m.weak_s, m.weak_pct), m.weak_samples > 0 ? Tone::Warn : Tone::Ok));
    rows.push_back(row("Dips under " + weak_value(s.weak_dbm), std::to_string(m.dips), m.dips > 0 ? Tone::Warn : Tone::Normal));
    const std::string nolink = walk_nolink_note(s);
    if (!nolink.empty()) rows.push_back(row("Without link", fmt("%.0f s", m.nolink_s), Tone::Warn));
    if (!m.roaming_known && s.iw_missing) {
        rows.push_back(row("Access point changes", "not known: iw is missing"));
    } else {
        rows.push_back(row("Access point changes", m.roams == 0 ? std::string("none") : std::to_string(m.roams)));
        int shown = 0;
        for (const WalkRoam &r : s.roams) {
            if (shown++ >= 5) break;
            rows.push_back(row("At " + mmss(static_cast<double>(r.t_ms) / 1000.0) + ", moved to", r.to));
        }
    }
    return rows;
}

} // namespace ui
} // namespace nt
