/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.3): Wake-on-LAN. The MAC parser against the three forms and hostile input, the magic packet's
 * bytes, the directed broadcasts, the packets planned on the recorded boards (tests/fixtures), the saved machines'
 * form, sending through a fake sender (errors in words) and through a real UDP socket on loopback, and the wake watch on
 * a fake echo socket (answers after a few pings, at once, never, cancel, ping not allowed).
 */
#include "check.hpp"

#include "interfaces.hpp"
#include "sock.hpp"
#include "source.hpp"
#include "text.hpp"
#include "wol.hpp"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

using namespace nt;

namespace {

class FakeSender : public DatagramSender {
public:
    std::vector<WolDestination> sent;
    std::vector<std::vector<std::uint8_t>> payloads;
    int fail_from = -1; /* this packet and the later ones fail */
    int error = ENETUNREACH;
    int send(const WolDestination &d, const std::vector<std::uint8_t> &payload) override
    {
        const int n = static_cast<int>(sent.size());
        sent.push_back(d);
        payloads.push_back(payload);
        return fail_from >= 0 && n >= fail_from ? error : 0;
    }
};

/* answers every ping from `from` on (0: never) */
class FakeEcho : public EchoSocket {
public:
    explicit FakeEcho(int from) : from_(from) {}
    int send(std::uint16_t seq) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (from_ > 0 && seq >= from_) waiting_.push_back(seq);
        return 0;
    }
    int receive(int timeout_ms, std::uint16_t &seq) override
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!waiting_.empty()) {
                seq = waiting_.front();
                waiting_.erase(waiting_.begin());
                return 1;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
        return 0;
    }

private:
    int from_;
    std::mutex mu_;
    std::vector<std::uint16_t> waiting_;
};

EchoFactory echo_from(int from, std::shared_ptr<std::atomic<int>> opened = nullptr)
{
    return [from, opened](const Address &, int, int *err) -> std::unique_ptr<EchoSocket> {
        if (opened) ++*opened;
        if (err) *err = 0;
        return std::make_unique<FakeEcho>(from);
    };
}

NetState board(const char *name)
{
    return read_net_state(*make_fixture_source(fixture_dir(name)));
}

WolRequest request(const std::string &host = "")
{
    WolRequest r;
    r.machine.name = "Desk PC";
    r.machine.mac = "AA-BB-CC-DD-EE-FF";
    r.machine.host = host;
    r.net = board("deck");
    r.watch_interval_ms = 20;
    r.watch_max_ms = 5000;
    return r;
}

void test_mac()
{
    struct Good {
        const char *in, *out;
    } good[] = {{"aa:bb:cc:dd:ee:ff", "aa:bb:cc:dd:ee:ff"}, {"AA-BB-CC-DD-EE-FF", "aa:bb:cc:dd:ee:ff"},
                {"aabbccddeeff", "aa:bb:cc:dd:ee:ff"},      {"  00:11:22:33:44:55\t", "00:11:22:33:44:55"},
                {"00:1A:2b:3C:4d:5E", "00:1a:2b:3c:4d:5e"},  {"b827eb123456", "b8:27:eb:12:34:56"}};
    for (const Good &g : good) {
        MacAddr m;
        std::string why;
        CHECK(parse_mac(g.in, m, &why));
        CHECK_EQ(m.text(), std::string(g.out));
    }
    const std::string bad[] = {"",
                               "aa:bb:cc:dd:ee",
                               "aa:bb:cc:dd:ee:ff:00",
                               "aa:bb-cc:dd:ee:ff",
                               "aa.bb.cc.dd.ee.ff",
                               "aabb.ccdd.eeff",
                               "gg:bb:cc:dd:ee:ff",
                               "aa:bb:cc:dd:ee:f",
                               "aa bb cc dd ee ff",
                               "00:00:00:00:00:00",
                               "ff:ff:ff:ff:ff:ff",
                               "FF-FF-FF-FF-FF-FF",
                               "01:00:5e:00:00:01",
                               "33:33:00:00:00:01",
                               std::string("aa:bb:cc:dd:ee:ff\0", 18),
                               "aa:bb:cc:dd:ee:ff; rm -rf /",
                               "0xaabbccddeeff",
                               "aabbccddeeffaa",
                               ":aa:bb:cc:dd:ee:f",
                               "aa::bb:cc:dd:ee:f",
                               "\xef\xbd\x81\xef\xbd\x81:bb:cc:dd:ee",
                               "zzzzzzzzzzzz",
                               "-a:bb:cc:dd:ee:ff",
                               std::string(4096, 'a')};
    for (const std::string &b : bad) {
        MacAddr m;
        m.b[0] = 0x42;
        std::string why;
        CHECK(!parse_mac(b, m, &why));
        CHECK(!why.empty());
        CHECK(m.b[0] == 0x42); /* untouched */
    }
    std::string why;
    MacAddr m;
    parse_mac("01:00:5e:00:00:01", m, &why);
    CHECK(why.find("multicast") != std::string::npos);
    parse_mac("ff:ff:ff:ff:ff:ff", m, &why);
    CHECK(why.find("broadcast") != std::string::npos);
    // the packet: 6 x 0xFF, then the address 16 times
    CHECK(parse_mac("00:11:22:33:44:55", m));
    const std::vector<std::uint8_t> p = magic_packet(m);
    CHECK_EQ(p.size(), kMagicPacketSize);
    bool ok = true;
    for (int i = 0; i < 6; ++i)
        if (p[static_cast<std::size_t>(i)] != 0xFF) ok = false;
    for (int r = 0; r < 16; ++r)
        for (int i = 0; i < 6; ++i)
            if (p[6 + static_cast<std::size_t>(r) * 6 + static_cast<std::size_t>(i)] != m.b[i]) ok = false;
    CHECK(ok);
    CHECK(p[6] == 0x00 && p[7] == 0x11 && p[101] == 0x55 && p[96] == 0x00);
}

void test_wol_plan()
{
    struct D {
        const char *cidr;
        bool ok;
        const char *out;
    } dirs[] = {{"192.168.68.121/22", true, "192.168.71.255"}, {"192.168.68.159/24", true, "192.168.68.255"},
                {"10.1.2.3/8", true, "10.255.255.255"},        {"172.16.5.4/12", true, "172.31.255.255"},
                {"192.168.1.10/30", true, "192.168.1.11"},     {"0.0.0.0/0", true, "255.255.255.255"},
                {"10.0.0.1/31", false, ""},                    {"10.0.0.1/32", false, ""},
                {"10.0.0.1", false, ""},                       {"fe80::1/64", false, ""},
                {"bad", false, ""},                            {"1.2.3.4/33", false, ""},
                {"1.2.3.4/-1", false, ""},                     {"1.2.3/24", false, ""}};
    for (const D &d : dirs) {
        std::string out;
        CHECK_EQ(directed_broadcast(d.cidr, out), d.ok);
        if (d.ok) CHECK_EQ(out, std::string(d.out));
    }
    WolMachine m;
    m.name = "PC";
    m.mac = "aa:bb:cc:dd:ee:ff";
    // the deck: wlan0 192.168.68.121/22: 255.255.255.255 and 192.168.71.255, ports 9 and 7, from 192.168.68.121
    const std::vector<WolDestination> deck = plan_wol(m, board("deck"));
    CHECK_EQ(deck.size(), static_cast<std::size_t>(4));
    if (deck.size() == 4) {
        CHECK(deck[0].iface == "wlan0" && deck[0].source == "192.168.68.121" && deck[0].address == "255.255.255.255" &&
              deck[0].port == 9);
        CHECK(deck[1].address == "255.255.255.255" && deck[1].port == 7);
        CHECK(deck[2].address == "192.168.71.255" && deck[2].port == 9 && deck[3].port == 7);
        CHECK_EQ(deck[0].label(), std::string("255.255.255.255 port 9 on wlan0"));
    }
    // the Pi 5 on Ethernet (its Wi-Fi off): eth0 only; an interface that is not there or down: nothing
    const std::vector<WolDestination> pi5 = plan_wol(m, board("pi-5"));
    CHECK(pi5.size() == 4 && pi5[0].iface == "eth0" && pi5[2].address == "192.168.71.255");
    CHECK(plan_wol(m, board("pi-5"), "wlan0").empty());
    CHECK(plan_wol(m, board("deck"), "eth9").empty());
    CHECK(plan_wol(m, board("deck"), "wlan0").size() == 4);
    CHECK(plan_wol(m, NetState()).empty());
    // another port: no port 7; a saved broadcast address: routed by the system, no interface
    m.port = 4000;
    CHECK(plan_wol(m, board("deck")).size() == 2);
    m.port = 9;
    m.broadcast = "10.9.8.255";
    const std::vector<WolDestination> routed = plan_wol(m, board("deck"));
    CHECK(routed.size() == 2 && routed[0].iface.empty() && routed[0].source.empty() && routed[0].address == "10.9.8.255" &&
          routed[1].port == 7);
    CHECK_EQ(routed[0].label(), std::string("10.9.8.255 port 9"));
}

void test_wol_machines()
{
    WolMachine m;
    m.name = "  NAS  ";
    m.mac = "AABBCCDDEEFF";
    m.host = "nas.lan";
    m.broadcast = "192.168.1.255";
    std::string why;
    CHECK(normalize_machine(m, &why));
    CHECK(m.name == "NAS" && m.mac == "aa:bb:cc:dd:ee:ff" && m.host == "nas.lan" && m.port == 9);
    CHECK_EQ(machine_to_setting(m), std::string("NAS|aa:bb:cc:dd:ee:ff|nas.lan|192.168.1.255|9"));
    WolMachine back;
    CHECK(machine_from_setting(machine_to_setting(m), back));
    CHECK(back.name == m.name && back.mac == m.mac && back.host == m.host && back.broadcast == m.broadcast && back.port == 9);
    CHECK(machine_from_setting("Game PC|00:11:22:33:44:55|||7", back) && back.host.empty() && back.port == 7);
    CHECK(machine_from_setting("\xf0\x9f\x96\xa5 PC|00:11:22:33:44:55|||9", back)); /* UTF-8 is fine */
    // refused, with words
    struct Bad {
        const char *name, *mac, *host, *broadcast;
        int port;
    } bad[] = {{"", "aa:bb:cc:dd:ee:ff", "", "", 9},
               {"a|b", "aa:bb:cc:dd:ee:ff", "", "", 9},
               {"tab\there", "aa:bb:cc:dd:ee:ff", "", "", 9},
               {"0123456789012345678901234567890123", "aa:bb:cc:dd:ee:ff", "", "", 9},
               {"PC", "aa:bb:cc:dd:ee", "", "", 9},
               {"PC", "aa:bb:cc:dd:ee:ff", "bad host", "", 9},
               {"PC", "aa:bb:cc:dd:ee:ff", "a..b", "", 9},
               {"PC", "aa:bb:cc:dd:ee:ff", "", "fe80::1", 9},
               {"PC", "aa:bb:cc:dd:ee:ff", "", "my.router", 9},
               {"PC", "aa:bb:cc:dd:ee:ff", "", "", 0},
               {"PC", "aa:bb:cc:dd:ee:ff", "", "", 70000}};
    for (const Bad &b : bad) {
        WolMachine x;
        x.name = b.name;
        x.mac = b.mac;
        x.host = b.host;
        x.broadcast = b.broadcast;
        x.port = b.port;
        why.clear();
        CHECK(!normalize_machine(x, &why) && !why.empty());
    }
    const char *bad_values[] = {"", "PC", "PC|aa:bb:cc:dd:ee:ff", "PC|aa:bb:cc:dd:ee:ff|||9|extra",
                                "PC|aa:bb:cc:dd:ee:ff|||nine", "PC|zz:bb:cc:dd:ee:ff|||9", "|aa:bb:cc:dd:ee:ff|||9",
                                "PC|aa:bb:cc:dd:ee:ff|||99999"};
    for (const char *v : bad_values) CHECK(!machine_from_setting(v, back));
}

void test_wol_send()
{
    MacAddr mac;
    CHECK(parse_mac("aa:bb:cc:dd:ee:ff", mac));
    WolMachine m;
    m.name = "PC";
    m.mac = mac.text();
    const std::vector<WolDestination> plan = plan_wol(m, board("deck"));
    {
        FakeSender s;
        const WolSendResult r = send_magic(mac, plan, s);
        CHECK(r.sent == 4 && r.errors == 0 && s.sent.size() == 4);
        CHECK_EQ(r.summary, std::string("4 packets sent on wlan0."));
        CHECK(s.payloads.size() == 4 && s.payloads[0] == magic_packet(mac));
        CHECK(r.packets[0].words == "sent");
    }
    {
        FakeSender s;
        s.fail_from = 2;
        const WolSendResult r = send_magic(mac, plan, s);
        CHECK(r.sent == 2 && r.errors == 2);
        CHECK_EQ(r.summary, std::string("2 packets sent on wlan0; 2 failed (network unreachable)."));
        CHECK(r.packets[3].error == ENETUNREACH && r.packets[3].words == "network unreachable");
    }
    {
        FakeSender s;
        s.fail_from = 0;
        s.error = EACCES;
        const WolSendResult r = send_magic(mac, plan, s);
        CHECK(r.sent == 0 && r.summary == "Nothing could be sent: not allowed.");
    }
    {
        FakeSender s;
        const WolSendResult r = send_magic(mac, {}, s);
        CHECK(r.sent == 0 && r.summary.find("no interface") != std::string::npos);
    }
    // the real socket, on loopback: the magic packet arrives whole
    {
        Fd rx(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        CHECK(rx.valid() && bind(rx.get(), reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0);
        socklen_t len = sizeof(a);
        getsockname(rx.get(), reinterpret_cast<sockaddr *>(&a), &len);
        WolDestination d;
        d.iface = "lo";
        d.source = "127.0.0.1";
        d.address = "127.0.0.1";
        d.port = ntohs(a.sin_port);
        UdpSender udp;
        CHECK_EQ(udp.send(d, magic_packet(mac)), 0);
        struct pollfd p = {rx.get(), POLLIN, 0};
        CHECK(poll(&p, 1, 2000) == 1);
        std::uint8_t buf[256];
        const ssize_t n = recv(rx.get(), buf, sizeof(buf), 0);
        CHECK_EQ(n, static_cast<ssize_t>(kMagicPacketSize));
        CHECK(n == static_cast<ssize_t>(kMagicPacketSize) && std::memcmp(buf, magic_packet(mac).data(), kMagicPacketSize) == 0);
        WolDestination bad = d;
        bad.address = "not an address";
        CHECK(udp.send(bad, magic_packet(mac)) != 0);
        bad = d;
        bad.source = "192.0.2.77"; /* not ours */
        CHECK_EQ(udp.send(bad, magic_packet(mac)), EADDRNOTAVAIL);
    }
}

void test_wol_watch()
{
    // no host: sent, done, no watch
    {
        FakeSender s;
        const WolSnapshot r = run_wol(request(), s, echo_from(1), CancelToken(), nullptr);
        CHECK(r.state == WolSnapshot::State::Done && !r.watch && r.send.sent == 4 && r.mac == "aa:bb:cc:dd:ee:ff");
    }
    // the machine answers from the 3rd ping: "answers after 1 s" (rounded up from less)
    {
        FakeSender s;
        std::vector<WolSnapshot> seen;
        const WolSnapshot r = run_wol(request("192.0.2.10"), s, echo_from(3), CancelToken(),
                                      [&](const WolSnapshot &x) { seen.push_back(x); });
        CHECK(r.state == WolSnapshot::State::Done && r.watch && r.answered && !r.answered_at_once);
        CHECK(r.attempts == 3 && r.watch_address == "192.0.2.10");
        CHECK_EQ(r.watch_words, std::string("answers after 1 s"));
        CHECK(r.hint.empty());
        bool watching = false;
        for (const WolSnapshot &x : seen)
            if (x.state == WolSnapshot::State::Watching && x.watch_words == "no answer yet (0 s)") watching = true;
        CHECK(watching && seen.front().state == WolSnapshot::State::Sending);
    }
    // it answers the first ping: it was probably awake already
    {
        FakeSender s;
        const WolSnapshot r = run_wol(request("192.0.2.10"), s, echo_from(1), CancelToken(), nullptr);
        CHECK(r.answered && r.answered_at_once && r.attempts == 1);
        CHECK(r.watch_words.find("awake already") != std::string::npos);
    }
    // never: "no answer after 1 s" and the hint
    {
        FakeSender s;
        WolRequest req = request("192.0.2.10");
        req.watch_interval_ms = 100;
        req.watch_max_ms = 1000;
        const std::int64_t t0 = now_ms();
        const WolSnapshot r = run_wol(req, s, echo_from(0), CancelToken(), nullptr);
        CHECK(r.state == WolSnapshot::State::Done && !r.answered && now_ms() - t0 >= 990 && now_ms() - t0 < 3000);
        CHECK_EQ(r.watch_words, std::string("no answer after 1 s"));
        CHECK(r.attempts >= 5 && r.attempts <= 11 && r.hint == kWolHint); /* one a 100 ms, fewer on a busy machine */
    }
    // a cancel ends the watch within a slice
    {
        FakeSender s;
        WolRequest req = request("192.0.2.10");
        req.watch_max_ms = 60000;
        CancelToken cancel;
        WolSnapshot r;
        std::thread t([&] { r = run_wol(req, s, echo_from(0), cancel, nullptr); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::int64_t t0 = now_ms();
        cancel.cancel();
        t.join();
        CHECK(r.state == WolSnapshot::State::Cancelled && now_ms() - t0 < 500);
    }
    // ping not allowed: the packets went, the watch says why it cannot watch
    {
        FakeSender s;
        const EchoFactory denied = [](const Address &, int, int *err) -> std::unique_ptr<EchoSocket> {
            if (err) *err = EACCES;
            return nullptr;
        };
        const WolSnapshot r = run_wol(request("192.0.2.10"), s, denied, CancelToken(), nullptr);
        CHECK(r.state == WolSnapshot::State::Done && r.send.sent == 4);
        CHECK_EQ(r.watch_words, std::string("cannot watch: ping is not allowed for this user"));
    }
    // a host name that cannot be resolved (no resolver given): said, the packets still went
    {
        FakeSender s;
        WolRequest req = request("nas.example");
        req.resolve.hosts_path = "";
        const WolSnapshot r = run_wol(req, s, echo_from(1), CancelToken(), nullptr);
        CHECK(r.state == WolSnapshot::State::Done && r.send.sent == 4 && starts_with(r.watch_words, "cannot watch: cannot find nas.example"));
    }
    // nothing to send on, a send that fails everywhere, a bad machine: Failed, in words
    {
        FakeSender s;
        WolRequest req = request();
        req.net = NetState();
        WolSnapshot r = run_wol(req, s, echo_from(1), CancelToken(), nullptr);
        CHECK(r.state == WolSnapshot::State::Failed && r.error.find("No interface is up") != std::string::npos);
        req = request();
        req.iface = "eth9";
        r = run_wol(req, s, echo_from(1), CancelToken(), nullptr);
        CHECK(r.state == WolSnapshot::State::Failed && r.error == "eth9 is not up or has no IPv4 address.");
        FakeSender failing;
        failing.fail_from = 0;
        r = run_wol(request("192.0.2.10"), failing, echo_from(1), CancelToken(), nullptr);
        CHECK(r.state == WolSnapshot::State::Failed && r.error.find("Nothing could be sent") != std::string::npos && !r.watch);
        req = request();
        req.machine.mac = "01:00:5e:00:00:01";
        r = run_wol(req, s, echo_from(1), CancelToken(), nullptr);
        CHECK(r.state == WolSnapshot::State::Failed && r.error.find("multicast") != std::string::npos);
    }
    // the session: a run to its end, and a stop during the watch
    {
        auto sender = std::make_shared<FakeSender>();
        WolSession s(sender, echo_from(2));
        CHECK(s.start(request("192.0.2.10")));
        CHECK(s.wait(3000));
        CHECK(s.latest()->state == WolSnapshot::State::Done && s.latest()->answered && sender->sent.size() == 4);
        WolSession never(std::make_shared<FakeSender>(), echo_from(0));
        CHECK(never.start(request("192.0.2.10")));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(never.latest()->state == WolSnapshot::State::Watching);
        never.stop();
        CHECK(never.wait(1000));
        CHECK(never.latest()->state == WolSnapshot::State::Cancelled);
    }
}

} // namespace

void test_wol()
{
    test_mac();
    test_wol_plan();
    test_wol_machines();
    test_wol_send();
    test_wol_watch();
}
