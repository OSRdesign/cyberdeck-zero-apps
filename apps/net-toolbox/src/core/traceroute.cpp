/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: traceroute in rounds (see traceroute.hpp).
 */
#include "traceroute.hpp"

#include "rdns.hpp"
#include "sock.hpp"
#include "text.hpp"

#include <linux/errqueue.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>

namespace nt {
namespace {

bool same_host(const Address &a, const Address &b)
{
    if (a.family != b.family) return false;
    return std::memcmp(a.bytes, b.bytes, a.family == AF_INET ? 4 : 16) == 0;
}

std::uint16_t checksum(const std::uint8_t *p, std::size_t n)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) sum += static_cast<std::uint32_t>((p[i] << 8) | p[i + 1]);
    if (n & 1) sum += static_cast<std::uint32_t>(p[n - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum & 0xFFFF);
}

class RealProbe : public ProbeSocket {
public:
    RealProbe(int fd, const Address &target, ProbeMethod method)
        : fd_(fd), target_(target), method_(method), v6_(target.family == AF_INET6)
    {
        if (getrandom(cookie_, sizeof(cookie_), GRND_NONBLOCK) != static_cast<ssize_t>(sizeof(cookie_))) {
            const std::int64_t t = now_us() ^ (static_cast<std::int64_t>(getpid()) << 20);
            std::memcpy(cookie_, &t, sizeof(cookie_));
        }
    }

    int send(int ttl, std::uint16_t seq) override
    {
        int e = send_once(ttl, seq);
        if (e) { /* an error left by an earlier probe's ICMP answer: keep the answers, try once more */
            drain();
            e = send_once(ttl, seq);
        }
        return e;
    }

    int receive(int timeout_ms, ProbeReply &out) override
    {
        const std::int64_t deadline = now_ms() + std::max(0, timeout_ms);
        int spins = 0;
        for (;;) {
            if (!pending_.empty()) {
                out = pending_.front();
                pending_.pop_front();
                return 1;
            }
            int r = read_errqueue(out);
            if (r == 1) return 1;
            if (r == 2) continue;
            if (r < 0) return r;
            r = read_data(out);
            if (r == 1) return 1;
            if (r == 2) continue;
            if (r != -EAGAIN && r != -EWOULDBLOCK) {
                /* the socket's pending error (an ICMP answer's errno, already queued on the error queue): read on */
                if (++spins < 16) continue;
            }
            spins = 0;
            const std::int64_t left = deadline - now_ms();
            if (left <= 0) return 0;
            struct pollfd p = {fd_.get(), POLLIN, 0};
            const int rc = poll(&p, 1, static_cast<int>(left));
            if (rc == 0) return 0;
            if (rc < 0 && errno != EINTR) return -errno;
        }
    }

private:
    int send_once(int ttl, std::uint16_t seq)
    {
        const int rc = v6_ ? setsockopt(fd_.get(), IPPROTO_IPV6, IPV6_UNICAST_HOPS, &ttl, sizeof(ttl))
                           : setsockopt(fd_.get(), IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
        if (rc != 0) return errno;
        std::uint8_t packet[24] = {};
        std::size_t len = 0;
        sockaddr_storage dst;
        socklen_t dl = 0;
        if (method_ == ProbeMethod::Icmp) {
            packet[0] = v6_ ? 128 : 8; /* echo request; the kernel sets the identifier */
            packet[6] = static_cast<std::uint8_t>(seq >> 8);
            packet[7] = static_cast<std::uint8_t>(seq & 0xFF);
            std::memcpy(packet + 8, cookie_, sizeof(cookie_));
            len = sizeof(packet);
            if (!v6_) {
                const std::uint16_t c = checksum(packet, len);
                packet[2] = static_cast<std::uint8_t>(c >> 8);
                packet[3] = static_cast<std::uint8_t>(c & 0xFF);
            }
            dl = target_.to_sockaddr(0, dst);
        } else {
            std::memcpy(packet, cookie_, sizeof(cookie_));
            packet[8] = static_cast<std::uint8_t>(seq >> 8);
            packet[9] = static_cast<std::uint8_t>(seq & 0xFF);
            len = 16;
            dl = target_.to_sockaddr(static_cast<std::uint16_t>(kTraceUdpBasePort + seq % kTraceSeqSpace), dst);
        }
        for (;;) {
            const ssize_t n = sendto(fd_.get(), packet, len, MSG_NOSIGNAL | MSG_DONTWAIT,
                                     reinterpret_cast<const sockaddr *>(&dst), dl);
            if (n >= 0) return 0;
            if (errno == EINTR) continue;
            return errno;
        }
    }

    void drain()
    {
        for (int i = 0; i < 64; ++i) {
            ProbeReply r;
            const int rc = read_errqueue(r);
            if (rc == 1) pending_.push_back(r);
            else if (rc != 2) break;
        }
    }

    /* 1: a reply; 2: something read and ignored; 0: nothing; < 0: -errno */
    int read_errqueue(ProbeReply &out)
    {
        std::uint8_t data[256];
        char control[512];
        sockaddr_storage name{};
        iovec iov = {data, sizeof(data)};
        msghdr msg{};
        msg.msg_name = &name;
        msg.msg_namelen = sizeof(name);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        const ssize_t n = recvmsg(fd_.get(), &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            if (errno == EINTR) return 2;
            return -errno;
        }
        const sock_extended_err *ee = nullptr;
        for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if ((c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) ||
                (c->cmsg_level == SOL_IPV6 && c->cmsg_type == IPV6_RECVERR))
                ee = reinterpret_cast<const sock_extended_err *>(CMSG_DATA(c));
        }
        if (!ee || (ee->ee_origin != SO_EE_ORIGIN_ICMP && ee->ee_origin != SO_EE_ORIGIN_ICMP6)) return 2;
        if (msg.msg_namelen > 0) { /* the probe's own destination */
            const Address dest = Address::from_sockaddr(reinterpret_cast<const sockaddr *>(&name));
            if (dest.valid() && !same_host(dest, target_)) return 2;
        }
        std::uint16_t seq = 0;
        if (method_ == ProbeMethod::Icmp) {
            if (n < 8 || data[0] != (v6_ ? 128 : 8)) return 2; /* the quoted echo request */
            if (n >= 16 && std::memcmp(data + 8, cookie_, sizeof(cookie_)) != 0) return 2;
            seq = static_cast<std::uint16_t>((data[6] << 8) | data[7]);
        } else {
            if (msg.msg_namelen == 0) return 2;
            std::uint16_t port = 0;
            if (name.ss_family == AF_INET) port = ntohs(reinterpret_cast<const sockaddr_in *>(&name)->sin_port);
            else if (name.ss_family == AF_INET6) port = ntohs(reinterpret_cast<const sockaddr_in6 *>(&name)->sin6_port);
            if (port < kTraceUdpBasePort || port >= kTraceUdpBasePort + kTraceSeqSpace) return 2;
            seq = static_cast<std::uint16_t>(port - kTraceUdpBasePort);
        }
        out = ProbeReply();
        out.seq = seq;
        out.from = Address::from_sockaddr(SO_EE_OFFENDER(ee));
        out.from.scope = 0;
        out.icmp_type = ee->ee_type;
        out.icmp_code = ee->ee_code;
        out.at_us = now_us();
        const bool from_target = same_host(out.from, target_);
        if (!v6_) {
            if (ee->ee_type == 11) out.kind = ProbeReply::Kind::TimeExceeded;
            else if (ee->ee_type == 3) out.kind = from_target ? ProbeReply::Kind::Reached : ProbeReply::Kind::Unreachable;
            else return 2;
        } else {
            if (ee->ee_type == 3) out.kind = ProbeReply::Kind::TimeExceeded;
            else if (ee->ee_type == 1) out.kind = from_target ? ProbeReply::Kind::Reached : ProbeReply::Kind::Unreachable;
            else return 2;
        }
        if (!out.from.valid()) return 2;
        return 1;
    }

    /* an echo reply (ICMP); 1, 2 (not ours), or -errno (-EAGAIN: nothing) */
    int read_data(ProbeReply &out)
    {
        std::uint8_t buf[512];
        sockaddr_storage from{};
        socklen_t fl = sizeof(from);
        const ssize_t n = recvfrom(fd_.get(), buf, sizeof(buf), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&from), &fl);
        if (n < 0) return errno == EINTR ? 2 : -errno;
        if (method_ != ProbeMethod::Icmp || n < 16) return 2;
        if (buf[0] != (v6_ ? 129 : 0) || std::memcmp(buf + 8, cookie_, sizeof(cookie_)) != 0) return 2;
        out = ProbeReply();
        out.seq = static_cast<std::uint16_t>((buf[6] << 8) | buf[7]);
        out.kind = ProbeReply::Kind::Reached;
        out.from = Address::from_sockaddr(reinterpret_cast<const sockaddr *>(&from));
        out.from.scope = 0;
        out.icmp_type = buf[0];
        out.at_us = now_us();
        return 1;
    }

    Fd fd_;
    Address target_;
    ProbeMethod method_;
    bool v6_;
    std::uint8_t cookie_[8] = {};
    std::deque<ProbeReply> pending_;
};

/* the per-hop counters of the engine */
struct HopState {
    PingStats stats;
    std::vector<std::pair<Address, int>> addrs; /* in the order first seen */
    int other = 0;
    std::string unreachable;
    bool reached = false;
    void clear() { *this = HopState(); }
    void add_address(const Address &a)
    {
        for (auto &p : addrs)
            if (p.first == a) {
                ++p.second;
                return;
            }
        if (addrs.size() < 2 * kTraceHopAddresses) addrs.emplace_back(a, 1);
        else ++other;
    }
};

struct InFlight {
    std::uint16_t seq;
    int ttl;
    int round;
    std::int64_t sent_us;
};

struct RoundState {
    int round;
    int outstanding;
    bool sending_done;
};

} // namespace

std::unique_ptr<ProbeSocket> open_probe_socket(const Address &target, ProbeMethod method, int *err)
{
    const auto fail = [&](int e) -> std::unique_ptr<ProbeSocket> {
        if (err) *err = e;
        return nullptr;
    };
    if (!target.valid()) return fail(EAFNOSUPPORT);
    const bool v6 = target.family == AF_INET6;
    int proto = 0;
    if (method == ProbeMethod::Icmp) proto = v6 ? static_cast<int>(IPPROTO_ICMPV6) : static_cast<int>(IPPROTO_ICMP);
    const int fd = socket(target.family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, proto);
    if (fd < 0) return fail(errno);
    const int one = 1;
    const int rc = v6 ? setsockopt(fd, SOL_IPV6, IPV6_RECVERR, &one, sizeof(one))
                      : setsockopt(fd, SOL_IP, IP_RECVERR, &one, sizeof(one));
    if (rc != 0) {
        const int e = errno;
        close(fd);
        return fail(e);
    }
    if (err) *err = 0;
    return std::make_unique<RealProbe>(fd, target, method);
}

std::string unreachable_words(int family, int type, int code)
{
    if (family == AF_INET6) {
        if (type != 1) return "unreachable";
        switch (code) {
        case 0: return "no route to the destination";
        case 1: return "communication prohibited";
        case 3: return "address unreachable";
        case 4: return "port unreachable";
        case 5: return "source address refused";
        case 6: return "rejected by a route";
        default: return "unreachable";
        }
    }
    if (type != 3) return "unreachable";
    switch (code) {
    case 0: return "network unreachable";
    case 1: return "host unreachable";
    case 2: return "protocol unreachable";
    case 3: return "port unreachable";
    case 4: return "fragmentation needed";
    case 6: return "network unknown";
    case 7: return "host unknown";
    case 9:
    case 10:
    case 13: return "communication prohibited";
    default: return "unreachable";
    }
}

void trace_derive(TraceSnapshot &s)
{
    std::size_t last_answer = 0;
    for (std::size_t i = 0; i < s.hops.size(); ++i) {
        TraceHop &h = s.hops[i];
        h.silent = h.stats.received == 0 && h.stats.lost > 0;
        h.multiple_addresses = h.addresses.size() + (h.other_answers > 0 ? 1 : 0) > 1;
        h.loss_only_here = false;
        if (h.stats.received > 0) last_answer = i + 1;
    }
    if ((s.reached || s.unreachable) && s.dest_hop > 0) s.shown = std::min<std::size_t>(s.dest_hop, s.hops.size());
    else s.shown = last_answer;
    for (std::size_t i = 0; i < s.shown; ++i) {
        TraceHop &h = s.hops[i];
        if (h.stats.received == 0 || h.stats.lost == 0) continue;
        bool later = false, clean = true;
        for (std::size_t j = i + 1; j < s.shown; ++j) {
            const TraceHop &k = s.hops[j];
            if (k.stats.received == 0) continue; /* a silent router says nothing either way */
            later = true;
            if (k.stats.lost > 0) clean = false;
        }
        h.loss_only_here = later && clean;
    }
}

TraceSnapshot run_trace(const TraceConfig &input, const ProbeFactory &factory, const CancelToken &cancel,
                        const std::function<void(const TraceSnapshot &)> &publish)
{
    TraceConfig cfg = input;
    cfg.max_hops = std::min(64, std::max(1, cfg.max_hops));
    cfg.interval_ms = std::min(60000, std::max(100, cfg.interval_ms));
    cfg.timeout_ms = std::min(10000, std::max(100, cfg.timeout_ms));
    cfg.rounds = std::max(0, cfg.rounds);
    TraceSnapshot snap;
    snap.target = trim(cfg.target);
    snap.max_hops = cfg.max_hops;
    snap.state = TraceSnapshot::State::Resolving;
    const auto emit = [&] {
        if (publish) publish(snap);
    };
    emit();

    const Resolved res = resolve_host(snap.target, cfg.resolve, cancel);
    if (cancel.cancelled()) {
        snap.state = TraceSnapshot::State::Cancelled;
        emit();
        return snap;
    }
    if (!res.ok) {
        snap.state = TraceSnapshot::State::Failed;
        snap.error = "Cannot find " + snap.target + ": " + res.error;
        emit();
        return snap;
    }
    const Address target = res.address;
    snap.address = target.text();
    snap.resolved_by = res.literal ? "" : res.from_hosts ? "hosts" : res.server;

    std::unique_ptr<ProbeSocket> sock;
    int icmp_err = 0, udp_err = 0;
    if (cfg.method != TraceMethod::Udp && factory) {
        sock = factory(target, ProbeMethod::Icmp, &icmp_err);
        if (sock) snap.method = "ICMP";
    }
    if (!sock && cfg.method != TraceMethod::Icmp && factory) {
        sock = factory(target, ProbeMethod::Udp, &udp_err);
        if (sock) {
            snap.method = "UDP";
            if (cfg.method == TraceMethod::Auto)
                snap.method_note = "ICMP sockets are not allowed here (" + errno_words(icmp_err ? icmp_err : EPERM) +
                                   "): UDP probes instead";
        }
    }
    if (!sock) {
        snap.state = TraceSnapshot::State::Failed;
        if (cfg.method == TraceMethod::Icmp && (icmp_err == EACCES || icmp_err == EPERM))
            snap.error = ping_unavailable_words(icmp_err, std::string());
        else
            snap.error = "The probe socket cannot be opened: " + errno_words(udp_err ? udp_err : icmp_err ? icmp_err : EIO);
        emit();
        return snap;
    }
    snap.state = TraceSnapshot::State::Running;
    emit();

    ReverseNames rdns(cfg.reverse_dns ? cfg.resolve.server : std::string(), cfg.resolve.port, 1500);
    std::vector<HopState> hops(static_cast<std::size_t>(cfg.max_hops));
    std::vector<InFlight> flight;
    std::vector<RoundState> rounds;
    int dest_hop = 0, highest = 0, round = 0, next_ttl = 0;
    bool reached = false, unreach = false;
    std::uint16_t next_seq = 0;
    const int spacing = std::min(25, std::max(2, cfg.interval_ms / (2 * cfg.max_hops)));
    std::int64_t next_round_ms = now_ms(), next_send_ms = 0, last_publish = 0;
    std::uint64_t names_seen = 0;
    bool dirty = false;
    const auto limit = [&] { return dest_hop ? dest_hop : cfg.max_hops; };
    const auto round_of = [&](int r) -> RoundState * {
        for (RoundState &s : rounds)
            if (s.round == r) return &s;
        return nullptr;
    };
    const auto end_sending = [&] {
        if (RoundState *rs = round_of(round)) rs->sending_done = true;
        next_ttl = 0;
    };
    const auto build = [&] {
        const int rows = dest_hop ? dest_hop : highest;
        snap.hops.clear();
        for (int t = 1; t <= rows; ++t) {
            const HopState &hs = hops[static_cast<std::size_t>(t - 1)];
            TraceHop h;
            h.ttl = t;
            h.stats = hs.stats;
            std::vector<std::pair<Address, int>> sorted = hs.addrs;
            std::stable_sort(sorted.begin(), sorted.end(),
                             [](const std::pair<Address, int> &a, const std::pair<Address, int> &b) {
                                 return a.second > b.second;
                             });
            h.other_answers = hs.other;
            for (std::size_t i = 0; i < sorted.size(); ++i) {
                if (i >= kTraceHopAddresses) {
                    h.other_answers += sorted[i].second;
                    continue;
                }
                TraceHopAddress ha;
                ha.address = sorted[i].first.text();
                ha.name = rdns.name(sorted[i].first);
                ha.count = sorted[i].second;
                h.addresses.push_back(ha);
            }
            h.destination = hs.reached && t == dest_hop;
            h.unreachable = hs.unreachable;
            snap.hops.push_back(h);
        }
        snap.reached = reached;
        snap.unreachable = unreach && !reached;
        snap.dest_hop = dest_hop;
        trace_derive(snap);
    };
    const auto settle = [&](const InFlight &f) {
        if (RoundState *rs = round_of(f.round)) --rs->outstanding;
    };

    while (!cancel.cancelled()) {
        std::int64_t now = now_ms();
        /* a new round */
        if (next_ttl == 0 && (cfg.rounds == 0 || round < cfg.rounds) && now >= next_round_ms) {
            ++round;
            rounds.push_back({round, 0, false});
            next_ttl = 1;
            next_send_ms = now;
            next_round_ms = now + cfg.interval_ms;
        }
        /* its probes, a few ms apart */
        while (next_ttl && now >= next_send_ms) {
            if (next_ttl > limit()) {
                end_sending();
                break;
            }
            HopState &hs = hops[static_cast<std::size_t>(next_ttl - 1)];
            const std::uint16_t seq = next_seq;
            next_seq = static_cast<std::uint16_t>((next_seq + 1) % kTraceSeqSpace);
            hs.stats.add_sent();
            highest = std::max(highest, next_ttl);
            const int e = sock->send(next_ttl, seq);
            if (e) {
                hs.stats.add_lost();
                snap.error = "Send failed: " + errno_words(e);
            } else {
                if (starts_with(snap.error, "Send failed")) snap.error.clear();
                flight.push_back({seq, next_ttl, round, now_us()});
                if (RoundState *rs = round_of(round)) ++rs->outstanding;
            }
            dirty = true;
            ++next_ttl;
            next_send_ms += spacing;
            if (next_ttl > limit()) end_sending();
        }
        /* probes that timed out */
        const std::int64_t now_u = now_us();
        for (auto it = flight.begin(); it != flight.end();) {
            if (now_u - it->sent_us < static_cast<std::int64_t>(cfg.timeout_ms) * 1000) {
                ++it;
                continue;
            }
            if (it->ttl <= limit()) hops[static_cast<std::size_t>(it->ttl - 1)].stats.add_lost();
            settle(*it);
            it = flight.erase(it);
            dirty = true;
        }
        /* rounds that are over */
        bool round_over = false;
        for (auto it = rounds.begin(); it != rounds.end();) {
            if (it->sending_done && it->outstanding <= 0) {
                ++snap.rounds;
                round_over = true;
                it = rounds.erase(it);
            } else {
                ++it;
            }
        }
        if (cfg.rounds && round >= cfg.rounds && next_ttl == 0 && flight.empty()) {
            snap.state = TraceSnapshot::State::Done;
            break;
        }
        rdns.service();
        now = now_ms();
        if (rdns.changes() != names_seen) {
            names_seen = rdns.changes();
            dirty = true;
        }
        if (dirty && (round_over || now - last_publish >= 100)) {
            build();
            emit();
            last_publish = now;
            dirty = false;
        }
        /* wait for the next answer, send or timeout */
        std::int64_t wait = kSliceMs;
        if (next_ttl) wait = std::min(wait, next_send_ms - now);
        else if (cfg.rounds == 0 || round < cfg.rounds) wait = std::min(wait, next_round_ms - now);
        for (const InFlight &f : flight)
            wait = std::min(wait, (f.sent_us + static_cast<std::int64_t>(cfg.timeout_ms) * 1000 - now_u) / 1000 + 1);
        if (rdns.busy()) wait = std::min<std::int64_t>(wait, 20);
        wait = std::max<std::int64_t>(wait, 0);
        ProbeReply rep;
        const int r = sock->receive(static_cast<int>(wait), rep);
        if (r < 0) {
            snap.error = "Receive failed: " + errno_words(-r);
            if (!cancel.sleep_ms(static_cast<int>(std::max<std::int64_t>(wait, 10)))) break;
            continue;
        }
        if (r == 0) continue;
        auto it = std::find_if(flight.begin(), flight.end(), [&](const InFlight &f) { return f.seq == rep.seq; });
        if (it == flight.end()) continue; /* late (after its timeout) or a duplicate */
        const InFlight f = *it;
        flight.erase(it);
        settle(f);
        dirty = true;
        const double rtt = static_cast<double>(std::max<std::int64_t>(0, rep.at_us - f.sent_us)) / 1000.0;
        if (rep.kind == ProbeReply::Kind::TimeExceeded) {
            if (dest_hop && f.ttl > dest_hop) continue; /* beyond the destination */
            HopState &hs = hops[static_cast<std::size_t>(f.ttl - 1)];
            hs.stats.add_reply(rtt);
            hs.add_address(rep.from);
            if (cfg.reverse_dns) rdns.want(rep.from);
            continue;
        }
        /* the end of the path: the destination, or a router saying it cannot be reached */
        if (dest_hop == 0 || f.ttl < dest_hop) {
            for (int t = f.ttl + 1; t <= cfg.max_hops; ++t) hops[static_cast<std::size_t>(t - 1)].clear();
            dest_hop = f.ttl;
            highest = std::min(highest, dest_hop);
            if (next_ttl > dest_hop) end_sending();
            reached = unreach = false;
        }
        if (f.ttl > dest_hop) continue;
        HopState &hs = hops[static_cast<std::size_t>(f.ttl - 1)];
        hs.stats.add_reply(rtt);
        hs.add_address(rep.from);
        if (cfg.reverse_dns) rdns.want(rep.from);
        if (rep.kind == ProbeReply::Kind::Reached) {
            reached = true;
            hs.reached = true;
            hs.unreachable.clear();
        } else {
            unreach = true;
            if (!hs.reached) hs.unreachable = unreachable_words(target.family, rep.icmp_type, rep.icmp_code);
        }
    }
    if (snap.state != TraceSnapshot::State::Done) snap.state = TraceSnapshot::State::Cancelled;
    rdns.service();
    build();
    emit();
    return snap;
}

TraceSession::TraceSession()
    : factory_([](const Address &target, ProbeMethod m, int *err) { return open_probe_socket(target, m, err); })
{
}

TraceSession::TraceSession(ProbeFactory factory) : factory_(std::move(factory)) {}

bool TraceSession::start(const TraceConfig &cfg)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    TraceSnapshot first;
    first.target = trim(cfg.target);
    first.max_hops = std::min(64, std::max(1, cfg.max_hops));
    std::string why;
    if (!valid_host(first.target, &why)) {
        first.state = TraceSnapshot::State::Failed;
        first.error = why;
        channel_.begin(gen, first);
        return false;
    }
    first.state = TraceSnapshot::State::Resolving;
    channel_.begin(gen, first);
    const ProbeFactory factory = factory_;
    const std::uint64_t started = worker_.start([this, gen, cfg, factory](const CancelToken &cancel) {
        run_trace(cfg, factory, cancel, [this, gen](const TraceSnapshot &s) { channel_.publish(gen, s); });
    });
    if (!started) {
        first.state = TraceSnapshot::State::Failed;
        first.error = "Could not start the trace (no thread)";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void TraceSession::stop()
{
    worker_.cancel();
}

} // namespace nt
