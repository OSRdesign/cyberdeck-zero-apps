/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.4): the Local devices subnet maths (the /24 cap, a /16 narrowed to /24, the private and
 * link-local checks, the mask edge cases /30, /31, /32) and the /proc/net/arp parser (complete vs incomplete entries,
 * other interfaces, the all-zero MAC).
 */
#include "check.hpp"

#include "interfaces.hpp"
#include "netaddr.hpp"
#include "subnet.hpp"

using namespace nt;

namespace {

Address a_of(const char *text)
{
    Address a;
    parse_ip(text, a);
    return a;
}

} // namespace

void test_subnet_math()
{
    // the private / link-local classification
    CHECK(is_ipv4_private(a_of("10.0.0.1")));
    CHECK(is_ipv4_private(a_of("10.255.255.254")));
    CHECK(is_ipv4_private(a_of("172.16.0.1")));
    CHECK(is_ipv4_private(a_of("172.31.255.1")));
    CHECK(is_ipv4_private(a_of("192.168.0.1")));
    CHECK(!is_ipv4_private(a_of("172.15.0.1")));
    CHECK(!is_ipv4_private(a_of("172.32.0.1")));
    CHECK(!is_ipv4_private(a_of("11.0.0.1")));
    CHECK(!is_ipv4_private(a_of("8.8.8.8")));
    CHECK(!is_ipv4_private(a_of("169.254.1.1")));
    CHECK(is_ipv4_scannable(a_of("169.254.1.1"))); /* link-local is scannable */
    CHECK(is_ipv4_scannable(a_of("192.168.1.1")));
    CHECK(!is_ipv4_scannable(a_of("1.1.1.1")));

    // a /22 is capped to the /24 around the address
    {
        const ScanRange r = scan_range_from_cidr("192.168.68.121/22", "wlan0");
        CHECK(r.ok && r.prefix == 24);
        CHECK_EQ(r.iface, std::string("wlan0"));
        CHECK_EQ(ipv4_text(r.first), std::string("192.168.68.1"));
        CHECK_EQ(ipv4_text(r.last), std::string("192.168.68.254"));
        CHECK_EQ(r.count(), 254);
        CHECK_EQ(ipv4_text(r.self), std::string("192.168.68.121"));
        CHECK(r.contains(r.self) && r.contains(ipv4_to_host_order(a_of("192.168.68.1"))));
        CHECK(!r.contains(ipv4_to_host_order(a_of("192.168.69.1"))));
    }
    // a /16 is also capped to a /24
    {
        const ScanRange r = scan_range_from_cidr("10.5.9.20/8", "eth0");
        CHECK(r.ok && r.prefix == 24);
        CHECK_EQ(ipv4_text(r.first), std::string("10.5.9.1"));
        CHECK_EQ(ipv4_text(r.last), std::string("10.5.9.254"));
    }
    // a plain /24
    {
        const ScanRange r = scan_range_from_cidr("192.168.1.41/24", "wlan0");
        CHECK(r.ok && r.count() == 254 && ipv4_text(r.first) == "192.168.1.1");
    }
    // /28 keeps its own 14 hosts
    {
        const ScanRange r = scan_range_from_cidr("172.20.10.2/28", "wlan1");
        CHECK(r.ok && r.prefix == 28);
        CHECK_EQ(ipv4_text(r.first), std::string("172.20.10.1"));
        CHECK_EQ(ipv4_text(r.last), std::string("172.20.10.14"));
        CHECK_EQ(r.count(), 14);
    }
    // /30 has two hosts
    {
        const ScanRange r = scan_range_from_cidr("192.168.1.5/30", "wlan0");
        CHECK(r.ok && r.prefix == 30 && r.count() == 2);
        CHECK_EQ(ipv4_text(r.first), std::string("192.168.1.5"));
        CHECK_EQ(ipv4_text(r.last), std::string("192.168.1.6"));
    }
    // /31 and /32 have no host range
    {
        const ScanRange r31 = scan_range_from_cidr("192.168.1.4/31", "wlan0");
        CHECK(!r31.ok && !r31.problem.empty());
        const ScanRange r32 = scan_range_from_cidr("192.168.1.4/32", "wlan0");
        CHECK(!r32.ok && !r32.problem.empty());
    }
    // a link-local address is scannable (capped to /24)
    {
        const ScanRange r = scan_range_from_cidr("169.254.5.9/16", "wlan0");
        CHECK(r.ok && r.prefix == 24 && ipv4_text(r.first) == "169.254.5.1");
    }
    // a public address is refused with plain words
    {
        const ScanRange r = scan_range_from_cidr("93.184.215.14/24", "wlan0");
        CHECK(!r.ok);
        CHECK(r.problem.find("private") != std::string::npos);
    }
    // no prefix: defaults to /24; bad text: "No network"
    {
        CHECK(scan_range_from_cidr("192.168.1.5", "wlan0").ok);
        CHECK(!scan_range_from_cidr("not-an-ip", "wlan0").ok);
        CHECK_EQ(scan_range_from_cidr("not-an-ip", "wlan0").problem, std::string("No network"));
        CHECK(!scan_range_from_cidr("192.168.1.5/40", "wlan0").ok);
    }

    // scan_range_of reads the interface's first IPv4 from a NetState
    {
        NetState st;
        Iface i;
        i.name = "wlan0";
        i.ipv4 = {"192.168.68.121/22"};
        st.ifaces.push_back(i);
        const ScanRange r = scan_range_of(st, "wlan0");
        CHECK(r.ok && r.prefix == 24 && r.iface == "wlan0");
        const ScanRange none = scan_range_of(st, "eth9"); /* no such interface */
        CHECK(!none.ok && none.problem == "No network");
        Iface noip;
        noip.name = "wlan1";
        st.ifaces.push_back(noip);
        const ScanRange noip_r = scan_range_of(st, "wlan1");
        CHECK(!noip_r.ok && noip_r.problem == "No network");
    }
}

void test_arp_parser()
{
    const std::string text =
        "IP address       HW type     Flags       HW address            Mask     Device\n"
        "192.168.68.1     0x1         0x2         AA:BB:CC:DD:EE:01     *        wlan0\n"
        "192.168.68.50    0x1         0x0         00:00:00:00:00:00     *        wlan0\n"  /* incomplete */
        "192.168.68.60    0x1         0x2         11:22:33:44:55:66     *        eth0\n"   /* other iface */
        "192.168.68.70    0x1         0x2         aa:bb:cc:dd:ee:70     *        wlan0\n"
        "garbage line that does not parse\n";

    const std::vector<ArpEntry> all = parse_arp_table(text);
    CHECK_EQ(all.size(), static_cast<std::size_t>(4)); /* the four address lines, header and garbage dropped */

    const std::vector<ArpEntry> wlan0 = arp_neighbours(text, "wlan0");
    CHECK_EQ(wlan0.size(), static_cast<std::size_t>(2)); /* only complete entries on wlan0 */
    if (wlan0.size() == 2) {
        CHECK_EQ(wlan0[0].ip, std::string("192.168.68.1"));
        CHECK_EQ(wlan0[0].mac, std::string("aa:bb:cc:dd:ee:01")); /* lower-cased */
        CHECK_EQ(wlan0[1].ip, std::string("192.168.68.70"));
    }
    const std::vector<ArpEntry> eth0 = arp_neighbours(text, "eth0");
    CHECK_EQ(eth0.size(), static_cast<std::size_t>(1));
    const std::vector<ArpEntry> all_if = arp_neighbours(text, ""); /* every interface */
    CHECK_EQ(all_if.size(), static_cast<std::size_t>(3));
    CHECK(arp_neighbours("", "wlan0").empty());
}
