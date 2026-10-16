/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the scan range and the ARP reader (see subnet.hpp).
 */
#include "subnet.hpp"

#include "text.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cstdio>

namespace nt {

std::uint32_t ipv4_to_host_order(const Address &a)
{
    if (a.family != AF_INET) return 0;
    return (static_cast<std::uint32_t>(a.bytes[0]) << 24) | (static_cast<std::uint32_t>(a.bytes[1]) << 16) |
           (static_cast<std::uint32_t>(a.bytes[2]) << 8) | static_cast<std::uint32_t>(a.bytes[3]);
}

Address host_order_to_ipv4(std::uint32_t h)
{
    Address a;
    a.family = AF_INET;
    a.bytes[0] = static_cast<std::uint8_t>((h >> 24) & 0xFF);
    a.bytes[1] = static_cast<std::uint8_t>((h >> 16) & 0xFF);
    a.bytes[2] = static_cast<std::uint8_t>((h >> 8) & 0xFF);
    a.bytes[3] = static_cast<std::uint8_t>(h & 0xFF);
    return a;
}

std::string ipv4_text(std::uint32_t h)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (h >> 24) & 0xFF, (h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF);
    return buf;
}

bool is_ipv4_private(const Address &a)
{
    if (a.family != AF_INET) return false;
    const std::uint8_t b0 = a.bytes[0], b1 = a.bytes[1];
    if (b0 == 10) return true;                          /* 10.0.0.0/8 */
    if (b0 == 172 && b1 >= 16 && b1 <= 31) return true; /* 172.16.0.0/12 */
    if (b0 == 192 && b1 == 168) return true;            /* 192.168.0.0/16 */
    return false;
}

bool is_ipv4_scannable(const Address &a)
{
    return is_ipv4_private(a) || is_ipv4_link_local(a);
}

std::string scan_target_problem(const Address &a, const ScanRange &range)
{
    if (a.family != AF_INET) return "No address to scan";
    const std::uint32_t h = ipv4_to_host_order(a);
    const std::uint8_t b0 = a.bytes[0];
    if (h == 0 || h == 0xFFFFFFFFu) return kScanTargetRefused;     /* 0.0.0.0, 255.255.255.255 */
    if (b0 == 127) return kScanTargetRefused;                        /* loopback */
    if (b0 >= 224) return kScanTargetRefused;                        /* multicast 224/4, reserved 240/4 */
    if (!is_ipv4_scannable(a)) return kScanTargetRefused;            /* a public address */
    if (range.ok && !range.contains(h)) return kScanTargetRefused;  /* another subnet, its network or broadcast */
    return std::string();
}

ScanRange scan_range_from_cidr(const std::string &cidr, const std::string &iface)
{
    ScanRange r;
    r.iface = iface;
    const std::size_t slash = cidr.find('/');
    const std::string addr_text = slash == std::string::npos ? cidr : cidr.substr(0, slash);
    Address a;
    if (!parse_ip(addr_text, a) || a.family != AF_INET) {
        r.problem = "No network";
        return r;
    }
    r.address = a.text();
    int prefix = 24;
    if (slash != std::string::npos) {
        int p = 0;
        if (!parse_int(cidr.substr(slash + 1), p) || p < 0 || p > 32) {
            r.problem = "No network";
            return r;
        }
        prefix = p;
    }
    if (!is_ipv4_scannable(a)) {
        r.problem = r.address + " is not a private address: Local devices only scans your own private network.";
        return r;
    }
    /* cap a wide mask (/0 .. /23) at a /24 around the address; keep /24 .. /30; refuse /31 and /32 */
    if (prefix < 24) prefix = 24;
    if (prefix > 30) {
        r.problem = "This network (/" + std::to_string(prefix) + ") has no other addresses to scan.";
        return r;
    }
    r.prefix = prefix;
    r.self = ipv4_to_host_order(a);
    const std::uint32_t mask = prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));
    const std::uint32_t network = r.self & mask;
    const std::uint32_t broadcast = network | ~mask;
    if (broadcast < network + 2) { /* no usable host range (should not happen for /24 .. /30) */
        r.problem = "This network has no other addresses to scan.";
        return r;
    }
    r.first = network + 1;
    r.last = broadcast - 1;
    if (r.count() > kMaxScanHosts) r.last = r.first + kMaxScanHosts - 1; /* defensive: a /24 is already 254 */
    r.ok = true;
    return r;
}

ScanRange scan_range_of(const NetState &st, const std::string &iface)
{
    ScanRange r;
    r.iface = iface;
    const Iface *i = st.find(iface);
    if (!i || i->ipv4.empty()) {
        r.problem = "No network";
        return r;
    }
    return scan_range_from_cidr(i->ipv4.front(), iface);
}

std::vector<ArpEntry> parse_arp_table(const std::string &text)
{
    std::vector<ArpEntry> out;
    const std::vector<std::string> lines = split_lines(text);
    for (std::size_t n = 0; n < lines.size(); ++n) {
        const std::vector<std::string> f = split_ws(lines[n]);
        /* columns: IP address, HW type, Flags, HW address, Mask, Device */
        if (f.size() < 6) continue;
        if (f[0] == "IP") continue; /* the header line */
        Address a;
        if (!parse_ip(f[0], a) || a.family != AF_INET) continue;
        std::uint32_t flags = 0;
        if (!parse_hex(f[2], flags)) continue;
        ArpEntry e;
        e.ip = f[0];
        e.flags = flags;
        e.mac = lower(f[3]);
        e.iface = f[5];
        out.push_back(e);
    }
    return out;
}

std::vector<ArpEntry> arp_neighbours(const std::string &text, const std::string &iface)
{
    std::vector<ArpEntry> out;
    for (ArpEntry &e : parse_arp_table(text)) {
        if (!(e.flags & 0x2)) continue; /* ATF_COM: only complete entries */
        if (!iface.empty() && e.iface != iface) continue;
        if (e.mac.empty() || e.mac == "00:00:00:00:00:00" || e.mac.size() != 17) continue;
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace nt
