/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the IPv4 subnet to scan and the ARP table reader for the Local devices tool (no LVGL, no network;
 * tests/test_subnet.cpp).
 *
 * The scan range comes from the chosen interface's own IPv4 address and mask (scope.hpp picks the interface):
 *   - the address must be private (RFC 1918: 10/8, 172.16/12, 192.168/16) or link-local (169.254/16); any other
 *     address refuses the scan with plain words (subnet_problem), so a public address is never swept;
 *   - the range is capped at a /24 (254 hosts): a mask wider than /24 (a /16, a /22) is narrowed to the /24 around the
 *     address; a /25 .. /30 keeps its own hosts; /31 and /32 have no host range to sweep and refuse.
 * The result (ScanRange) is the first and last usable host address (network + 1 .. broadcast - 1), in host byte order,
 * plus the interface, the address and the prefix, so the engine can iterate the range without re-doing the maths.
 *
 * The ARP reader parses /proc/net/arp (the kernel's neighbour cache): only complete entries (flag 0x2 = ATF_COM) on the
 * scanned interface with a real MAC are kept; incomplete entries (flags 0x0), other interfaces and the all-zero MAC are
 * dropped. The file is small and read on the worker thread.
 */
#pragma once

#include "interfaces.hpp"
#include "netaddr.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nt {

/* ---------------------------------------------------------------- the range to scan */

constexpr int kMaxScanHosts = 254; /* never more than a /24 */

struct ScanRange {
    bool ok = false;
    std::string problem;      /* when not ok: plain words (subnet_problem) */
    std::string iface;        /* the interface the range belongs to */
    std::string address;      /* the interface's IPv4 address, "192.168.68.121" */
    int prefix = 0;           /* the mask bits actually used (24 when a wider mask was capped) */
    std::uint32_t self = 0;   /* the interface's address, host byte order */
    std::uint32_t first = 0;  /* first usable host, host byte order */
    std::uint32_t last = 0;   /* last usable host, host byte order (inclusive) */
    int count() const { return last >= first ? static_cast<int>(last - first + 1) : 0; }
    bool contains(std::uint32_t host_order) const { return ok && host_order >= first && host_order <= last; }
};

/* 10/8, 172.16/12, 192.168/16 */
bool is_ipv4_private(const Address &a);
/* private or 169.254/16 (link-local): an address a local scan is allowed to sweep */
bool is_ipv4_scannable(const Address &a);

/* The range for "192.168.68.121/22" (CIDR as interfaces.cpp writes it): capped to a /24, refused for a non-private
 * address, /31, /32 or a bad text. */
ScanRange scan_range_from_cidr(const std::string &cidr, const std::string &iface);

/* The scan range of a NetScope's effective interface, read from `st` (its first IPv4 address). Refuses with words when
 * the interface has no IPv4 address ("No network") or the address is not private. */
ScanRange scan_range_of(const NetState &st, const std::string &iface);

/* The words a port scan refuses a target with. */
constexpr const char *kScanTargetRefused = "Only devices on your own network can be scanned.";

/* Why a port scan of `a` is refused ("" = allowed). Allowed: a private (RFC 1918) or link-local (169.254/16) IPv4
 * address; never 0.0.0.0, loopback, multicast, 240/4 or 255.255.255.255. When `range` is ok, also only a host inside it
 * (first .. last: neither the network nor the broadcast address, nor another subnet). A non-IPv4 address gives
 * "No address to scan", anything else refused kScanTargetRefused. */
std::string scan_target_problem(const Address &a, const ScanRange &range);

std::uint32_t ipv4_to_host_order(const Address &a); /* 0 when not IPv4 */
Address host_order_to_ipv4(std::uint32_t host_order);
std::string ipv4_text(std::uint32_t host_order);

/* ---------------------------------------------------------------- the ARP table */

struct ArpEntry {
    std::string ip;     /* "192.168.68.1" */
    std::string mac;    /* lower-case "aa:bb:cc:dd:ee:ff" */
    std::string iface;
    std::uint32_t flags = 0;
};

/* Every line of /proc/net/arp (its header dropped); unparsable lines skipped. No filtering here. */
std::vector<ArpEntry> parse_arp_table(const std::string &text);
/* The complete entries (flag 0x2) on `iface` with a usable MAC, as ip -> mac (lower case). */
std::vector<ArpEntry> arp_neighbours(const std::string &text, const std::string &iface);

} // namespace nt
