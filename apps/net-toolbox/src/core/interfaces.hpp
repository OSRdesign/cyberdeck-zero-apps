/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the network as the board sees it (no LVGL; tests/test_parsers.cpp runs it on fixture folders of the deck,
 * a Pi 3A+ and a Pi 5).
 *
 *   interfaces   /sys/class/net/<if>/{flags,operstate,carrier,address,mtu,speed,wireless|phy80211} and getifaddrs (the
 *                addresses), through a Source (the system or a fixture folder)
 *   gateway      /proc/net/route, the default route with the lowest metric (IPv4)
 *   DNS servers  /etc/resolv.conf, in the order listed (the first is the "system default" resolver)
 *   Wi-Fi        /proc/net/wireless (signal level, any user, no command); the network name and bit rate come from
 *                `/usr/sbin/iw dev <if> link` (full path: /usr/sbin is not in every PATH), which the UI runs through the
 *                shared async_run (it needs the LVGL thread) with iw_link_argv() and kIwTimeoutMs, then hands the
 *                output to parse_iw_link() and apply_iw_link(). Optional: without iw the level still shows.
 *
 * Every parser takes the whole file text, tolerates "\r", a missing trailing newline and unknown lines, and reports
 * "not found" instead of throwing. Reading is a few small files and one getifaddrs: fast enough for any thread.
 */
#pragma once

#include "source.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nt {

/* ---------------------------------------------------------------- parsers (pure) */

struct Route {
    bool ok = false;
    std::string iface;
    std::string gateway;     /* "192.168.68.1" */
    std::uint32_t metric = 0;
};

/* /proc/net/route: the default route (destination 0, mask 0, RTF_UP and RTF_GATEWAY) with the lowest metric */
Route parse_default_route(const std::string &text);

struct ResolvConf {
    std::vector<std::string> servers; /* "nameserver" lines in order, at most 8 */
    std::vector<std::string> search;  /* "search" / "domain" names */
};

ResolvConf parse_resolv_conf(const std::string &text);

struct WirelessLevel {
    std::string iface;
    double quality = 0;   /* link quality, driver units (mostly /70) */
    double level_dbm = 0;
};

/* /proc/net/wireless: one entry per wireless interface with a level in dBm (a level >= 0, "not associated" or a
 * relative value, is left out) */
std::vector<WirelessLevel> parse_net_wireless(const std::string &text);

struct IwLink {
    bool connected = false;
    std::string ssid;
    std::string bssid;
    bool has_signal = false;
    int signal_dbm = 0;
    bool has_bitrate = false;
    double tx_mbit = 0;
    int freq_mhz = 0;
};

/* `iw dev wlan0 link` (LC_ALL=C); "Not connected." gives connected = false */
IwLink parse_iw_link(const std::string &text);

constexpr const char *kIwPath = "/usr/sbin/iw";
constexpr int kIwTimeoutMs = 1500;
/* {"/usr/sbin/iw", "dev", iface, "link"} */
std::vector<std::string> iw_link_argv(const std::string &iface);

/* /sys/class/net/<if>/flags: "0x1003" (the interface's own flags: IFF_RUNNING is never there, see Iface::running) */
constexpr std::uint32_t kIffUp = 0x1, kIffLoopback = 0x8;

/* ---------------------------------------------------------------- the whole picture */

struct Iface {
    std::string name;
    bool up = false;               /* IFF_UP: switched on */
    bool running = false;          /* a link: operstate "up", or /sys carrier 1 (Wi-Fi associated, cable in) */
    bool loopback = false;
    bool wireless = false;
    std::string operstate;         /* "up", "down", "dormant", "unknown", "lowerlayerdown", "" */
    int speed_mbps = -1;           /* /sys speed (wired, link up); -1 unknown */
    int mtu = 0;
    std::string mac;               /* "b8:27:eb:12:34:56"; "" for none (lo) */
    std::vector<std::string> ipv4; /* "192.168.68.121/22" */
    std::vector<std::string> ipv6; /* "fe80::ba27:ebff:fe12:3456/64" */
    bool has_level = false;        /* /proc/net/wireless */
    double level_dbm = 0;
    double quality = 0;
    bool link_valid = false;       /* iw was asked (apply_iw_link) */
    IwLink link;

    /* a usable link: up, and operstate "up" (or "unknown" while running, as some drivers report) */
    bool connected() const;
    /* the first IPv4 address without its prefix, "" */
    std::string first_ipv4() const;
};

struct NetState {
    std::vector<Iface> ifaces; /* the main interface first, then the others by name, lo last */
    Route route;
    ResolvConf dns;
    std::string main_iface;    /* the default route's, else the first connected one with an IPv4, else "" */

    const Iface *find(const std::string &name) const;
    const Iface *main() const { return find(main_iface); }
};

NetState read_net_state(const Source &src);
/* iw's answer for one interface, merged into the state (a disconnected answer clears the link) */
void apply_iw_link(NetState &state, const std::string &iface, const IwLink &link);

} // namespace nt
