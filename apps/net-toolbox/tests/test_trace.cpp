/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: traceroute. The engine on a fake hop network injected at the socket layer (per-hop latency, loss,
 * silent hops, a hop answering from two addresses, ICMP rate limiting at one hop, the destination at hop N, a
 * destination that never answers, a router saying "unreachable", the ICMP -> UDP fallback, rounds in parallel, cancel),
 * the statistics, the derived flags, the reverse names through the fake DNS server on loopback, and a real trace of
 * 127.0.0.1 and ::1 (UDP always; ICMP when this host allows ICMP datagram sockets).
 */
#include "check.hpp"
#include "fakes.hpp"

#include "dns.hpp"
#include "rdns.hpp"
#include "traceroute.hpp"

#include <netinet/in.h>

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

struct FakeHop {
    std::vector<std::string> addrs;   /* the addresses it answers from, in turn */
    std::vector<int> latency = {5};   /* ms, in turn */
    int drop_every = 0;               /* drops its n-th, 2n-th ... probe (0: never) */
    bool silent = false;
};

/* routers at hops 1 .. hops.size(); the destination (or the unreachable router) at hops.size() + 1 and beyond */
struct FakeNet {
    std::vector<FakeHop> hops;
    std::string dest = "198.51.100.10";
    std::vector<int> dest_latency = {20};
    int dest_drop_every = 0;
    bool dest_answers = true;
    std::string unreachable_from;     /* set: that router answers "host unreachable" instead of the destination */
    bool icmp_allowed = true;
    int opened = 0;
    std::string method_opened;
    std::mutex mu;
    std::map<int, int> probes;        /* per TTL */
};

Address addr(const std::string &text)
{
    Address a;
    parse_ip(text, a);
    return a;
}

class FakeProbe : public ProbeSocket {
public:
    explicit FakeProbe(std::shared_ptr<FakeNet> net) : net_(std::move(net)) {}
    int send(int ttl, std::uint16_t seq) override
    {
        std::lock_guard<std::mutex> lock(net_->mu);
        const int n = ++net_->probes[ttl];
        ProbeReply r;
        r.seq = seq;
        const int dest_hop = static_cast<int>(net_->hops.size()) + 1;
        int latency = 0;
        if (ttl < dest_hop) {
            const FakeHop &h = net_->hops[static_cast<std::size_t>(ttl - 1)];
            if (h.silent || (h.drop_every && n % h.drop_every == 0)) return 0;
            r.kind = ProbeReply::Kind::TimeExceeded;
            r.from = addr(h.addrs[static_cast<std::size_t>(n - 1) % h.addrs.size()]);
            r.icmp_type = 11;
            latency = h.latency[static_cast<std::size_t>(n - 1) % h.latency.size()];
        } else if (!net_->unreachable_from.empty()) {
            r.kind = ProbeReply::Kind::Unreachable;
            r.from = addr(net_->unreachable_from);
            r.icmp_type = 3;
            r.icmp_code = 1;
            latency = 8;
        } else {
            if (!net_->dest_answers) return 0;
            if (ttl == dest_hop && net_->dest_drop_every && n % net_->dest_drop_every == 0) return 0;
            r.kind = ProbeReply::Kind::Reached;
            r.from = addr(net_->dest);
            latency = net_->dest_latency[static_cast<std::size_t>(n - 1) % net_->dest_latency.size()];
        }
        due_.push_back({now_us() + static_cast<std::int64_t>(latency) * 1000, r});
        return 0;
    }
    int receive(int timeout_ms, ProbeReply &out) override
    {
        const std::int64_t deadline = now_us() + static_cast<std::int64_t>(timeout_ms) * 1000;
        for (;;) {
            const std::int64_t now = now_us();
            std::size_t best = due_.size();
            for (std::size_t i = 0; i < due_.size(); ++i)
                if (due_[i].first <= now && (best == due_.size() || due_[i].first < due_[best].first)) best = i;
            if (best < due_.size()) {
                out = due_[best].second;
                out.at_us = now;
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
    std::shared_ptr<FakeNet> net_;
    std::vector<std::pair<std::int64_t, ProbeReply>> due_;
};

ProbeFactory factory_of(const std::shared_ptr<FakeNet> &net)
{
    return [net](const Address &, ProbeMethod m, int *err) -> std::unique_ptr<ProbeSocket> {
        if (m == ProbeMethod::Icmp && !net->icmp_allowed) {
            *err = EACCES;
            return nullptr;
        }
        ++net->opened;
        net->method_opened = m == ProbeMethod::Icmp ? "icmp" : "udp";
        *err = 0;
        return std::make_unique<FakeProbe>(net);
    };
}

FakeHop hop(const char *a, int latency, int drop_every = 0)
{
    FakeHop h;
    h.addrs = {a};
    h.latency = {latency};
    h.drop_every = drop_every;
    return h;
}

TraceConfig config(int rounds, int max_hops = 30, int timeout_ms = 300, int interval_ms = 100)
{
    TraceConfig c;
    c.target = "198.51.100.10";
    c.rounds = rounds;
    c.max_hops = max_hops;
    c.timeout_ms = timeout_ms;
    c.interval_ms = interval_ms;
    c.reverse_dns = false;
    c.resolve.hosts_path = "";
    return c;
}

bool stats_sane(const TraceSnapshot &s)
{
    for (const TraceHop &h : s.hops)
        if (h.stats.received + h.stats.lost > h.stats.transmitted || h.stats.received < 0 || h.stats.lost < 0) return false;
    return true;
}

} // namespace

void test_trace_engine()
{
    CancelToken none;
    // the destination at hop 4; nothing beyond it is counted
    {
        auto net = std::make_shared<FakeNet>();
        net->hops = {hop("192.0.2.1", 2), hop("192.0.2.2", 8), hop("192.0.2.3", 14)};
        int published = 0;
        const TraceSnapshot s = run_trace(config(3), factory_of(net), none, [&](const TraceSnapshot &) { ++published; });
        CHECK(s.state == TraceSnapshot::State::Done && s.reached && !s.unreachable && s.dest_hop == 4);
        CHECK(s.method == "ICMP" && s.method_note.empty() && s.address == "198.51.100.10" && s.rounds == 3);
        CHECK(s.hops.size() == 4 && s.shown == 4 && stats_sane(s) && published >= 3);
        for (const TraceHop &h : s.hops) {
            CHECK(h.stats.transmitted == 3 && h.stats.received == 3 && h.stats.lost == 0 && !h.silent);
            CHECK(!h.loss_only_here && !h.multiple_addresses && h.addresses.size() == 1);
        }
        if (s.hops.size() == 4) {
            CHECK(s.hops[0].address() == "192.0.2.1" && s.hops[2].address() == "192.0.2.3");
            CHECK(s.hops[3].destination && s.hops[3].address() == "198.51.100.10" && !s.hops[2].destination);
            CHECK(s.hops[0].stats.avg_ms() >= 1.5 && s.hops[0].stats.avg_ms() < 8);
            CHECK(s.hops[3].stats.avg_ms() >= 19.5 && s.hops[3].stats.avg_ms() < 28);
            CHECK(s.hops[0].ttl == 1 && s.hops[3].ttl == 4);
        }
        CHECK(net->opened == 1 && net->method_opened == "icmp");
    }
    // the statistics of one hop over the rounds: 10, 20, 30, 20 ms
    {
        auto net = std::make_shared<FakeNet>();
        FakeHop h = hop("192.0.2.1", 0);
        h.latency = {10, 20, 30, 20};
        net->hops = {h};
        const TraceSnapshot s = run_trace(config(4), factory_of(net), none, nullptr);
        CHECK(s.hops.size() == 2);
        if (!s.hops.empty()) {
            const PingStats &st = s.hops[0].stats;
            CHECK(st.transmitted == 4 && st.received == 4 && st.loss_pct() == 0);
            CHECK(st.min_ms >= 9.5 && st.min_ms < 14);   /* best */
            CHECK(st.max_ms >= 29.5 && st.max_ms < 34);  /* worst */
            CHECK(st.avg_ms() >= 19.5 && st.avg_ms() < 24);
            CHECK(st.last_ms >= 19.5 && st.last_ms < 24);
            CHECK(st.jitter_ms() >= 7 && st.jitter_ms() < 13); /* mean of |20-10|, |30-20|, |20-30| */
        }
    }
    // ICMP rate limiting at hop 2 (every second probe unanswered), the path after it clean
    {
        auto net = std::make_shared<FakeNet>();
        net->hops = {hop("192.0.2.1", 2), hop("192.0.2.2", 4, 2), hop("192.0.2.3", 6)};
        const TraceSnapshot s = run_trace(config(6), factory_of(net), none, nullptr);
        CHECK(s.reached && s.hops.size() == 4 && stats_sane(s));
        if (s.hops.size() == 4) {
            CHECK(s.hops[1].stats.lost == 3 && s.hops[1].stats.received == 3);
            CHECK_NEAR(s.hops[1].stats.loss_pct(), 50.0, 1e-9);
            CHECK(s.hops[1].loss_only_here && !s.hops[0].loss_only_here && !s.hops[2].loss_only_here);
            CHECK(s.hops[2].stats.lost == 0 && s.hops[3].stats.lost == 0);
        }
    }
    // real loss: it goes on after hop 2, so hop 2 is not "only here"
    {
        auto net = std::make_shared<FakeNet>();
        net->hops = {hop("192.0.2.1", 2), hop("192.0.2.2", 4, 2), hop("192.0.2.3", 6, 3)};
        net->dest_drop_every = 3;
        const TraceSnapshot s = run_trace(config(6), factory_of(net), none, nullptr);
        CHECK(s.reached && s.hops.size() == 4);
        if (s.hops.size() == 4) {
            CHECK(!s.hops[1].loss_only_here && s.hops[1].stats.lost == 3);
            CHECK(s.hops[2].stats.lost == 2 && s.hops[3].stats.lost == 2);
            CHECK(!s.hops[2].loss_only_here); /* the destination loses too */
        }
    }
    // a silent hop: no answer at all, marked silent, not an error; the trace goes on past it
    {
        auto net = std::make_shared<FakeNet>();
        FakeHop quiet;
        quiet.silent = true;
        quiet.addrs = {"192.0.2.2"};
        net->hops = {hop("192.0.2.1", 2), quiet, hop("192.0.2.3", 6)};
        const TraceSnapshot s = run_trace(config(2), factory_of(net), none, nullptr);
        CHECK(s.state == TraceSnapshot::State::Done && s.reached && s.dest_hop == 4 && s.shown == 4 && s.error.empty());
        if (s.hops.size() == 4) {
            CHECK(s.hops[1].silent && s.hops[1].addresses.empty() && s.hops[1].stats.lost == 2);
            CHECK(!s.hops[1].loss_only_here && s.hops[1].address().empty());
            CHECK(!s.hops[0].silent && !s.hops[2].silent);
        }
    }
    // a hop that answers from two addresses (a balanced path): the most frequent first
    {
        auto net = std::make_shared<FakeNet>();
        FakeHop two = hop("192.0.2.20", 4);
        two.addrs = {"192.0.2.20", "192.0.2.21", "192.0.2.20"};
        net->hops = {hop("192.0.2.1", 2), two};
        const TraceSnapshot s = run_trace(config(6), factory_of(net), none, nullptr);
        CHECK(s.hops.size() == 3);
        if (s.hops.size() == 3) {
            const TraceHop &h = s.hops[1];
            CHECK(h.multiple_addresses && h.addresses.size() == 2 && h.other_answers == 0);
            CHECK(h.address() == "192.0.2.20" && h.addresses[0].count == 4 && h.addresses[1].address == "192.0.2.21" &&
                  h.addresses[1].count == 2);
        }
    }
    // the destination never answers: every hop up to max_hops is probed; the summary stops at the last answer
    {
        auto net = std::make_shared<FakeNet>();
        net->hops = {hop("192.0.2.1", 2), hop("192.0.2.2", 4)};
        net->dest_answers = false;
        const std::int64_t t0 = now_ms();
        const TraceSnapshot s = run_trace(config(2, 8, 250), factory_of(net), none, nullptr);
        const std::int64_t took = now_ms() - t0;
        CHECK(s.state == TraceSnapshot::State::Done && !s.reached && !s.unreachable && s.dest_hop == 0);
        CHECK(s.hops.size() == 8 && s.shown == 2 && s.rounds == 2);
        for (std::size_t i = 2; i < s.hops.size(); ++i) CHECK(s.hops[i].silent && s.hops[i].stats.lost == 2);
        CHECK(took < 900); /* probes in parallel: about interval + timeout, not 8 x 250 ms per round */
    }
    // a router answers "host unreachable": the trace ends there
    {
        auto net = std::make_shared<FakeNet>();
        net->hops = {hop("192.0.2.1", 2), hop("192.0.2.2", 4)};
        net->unreachable_from = "192.0.2.254";
        const TraceSnapshot s = run_trace(config(2), factory_of(net), none, nullptr);
        CHECK(!s.reached && s.unreachable && s.dest_hop == 3 && s.hops.size() == 3 && s.shown == 3);
        if (s.hops.size() == 3) {
            CHECK(s.hops[2].unreachable == "host unreachable" && s.hops[2].address() == "192.0.2.254");
            CHECK(!s.hops[2].destination && s.hops[2].stats.received == 2);
        }
    }
    // a long path in one round: all hops in flight together
    {
        auto net = std::make_shared<FakeNet>();
        for (int i = 0; i < 12; ++i) net->hops.push_back(hop(("192.0.2." + std::to_string(i + 1)).c_str(), 40));
        net->dest_latency = {60};
        const std::int64_t t0 = now_ms();
        const TraceSnapshot s = run_trace(config(1, 30, 1000), factory_of(net), none, nullptr);
        CHECK(s.reached && s.dest_hop == 13 && s.rounds == 1 && now_ms() - t0 < 500);
    }
    // ICMP not allowed: UDP, and why; ICMP asked for: fails with the fix; nothing allowed: fails
    {
        auto net = std::make_shared<FakeNet>();
        net->hops = {hop("192.0.2.1", 2)};
        net->icmp_allowed = false;
        TraceSnapshot s = run_trace(config(1), factory_of(net), none, nullptr);
        CHECK(s.reached && s.method == "UDP" && s.method_note.find("ICMP sockets are not allowed here") == 0);
        CHECK(net->method_opened == "udp");
        TraceConfig c = config(1);
        c.method = TraceMethod::Icmp;
        s = run_trace(c, factory_of(net), none, nullptr);
        CHECK(s.state == TraceSnapshot::State::Failed && s.error.find("Ping is not allowed") == 0);
        c.method = TraceMethod::Udp;
        net->icmp_allowed = true;
        s = run_trace(c, factory_of(net), none, nullptr);
        CHECK(s.method == "UDP" && s.method_note.empty());
        const ProbeFactory none_allowed = [](const Address &, ProbeMethod, int *err) -> std::unique_ptr<ProbeSocket> {
            *err = EPERM;
            return nullptr;
        };
        s = run_trace(config(1), none_allowed, CancelToken(), nullptr);
        CHECK(s.state == TraceSnapshot::State::Failed && s.error.find("cannot be opened") != std::string::npos);
        c = config(1);
        c.target = "nope.invalid";
        s = run_trace(c, factory_of(net), CancelToken(), nullptr);
        CHECK(s.state == TraceSnapshot::State::Failed && s.error.find("Cannot find nope.invalid") == 0);
    }
    // the session: until stopped; a stop ends it within a slice; a bad target fails at once; a restart wins
    {
        auto net = std::make_shared<FakeNet>();
        net->hops = {hop("192.0.2.1", 2), hop("192.0.2.2", 4, 2)};
        TraceSession session(factory_of(net));
        CHECK(session.start(config(0)));
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
        auto snap = session.latest();
        CHECK(snap->state == TraceSnapshot::State::Running && snap->rounds >= 2);
        const std::int64_t t0 = now_ms();
        session.stop();
        CHECK(session.wait(1000) && now_ms() - t0 < 300);
        snap = session.latest();
        CHECK(snap->state == TraceSnapshot::State::Cancelled && snap->reached && stats_sane(*snap));
        TraceConfig bad = config(1);
        bad.target = "bad host";
        CHECK(!session.start(bad) && session.latest()->state == TraceSnapshot::State::Failed);
        CHECK(session.start(config(0)));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TraceConfig other = config(1);
        other.target = "198.51.100.10";
        other.max_hops = 5;
        CHECK(session.start(other));
        CHECK(session.wait(2000));
        snap = session.latest();
        CHECK(snap->state == TraceSnapshot::State::Done && snap->max_hops == 5 && snap->rounds == 1);
    }
    // the derived flags on a hand-made snapshot
    {
        TraceSnapshot s;
        s.hops.resize(5);
        for (int i = 0; i < 5; ++i) s.hops[static_cast<std::size_t>(i)].ttl = i + 1;
        const auto set = [&](int i, int sent, int got, int lost) {
            PingStats &st = s.hops[static_cast<std::size_t>(i)].stats;
            st.transmitted = sent;
            st.received = got;
            st.lost = lost;
        };
        set(0, 10, 10, 0);
        set(1, 10, 6, 4);  /* loss here only */
        set(2, 10, 0, 10); /* silent */
        set(3, 10, 10, 0);
        set(4, 10, 0, 10); /* silent at the end */
        trace_derive(s);
        CHECK(s.shown == 4 && s.hops[2].silent && s.hops[4].silent && s.hops[1].loss_only_here);
        CHECK(!s.hops[0].loss_only_here && !s.hops[2].loss_only_here);
        set(3, 10, 9, 1);
        trace_derive(s);
        CHECK(!s.hops[1].loss_only_here && !s.hops[3].loss_only_here); /* the last one has nothing after it */
        s.reached = true;
        s.dest_hop = 5;
        trace_derive(s);
        CHECK(s.shown == 5);
        CHECK_EQ(unreachable_words(AF_INET, 3, 13), std::string("communication prohibited"));
        CHECK_EQ(unreachable_words(AF_INET, 3, 0), std::string("network unreachable"));
        CHECK_EQ(unreachable_words(AF_INET6, 1, 0), std::string("no route to the destination"));
        CHECK_EQ(unreachable_words(AF_INET6, 1, 3), std::string("address unreachable"));
    }
}

void test_trace_rdns()
{
    fake::Dns dns;
    if (!dns.ok()) {
        SKIP("cannot bind the fake DNS server on 127.0.0.1");
        return;
    }
    dns.add("1.2.0.192.in-addr.arpa", dns::kPTR, {"1.2.0.192.in-addr.arpa", dns::kPTR, 300, fake::rd_name("gw.example")});
    dns.add("10.100.51.198.in-addr.arpa", dns::kPTR,
            {"10.100.51.198.in-addr.arpa", dns::kPTR, 300, fake::rd_name("target.example")});
    // the cache on its own
    {
        ReverseNames names("127.0.0.1", dns.port(), 1500);
        names.want(addr("192.0.2.1"));
        names.want(addr("192.0.2.1")); /* once */
        names.want(addr("192.0.2.9"));
        const std::int64_t t0 = now_ms();
        while (names.busy() && now_ms() - t0 < 2000) {
            names.service();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        bool done = false;
        CHECK(names.name(addr("192.0.2.1"), &done) == "gw.example" && done);
        CHECK(names.name(addr("192.0.2.9"), &done).empty() && done); /* NXDOMAIN: no name */
        CHECK(names.size() == 2 && names.changes() == 2);
        for (int i = 0; i < 100; ++i) names.want(addr("10.0." + std::to_string(i / 250) + "." + std::to_string(i % 250)));
        CHECK(names.size() == ReverseNames::kMaxEntries); /* bounded */
        ReverseNames off("", 53, 1500);
        off.want(addr("192.0.2.1"));
        CHECK(!off.busy() && off.size() == 0);
    }
    // through the trace
    {
        auto net = std::make_shared<FakeNet>();
        net->hops = {hop("192.0.2.1", 2), hop("192.0.2.2", 4)};
        TraceConfig c = config(3);
        c.reverse_dns = true;
        c.resolve.server = "127.0.0.1";
        c.resolve.port = dns.port();
        const TraceSnapshot s = run_trace(c, factory_of(net), CancelToken(), nullptr);
        CHECK(s.reached && s.hops.size() == 3);
        if (s.hops.size() == 3) {
            CHECK_EQ(s.hops[0].name(), std::string("gw.example"));
            CHECK_EQ(s.hops[1].name(), std::string(""));
            CHECK_EQ(s.hops[2].name(), std::string("target.example"));
        }
    }
    // a silent DNS server does not slow the probing
    {
        dns.set_mode(fake::Dns::Mode::Silent);
        auto net = std::make_shared<FakeNet>();
        net->hops = {hop("192.0.2.1", 2), hop("192.0.2.2", 4)};
        TraceConfig c = config(2);
        c.reverse_dns = true;
        c.resolve.server = "127.0.0.1";
        c.resolve.port = dns.port();
        const std::int64_t t0 = now_ms();
        const TraceSnapshot s = run_trace(c, factory_of(net), CancelToken(), nullptr);
        CHECK(s.reached && now_ms() - t0 < 600 && s.hops.size() == 3);
        if (s.hops.size() == 3) CHECK(s.hops[0].name().empty() && s.hops[0].stats.received == 2);
    }
}

void test_trace_real()
{
    // UDP: open to every user; 127.0.0.1 answers "port unreachable" at hop 1
    for (const char *target : {"127.0.0.1", "::1"}) {
        int err = 0;
        const bool v6 = target[0] == ':';
        if (!open_probe_socket(addr(target), ProbeMethod::Udp, &err)) {
            SKIP(v6 ? "no UDP socket for ::1 on this host" : "no UDP socket on this host");
            continue;
        }
        TraceSession s;
        TraceConfig c = config(2, 5, 1000, 100);
        c.target = target;
        c.method = TraceMethod::Udp;
        CHECK(s.start(c));
        CHECK(s.wait(4000));
        const auto snap = s.latest();
        if (v6 && !snap->reached) {
            SKIP("::1 did not answer the UDP probes (IPv6 loopback off?)");
            continue;
        }
        CHECK(snap->state == TraceSnapshot::State::Done && snap->reached && snap->dest_hop == 1 && snap->method == "UDP");
        CHECK(snap->hops.size() == 1 && snap->hops[0].stats.received == 2 && snap->hops[0].address() == target);
        CHECK(snap->hops[0].stats.max_ms < 100);
    }
    // ICMP: only when the host allows ICMP datagram sockets (ping_group_range)
    const PingAvailability a = ping_available();
    if (!a.ipv4) {
        const std::string why = "ICMP datagram sockets are not allowed on this host, the real ICMP trace is not tested: " + a.why;
        SKIP(why.c_str());
        return;
    }
    TraceSession s;
    TraceConfig c = config(2, 5, 1000, 100);
    c.target = "127.0.0.1";
    c.method = TraceMethod::Icmp;
    CHECK(s.start(c));
    CHECK(s.wait(4000));
    auto snap = s.latest();
    CHECK(snap->reached && snap->dest_hop == 1 && snap->method == "ICMP" && snap->hops[0].stats.received == 2);
    if (a.ipv6) {
        c.target = "::1";
        CHECK(s.start(c));
        CHECK(s.wait(4000));
        snap = s.latest();
        if (snap->reached) CHECK(snap->dest_hop == 1 && snap->method == "ICMP");
        else SKIP("::1 did not answer the ICMP probes");
    }
}
