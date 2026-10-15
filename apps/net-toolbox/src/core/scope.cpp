/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the chosen network (see scope.hpp).
 */
#include "scope.hpp"

#include "netaddr.hpp"
#include "sock.hpp"
#include "text.hpp"

#include <algorithm>

namespace nt {

std::string NetScope::note() const
{
    return missing ? chosen + " not found: using Automatic" : std::string();
}

std::string NetScope::via() const
{
    return iface.empty() ? std::string() : "via " + iface;
}

NetScope resolve_scope(const std::string &chosen, const NetState &st)
{
    NetScope s;
    s.main = st.main_iface;
    s.chosen = chosen;
    if (chosen.empty()) return s;
    const Iface *i = valid_iface_name(chosen) ? st.find(chosen) : nullptr;
    if (!i || i->loopback) {
        s.missing = true;
        return s;
    }
    s.iface = chosen;
    return s;
}

std::string scope_gateway(const NetScope &scope, const NetState &st)
{
    if (scope.automatic()) return st.route.ok ? st.route.gateway : std::string();
    const Route r = st.route_of(scope.iface);
    return r.ok ? r.gateway : std::string();
}

std::vector<std::string> nmcli_dns_argv(const std::string &iface)
{
    return {"nmcli", "-g", "IP4.DNS,IP6.DNS", "device", "show", iface};
}

namespace {

/* "2a01\:cb00\::1" -> "2a01:cb00::1" */
std::string unescape(const std::string &s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) ++i;
        out += s[i];
    }
    return out;
}

bool link_local_v6(const Address &a)
{
    return a.family == AF_INET6 && a.bytes[0] == 0xfe && (a.bytes[1] & 0xc0) == 0x80;
}

} // namespace

std::vector<std::string> parse_nmcli_dns(const std::string &text)
{
    std::vector<std::string> out;
    for (const std::string &raw : split_lines(text)) {
        std::string line = trim(raw);
        if (line.empty()) continue;
        /* "IP4.DNS[1]:1.1.1.1" (nmcli -t): the key and its first unescaped ':' go */
        if (starts_with(line, "IP4.DNS") || starts_with(line, "IP6.DNS")) {
            for (std::size_t i = 0; i < line.size(); ++i) {
                if (line[i] == '\\') {
                    ++i;
                } else if (line[i] == ':') {
                    line = line.substr(i + 1);
                    break;
                }
            }
        }
        std::size_t pos = 0;
        while (pos <= line.size()) {
            std::size_t bar = line.find('|', pos);
            if (bar == std::string::npos) bar = line.size();
            const std::string item = unescape(trim(line.substr(pos, bar - pos)));
            pos = bar + 1;
            Address a;
            if (item.empty() || !parse_ip(item, a) || link_local_v6(a)) continue;
            const std::string text_form = a.text();
            if (std::find(out.begin(), out.end(), text_form) != out.end()) continue;
            if (out.size() < 8) out.push_back(text_form);
        }
    }
    return out;
}

ScopeDns scope_dns(const NetScope &scope, const NetState &st, const std::string &nmcli_text, bool nmcli_ran)
{
    ScopeDns d;
    d.from = ScopeDns::From::System;
    d.label = "System default";
    d.servers = st.dns.servers;
    if (scope.automatic()) return d;
    if (nmcli_ran) {
        const std::vector<std::string> own = parse_nmcli_dns(nmcli_text);
        if (!own.empty()) {
            d.from = ScopeDns::From::Interface;
            d.servers = own;
            d.label = scope.iface + "'s DNS";
            return d;
        }
    }
    const std::string gw = scope_gateway(scope, st);
    if (!gw.empty()) {
        d.from = ScopeDns::From::Gateway;
        d.servers = {gw};
        d.label = scope.iface + "'s gateway";
        return d;
    }
    d.from = ScopeDns::From::SystemFallback;
    d.label = "System default (not " + scope.iface + "'s)";
    return d;
}

} // namespace nt
