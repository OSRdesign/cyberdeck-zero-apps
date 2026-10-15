/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the network as the board sees it (see interfaces.hpp).
 */
#include "interfaces.hpp"

#include "text.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdio>

namespace nt {

Route parse_default_route(const std::string &text)
{
    Route best;
    for (const std::string &line : split_lines(text)) {
        const std::vector<std::string> f = split_ws(line);
        if (f.size() < 8 || f[0] == "Iface") continue;
        std::uint32_t dest = 1, gateway = 0, flags = 0, mask = 1;
        if (!parse_hex(f[1], dest) || !parse_hex(f[2], gateway) || !parse_hex(f[3], flags) || !parse_hex(f[7], mask))
            continue;
        if (dest != 0 || mask != 0 || !(flags & 0x1) || !(flags & 0x2)) continue; /* RTF_UP, RTF_GATEWAY */
        std::uint64_t metric = 0;
        if (!parse_u64(f[6], metric) || metric > 0xFFFFFFFFu) metric = 0xFFFFFFFFu;
        if (best.ok && metric >= best.metric) continue;
        /* the kernel prints the address as the hex of its 32-bit value in host order (little endian on the Pi):
         * 0144A8C0 = bytes C0 A8 44 01 in memory = 192.168.68.1 */
        const std::uint8_t b[4] = {static_cast<std::uint8_t>(gateway & 0xFF), static_cast<std::uint8_t>((gateway >> 8) & 0xFF),
                                   static_cast<std::uint8_t>((gateway >> 16) & 0xFF),
                                   static_cast<std::uint8_t>((gateway >> 24) & 0xFF)};
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        best.ok = true;
        best.iface = f[0];
        best.gateway = buf;
        best.metric = static_cast<std::uint32_t>(metric);
    }
    return best;
}

ResolvConf parse_resolv_conf(const std::string &text)
{
    ResolvConf out;
    for (const std::string &raw : split_lines(text)) {
        std::string line = raw;
        const std::size_t hash = line.find_first_of("#;");
        if (hash != std::string::npos) line.erase(hash);
        const std::vector<std::string> f = split_ws(line);
        if (f.size() < 2) continue;
        if (f[0] == "nameserver") {
            if (out.servers.size() < 8 &&
                std::find(out.servers.begin(), out.servers.end(), f[1]) == out.servers.end())
                out.servers.push_back(f[1]);
        } else if (f[0] == "search" || f[0] == "domain") {
            if (f[0] == "search") out.search.clear(); /* the last search line wins (resolv.conf(5)) */
            for (std::size_t i = 1; i < f.size() && out.search.size() < 6; ++i) out.search.push_back(f[i]);
        }
    }
    return out;
}

std::vector<WirelessLevel> parse_net_wireless(const std::string &text)
{
    std::vector<WirelessLevel> out;
    for (const std::string &line : split_lines(text)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = trim(line.substr(0, colon));
        if (name.empty() || name.find('|') != std::string::npos || name.find(' ') != std::string::npos) continue;
        const std::vector<std::string> f = split_ws(line.substr(colon + 1));
        if (f.size() < 3) continue;
        WirelessLevel w;
        double level = 0;
        if (!parse_double(f[1], w.quality) || !parse_double(f[2], level)) continue;
        if (level >= 0) continue;
        w.iface = name;
        w.level_dbm = level;
        out.push_back(w);
    }
    return out;
}

IwLink parse_iw_link(const std::string &text)
{
    IwLink link;
    for (const std::string &raw : split_lines(text)) {
        const std::string line = trim(raw);
        if (starts_with(line, "Connected to")) {
            link.connected = true;
            const std::vector<std::string> w = split_ws(line.substr(12));
            if (!w.empty() && w[0].size() == 17) link.bssid = lower(w[0]);
        } else if (starts_with(line, "SSID:")) {
            link.ssid = trim(line.substr(5));
        } else if (starts_with(line, "freq:")) {
            const std::vector<std::string> w = split_ws(line.substr(5));
            double f = 0;
            if (!w.empty() && parse_double(w[0], f) && f > 0 && f < 100000) link.freq_mhz = static_cast<int>(f);
        } else if (starts_with(line, "signal:")) {
            const std::vector<std::string> w = split_ws(line.substr(7));
            double s = 0;
            if (!w.empty() && parse_double(w[0], s) && s > -200 && s < 50) {
                link.signal_dbm = static_cast<int>(s);
                link.has_signal = true;
            }
        } else if (starts_with(line, "tx bitrate:")) {
            const std::vector<std::string> w = split_ws(line.substr(11));
            double r = 0;
            if (!w.empty() && parse_double(w[0], r) && r >= 0 && r < 100000) {
                link.tx_mbit = r;
                link.has_bitrate = true;
            }
        }
    }
    if (!link.connected) link = IwLink();
    return link;
}

std::vector<std::string> iw_link_argv(const std::string &iface)
{
    return {kIwPath, "dev", iface, "link"};
}

bool Iface::connected() const
{
    if (loopback || !up) return false;
    return operstate == "up" || ((operstate == "unknown" || operstate.empty()) && running);
}

std::string Iface::first_ipv4() const
{
    if (ipv4.empty()) return std::string();
    return ipv4.front().substr(0, ipv4.front().find('/'));
}

const Iface *NetState::find(const std::string &name) const
{
    if (name.empty()) return nullptr;
    for (const Iface &i : ifaces)
        if (i.name == name) return &i;
    return nullptr;
}

NetState read_net_state(const Source &src)
{
    NetState st;
    st.route = parse_default_route(src.read("/proc/net/route"));
    st.dns = parse_resolv_conf(src.read("/etc/resolv.conf"));
    const std::vector<WirelessLevel> levels = parse_net_wireless(src.read("/proc/net/wireless"));
    const std::vector<IfAddr> addrs = src.addresses();

    std::vector<std::string> names = src.list("/sys/class/net");
    for (const IfAddr &a : addrs) /* an interface with an address that sysfs does not list (fixtures, containers) */
        if (std::find(names.begin(), names.end(), a.iface) == names.end()) names.push_back(a.iface);
    if (names.size() > 64) names.resize(64);

    for (const std::string &name : names) {
        if (name.empty() || name.find('/') != std::string::npos || name[0] == '.') continue;
        const std::string base = "/sys/class/net/" + name + "/";
        Iface i;
        i.name = name;
        i.operstate = trim(src.read(base + "operstate"));
        bool ok = false;
        const std::string flags_text = src.read(base + "flags", &ok);
        std::uint32_t flags = 0;
        if (ok && parse_hex(flags_text, flags)) {
            i.up = flags & kIffUp;
            i.loopback = flags & kIffLoopback;
        } else {
            i.up = i.operstate == "up" || i.operstate == "unknown";
            i.loopback = name == "lo";
        }
        /* sysfs "flags" never holds IFF_RUNNING (the kernel adds it from the link state): the carrier tells it (its
         * read fails while the interface is down) */
        int carrier = 0;
        i.running = i.operstate == "up" || (parse_int(src.read(base + "carrier"), carrier) && carrier == 1);
        int v = 0;
        if (parse_int(src.read(base + "mtu"), v) && v > 0) i.mtu = v;
        if (parse_int(src.read(base + "speed"), v) && v > 0) i.speed_mbps = v;
        i.mac = lower(trim(src.read(base + "address")));
        if (i.mac == "00:00:00:00:00:00" || i.mac.size() > 64) i.mac.clear();
        i.wireless = src.exists(base + "wireless") || src.exists(base + "phy80211");
        if (i.wireless) i.speed_mbps = -1; /* sysfs gives none for Wi-Fi: the bit rate comes from iw */
        for (const IfAddr &a : addrs) {
            if (a.iface != name) continue;
            const std::string text = a.address + "/" + std::to_string(a.prefix);
            if (a.family == AF_INET && i.ipv4.size() < 8) i.ipv4.push_back(text);
            else if (a.family == AF_INET6 && i.ipv6.size() < 8) i.ipv6.push_back(text);
        }
        for (const WirelessLevel &w : levels) {
            if (w.iface != name) continue;
            i.has_level = true;
            i.level_dbm = w.level_dbm;
            i.quality = w.quality;
            i.wireless = true;
        }
        st.ifaces.push_back(i);
    }

    if (st.route.ok && st.find(st.route.iface)) st.main_iface = st.route.iface;
    if (st.main_iface.empty())
        for (const Iface &i : st.ifaces)
            if (i.connected() && !i.ipv4.empty()) {
                st.main_iface = i.name;
                break;
            }

    const std::string main = st.main_iface;
    std::stable_sort(st.ifaces.begin(), st.ifaces.end(), [&main](const Iface &a, const Iface &b) {
        const auto rank = [&main](const Iface &i) { return i.name == main ? 0 : i.loopback ? 2 : 1; };
        return rank(a) < rank(b);
    });
    return st;
}

void apply_iw_link(NetState &state, const std::string &iface, const IwLink &link)
{
    for (Iface &i : state.ifaces) {
        if (i.name != iface) continue;
        i.link_valid = true;
        i.link = link;
        if (link.connected) i.wireless = true;
    }
}

} // namespace nt
