/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.4): the Local devices port scan against loopback listeners (open, closed and a filtered port
 * whose connect times out), the progress counters, the service names, the 1-1024 and common lists, and cancel.
 */
#include "check.hpp"
#include "fakes.hpp"

#include "devices.hpp"
#include "netaddr.hpp"
#include "sock.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <thread>
#include <vector>

using namespace nt;

namespace {

Address loopback()
{
    Address a;
    parse_ip("127.0.0.1", a);
    return a;
}

/* a listener whose accept queue is full: a connect to it times out ("filtered") */
class FullBacklog {
public:
    FullBacklog()
    {
        listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (listener_ < 0 || bind(listener_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 ||
            listen(listener_, 0) != 0)
            return;
        socklen_t len = sizeof(a);
        getsockname(listener_, reinterpret_cast<sockaddr *>(&a), &len);
        port_ = ntohs(a.sin_port);
        for (int i = 0; i < 2; ++i) {
            Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
            if (connect(fd.get(), reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0 || errno == EINPROGRESS)
                fillers_.push_back(std::move(fd));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~FullBacklog()
    {
        fillers_.clear();
        if (listener_ >= 0) close(listener_);
    }
    std::uint16_t port() const { return port_; }

private:
    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::vector<Fd> fillers_;
};

bool has_open(const PortScanSnapshot &s, std::uint16_t port)
{
    for (const OpenPort &p : s.open)
        if (p.port == port) return true;
    return false;
}

} // namespace

void test_devscan()
{
    // the lists and the service names
    CHECK(!local_common_ports().empty());
    CHECK_EQ(std::string(local_port_service(443)), std::string("https"));
    CHECK_EQ(std::string(local_port_service(62078)), std::string("iphone-sync"));
    CHECK_EQ(std::string(local_port_service(22)), std::string("ssh"));
    CHECK_EQ(std::string(local_port_service(65000)), std::string(""));

    // a bad address
    {
        PortScanConfig cfg;
        const PortScanSnapshot s = run_port_scan(cfg, CancelToken(), nullptr);
        CHECK(s.state == PortScanSnapshot::State::Failed);
    }

    // only private hosts of the scanned subnet: refused in the engine itself, before any socket (no override here)
    {
        const ScanRange lan = scan_range_from_cidr("192.168.68.121/24", "wlan0");
        CHECK(lan.ok);
        const auto refused = [&](const char *ip, const ScanRange &range) {
            PortScanConfig cfg;
            parse_ip(ip, cfg.address);
            cfg.range = range;
            cfg.ports = {80};
            const PortScanSnapshot s = run_port_scan(cfg, CancelToken(), nullptr);
            return s.state == PortScanSnapshot::State::Failed && s.error == kScanTargetRefused && s.scanned == 0;
        };
        CHECK(refused("8.8.8.8", ScanRange()));          /* public */
        CHECK(refused("93.184.215.14", lan));            /* public, with a subnet */
        CHECK(refused("127.0.0.1", ScanRange()));        /* loopback without the test override */
        CHECK(refused("224.0.0.251", ScanRange()));      /* multicast */
        CHECK(refused("239.255.255.250", lan));          /* multicast (SSDP) */
        CHECK(refused("0.0.0.0", ScanRange()));
        CHECK(refused("255.255.255.255", ScanRange()));  /* limited broadcast */
        CHECK(refused("192.168.68.255", lan));           /* the interface's own broadcast */
        CHECK(refused("192.168.68.0", lan));             /* its network address */
        CHECK(refused("192.168.69.20", lan));            /* private, another subnet */
        CHECK(refused("10.0.0.5", lan));                 /* private, another network */
        // the pure check accepts a host of the subnet, and a private host when no subnet is supplied
        Address ok;
        parse_ip("192.168.68.50", ok);
        CHECK(scan_target_problem(ok, lan).empty());
        CHECK(scan_target_problem(ok, ScanRange()).empty());
        Address ll;
        parse_ip("169.254.10.20", ll);
        CHECK(scan_target_problem(ll, ScanRange()).empty()); /* link-local */
        CHECK_EQ(scan_target_problem(Address(), lan), std::string("No address to scan"));
    }

    // the common / full ranges set a total without an explicit list
    {
        PortScanConfig cfg;
        cfg.address = loopback();
        cfg.allow_loopback = true; /* tests only */
        cfg.full_range = true;
        cfg.concurrency = 128;
        cfg.timeout_ms = 150;
        PortScanSnapshot last;
        const PortScanSnapshot s =
            run_port_scan(cfg, CancelToken(), [&](const PortScanSnapshot &x) { last = x; });
        CHECK(s.total == 1024 && s.scanned == 1024 && s.state == PortScanSnapshot::State::Done);
    }

    // open, closed and filtered on loopback (an explicit list of real ports)
    fake::Tcp server;
    if (!server.ok()) {
        SKIP("cannot listen on 127.0.0.1");
        return;
    }
    const std::uint16_t open_port = server.port();
    const std::uint16_t closed_port = fake::closed_port();
    FullBacklog full;
    bool filtered_ok = false; /* the full-queue port genuinely times out on this kernel */

    {
        PortScanConfig cfg;
        cfg.address = loopback();
        cfg.allow_loopback = true; /* tests only: the listeners live on 127.0.0.1 */
        cfg.timeout_ms = 300;
        cfg.concurrency = 8;
        cfg.ports = {open_port, closed_port};
        if (full.port()) cfg.ports.push_back(full.port());
        std::vector<PortScanSnapshot> pub;
        const std::int64_t t0 = now_ms();
        const PortScanSnapshot s =
            run_port_scan(cfg, CancelToken(), [&](const PortScanSnapshot &x) { pub.push_back(x); });
        const std::int64_t took = now_ms() - t0;
        CHECK(s.state == PortScanSnapshot::State::Done);
        CHECK(s.total == static_cast<int>(cfg.ports.size()) && s.scanned == s.total);
        CHECK(has_open(s, open_port));
        CHECK(!has_open(s, closed_port));
        if (full.port()) {
            if (!has_open(s, full.port())) {
                filtered_ok = true;
                CHECK(took >= 290); /* no answer within the timeout: filtered, waited it out */
            } else {
                SKIP("this kernel accepted a connection past a full queue: no filtered port on loopback");
            }
        }
        CHECK(!pub.empty() && pub.front().state == PortScanSnapshot::State::Scanning);
        // the open port's connect time is recorded
        for (const OpenPort &p : s.open)
            if (p.port == open_port) CHECK(p.connect_ms >= 0 && p.connect_ms < 300);
    }

    // cancel: a scan of a filtered (full-queue) port hangs on every connect; a cancel ends it within a slice
    if (filtered_ok) {
        PortScanConfig cfg;
        cfg.address = loopback();
        cfg.allow_loopback = true;
        cfg.timeout_ms = 5000;
        cfg.concurrency = 2;
        cfg.ports = {full.port(), full.port(), full.port(), full.port()};
        Worker w;
        PortScanSnapshot got;
        w.start([&](const CancelToken &c) { got = run_port_scan(cfg, c, nullptr); });
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const std::int64_t t1 = now_ms();
        w.cancel();
        const bool ended = w.wait(2000);
        CHECK(ended && now_ms() - t1 < 400);
        if (ended) CHECK(got.state == PortScanSnapshot::State::Cancelled);
    } else {
        SKIP("no filtered loopback port: the port-scan cancel timing is not tested here");
    }

    // the session: the subnet is required and the target checked before the thread starts
    {
        const ScanRange lan = scan_range_from_cidr("192.168.68.121/24", "wlan0");
        PortScanSession session;
        Address pub, other, multi;
        parse_ip("8.8.8.8", pub);
        parse_ip("192.168.69.20", other);
        parse_ip("224.0.0.251", multi);
        CHECK(!session.start(pub, false, lan));
        CHECK(session.latest()->state == PortScanSnapshot::State::Failed &&
              session.latest()->error == kScanTargetRefused);
        CHECK(!session.start(other, false, lan));
        CHECK(!session.start(multi, false, lan));
        CHECK(!session.start(loopback(), false, lan)); /* no override: loopback refused */
        CHECK(session.latest()->error == kScanTargetRefused);
        Address host;
        parse_ip("192.168.68.50", host);
        CHECK(!session.start(host, false, ScanRange())); /* no subnet: refused */
        CHECK_EQ(session.latest()->error, std::string("No network"));
        CHECK(!session.start(Address(), false, lan));    /* not an address */
        CHECK(!session.running());

        // accepted: the test override, on a loopback listener (the common list, stopped quickly)
        session.allow_loopback_for_tests(true);
        CHECK(session.start(loopback(), false, lan));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        session.stop();
        CHECK(session.wait(3000));
        auto snap = session.latest();
        CHECK(snap && (snap->state == PortScanSnapshot::State::Cancelled || snap->state == PortScanSnapshot::State::Done));
    }
}
