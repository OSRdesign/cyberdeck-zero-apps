/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: Wi-Fi networks (see wifiscan.hpp).
 */
#include "wifiscan.hpp"

#include "sock.hpp"
#include "text.hpp"

#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

namespace nt {

/* ---------------------------------------------------------------- the loops' clock */

LoopClock real_loop_clock()
{
    LoopClock c;
    c.now = [] { return now_ms(); };
    c.sleep = [](int ms) {
        if (ms <= 0) return;
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
        while (nanosleep(&ts, &ts) != 0) {
        }
    };
    return c;
}

bool wait_until(const LoopClock &clock, std::int64_t deadline, const CancelToken &cancel, std::atomic<bool> *wake)
{
    for (;;) {
        if (cancel.cancelled()) return false;
        if (wake && wake->load()) return true;
        const std::int64_t now = clock.now();
        if (now >= deadline) return true;
        clock.sleep(static_cast<int>(std::min<std::int64_t>(kSliceMs, deadline - now)));
    }
}

/* ---------------------------------------------------------------- one network */

namespace {

/* the leading whole number of "2452 MHz", "270 Mbit/s", " 57" */
bool lead_int(const std::string &s, int &out)
{
    const std::string t = trim(s);
    std::size_t i = 0;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
    if (i == 0 || i > 6) return false;
    return parse_int(t.substr(0, i), out);
}

bool has_token(const std::vector<std::string> &tokens, const char *word)
{
    return std::find(tokens.begin(), tokens.end(), std::string(word)) != tokens.end();
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

const char *band_words(WifiBand b)
{
    switch (b) {
    case WifiBand::G24: return "2.4 GHz";
    case WifiBand::G5: return "5 GHz";
    case WifiBand::G6: return "6 GHz";
    default: return "";
    }
}

WifiBand band_of_freq(int mhz)
{
    if (mhz >= 2400 && mhz <= 2500) return WifiBand::G24;
    if (mhz >= 4900 && mhz < 5925) return WifiBand::G5;
    if (mhz >= 5925 && mhz <= 7125) return WifiBand::G6;
    return WifiBand::Unknown;
}

int channel_of_freq(int mhz)
{
    if (mhz == 2484) return 14;
    if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0) return (mhz - 2407) / 5;
    if (mhz >= 4910 && mhz <= 4980 && mhz % 5 == 0) return (mhz - 4000) / 5; /* 4.9 GHz (Japan) */
    if (mhz >= 5000 && mhz < 5925 && mhz % 5 == 0) return (mhz - 5000) / 5;
    if (mhz == 5935) return 2;                                              /* 6 GHz channel 2 */
    if (mhz >= 5955 && mhz <= 7115 && (mhz - 5950) % 5 == 0) return (mhz - 5950) / 5;
    return 0;
}

WifiSecurity security_class(const std::string &nm)
{
    const std::vector<std::string> t = split_ws(nm);
    if (t.empty() || (t.size() == 1 && t[0] == "--")) return WifiSecurity::Open;
    if (has_token(t, "802.1X")) return WifiSecurity::Enterprise;
    const bool wpa3 = has_token(t, "WPA3"), wpa2 = has_token(t, "WPA2"), wpa1 = has_token(t, "WPA1");
    /* a transition mode is as strong as its weakest part: a client may use it */
    if (wpa1) return WifiSecurity::Wpa;
    if (wpa2) return WifiSecurity::Wpa2;
    if (wpa3) return WifiSecurity::Wpa3;
    if (has_token(t, "WEP")) return WifiSecurity::Wep;
    if (has_token(t, "OWE")) return WifiSecurity::EnhancedOpen;
    return WifiSecurity::Wpa2; /* a word this version does not know: protected, at least */
}

std::string security_words(const std::string &nm)
{
    const std::vector<std::string> t = split_ws(nm);
    switch (security_class(nm)) {
    case WifiSecurity::Open: return "Open";
    case WifiSecurity::EnhancedOpen: return "Enhanced Open";
    case WifiSecurity::Wep: return "WEP";
    case WifiSecurity::Enterprise: return "Enterprise";
    case WifiSecurity::Wpa:
        return std::string("WPA") + (has_token(t, "WPA2") ? "/WPA2" : "") + (has_token(t, "WPA3") ? "/WPA3" : "");
    case WifiSecurity::Wpa2:
        if (has_token(t, "WPA3")) return "WPA2/WPA3";
        if (has_token(t, "WPA2")) return "WPA2";
        return printable(trim(nm)); /* unknown words, as nmcli said them */
    case WifiSecurity::Wpa3: return "WPA3";
    }
    return "Open";
}

bool security_weak(WifiSecurity s)
{
    return s == WifiSecurity::Open || s == WifiSecurity::Wep;
}

int nm_signal_to_dbm(int pct)
{
    pct = std::min(100, std::max(0, pct));
    return -40 - static_cast<int>(std::lround(0.6 * (100 - pct)));
}

int nm_dbm_to_signal(int dbm)
{
    const int d = std::abs(std::min(-40, std::max(-100, dbm)) + 40);
    return std::min(100, std::max(0, 100 - static_cast<int>((100.0 * d) / 60.0)));
}

bool valid_bssid(const std::string &s)
{
    if (s.size() != 17) return false;
    for (std::size_t i = 0; i < 17; ++i) {
        if (i % 3 == 2) {
            if (s[i] != ':') return false;
        } else if (hex_value(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

bool bssid_is_local(const std::string &bssid)
{
    if (bssid.size() < 2) return false;
    const int lo = hex_value(bssid[1]);
    return lo >= 0 && (lo & 0x2);
}

std::vector<std::string> split_terse(const std::string &line)
{
    std::vector<std::string> fields;
    std::string cur;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            cur += line[++i];
        } else if (c == ':') {
            fields.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    fields.push_back(cur);
    return fields;
}

std::string clean_ssid(const std::string &raw)
{
    std::string out;
    const std::size_t n = raw.size();
    std::size_t i = 0;
    while (i < n && out.size() < 96) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c < 0x80) {
            out += (c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c);
            ++i;
            continue;
        }
        int len = 0;
        std::uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        }
        bool ok = len > 0 && i + static_cast<std::size_t>(len) <= n;
        for (int k = 1; ok && k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(raw[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok) {
            /* overlong forms, surrogates, beyond U+10FFFF, C1 controls */
            if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
                (cp >= 0xD800 && cp <= 0xDFFF))
                ok = false;
        }
        if (!ok) {
            out += '?';
            ++i;
            continue;
        }
        if (cp >= 0x80 && cp <= 0x9F) out += '?';
        else if (out.size() + static_cast<std::size_t>(len) <= 96) out.append(raw, i, static_cast<std::size_t>(len));
        else break;
        i += static_cast<std::size_t>(len);
    }
    return out;
}

bool parse_nmcli_wifi_line(const std::string &line, WifiNetwork &out)
{
    out = WifiNetwork();
    const std::vector<std::string> f = split_terse(line);
    const std::size_t n = f.size();
    if (n < 9) return false;
    const std::string bssid = lower(trim(f[1]));
    if (!valid_bssid(bssid)) return false;
    std::string ssid = f[2];
    for (std::size_t i = 3; i + 6 < n; ++i) ssid += ":" + f[i]; /* a ':' nmcli did not escape */
    int chan = 0, freq = 0, rate = 0, signal = 0;
    lead_int(f[n - 5], chan);
    lead_int(f[n - 4], freq);
    lead_int(f[n - 3], rate);
    if (!lead_int(f[n - 2], signal)) return false;
    if (freq <= 0 && chan <= 0) return false;

    out.in_use = trim(f[0]) == "*";
    out.bssid = bssid;
    out.local_bssid = bssid_is_local(bssid);
    out.hidden = ssid.empty() || ssid == "--";
    out.ssid = out.hidden ? std::string() : clean_ssid(ssid);
    out.adhoc = trim(f[n - 6]) == "Ad-Hoc";
    out.freq_mhz = freq;
    out.band = band_of_freq(freq);
    out.channel = chan > 0 ? chan : channel_of_freq(freq);
    if (out.band == WifiBand::Unknown && freq <= 0 && out.channel >= 1 && out.channel <= 14) out.band = WifiBand::G24;
    out.rate_mbit = rate;
    out.signal_pct = std::min(100, std::max(0, signal));
    out.dbm = nm_signal_to_dbm(out.signal_pct);
    std::string sec = trim(f[n - 1]);
    if (sec == "--") sec.clear();
    if (sec.size() > 64) sec.resize(64);
    out.security_raw = printable(sec);
    out.security = security_class(sec);
    out.security_text = security_words(sec);
    return true;
}

std::vector<WifiNetwork> parse_nmcli_wifi_list(const std::string &text)
{
    std::vector<WifiNetwork> out;
    std::map<std::string, std::size_t> at;
    for (const std::string &line : split_lines(text)) {
        WifiNetwork w;
        if (!parse_nmcli_wifi_line(line, w)) continue;
        const auto it = at.find(w.bssid);
        if (it != at.end()) {
            WifiNetwork &old = out[it->second];
            if ((w.in_use && !old.in_use) || (w.in_use == old.in_use && w.signal_pct > old.signal_pct)) old = w;
            continue;
        }
        if (out.size() >= kWifiMaxNetworks) continue;
        at[w.bssid] = out.size();
        out.push_back(w);
    }
    return out;
}

void sort_networks(std::vector<WifiNetwork> &list)
{
    std::stable_sort(list.begin(), list.end(), [](const WifiNetwork &a, const WifiNetwork &b) {
        if (a.signal_pct != b.signal_pct) return a.signal_pct > b.signal_pct;
        if (a.in_use != b.in_use) return a.in_use;
        if (a.hidden != b.hidden) return !a.hidden;
        if (a.ssid != b.ssid) return a.ssid < b.ssid;
        return a.bssid < b.bssid;
    });
}

/* ---------------------------------------------------------------- the channels summary */

namespace {

int overlap_weight_24(const std::vector<WifiNetwork> &list, int ch)
{
    int score = 0;
    for (const WifiNetwork &w : list)
        if (w.band == WifiBand::G24 && w.channel >= 1 && w.channel <= 14) score += std::max(0, 5 - std::abs(w.channel - ch));
    return score;
}

/* 5 and 6 GHz: the channels seen plus `usual`, the fewest networks first (ties: the lowest channel) */
BandChannels wide_band(const std::vector<WifiNetwork> &list, WifiBand band, std::initializer_list<int> usual)
{
    BandChannels b;
    b.band = band;
    std::map<int, int> count;
    for (const WifiNetwork &w : list)
        if (w.band == band && w.channel > 0) {
            ++count[w.channel];
            ++b.networks;
        }
    if (b.networks == 0) return b;
    for (const int ch : usual) count.emplace(ch, 0);
    int best = 0, best_n = 0;
    for (const auto &kv : count) {
        ChannelLoad c;
        c.channel = kv.first;
        c.networks = kv.second;
        c.weight = 5 * kv.second;
        b.channels.push_back(c);
        if (best == 0 || kv.second < best_n) {
            best = kv.first;
            best_n = kv.second;
        }
    }
    b.recommended = best;
    return b;
}

} // namespace

int least_crowded_24(const std::vector<WifiNetwork> &list)
{
    if (list.empty()) return 0;
    int best = 0, best_score = 0;
    for (int ch = 1; ch <= 13; ++ch) {
        const int score = overlap_weight_24(list, ch);
        const bool preferred = ch == 1 || ch == 6 || ch == 11;
        const bool best_preferred = best == 1 || best == 6 || best == 11;
        if (best == 0 || score < best_score || (score == best_score && preferred && !best_preferred)) {
            best = ch;
            best_score = score;
        }
    }
    return best;
}

ChannelSummary summarize_channels(const std::vector<WifiNetwork> &list)
{
    ChannelSummary s;
    s.g24.band = WifiBand::G24;
    int count24[15] = {};
    for (const WifiNetwork &w : list)
        if (w.band == WifiBand::G24) {
            ++s.g24.networks;
            if (w.channel >= 1 && w.channel <= 14) ++count24[w.channel];
        }
    const int last = count24[14] > 0 ? 14 : 13;
    for (int ch = 1; ch <= last; ++ch) {
        ChannelLoad c;
        c.channel = ch;
        c.networks = count24[ch];
        c.weight = overlap_weight_24(list, ch);
        s.g24.channels.push_back(c);
    }
    s.g24.recommended = least_crowded_24(list);
    s.g5 = wide_band(list, WifiBand::G5, {36, 40, 44, 48, 149, 153, 157, 161, 165});
    s.g6 = wide_band(list, WifiBand::G6, {5, 21, 37, 53, 69, 85, 101, 117, 133, 149, 165, 181, 197, 213, 229});
    return s;
}

/* ---------------------------------------------------------------- nmcli */

std::vector<std::string> nmcli_wifi_argv(const std::string &iface, const std::string &rescan)
{
    std::vector<std::string> a = {"nmcli", "-t", "-e", "yes", "-f", "IN-USE,BSSID,SSID,MODE,CHAN,FREQ,RATE,SIGNAL,SECURITY",
                                  "device", "wifi", "list"};
    if (!iface.empty()) {
        a.push_back("ifname");
        a.push_back(iface);
    }
    a.push_back("--rescan");
    a.push_back(rescan == "yes" || rescan == "no" ? rescan : std::string("auto"));
    return a;
}

std::vector<std::string> nmcli_radio_argv()
{
    return {"nmcli", "-t", "-f", "WIFI-HW,WIFI", "radio"};
}

bool nmcli_radio_enabled(const std::string &text)
{
    return text.find("disabled") == std::string::npos;
}

NmcliFailure classify_nmcli(const CommandResult &r)
{
    if (r.cancelled) return NmcliFailure::Cancelled;
    if (!r.started || r.exit_code == 127) return NmcliFailure::Missing;
    if (r.timed_out) return NmcliFailure::TimedOut;
    const std::string &o = r.output;
    if (o.find("NetworkManager is not running") != std::string::npos || r.exit_code == 8) return NmcliFailure::NotRunning;
    if (o.find("not a Wi-Fi device") != std::string::npos ||
        (o.find("Error: Device") != std::string::npos && o.find("not found") != std::string::npos))
        return NmcliFailure::NoAdapter;
    if (r.term_signal == 0 && r.exit_code == 0) return NmcliFailure::None;
    if (o.find("Scanning not allowed") != std::string::npos || o.find("scan") != std::string::npos)
        return NmcliFailure::Refused;
    return NmcliFailure::Other;
}

std::string nmcli_failure_words(NmcliFailure f, const CommandResult &r, const std::string &iface)
{
    switch (f) {
    case NmcliFailure::None: return "";
    case NmcliFailure::Missing: return "nmcli is missing: the Wi-Fi scan needs NetworkManager (package network-manager).";
    case NmcliFailure::NotRunning: return "NetworkManager is not running: the Wi-Fi scan needs it.";
    case NmcliFailure::NoAdapter:
        return iface.empty() ? std::string("No Wi-Fi adapter on this board") : "No Wi-Fi adapter on " + printable(iface);
    case NmcliFailure::Refused: return "NetworkManager did not allow a new scan now.";
    case NmcliFailure::TimedOut: return "NetworkManager did not answer in time.";
    case NmcliFailure::Cancelled: return "";
    case NmcliFailure::Other: break;
    }
    std::string first;
    for (const std::string &l : split_lines(r.output)) {
        first = trim(l);
        if (!first.empty()) break;
    }
    if (first.size() > 120) first.resize(120);
    std::string words = "nmcli failed";
    if (r.term_signal) words += " (signal " + std::to_string(r.term_signal) + ")";
    else words += " (exit " + std::to_string(r.exit_code) + ")";
    if (!first.empty()) words += ": " + clean_ssid(first);
    return words;
}

/* ---------------------------------------------------------------- the interface */

bool iface_is_wireless(const Source &src, const std::string &iface)
{
    if (!valid_iface_name(iface)) return false;
    const std::string base = "/sys/class/net/" + iface + "/";
    return src.exists(base + "wireless") || src.exists(base + "phy80211");
}

std::string wifi_scan_iface(const NetScope &scope, const NetState &st)
{
    if (!scope.chosen.empty()) return scope.chosen;
    const Iface *m = st.main();
    if (m && m->wireless && !m->loopback) return m->name;
    for (const Iface &i : st.ifaces)
        if (i.wireless && !i.loopback) return i.name;
    return std::string();
}

/* ---------------------------------------------------------------- the scan loop */

WifiSnapshot run_wifi_scan(const WifiScanConfig &input, const Source &src, CommandRunner &cmd, const LoopClock &clock,
                           const CancelToken &cancel, std::atomic<bool> *wake,
                           const std::function<void(const WifiSnapshot &)> &publish)
{
    WifiScanConfig cfg = input;
    cfg.interval_ms = std::min(kWifiIntervalMaxMs, std::max(kWifiIntervalMinMs, cfg.interval_ms));
    if (cfg.program.empty() || cfg.program[0].empty()) cfg.program = {"nmcli"};
    WifiSnapshot snap;
    snap.state = WifiSnapshot::State::Scanning;
    snap.iface = cfg.iface;
    snap.interval_ms = cfg.interval_ms;
    const auto emit = [&] {
        if (publish) publish(snap);
    };
    const auto argv = [&](std::vector<std::string> a) {
        a.erase(a.begin());
        a.insert(a.begin(), cfg.program.begin(), cfg.program.end());
        return a;
    };
    const auto run = [&](std::vector<std::string> a, int timeout_ms) {
        CommandSpec spec;
        spec.argv = argv(std::move(a));
        spec.timeout_ms = timeout_ms;
        spec.max_output = 128 * 1024;
        return cmd.run(spec, cancel);
    };
    const auto fail_hard = [&](const std::string &why) {
        snap.state = WifiSnapshot::State::Failed;
        snap.error = why;
        snap.cached = false;
        snap.networks.clear();
        snap.channels = ChannelSummary();
    };
    const auto fail_soft = [&](const std::string &why) {
        snap.error = why;
        if (snap.state != WifiSnapshot::State::Ready) snap.state = WifiSnapshot::State::Failed;
    };
    emit();

    std::map<std::string, std::string> vendors; /* OUI -> the lookup's answer */
    bool fresh_done = false;                    /* a list of this run came: later ones ask "auto" */
    int rounds = 0;
    bool stopped = false;
    while (!stopped) {
        if (cancel.cancelled()) break;
        const std::int64_t started = clock.now();
        const bool manual = wake && wake->exchange(false);
        if (!iface_is_wireless(src, cfg.iface)) {
            fail_hard(nmcli_failure_words(NmcliFailure::NoAdapter, CommandResult(), cfg.iface));
            emit();
        } else {
            std::string mode = (!fresh_done || manual) ? "yes" : "auto";
            snap.busy = true;
            emit();
            CommandResult r = run(nmcli_wifi_argv(cfg.iface, mode), kNmcliScanTimeoutMs);
            if (r.cancelled || cancel.cancelled()) break;
            NmcliFailure f = classify_nmcli(r);
            bool cached = false;
            if (mode == "yes" && (f == NmcliFailure::Refused || f == NmcliFailure::Other)) {
                /* busy, or a scan a moment ago: NetworkManager's last results */
                const CommandResult r2 = run(nmcli_wifi_argv(cfg.iface, "no"), 8000);
                if (r2.cancelled || cancel.cancelled()) break;
                if (classify_nmcli(r2) == NmcliFailure::None) {
                    r = r2;
                    f = NmcliFailure::None;
                    cached = true;
                    mode = "no";
                }
            }
            if (f == NmcliFailure::None) {
                std::vector<WifiNetwork> list = parse_nmcli_wifi_list(r.output);
                bool radio_off = false;
                if (list.empty()) {
                    const CommandResult rr = run(nmcli_radio_argv(), 5000);
                    if (rr.cancelled || cancel.cancelled()) break;
                    radio_off = rr.ok() && !nmcli_radio_enabled(rr.output);
                }
                if (radio_off) {
                    fail_hard("Wi-Fi is switched off.");
                } else {
                    for (WifiNetwork &w : list) {
                        if (w.local_bssid || !cfg.vendor) continue;
                        const std::string oui = w.bssid.substr(0, 8);
                        auto it = vendors.find(oui);
                        if (it == vendors.end()) it = vendors.emplace(oui, printable(cfg.vendor(w.bssid))).first;
                        w.vendor = it->second;
                    }
                    sort_networks(list);
                    snap.channels = summarize_channels(list);
                    snap.networks = std::move(list);
                    snap.state = WifiSnapshot::State::Ready;
                    snap.error.clear();
                    snap.cached = cached;
                    snap.rescan = mode;
                    ++snap.lists;
                    snap.list_at_ms = clock.now();
                    fresh_done = true;
                }
            } else if (f == NmcliFailure::Missing || f == NmcliFailure::NotRunning || f == NmcliFailure::NoAdapter) {
                fail_hard(nmcli_failure_words(f, r, cfg.iface));
            } else {
                fail_soft(nmcli_failure_words(f, r, cfg.iface));
            }
            snap.busy = false;
            emit();
        }
        ++rounds;
        if (cfg.max_lists > 0 && rounds >= cfg.max_lists) break;
        const std::int64_t next = std::max(started + cfg.interval_ms, clock.now() + 1000);
        if (!wait_until(clock, next, cancel, wake)) stopped = true;
    }
    snap.busy = false;
    snap.state = WifiSnapshot::State::Stopped;
    emit();
    return snap;
}

WifiScanSession::WifiScanSession()
    : src_(make_system_source()), cmd_(std::make_shared<DirectRunner>()), clock_(real_loop_clock())
{
}

WifiScanSession::WifiScanSession(std::shared_ptr<const Source> src, std::shared_ptr<CommandRunner> cmd, LoopClock clock)
    : src_(std::move(src)), cmd_(std::move(cmd)), clock_(std::move(clock))
{
}

bool WifiScanSession::start(const WifiScanConfig &cfg)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    WifiSnapshot first;
    first.state = WifiSnapshot::State::Scanning;
    first.iface = cfg.iface;
    first.interval_ms = std::min(kWifiIntervalMaxMs, std::max(kWifiIntervalMinMs, cfg.interval_ms));
    channel_.begin(gen, first);
    wake_ = std::make_shared<std::atomic<bool>>(false);
    const std::shared_ptr<std::atomic<bool>> wake = wake_;
    const std::shared_ptr<const Source> src = src_;
    const std::shared_ptr<CommandRunner> cmd = cmd_;
    const LoopClock clock = clock_;
    const std::uint64_t started = worker_.start([this, gen, cfg, src, cmd, clock, wake](const CancelToken &cancel) {
        run_wifi_scan(cfg, *src, *cmd, clock, cancel, wake.get(),
                      [this, gen](const WifiSnapshot &s) { channel_.publish(gen, s); });
    });
    if (!started) {
        first.state = WifiSnapshot::State::Failed;
        first.error = "Could not start the scan (no thread).";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void WifiScanSession::stop()
{
    worker_.cancel();
}

void WifiScanSession::rescan()
{
    if (wake_) wake_->store(true);
}

/* ---------------------------------------------------------------- the detail page's network */

void WifiFollower::follow(const WifiNetwork &n)
{
    net_ = n;
    active_ = true;
    gone_ = false;
    gone_lists_ = 0;
    seen_any_ = false;
    seen_lists_ = 0;
    seen_at_ = 0;
}

void WifiFollower::clear()
{
    *this = WifiFollower();
}

int WifiFollower::index_in(const WifiSnapshot &snap, const std::string &bssid)
{
    for (std::size_t i = 0; i < snap.networks.size(); ++i)
        if (snap.networks[i].bssid == bssid) return static_cast<int>(i);
    return -1;
}

void WifiFollower::update(const WifiSnapshot &snap)
{
    if (!active_ || snap.lists == 0) return;
    /* a new list: its number or its time moved (a new run counts from 1 again) */
    if (seen_any_ && snap.lists == seen_lists_ && snap.list_at_ms == seen_at_) return;
    seen_any_ = true;
    seen_lists_ = snap.lists;
    seen_at_ = snap.list_at_ms;
    const int i = index_in(snap, net_.bssid);
    if (i >= 0) {
        net_ = snap.networks[static_cast<std::size_t>(i)];
        gone_ = false;
        gone_lists_ = 0;
    } else {
        gone_ = true;
        ++gone_lists_;
    }
}

} // namespace nt
