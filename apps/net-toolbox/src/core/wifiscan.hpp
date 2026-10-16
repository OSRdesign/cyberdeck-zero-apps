/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: Wi-Fi networks, the core (no LVGL; tests/test_wifiscan.cpp on nmcli output recorded on a Pi 3A+ and
 * on made-up lines for the rare cases). A port of Wi-Fi Survey's scanner (apps/wifi-survey, MIT, our own code).
 *
 * How: NetworkManager's scan results, through its `nmcli` program (no root; never a shell):
 *     nmcli -t -e yes -f IN-USE,BSSID,SSID,MODE,CHAN,FREQ,RATE,SIGNAL,SECURITY device wifi list ifname <if> --rescan <m>
 * run by the shared command runner (cmdrun.hpp: its own process group, LC_ALL=C, a timeout, cancellable). Terse mode
 * (-t) separates the fields with ':' and escapes ':' and '\' inside a value with '\' (-e yes, nmcli's default, said out
 * loud); a hidden network has an empty SSID. Nothing is sent but what NetworkManager's own scan sends (the adapter's
 * probe requests, as any phone's scan); the app never connects to anything.
 *
 * The cadence, cooperative with NetworkManager's own limits: the first list of a run asks `--rescan yes` (a fresh scan,
 * which nmcli waits for), the next ones every interval (5 s by default; setting `wifi_rescan_ms`) ask `--rescan auto`
 * (nmcli rescans only when NetworkManager's list is older than about 30 s, else reads it at once: about 60 ms on the Pi
 * 3A+), a manual rescan asks `yes` again. When NetworkManager refuses a fresh scan (one ran a moment ago, the adapter is
 * busy) its last results are read with `--rescan no` and the snapshot says so (cached).
 *
 * The interface: the one of the network chooser (settings `interface=`; wifi_scan_iface() picks it, a Wi-Fi interface
 * for Automatic). An interface that is not a Wi-Fi adapter, or that is not there, ends the run as Failed with the words
 * "No Wi-Fi adapter on <iface>" (the run keeps trying every interval: an adapter plugged in again is used again).
 *
 * The signal: NetworkManager only gives a percentage. It computes it from the dBm level as
 *     pct = 100 - 100 * (|clamp(dBm, -100, -40) + 40|) / 60      (NetworkManager's nm_wifi_utils_level_to_quality)
 * so the estimate is dBm = -40 - 0.6 * (100 - pct) (100 % means -40 dBm or more, 0 % -100 dBm or less). Checked on the
 * Pi 3A+: 57 % in nmcli for the network iw read at -65 dBm (-66 by this formula; Wi-Fi Survey's pct / 2 - 100 gave -72).
 *
 * The channels summary (Wi-Fi Survey's logic): networks per channel on 2.4, 5 and 6 GHz. 2.4 GHz channels closer than 5
 * apart overlap, so each network weighs 5 on its channel and 4, 3, 2, 1 on the channels 1 to 4 away; the least crowded
 * 2.4 GHz channel (1 to 13) has the lowest weight, ties going to 1, 6, 11, then the lowest. 5 GHz: the channels seen
 * plus the usual ones (36, 40, 44, 48, 149, 153, 157, 161, 165), the least crowded is the one with the fewest networks
 * (ties: the lowest). 6 GHz: the channels seen plus the preferred scanning channels (5, 21, 37, ... 229), likewise. A 5
 * or 6 GHz band where nothing was seen has no recommendation (the adapter may not have that band: the Pi Zero 2 W is
 * 2.4 GHz only); 2.4 GHz has one as soon as the scan saw any network.
 *
 * Vendors: an injected lookup (the pages wire the OUI table of Local devices), asked once per OUI and run, never for a
 * locally administered BSSID (a virtual or random address: local_bssid, no vendor).
 */
#pragma once

#include "cmdrun.hpp"
#include "interfaces.hpp"
#include "scope.hpp"
#include "source.hpp"
#include "worker.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

/* ---------------------------------------------------------------- the loops' clock (shared with walktest.hpp)
 *
 * The real one: now() = now_ms() (monotonic), sleep(ms) a plain sleep. The tests inject a fake one that moves at once,
 * so a cadence of seconds is checked in microseconds. The loops never ask a sleep longer than kSliceMs, and look at the
 * cancel token (and the manual rescan) between two. */
struct LoopClock {
    std::function<std::int64_t()> now;
    std::function<void(int ms)> sleep;
};
LoopClock real_loop_clock();
/* Sleeps in slices until `deadline` (the clock's ms). False when cancelled first; true when the time came or `wake` is
 * set (left set: the caller reads and clears it; nullptr: none). */
bool wait_until(const LoopClock &clock, std::int64_t deadline, const CancelToken &cancel, std::atomic<bool> *wake);

/* ---------------------------------------------------------------- one network (pure parsing) */

enum class WifiBand { Unknown, G24, G5, G6 };
/* "2.4 GHz", "5 GHz", "6 GHz", "" */
const char *band_words(WifiBand b);
/* from the centre frequency: 2.4 GHz 2400..2500 MHz, 5 GHz 4900..5924 MHz, 6 GHz 5925..7125 MHz */
WifiBand band_of_freq(int mhz);
/* the channel number of a centre frequency (2484 -> 14, 5180 -> 36, 6135 -> 37 (6 GHz)), 0 when unknown */
int channel_of_freq(int mhz);

/* the class of protection, from nmcli's SECURITY words ("", "--", "WEP", "WPA1", "WPA2", "WPA3", "802.1X", "OWE", combined
 * with blanks). Enterprise: 802.1X (a login per user, any WPA version). */
enum class WifiSecurity { Open, EnhancedOpen, Wep, Wpa, Wpa2, Wpa3, Enterprise };
/* plain words: "Open", "Enhanced Open" (OWE: encrypted, no password), "WEP", "WPA", "WPA2", "WPA3", "Enterprise";
 * transition modes as "WPA/WPA2", "WPA2/WPA3" */
std::string security_words(const std::string &nm_security);
WifiSecurity security_class(const std::string &nm_security);
/* Open and WEP: anyone near can read the traffic (WEP is broken) */
bool security_weak(WifiSecurity s);

/* NetworkManager's percentage to dBm (see above): 100 -> -40, 57 -> -66, 0 -> -100 (exact for every whole dBm from
 * -100 to -40: nm_signal_to_dbm(nm_dbm_to_signal(d)) == d) */
int nm_signal_to_dbm(int pct);
/* NetworkManager's own conversion, dBm to percentage (for the tests and for comparing with iw's dBm) */
int nm_dbm_to_signal(int dbm);

struct WifiNetwork {
    std::string ssid;           /* printable UTF-8 (invalid bytes and control characters as '?'), "" when hidden */
    bool hidden = false;
    std::string bssid;          /* "b0:4e:26:00:00:01", lower case */
    bool local_bssid = false;   /* locally administered (a virtual BSSID of a multi-network access point, or random) */
    std::string vendor;         /* the lookup's answer, "" unknown (never asked for a local BSSID) */
    bool adhoc = false;         /* MODE Ad-Hoc (else infrastructure; Mesh read as infrastructure) */
    int channel = 0;
    int freq_mhz = 0;
    WifiBand band = WifiBand::Unknown;
    int rate_mbit = 0;          /* the highest bit rate it announces */
    int signal_pct = 0;         /* NetworkManager's 0..100 */
    int dbm = -100;             /* nm_signal_to_dbm(signal_pct): an estimate */
    std::string security_raw;   /* nmcli's words: "WPA2 WPA3", "WPA2 802.1X", "" */
    WifiSecurity security = WifiSecurity::Open;
    std::string security_text;  /* security_words() */
    bool in_use = false;        /* the network this interface is connected to (IN-USE '*') */

    /* the SSID, or "(hidden)" */
    std::string name() const { return hidden ? std::string("(hidden)") : ssid; }
};

/* One terse line split on the ':' that are not escaped, the '\' escapes removed ("a\:b:c" -> "a:b", "c"). */
std::vector<std::string> split_terse(const std::string &line);
/* An SSID as nmcli printed it, made printable: valid UTF-8 kept, every other byte and every control character as '?',
 * at most 32 bytes of SSID (the 802.11 limit; nmcli's own conversion may make it longer: cut at 96 bytes). */
std::string clean_ssid(const std::string &raw);
/* One line of the list (the nine fields above); false for a line that is not a network (too few fields, no valid
 * BSSID, no frequency nor channel). An SSID with a ':' that nmcli did not escape (more than nine fields) is read too:
 * the SSID is the only free text, the fields after it are taken from the end. */
bool parse_nmcli_wifi_line(const std::string &line, WifiNetwork &out);
/* The whole output: networks in the order printed, one per BSSID (a BSSID printed twice keeps the in-use one, else the
 * strongest), at most kWifiMaxNetworks. Unknown lines (an "Error: ..." line, blank lines) are skipped. */
std::vector<WifiNetwork> parse_nmcli_wifi_list(const std::string &text);
constexpr std::size_t kWifiMaxNetworks = 256;

/* strongest first; equal signals: the network in use first, then by name (hidden last), then by BSSID */
void sort_networks(std::vector<WifiNetwork> &list);

/* "aa:bb:cc:dd:ee:ff" (either case) */
bool valid_bssid(const std::string &s);
/* bit 1 of the first byte */
bool bssid_is_local(const std::string &bssid);

/* ---------------------------------------------------------------- the channels summary (pure) */

struct ChannelLoad {
    int channel = 0;
    int networks = 0;           /* on this channel */
    int weight = 0;             /* 2.4 GHz: the overlap weight (5 per network on it, 4..1 for those 1..4 away); 5 and 6
                                   GHz: 5 per network on it */
};

struct BandChannels {
    WifiBand band = WifiBand::Unknown;
    int networks = 0;           /* in this band */
    std::vector<ChannelLoad> channels; /* by channel number: 2.4 GHz 1..13 (14 when seen); 5 and 6 GHz: see above, empty
                                          when nothing was seen in the band */
    int recommended = 0;        /* the least crowded channel, 0 = none */
};

struct ChannelSummary {
    BandChannels g24, g5, g6;
};

/* Wi-Fi Survey's 2.4 GHz rule: 0 for an empty list */
int least_crowded_24(const std::vector<WifiNetwork> &list);
ChannelSummary summarize_channels(const std::vector<WifiNetwork> &list);

/* ---------------------------------------------------------------- nmcli */

constexpr int kNmcliScanTimeoutMs = 25000; /* a fresh scan takes a few seconds; nmcli waits for it */
constexpr int kWifiIntervalMs = 5000, kWifiIntervalMinMs = 2000, kWifiIntervalMaxMs = 60000;

/* {"nmcli", "-t", "-e", "yes", "-f", "IN-USE,...,SECURITY", "device", "wifi", "list", "ifname", iface, "--rescan", mode}
 * mode "yes", "auto" or "no"; without "ifname" when iface is "" */
std::vector<std::string> nmcli_wifi_argv(const std::string &iface, const std::string &rescan);
/* {"nmcli", "-t", "-f", "WIFI-HW,WIFI", "radio"}: "enabled:enabled" */
std::vector<std::string> nmcli_radio_argv();
/* false when nmcli's radio answer says disabled (hardware switch or software) */
bool nmcli_radio_enabled(const std::string &text);

/* What a failed list means, in plain words. */
enum class NmcliFailure {
    None,           /* it worked */
    Missing,        /* nmcli is not installed */
    NotRunning,     /* NetworkManager is not running */
    NoAdapter,      /* "Device 'x' not found", "is not a Wi-Fi device" */
    Refused,        /* the fresh scan was refused (busy, too soon): read the last results instead */
    TimedOut,
    Cancelled,
    Other
};
NmcliFailure classify_nmcli(const CommandResult &r);
/* "nmcli is missing: the Wi-Fi scan needs NetworkManager (package network-manager).", "No Wi-Fi adapter on wlan1", ... */
std::string nmcli_failure_words(NmcliFailure f, const CommandResult &r, const std::string &iface);

/* ---------------------------------------------------------------- the interface */

/* /sys/class/net/<if>/wireless or phy80211 */
bool iface_is_wireless(const Source &src, const std::string &iface);
/* The interface to scan with: the chosen one when a network is chosen (even when it is missing: the scan then says
 * "No Wi-Fi adapter on wlan1"); Automatic: the main interface when it is Wi-Fi, else the first Wi-Fi interface; "" when
 * the board has none. */
std::string wifi_scan_iface(const NetScope &scope, const NetState &st);

/* ---------------------------------------------------------------- the scan loop */

/* the injected vendor lookup: "TP-Link" for "b0:4e:26:00:00:01", "" unknown. Called on the scan's worker thread. */
using VendorLookup = std::function<std::string(const std::string &mac)>;

struct WifiScanConfig {
    std::string iface;          /* wifi_scan_iface(); "" = no Wi-Fi adapter on the board */
    int interval_ms = kWifiIntervalMs; /* kWifiIntervalMinMs .. kWifiIntervalMaxMs, from the start of one list to the next */
    int max_lists = 0;          /* tests: stop after that many lists (0 = until stopped) */
    VendorLookup vendor;        /* nullptr: no vendors */
    std::vector<std::string> program = {"nmcli"}; /* tests: a fake */
};

struct WifiSnapshot {
    enum class State {
        Idle,
        Scanning,   /* the first list is coming */
        Ready,      /* networks hold a list (busy: the next one is coming) */
        Failed,     /* error says why; no list (the run tries again every interval) */
        Stopped     /* stop(): the last list stays */
    };
    State state = State::Idle;
    std::string iface;
    std::string error;          /* Failed: why; Ready: why the last list could not be refreshed ("" when it was) */
    bool busy = false;          /* nmcli runs now */
    bool cached = false;        /* NetworkManager refused a fresh scan: its last results */
    std::string rescan;         /* the mode of the last list: "yes", "auto", "no" */
    std::vector<WifiNetwork> networks; /* sorted (sort_networks) */
    ChannelSummary channels;
    std::uint64_t lists = 0;    /* lists read in this run: a follower refreshes when it moves */
    std::int64_t list_at_ms = 0;/* the loop clock when the last list came */
    int interval_ms = kWifiIntervalMs;
    bool running() const { return state == State::Scanning || state == State::Ready || state == State::Failed; }
};

/* The whole run on the calling thread (the session's worker; tests call it with a fake clock). `wake` (may be nullptr):
 * set it for a manual rescan (a fresh scan at once). publish() is called after every change of the snapshot. Returns
 * the last snapshot (Stopped when cancelled or after max_lists). */
WifiSnapshot run_wifi_scan(const WifiScanConfig &cfg, const Source &src, CommandRunner &cmd, const LoopClock &clock,
                           const CancelToken &cancel, std::atomic<bool> *wake,
                           const std::function<void(const WifiSnapshot &)> &publish);

class WifiScanSession {
public:
    WifiScanSession(); /* the system's /sys, DirectRunner, the real clock */
    WifiScanSession(std::shared_ptr<const Source> src, std::shared_ptr<CommandRunner> cmd, LoopClock clock);
    bool start(const WifiScanConfig &cfg); /* LVGL thread; a running scan is stopped first */
    void stop();                           /* the list stays (state Stopped), nmcli is ended */
    void rescan();                         /* a fresh scan now (a running session) */
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const WifiSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    std::shared_ptr<const Source> src_;
    std::shared_ptr<CommandRunner> cmd_;
    LoopClock clock_;
    std::shared_ptr<std::atomic<bool>> wake_;
    Channel<WifiSnapshot> channel_;
    Worker worker_;
};

/* ---------------------------------------------------------------- the detail page's network (pure)
 *
 * Follows one network by its BSSID across the lists (the list reorders by signal): each new list refreshes its values;
 * a list without it marks it gone and keeps its last values; it comes back when a later list has it. A failed scan (no
 * new list) changes nothing. */
class WifiFollower {
public:
    void follow(const WifiNetwork &n);
    void clear();
    bool active() const { return active_; }
    /* call with every snapshot (cheap when snap.lists did not move) */
    void update(const WifiSnapshot &snap);
    const WifiNetwork &network() const { return net_; }
    bool gone() const { return gone_; }
    int gone_lists() const { return gone_lists_; } /* lists in a row without it */
    /* its index in snap.networks, -1 when not there */
    static int index_in(const WifiSnapshot &snap, const std::string &bssid);

private:
    WifiNetwork net_;
    bool active_ = false;
    bool gone_ = false;
    int gone_lists_ = 0;
    bool seen_any_ = false;      /* the list last looked at: its number and time */
    std::uint64_t seen_lists_ = 0;
    std::int64_t seen_at_ = 0;
};

} // namespace nt
