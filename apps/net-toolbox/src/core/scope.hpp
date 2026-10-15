/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the chosen network (no LVGL; tests/test_scope.cpp on the fixture boards).
 *
 * A Pi may have several ways out: its Wi-Fi, a second Wi-Fi adapter, an Ethernet adapter on a hub. The setting
 * `interface` (settings.hpp) is "" = Automatic (the system's route, as before 0.3) or the name of one interface. A
 * NetScope resolves that against the current NetState:
 *   chosen found      iface = its name: every tool that sends traffic binds to it (sock.hpp: SO_BINDTODEVICE; cmdrun.hpp:
 *                     curl --interface), the diagnosis checks it, its own gateway and DNS servers are used;
 *   chosen not found  (an adapter unplugged) the scope falls back to Automatic: iface = "", missing = true and note()
 *                     says "wlan1 not found: using Automatic". The setting is kept: the adapter plugged in again is used
 *                     again. Never an error, never a crash.
 *   Automatic         iface = "": nothing is bound, the tools behave exactly as in 0.2.
 *
 * Which gateway: the default route through that interface (route_of; /proc/net/route, not the system's default one).
 * Which DNS servers (scope_dns): the interface's own, from NetworkManager (`nmcli -g IP4.DNS,IP6.DNS device show <if>`,
 * run by the caller through its command runner: an argv here, a parser, and the choice of the answer), else the
 * interface's gateway (most routers and phones answer DNS), else the system's resolv.conf list (which may be unreachable
 * over this interface: the DNS page's label says so). Automatic: resolv.conf, as before.
 *
 * The UI asks `ToolboxApp::scope()` for the effective choice; a tool needs nothing else.
 */
#pragma once

#include "interfaces.hpp"

#include <string>
#include <vector>

namespace nt {

struct NetScope {
    std::string chosen;       /* the setting: "" = Automatic, else an interface name */
    std::string iface;        /* the interface in use: chosen when found, else "" (Automatic) */
    std::string main;         /* the system's main interface now (default route), "" when none */
    bool missing = false;     /* chosen is set but there is no such interface: fell back to Automatic */

    bool automatic() const { return iface.empty(); }
    /* the interface the tools go out of: iface, else the system's main one ("" when none) */
    std::string effective() const { return iface.empty() ? main : iface; }
    /* "wlan1 not found: using Automatic", "" when nothing is missing */
    std::string note() const;
    /* "via wlan1" for a status line, "" for Automatic */
    std::string via() const;
};

/* chosen "" = Automatic; a name that is not a valid interface name, the loopback, or not in `st` = missing */
NetScope resolve_scope(const std::string &chosen, const NetState &st);

/* The gateway of the scope: Automatic = the system's default route, else the default route through the interface; "" none */
std::string scope_gateway(const NetScope &scope, const NetState &st);

/* ---------------------------------------------------------------- DNS servers of an interface */

/* {"nmcli", "-g", "IP4.DNS,IP6.DNS", "device", "show", iface} */
std::vector<std::string> nmcli_dns_argv(const std::string &iface);
constexpr int kNmcliTimeoutMs = 3000;

/* The addresses in nmcli's answer, in order, no duplicate, at most 8. nmcli -g prints one line per field, the values of a
 * field separated by " | ", ':' escaped as "\:" (IPv6): "192.168.1.1 | 192.168.68.1" then "2a01\:cb00\::1". IPv6
 * link-local addresses (fe80::) are left out (they need a zone to be used). Lines "IP4.DNS[1]:1.1.1.1" (nmcli -t) read
 * too. */
std::vector<std::string> parse_nmcli_dns(const std::string &text);

struct ScopeDns {
    enum class From { System, Interface, Gateway, SystemFallback };
    From from = From::System;
    std::vector<std::string> servers; /* in the order they are asked; the first is the "System default" resolver */
    /* "System default", "wlan1's DNS", "wlan1's gateway", "System default (not wlan1's)" */
    std::string label;
};

/* nmcli_text: nmcli's output; nmcli_ran false = it could not run or failed (no NetworkManager) */
ScopeDns scope_dns(const NetScope &scope, const NetState &st, const std::string &nmcli_text, bool nmcli_ran);

} // namespace nt
