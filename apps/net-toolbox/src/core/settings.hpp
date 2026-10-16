/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the settings file (no LVGL; tests/test_core.cpp).
 *
 * ${XDG_CONFIG_HOME:-$HOME/.config}/cyberdeckzero/net-toolbox.conf (NET_TOOLBOX_CONFIG overrides the path), one
 * key=value per line:
 *   v=1
 *   target=1.1.1.1       a saved ping target (repeated, in order, at most kMaxTargets)
 *   ping_target=...      the last values used, per tool
 *   ping_interval=1000   ms (200 .. 10000)
 *   ping_timeout=2000    ms (100 .. 10000)
 *   ping_count=0         0 = until stopped (0 .. 10000)
 *   dns_name=example.com
 *   dns_type=A           A, AAAA, MX, TXT, NS, PTR, CNAME
 *   dns_resolver=        "" = the system's; else an address
 *   port_host=...
 *   port=443             1 .. 65535
 *   port_preset=Web      the label of a preset, "" for one port
 *   trace_target=...     0.2: the last traceroute target
 *   http_url=...         0.2: the last HTTP check address (as typed; it must pass parse_http_target)
 *   http_insecure=0      0.2: 1 = ignore certificate errors (curl -k); off unless the user turned it on
 *   whois_query=...      0.2: the last whois query (a domain or an address)
 *   wol=name|mac|host|broadcast|port   0.3: a saved Wake-on-LAN machine (repeated, in order, at most kMaxWolMachines;
 *                        host and broadcast may be empty; see wol.hpp)
 *   wol_iface=           0.3: the interface to send on, "" = every interface that is up with an IPv4 address
 *   wol_watch=1          0.3: ping the machine after sending (when it has a host)
 *   speed_ping=1.1.1.1   0.3: the latency-under-load target of the speed test: a host, "gateway" (the UI puts the
 *                        default gateway in) or "off"
 *   ble_seconds=60       0.3: the length of one BLE scan (5 .. 60)
 *   interface=wlan1      0.3: the chosen network (scope.hpp); absent or empty = Automatic (the system's route). The name is
 *                        kept while that adapter is unplugged (the tools then use Automatic and say so). "auto" and
 *                        "automatic" read as Automatic; an invalid name is ignored.
 *   wifi_rescan_ms=5000  0.4: the Wi-Fi networks list's interval (2000 .. 60000 ms)
 *   walk_weak_dbm=-75    0.4: the walk test's weak zone: below this level (-95 .. -40 dBm)
 * A 0.1 file (without the 0.2 keys) reads with their defaults; 0.1 ignores the 0.2 keys. Likewise for the 0.3 keys: a
 * 0.2 file reads with their defaults (no machine saved), 0.2 ignores them (unknown keys).
 * Parsed, never executed; unknown keys and bad values (a host that is not a valid name or address) are ignored;
 * written through a temporary file and a rename.
 */
#pragma once

#include "wol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nt {

struct Settings {
    static constexpr std::size_t kMaxTargets = 8;
    static constexpr int kBleSecondsMin = 5, kBleSecondsMax = 60;

    std::vector<std::string> targets;
    std::string ping_target;
    int ping_interval_ms = 1000;
    int ping_timeout_ms = 2000;
    int ping_count = 0;
    std::string dns_name;
    std::string dns_type = "A";
    std::string dns_resolver;
    std::string port_host;
    int port = 443;
    std::string port_preset;
    std::string trace_target;
    std::string http_url;
    bool http_insecure = false;
    std::string whois_query;
    /* 0.3 */
    std::vector<WolMachine> wol_machines; /* in their stored form (normalize_machine) */
    std::string wol_iface;
    bool wol_watch = true;
    std::string speed_ping = "1.1.1.1";
    int ble_seconds = 60;
    std::string interface;      /* "" = Automatic; else an interface name (valid_iface_name) */
    int wifi_rescan_ms = 5000;  /* 0.4: wifi_rescan_ms=, the Wi-Fi list's interval (2000 .. 60000, wifiscan.hpp) */
    int walk_weak_dbm = -75;    /* 0.4: walk_weak_dbm=, the walk test's weak zone threshold (-95 .. -40, walktest.hpp) */

    /* a saved target: added at the end (moved there when already saved; the oldest goes past kMaxTargets); false
     * for an invalid host */
    bool add_target(const std::string &host);
    bool remove_target(const std::string &host);
    /* 0.3: a machine is saved under its name (the same name, in any case, replaces it in place); a new one when 20
     * are saved, or an invalid one, is refused with words in *why */
    bool save_machine(const WolMachine &m, std::string *why = nullptr);
    bool remove_machine(const std::string &name);
    const WolMachine *find_machine(const std::string &name) const;

    std::string serialize() const;
    static Settings parse(const std::string &text);
};

/* "" when neither NET_TOOLBOX_CONFIG nor HOME is set */
std::string settings_path();
Settings load_settings(const std::string &path);
bool save_settings(const std::string &path, const Settings &settings);

} // namespace nt
