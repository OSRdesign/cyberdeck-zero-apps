/*
 * SPDX-License-Identifier: MIT
 *
 * Wi-Fi survey backend. Passive only: it asks NetworkManager for its scan results
 * (`nmcli -t device wifi list`), which needs no root. No injection, no capture, no connecting.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wifisurvey {

struct AccessPoint {
    std::string ssid;           // empty: hidden network
    std::string bssid;          // aa:bb:cc:dd:ee:ff
    std::string vendor;         // from the OUI table, may be empty
    std::string security;       // as reported by NetworkManager ("WPA2", "WPA2 WPA3", "--" ...)
    int channel = 0;
    int freq_mhz = 0;
    int rate_mbps = 0;
    int signal_pct = 0;         // 0..100, NetworkManager's own scale
    bool connected = false;     // the network the deck is connected to
    bool infra = true;          // false: ad-hoc

    /* NetworkManager only reports a percentage; this is the usual inverse of its dBm -> % mapping. */
    int dbm() const { return signal_pct / 2 - 100; }
    /* 0 = 2.4 GHz, 1 = 5 GHz, 2 = 6 GHz */
    int band() const { return freq_mhz >= 5925 ? 2 : (freq_mhz >= 3000 ? 1 : 0); }
    bool hidden() const { return ssid.empty(); }
    /* "Open", "WPA2", "WPA3", "WPA2/3", "WPA", "WEP", "OWE" (+ "-EAP" for 802.1X) */
    std::string security_short() const;
};

enum class State {
    Idle,        // nothing scanned yet
    Ok,
    NoNmcli,     // nmcli is not installed
    NoNM,        // NetworkManager is not running / not reachable
    NoAdapter,   // no Wi-Fi device
    WifiOff,     // Wi-Fi radio is disabled
    Error,       // anything else (note has the detail)
};

struct Snapshot {
    State state = State::Idle;
    std::string note;                // short human readable detail (also set for Ok when results are cached)
    std::string iface;
    std::vector<AccessPoint> aps;    // as reported, unsorted
    uint64_t generation = 0;         // increases with every finished scan
    bool scanning = false;
    bool cached = false;             // the radio was busy: these are NetworkManager's previous results
};

class Scanner {
public:
    struct Impl;

    Scanner();
    ~Scanner();

    /* Starts one scan in the background; does nothing while one is running. */
    void request();
    Snapshot snapshot() const;

private:
    std::shared_ptr<Impl> s_;   // shared with the (detached) worker thread
};

/* ---- pure helpers (exposed for the host test) ---- */

/* Splits one line of `nmcli -t` output on unescaped ':' and removes the backslash escapes. */
std::vector<std::string> split_terse(const std::string &line);
/* Parses the output of `nmcli -t -f IN-USE,BSSID,SSID,MODE,CHAN,FREQ,RATE,SIGNAL,SECURITY device wifi list`. */
std::vector<AccessPoint> parse_wifi_list(const std::string &text);
/* Vendor of a MAC (OUI table; "Locally administered" when bit 1 of the first byte is set). */
std::string vendor_for(const std::string &mac);

} // namespace wifisurvey
