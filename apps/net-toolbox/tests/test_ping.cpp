/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: the ping statistics (loss, min / avg / max, jitter, the slow flag), the ring, the engine on a
 * scripted fake socket (replies, losses, a send error, out-of-order and late replies, cancel), the availability words,
 * and a real ping of 127.0.0.1 (and ::1) when this host allows ICMP datagram sockets.
 */
#include "check.hpp"

#include "ping.hpp"

#include <cerrno>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace nt;

namespace {

/* What the fake network does with each sequence number: a reply delay in ms, -1 = no reply, -2 = the send fails. */
struct Script {
    std::map<std::uint16_t, int> delay;
    int default_delay = 5;
    int opened = 0;
};

class FakeEcho : public EchoSocket {
public:
    explicit FakeEcho(std::shared_ptr<Script> s) : s_(std::move(s)) {}
    int send(std::uint16_t seq) override
    {
        const auto it = s_->delay.find(seq);
        const int d = it == s_->delay.end() ? s_->default_delay : it->second;
        if (d == -2) return ENETUNREACH;
        if (d >= 0) due_.push_back({now_us() + static_cast<std::int64_t>(d) * 1000, seq});
        return 0;
    }
    int receive(int timeout_ms, std::uint16_t &seq) override
    {
        const std::int64_t deadline = now_us() + static_cast<std::int64_t>(timeout_ms) * 1000;
        for (;;) {
            const std::int64_t now = now_us();
            std::size_t best = due_.size();
            for (std::size_t i = 0; i < due_.size(); ++i)
                if (due_[i].first <= now && (best == due_.size() || due_[i].first < due_[best].first)) best = i;
            if (best < due_.size()) {
                seq = due_[best].second;
                due_.erase(due_.begin() + static_cast<std::ptrdiff_t>(best));
                return 1;
            }
            if (now >= deadline) return 0;
            std::int64_t next = deadline;
            for (const auto &d : due_) next = std::min(next, d.first);
            std::this_thread::sleep_for(std::chrono::microseconds(std::max<std::int64_t>(100, next - now)));
        }
    }

private:
    std::shared_ptr<Script> s_;
    std::vector<std::pair<std::int64_t, std::uint16_t>> due_;
};

EchoFactory factory_of(const std::shared_ptr<Script> &script)
{
    return [script](const Address &, int, int *err) -> std::unique_ptr<EchoSocket> {
        ++script->opened;
        *err = 0;
        return std::make_unique<FakeEcho>(script);
    };
}

PingConfig config(const char *target, int count, int interval, int timeout)
{
    PingConfig c;
    c.target = target;
    c.count = count;
    c.interval_ms = interval;
    c.timeout_ms = timeout;
    c.resolve.hosts_path = "";
    return c;
}

std::vector<PacketState> states(const PingSnapshot &s)
{
    std::vector<PacketState> out;
    for (const PacketResult &p : s.recent) out.push_back(p.state);
    return out;
}

} // namespace

void test_ping_stats()
{
    PingStats s;
    CHECK(s.loss_pct() == 0 && s.avg_ms() == 0 && s.jitter_ms() == 0 && s.slow_threshold_ms() == 100);
    for (int i = 0; i < 4; ++i) s.add_sent();
    CHECK(s.add_reply(10) == PacketState::Ok);
    CHECK(s.add_reply(12) == PacketState::Ok);
    s.add_lost();
    CHECK(s.add_reply(11) == PacketState::Ok);
    CHECK(s.transmitted == 4 && s.received == 3 && s.lost == 1 && s.pending() == 0);
    CHECK_NEAR(s.loss_pct(), 25.0, 1e-9);
    CHECK_NEAR(s.avg_ms(), 11.0, 1e-9);
    CHECK(s.min_ms == 10 && s.max_ms == 12);
    CHECK_NEAR(s.jitter_ms(), 1.5, 1e-9); /* |12-10| and |11-12|: the lost packet does not reset it */
    s.add_sent();
    CHECK(s.pending() == 1);
    CHECK_NEAR(s.loss_pct(), 25.0, 1e-9); /* a packet still waiting is not lost */

    // slow: above max(100 ms, 3 x the average of the replies before it)
    PingStats f;
    CHECK(f.add_reply(150) == PacketState::Slow); /* no average yet: the 100 ms floor */
    PingStats g;
    for (int i = 0; i < 5; ++i) g.add_reply(20);
    CHECK_NEAR(g.slow_threshold_ms(), 100.0, 1e-9);
    CHECK(g.add_reply(99) == PacketState::Ok);
    PingStats h;
    for (int i = 0; i < 5; ++i) h.add_reply(50);
    CHECK_NEAR(h.slow_threshold_ms(), 150.0, 1e-9);
    CHECK(h.add_reply(149) == PacketState::Ok);
    PingStats k;
    for (int i = 0; i < 5; ++i) k.add_reply(50);
    CHECK(k.add_reply(151) == PacketState::Slow);
    CHECK(k.add_reply(-3) == PacketState::Ok && k.min_ms == 0); /* a clock oddity counts as 0 */
    PingStats j;
    j.add_reply(10);
    j.add_reply(30);
    j.add_reply(10);
    CHECK_NEAR(j.jitter_ms(), 20.0, 1e-9);

    // the ring keeps the last 120, oldest first
    PacketRing ring;
    for (std::uint32_t n = 1; n <= 130; ++n) ring.push({n, PacketState::Ok, static_cast<double>(n)});
    const std::vector<PacketResult> items = ring.items();
    CHECK(items.size() == kRingSize && ring.size() == kRingSize);
    CHECK(items.front().number == 11 && items.back().number == 130);
    CHECK(ring.find(5) == nullptr && ring.find(125) && ring.find(125)->rtt_ms == 125);
    ring.find(125)->state = PacketState::Lost;
    CHECK(ring.items()[114].state == PacketState::Lost);
    ring.clear();
    CHECK(ring.items().empty());

    // availability words
    CHECK(ping_group_allows("0\t2147483647\n", 1000));
    CHECK(ping_group_allows(fixture("deck", "proc/sys/net/ipv4/ping_group_range"), 1000));
    CHECK(!ping_group_allows("1\t0", 1000) && !ping_group_allows("1 0", 0));
    CHECK(ping_group_allows("1000 1000", 1000) && !ping_group_allows("garbage", 0));
    const std::string w = ping_unavailable_words(EACCES, "1\t0\n");
    CHECK(w.find("net.ipv4.ping_group_range is \"1 0\"") != std::string::npos);
    CHECK(w.find("sudo sysctl -w net.ipv4.ping_group_range=\"0 2147483647\"") != std::string::npos);
    CHECK(ping_unavailable_words(EPROTONOSUPPORT, "").find("no unprivileged ping socket") != std::string::npos);
    CHECK(ping_unavailable_words(EMFILE, "").find("too many open files") != std::string::npos);
}

void test_ping_engine()
{
    CancelToken none;
    // five packets: ok, lost, slow, ok, a send error
    {
        auto script = std::make_shared<Script>();
        script->delay = {{1, 5}, {2, -1}, {3, 150}, {4, 5}, {5, -2}};
        int published = 0;
        const std::int64_t t0 = now_ms();
        const PingSnapshot s =
            run_ping(config("192.0.2.7", 5, 200, 300), factory_of(script), none, [&](const PingSnapshot &) { ++published; });
        const std::int64_t took = now_ms() - t0;
        CHECK(s.state == PingSnapshot::State::Done && s.address == "192.0.2.7" && s.resolved_by.empty());
        CHECK(s.stats.transmitted == 5 && s.stats.received == 3 && s.stats.lost == 2 && s.stats.pending() == 0);
        CHECK_NEAR(s.stats.loss_pct(), 40.0, 1e-9);
        const std::vector<PacketState> want = {PacketState::Ok, PacketState::Lost, PacketState::Slow, PacketState::Ok,
                                               PacketState::Lost};
        CHECK(states(s) == want);
        CHECK(s.recent[2].rtt_ms >= 149 && s.recent[2].rtt_ms < 230);
        CHECK(s.stats.min_ms >= 4.5 && s.stats.min_ms < 40);
        CHECK_EQ(s.error, std::string("Send failed: network unreachable"));
        CHECK(published >= 10);
        CHECK(took >= 790 && took < 1600);
        CHECK(script->opened == 1);
    }
    // out of order: the first reply comes after the second
    {
        auto script = std::make_shared<Script>();
        script->delay = {{1, 260}, {2, 10}};
        const PingSnapshot s = run_ping(config("192.0.2.7", 2, 200, 1000), factory_of(script), none, nullptr);
        CHECK(s.state == PingSnapshot::State::Done && s.stats.received == 2 && s.stats.lost == 0);
        CHECK(s.recent.size() == 2 && s.recent[0].number == 1 && s.recent[0].rtt_ms >= 255);
        CHECK(s.recent[1].rtt_ms < 100);
    }
    // a reply after its timeout is not counted
    {
        auto script = std::make_shared<Script>();
        script->delay = {{1, 400}};
        const PingSnapshot s = run_ping(config("192.0.2.7", 1, 200, 300), factory_of(script), none, nullptr);
        CHECK(s.state == PingSnapshot::State::Done && s.stats.received == 0 && s.stats.lost == 1);
        CHECK(states(s) == std::vector<PacketState>{PacketState::Lost});
    }
    // the values are clamped (interval 1 ms -> 200 ms)
    {
        auto script = std::make_shared<Script>();
        const std::int64_t t0 = now_ms();
        const PingSnapshot s = run_ping(config("192.0.2.7", 3, 1, 50), factory_of(script), none, nullptr);
        CHECK(s.stats.received == 3 && now_ms() - t0 >= 390);
    }
    // no name server: Failed with the reason; the socket refused: Failed with the fix
    {
        auto script = std::make_shared<Script>();
        PingSnapshot s = run_ping(config("nope.invalid", 3, 200, 300), factory_of(script), none, nullptr);
        CHECK(s.state == PingSnapshot::State::Failed && s.error.find("Cannot find nope.invalid: No DNS server") == 0);
        CHECK(script->opened == 0);
        const EchoFactory refuse = [](const Address &, int, int *err) -> std::unique_ptr<EchoSocket> {
            *err = EACCES;
            return nullptr;
        };
        s = run_ping(config("192.0.2.7", 3, 200, 300), refuse, none, nullptr);
        CHECK(s.state == PingSnapshot::State::Failed && s.error.find("Ping is not allowed") == 0);
    }
    // the session: continuous until stopped; a stop ends it within a slice; a bad target fails at once
    {
        auto script = std::make_shared<Script>();
        PingSession session(factory_of(script));
        CHECK(session.start(config("192.0.2.7", 0, 200, 1000)));
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        auto snap = session.latest();
        CHECK(snap->state == PingSnapshot::State::Running && snap->stats.transmitted >= 3);
        const std::uint64_t seq = session.seq();
        const std::int64_t t0 = now_ms();
        session.stop();
        CHECK(session.wait(1000));
        CHECK(now_ms() - t0 < 300);
        snap = session.latest();
        CHECK(snap->state == PingSnapshot::State::Cancelled && session.seq() > seq);
        CHECK(snap->stats.received >= 3);
        CHECK(!session.start(config("bad host", 0, 200, 1000)));
        CHECK(session.latest()->state == PingSnapshot::State::Failed);
        // a restart while running: the new run's snapshots win
        CHECK(session.start(config("192.0.2.7", 0, 200, 1000)));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(session.start(config("192.0.2.8", 2, 200, 300)));
        CHECK(session.wait(2000));
        snap = session.latest();
        CHECK(snap->target == "192.0.2.8" && snap->state == PingSnapshot::State::Done && snap->stats.transmitted == 2);
    }
}

void test_ping_real()
{
    const PingAvailability a = ping_available();
    if (!a.ipv4) {
        const std::string why = "ICMP datagram sockets are not allowed on this host, the real ping is not tested: " + a.why;
        SKIP(why.c_str());
        CHECK(a.error == EACCES || a.error == EPERM || a.error == EPROTONOSUPPORT || a.error == EAFNOSUPPORT);
        CHECK(!a.why.empty());
        return;
    }
    PingSession s;
    PingConfig c = config("127.0.0.1", 3, 200, 1000);
    CHECK(s.start(c));
    CHECK(s.wait(3000));
    auto snap = s.latest();
    CHECK(snap->state == PingSnapshot::State::Done && snap->stats.received == 3 && snap->stats.lost == 0);
    CHECK(snap->stats.max_ms < 100);
    if (a.ipv6) {
        c.target = "::1";
        CHECK(s.start(c));
        CHECK(s.wait(3000));
        snap = s.latest();
        if (snap->state == PingSnapshot::State::Done && snap->stats.received == 3) CHECK(true);
        else SKIP("::1 did not answer (IPv6 loopback off?)");
    } else {
        SKIP("no ICMPv6 datagram socket on this host");
    }
}
