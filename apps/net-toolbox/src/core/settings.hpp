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
 * Parsed, never executed; unknown keys and bad values (a host that is not a valid name or address) are ignored;
 * written through a temporary file and a rename.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nt {

struct Settings {
    static constexpr std::size_t kMaxTargets = 8;

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

    /* a saved target: added at the end (moved there when already saved; the oldest goes past kMaxTargets); false
     * for an invalid host */
    bool add_target(const std::string &host);
    bool remove_target(const std::string &host);

    std::string serialize() const;
    static Settings parse(const std::string &text);
};

/* "" when neither NET_TOOLBOX_CONFIG nor HOME is set */
std::string settings_path();
Settings load_settings(const std::string &path);
bool save_settings(const std::string &path, const Settings &settings);

} // namespace nt
