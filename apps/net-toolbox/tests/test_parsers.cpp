/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: addresses and names, and the interface reader on the fixtures of the deck (Pi Zero 2 W), the
 * deck offline, a Pi 3A+ and a Pi 5.
 */
#include "check.hpp"

#include "interfaces.hpp"
#include "netaddr.hpp"
#include "source.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace nt;

void test_netaddr()
{
    Address a;
    CHECK(parse_ip("192.168.68.1", a) && a.family == AF_INET && a.text() == "192.168.68.1");
    CHECK(parse_ip("2606:4700:4700::1111", a) && a.family == AF_INET6 && a.text() == "2606:4700:4700::1111");
    CHECK(parse_ip("[::1]", a) && is_loopback(a));
    CHECK(parse_ip("127.0.0.53", a) && is_loopback(a));
    CHECK(!parse_ip("1.2.3", a) && !parse_ip("example.com", a) && !parse_ip("", a) && !parse_ip("1.2.3.4.5", a));
    CHECK(!parse_ip("fe80::1%no-such-if0", a)); /* an unknown zone cannot be used */
    CHECK(parse_ip("fe80::1%1", a) && a.scope == 1);
    CHECK(parse_ip("169.254.10.3", a) && is_ipv4_link_local(a));
    CHECK(parse_ip("192.168.1.1", a) && !is_ipv4_link_local(a) && !is_loopback(a));
    CHECK(parse_ip("1.2.3.4", a) && reverse_name(a) == "4.3.2.1.in-addr.arpa");
    CHECK(parse_ip("2001:db8::567:89ab", a) &&
          reverse_name(a) == "b.a.9.8.7.6.5.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.8.b.d.0.1.0.0.2.ip6.arpa");
    Address b;
    parse_ip("1.2.3.4", a);
    parse_ip("1.2.3.4", b);
    CHECK(a == b);
    parse_ip("1.2.3.5", b);
    CHECK(a != b);
    sockaddr_storage ss;
    CHECK(a.to_sockaddr(53, ss) == sizeof(sockaddr_in));
    CHECK(Address::from_sockaddr(reinterpret_cast<sockaddr *>(&ss)) == a);
    CHECK(Address().to_sockaddr(53, ss) == 0 && Address().text().empty());

    std::string why;
    CHECK(valid_hostname("example.com") && valid_hostname("example.com.") && valid_hostname("_dmarc.example.com"));
    CHECK(valid_hostname("xn--bcher-kva.example") && valid_hostname("a"));
    CHECK(!valid_hostname("", &why) && !why.empty());
    CHECK(!valid_hostname("a..b", &why) && why == "Empty part between dots");
    CHECK(!valid_hostname("two words", &why) && why == "No spaces in a host name");
    CHECK(!valid_hostname("a/b") && !valid_hostname(".a") && !valid_hostname("a;rm"));
    CHECK(!valid_hostname(std::string(64, 'a') + ".com"));
    CHECK(valid_hostname(std::string(63, 'a') + ".com"));
    std::string long_name;
    for (int i = 0; i < 64; ++i) long_name += "abc.";
    CHECK(!valid_hostname(long_name)); /* 256 bytes */
    CHECK(valid_host("10.0.0.1") && valid_host("nas.lan") && !valid_host(" "));
}

static const Iface *find(const NetState &st, const char *name)
{
    return st.find(name);
}

void test_interfaces()
{
    // the parsers on their own
    Route r = parse_default_route(fixture("deck", "proc/net/route"));
    CHECK(r.ok && r.iface == "wlan0" && r.gateway == "192.168.68.1" && r.metric == 600);
    CHECK(!parse_default_route(fixture("deck-offline", "proc/net/route")).ok);
    CHECK(!parse_default_route("").ok && !parse_default_route("garbage\n1 2 3\n").ok);
    r = parse_default_route("Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\n"
                            "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
                            "eth0\t00000000\t0144A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
                            "usb0\t00000000\t00000000\t0001\t0\t0\t50\t00000000\t0\t0\t0\n"); /* no RTF_GATEWAY */
    CHECK(r.ok && r.iface == "eth0" && r.gateway == "192.168.68.1");

    ResolvConf rc = parse_resolv_conf(fixture("deck", "etc/resolv.conf"));
    CHECK_EQ(rc.servers.size(), static_cast<std::size_t>(2));
    CHECK_EQ(rc.servers[0], std::string("192.168.1.1")); /* the order listed: not the gateway first */
    CHECK_EQ(rc.servers[1], std::string("192.168.68.1"));
    rc = parse_resolv_conf("domain a.lan\nsearch lan home\r\nnameserver 1.1.1.1 # cf\nnameserver 1.1.1.1\n;x\noptions edns0\n");
    CHECK(rc.servers.size() == 1 && rc.search.size() == 2 && rc.search[0] == "lan");
    CHECK(parse_resolv_conf(fixture("pi-5", "etc/resolv.conf")).servers[1] == "fd7a:1b2c:3d4e::1");
    CHECK(parse_resolv_conf("").servers.empty());

    std::vector<WirelessLevel> lv = parse_net_wireless(fixture("deck", "proc/net/wireless"));
    CHECK(lv.size() == 1 && lv[0].iface == "wlan0" && lv[0].level_dbm == -58 && lv[0].quality == 52);
    CHECK(parse_net_wireless(fixture("deck-offline", "proc/net/wireless")).empty()); /* level 0: not associated */
    CHECK(parse_net_wireless(fixture("pi-5", "proc/net/wireless")).empty());

    IwLink l = parse_iw_link(fixture("deck", "cmd/iw_dev_wlan0_link"));
    CHECK(l.connected && l.ssid == "DeckNet" && l.bssid == "5c:62:8b:a1:07:3e");
    CHECK(l.has_signal && l.signal_dbm == -58 && l.has_bitrate && l.tx_mbit == 65.0 && l.freq_mhz == 2437);
    l = parse_iw_link(fixture("pi-3a-plus", "cmd/iw_dev_wlan0_link"));
    CHECK(l.connected && l.ssid == "DeckNet-5G" && l.freq_mhz == 5180 && l.tx_mbit == 150.0 && l.signal_dbm == -67);
    l = parse_iw_link(fixture("pi-5", "cmd/iw_dev_wlan0_link"));
    CHECK(!l.connected && l.ssid.empty() && !l.has_signal);
    CHECK(!parse_iw_link("command failed: No such device (-19)\n").connected);
    const std::vector<std::string> argv = iw_link_argv("wlan0");
    CHECK(argv.size() == 4 && argv[0] == "/usr/sbin/iw" && argv[2] == "wlan0" && argv[3] == "link");

    const std::vector<IfAddr> addrs = parse_ifaddrs_fixture("lo inet 127.0.0.1/8\nwlan0 inet6 fe80::1/64\nbad\nx ipx 1/2\n");
    CHECK(addrs.size() == 2 && addrs[1].family == AF_INET6 && addrs[1].prefix == 64);

    // the deck
    {
        const auto src = make_fixture_source(fixture_dir("deck"));
        const NetState st = read_net_state(*src);
        CHECK_EQ(st.main_iface, std::string("wlan0"));
        CHECK_EQ(st.ifaces.size(), static_cast<std::size_t>(2));
        CHECK_EQ(st.ifaces.front().name, std::string("wlan0")); /* the main one first, lo last */
        CHECK_EQ(st.ifaces.back().name, std::string("lo"));
        const Iface *w = find(st, "wlan0");
        CHECK(w && w->up && w->running && w->connected() && w->wireless && !w->loopback);
        CHECK(w && w->mac == "d8:3a:dd:4c:21:9e" && w->mtu == 1500 && w->speed_mbps == -1);
        CHECK(w && w->ipv4.size() == 1 && w->ipv4[0] == "192.168.68.121/22" && w->first_ipv4() == "192.168.68.121");
        CHECK(w && w->ipv6.size() == 1 && w->ipv6[0] == "fe80::7c3e:1d5a:93b2:4f10/64");
        CHECK(w && w->has_level && w->level_dbm == -58);
        CHECK(w && !w->link_valid);
        const Iface *lo = find(st, "lo");
        CHECK(lo && lo->loopback && !lo->connected() && lo->mac.empty() && lo->ipv4[0] == "127.0.0.1/8");
        CHECK(st.route.gateway == "192.168.68.1" && st.dns.servers.front() == "192.168.1.1");
        NetState with_link = st;
        apply_iw_link(with_link, "wlan0", parse_iw_link(fixture("deck", "cmd/iw_dev_wlan0_link")));
        CHECK(with_link.main()->link_valid && with_link.main()->link.ssid == "DeckNet");
    }
    // the deck, Wi-Fi not associated
    {
        const auto src = make_fixture_source(fixture_dir("deck-offline"));
        const NetState st = read_net_state(*src);
        CHECK(st.main_iface.empty() && !st.route.ok && st.dns.servers.empty());
        const Iface *w = find(st, "wlan0");
        CHECK(w && w->up && !w->running && !w->connected() && w->operstate == "dormant" && w->ipv4.empty());
        CHECK(w && w->wireless && !w->has_level);
    }
    // a Pi 3A+
    {
        const auto src = make_fixture_source(fixture_dir("pi-3a-plus"));
        const NetState st = read_net_state(*src);
        const Iface *w = st.main();
        CHECK(w && w->name == "wlan0" && w->wireless && w->first_ipv4() == "192.168.68.170");
        CHECK(w && w->ipv6.size() == 2 && w->level_dbm == -67);
        CHECK(st.dns.search.size() == 1 && st.dns.search[0] == "lan");
    }
    // a Pi 5: Ethernet main, Wi-Fi off
    {
        const auto src = make_fixture_source(fixture_dir("pi-5"));
        const NetState st = read_net_state(*src);
        CHECK_EQ(st.main_iface, std::string("eth0"));
        CHECK_EQ(st.ifaces.front().name, std::string("eth0"));
        const Iface *e = find(st, "eth0");
        CHECK(e && e->connected() && !e->wireless && e->speed_mbps == 1000 && e->mac == "2c:cf:67:0a:5e:13");
        const Iface *w = find(st, "wlan0");
        CHECK(w && !w->up && !w->connected() && w->wireless && w->operstate == "down" && w->ipv4.empty());
        CHECK(st.dns.servers.size() == 2);
    }
    // an empty folder: nothing, no crash
    {
        const std::string dir = test_tmpdir("empty");
        const auto src = make_fixture_source(dir);
        const NetState st = read_net_state(*src);
        CHECK(st.ifaces.empty() && st.main_iface.empty() && !st.route.ok && st.main() == nullptr);
        rmdir(dir.c_str());
    }
    // the system itself: whatever it is, it reads
    {
        const auto src = make_system_source();
        const NetState st = read_net_state(*src);
        CHECK(!st.ifaces.empty());
        CHECK(find(st, "lo") != nullptr);
    }
}
