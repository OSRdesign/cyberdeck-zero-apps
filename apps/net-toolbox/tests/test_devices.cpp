/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.4): the Local devices discovery state machine on fakes (a fake prober and a fake neighbour
 * reader, as the other engines' fakes), the live list, this device and the gateway marked, the ICMP round trip, names
 * flowing in by priority, a rescan that keeps the known devices, and cancel through the session.
 */
#include "check.hpp"

#include "devices.hpp"
#include "netaddr.hpp"
#include "subnet.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#ifndef CDK_APP_DIR
#define CDK_APP_DIR "."
#endif

using namespace nt;

namespace {

Address a_of(const char *text)
{
    Address a;
    parse_ip(text, a);
    return a;
}

/* records the addresses poked; hands out a scripted round-trip per address */
class FakeProber : public Prober {
public:
    std::mutex mu;
    std::vector<std::uint32_t> poked;
    std::map<std::uint32_t, double> rtt;
    void probe(std::uint32_t host) override /* reached only through Prober::poke's range guard */
    {
        std::lock_guard<std::mutex> l(mu);
        poked.push_back(host);
    }
    double rtt_ms(std::uint32_t host) const override
    {
        const auto it = rtt.find(host);
        return it == rtt.end() ? -1.0 : it->second;
    }
};

/* returns a fixed /proc/net/arp text (threadsafe) */
class FakeArp : public NeighbourReader {
public:
    explicit FakeArp(std::string text = "") : text_(std::move(text)) {}
    void set(const std::string &t)
    {
        std::lock_guard<std::mutex> l(mu_);
        text_ = t;
    }
    std::string read() override
    {
        std::lock_guard<std::mutex> l(mu_);
        return text_;
    }

private:
    std::mutex mu_;
    std::string text_;
};

/* a scripted name lookup: hands out a name for an address after it has been serviced twice */
class FakeNames : public NameLookup {
public:
    std::map<std::uint32_t, std::string> names;
    std::vector<std::uint32_t> asked;
    int services = 0;
    void ask(const Address &a) override { asked.push_back(ipv4_to_host_order(a)); } /* behind NameLookup's guard */
    void service() override { ++services; }
    std::string name(const Address &a, NameSource *source) const override
    {
        if (services < 1) {
            if (source) *source = NameSource::None;
            return std::string();
        }
        const auto it = names.find(ipv4_to_host_order(a));
        if (it == names.end()) {
            if (source) *source = NameSource::None;
            return std::string();
        }
        if (source) *source = NameSource::Mdns;
        return it->second;
    }
    bool busy() const override { return services < 2; }
};

const Device *dev(const DevicesSnapshot &s, const char *ip)
{
    for (const Device &d : s.devices)
        if (d.ip == ip) return &d;
    return nullptr;
}

DevicesConfig small_config()
{
    DevicesConfig cfg;
    cfg.range = scan_range_from_cidr("192.168.68.10/29", "wlan0"); /* .9 .. .14, self .10 */
    cfg.self_mac = "b8:27:eb:aa:bb:cc";
    cfg.gateway = "192.168.68.9";
    cfg.wave = 2;
    cfg.settle_ms = 20;
    cfg.total_budget_ms = 3000;
    cfg.rounds = 3;
    return cfg;
}

const char *kArpText =
    "IP address       HW type     Flags       HW address            Mask     Device\n"
    "192.168.68.9     0x1         0x2         aa:bb:cc:00:00:09     *        wlan0\n"  /* gateway */
    "192.168.68.11    0x1         0x0         00:00:00:00:00:00     *        wlan0\n"  /* incomplete */
    "192.168.68.12    0x1         0x2         11:22:33:44:55:12     *        wlan0\n"
    "192.168.68.13    0x1         0x2         a6:11:22:33:44:13     *        wlan0\n"  /* locally administered */
    "192.168.68.14    0x1         0x2         de:ad:be:ef:00:14     *        eth0\n";  /* other interface */

} // namespace

void test_devices_engine()
{
    /* parse the shipped OUI table (oui_table() caches it): exercises the real data under ASan */
    setenv("NET_TOOLBOX_OUI",
           (std::string(CDK_APP_DIR) + "/root/usr/share/APPLaunch/share/net-toolbox-oui.tsv").c_str(), 1);

    CHECK(scan_range_from_cidr("192.168.68.10/29", "wlan0").ok);

    DevicesConfig cfg = small_config();
    FakeProber prober;
    prober.rtt[ipv4_to_host_order(a_of("192.168.68.12"))] = 5.0;
    FakeArp arp(kArpText);
    FakeNames names;
    names.names[ipv4_to_host_order(a_of("192.168.68.12"))] = "mylaptop";

    std::vector<DevicesSnapshot> pub;
    const DevicesSnapshot s =
        run_devices(cfg, prober, arp, &names, CancelToken(), [&](const DevicesSnapshot &x) { pub.push_back(x); });

    CHECK(s.state == DevicesSnapshot::State::Done);
    CHECK_EQ(s.interface, std::string("wlan0"));
    CHECK_EQ(s.subnet, std::string("192.168.68.8/29"));
    CHECK_EQ(s.gateway, std::string("192.168.68.9"));
    CHECK_EQ(s.total, 6);
    CHECK(s.probed == 6);
    CHECK(!pub.empty() && pub.front().state == DevicesSnapshot::State::Scanning);

    // the devices found: self .10, gateway .9, .12, .13 (incomplete .11 and other-iface .14 dropped)
    CHECK_EQ(s.devices.size(), static_cast<std::size_t>(4));
    CHECK(s.found == 4);
    // sorted by IP numeric
    CHECK(std::is_sorted(s.devices.begin(), s.devices.end(),
                         [](const Device &a, const Device &b) { return a.ip_order <= b.ip_order; }));
    if (s.devices.size() == 4) {
        CHECK_EQ(s.devices[0].ip, std::string("192.168.68.9"));
        CHECK_EQ(s.devices[1].ip, std::string("192.168.68.10"));
    }
    const Device *self = dev(s, "192.168.68.10");
    CHECK(self && self->is_self && self->mac == "b8:27:eb:aa:bb:cc" && !self->is_gateway);
    const Device *gw = dev(s, "192.168.68.9");
    CHECK(gw && gw->is_gateway && gw->mac == "aa:bb:cc:00:00:09");
    const Device *d12 = dev(s, "192.168.68.12");
    CHECK(d12 && d12->mac == "11:22:33:44:55:12");
    CHECK(d12 && d12->has_rtt && d12->rtt_ms == 5.0);          /* the ICMP round trip */
    CHECK(d12 && d12->name == "mylaptop" && d12->name_source == NameSource::Mdns);
    const Device *d13 = dev(s, "192.168.68.13");
    CHECK(d13 && d13->vendor_private && d13->vendor.empty()); /* a6 = locally administered: private, no vendor */
    CHECK(dev(s, "192.168.68.11") == nullptr);
    CHECK(dev(s, "192.168.68.14") == nullptr);
    CHECK(!s.naming); /* names settled before Done */

    // the prober was asked for every address but this device
    CHECK(std::find(prober.poked.begin(), prober.poked.end(), cfg.range.self) == prober.poked.end());
    CHECK(prober.poked.size() == 5);
    CHECK(prober.refused() == 0);
    // every probe and every name question stayed inside the range, never this device
    for (const std::uint32_t h : prober.poked) CHECK(cfg.range.contains(h) && h != cfg.range.self);
    CHECK(!names.asked.empty());
    for (const std::uint32_t h : names.asked) CHECK(cfg.range.contains(h) && h != cfg.range.self);
    CHECK(s.range.ok && s.range.first == cfg.range.first && s.range.last == cfg.range.last);
}

void test_devices_guards()
{
    const ScanRange range = scan_range_from_cidr("192.168.68.10/29", "wlan0"); /* .9 .. .14, self .10 */
    const std::uint32_t in = ipv4_to_host_order(a_of("192.168.68.12"));
    const std::uint32_t self = ipv4_to_host_order(a_of("192.168.68.10"));
    const std::uint32_t other_subnet = ipv4_to_host_order(a_of("192.168.69.12"));
    const std::uint32_t network = ipv4_to_host_order(a_of("192.168.68.8"));
    const std::uint32_t broadcast = ipv4_to_host_order(a_of("192.168.68.15"));
    const std::uint32_t pub = ipv4_to_host_order(a_of("8.8.8.8"));

    // the Prober base refuses everything outside the range (fail closed before begin)
    {
        FakeProber p;
        CHECK(!p.poke(in)); /* no range yet */
        p.begin(range);
        CHECK(!p.poke(self));
        CHECK(!p.poke(other_subnet));
        CHECK(!p.poke(network));
        CHECK(!p.poke(broadcast));
        CHECK(!p.poke(pub));
        CHECK(p.poke(in));
        CHECK(p.poked.size() == 1 && p.poked[0] == in);
        CHECK_EQ(p.refused(), 6);
    }
    // the real prober inherits it (only refused addresses are tried here: nothing is sent)
    {
        std::unique_ptr<Prober> real = make_icmp_tcp_prober();
        CHECK(!real->poke(pub)); /* before begin */
        real->begin(range);
        CHECK(!real->poke(pub));
        CHECK(!real->poke(other_subnet));
        CHECK(!real->poke(self));
        CHECK_EQ(real->refused(), 4);
    }
    // the NameLookup base: only IPv4 hosts of the range (no multicast, no public, not this device)
    {
        FakeNames n;
        CHECK(!n.want(a_of("192.168.68.12"))); /* no range yet */
        n.begin(range);
        CHECK(!n.want(a_of("8.8.8.8")));
        CHECK(!n.want(a_of("224.0.0.251")));
        CHECK(!n.want(a_of("192.168.68.10")));
        CHECK(!n.want(a_of("192.168.69.12")));
        CHECK(!n.want(a_of("::1")));
        CHECK(n.want(a_of("192.168.68.12")));
        CHECK(n.asked.size() == 1 && n.asked[0] == in);
    }
    {
        std::unique_ptr<NameLookup> real = make_name_lookup("");
        CHECK(!real->want(a_of("8.8.8.8")));
        real->begin(range);
        CHECK(!real->want(a_of("8.8.8.8")));
        CHECK(!real->want(a_of("224.0.0.251")));
        CHECK(!real->want(a_of("192.168.69.12")));
    }
    // LocalNames on its own: nothing is queued without a range, nor outside it
    {
        LocalNames ln;
        ln.want(a_of("192.168.68.12")); /* no range: ignored */
        CHECK(ln.size() == 0);
        ln.set_range(range);
        ln.want(a_of("8.8.8.8"));
        ln.want(a_of("224.0.0.251"));
        ln.want(a_of("192.168.68.10")); /* this device */
        CHECK(ln.size() == 0);
    }
}

void test_devices_rescan()
{
    DevicesConfig cfg = small_config();
    FakeProber prober;
    FakeArp arp(kArpText);
    const DevicesSnapshot first = run_devices(cfg, prober, arp, nullptr, CancelToken(), nullptr);
    CHECK(first.devices.size() == 4);

    // a rescan where the ARP table is now empty must keep the already-known devices
    DevicesConfig re = small_config();
    re.keep = first.devices;
    FakeProber prober2;
    FakeArp empty("");
    const DevicesSnapshot second = run_devices(re, prober2, empty, nullptr, CancelToken(), nullptr);
    CHECK(second.state == DevicesSnapshot::State::Done);
    CHECK(second.devices.size() == 4); /* kept from the previous scan */
    CHECK(dev(second, "192.168.68.12") != nullptr);
    const Device *self = dev(second, "192.168.68.10");
    CHECK(self && self->is_self);
}

void test_devices_failed()
{
    // a public address refuses with words
    DevicesConfig cfg;
    cfg.range = scan_range_from_cidr("8.8.8.8/24", "wlan0");
    FakeProber prober;
    FakeArp arp("");
    const DevicesSnapshot s = run_devices(cfg, prober, arp, nullptr, CancelToken(), nullptr);
    CHECK(s.state == DevicesSnapshot::State::Failed && !s.error.empty());
}

void test_devices_session_cancel()
{
    auto prober = std::make_shared<FakeProber>();
    auto arp = std::make_shared<FakeArp>("");
    DevicesSession session(prober, arp, nullptr);
    NetState net;
    Iface i;
    i.name = "wlan0";
    i.mac = "b8:27:eb:aa:bb:cc";
    i.ipv4 = {"192.168.68.10/24"};
    net.ifaces.push_back(i);

    CHECK(session.start(net, "wlan0", ""));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const std::int64_t t0 = now_ms();
    session.stop();
    CHECK(session.wait(2000));
    CHECK(now_ms() - t0 < 600); /* ends within a slice of the cancel */
    auto snap = session.latest();
    CHECK(snap && (snap->state == DevicesSnapshot::State::Cancelled || snap->state == DevicesSnapshot::State::Done));

    // a failed start: an interface with no IPv4 address
    NetState bare;
    Iface j;
    j.name = "wlan1";
    bare.ifaces.push_back(j);
    CHECK(!session.start(bare, "wlan1", ""));
    CHECK(session.latest()->state == DevicesSnapshot::State::Failed);
}
