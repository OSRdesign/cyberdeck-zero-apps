/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: traceroute in rounds, as mtr does (no LVGL; tests/test_trace.cpp: the engine on a fake hop network
 * injected at the socket layer, the statistics, a real trace of 127.0.0.1 and ::1).
 *
 * No root: one unprivileged socket for the whole run. ICMP echo over SOCK_DGRAM IPPROTO_ICMP (net.ipv4.ping_group_range,
 * as ping) and, when that socket is not allowed, UDP datagrams to high ports (33434 + the probe number). Each probe
 * leaves with its own TTL (IP_TTL / IPV6_UNICAST_HOPS); the answers come back without root through IP_RECVERR /
 * IPV6_RECVERR on the socket's error queue (MSG_ERRQUEUE): "time exceeded" names the router of that hop; an echo reply
 * (ICMP), or "port unreachable" from the destination (UDP), means the destination was reached; "unreachable" from a
 * router ends the trace there. A probe is known by its ICMP sequence (quoted in the error) or its UDP port.
 *
 * A round probes every hop up to the destination (all of them while it is not known, at most max_hops), a few ms
 * apart, all in flight together and waited for in one poll() loop, so a round takes about one probe timeout, not
 * hops x timeout. A new round starts every interval. Each hop keeps its statistics across the rounds (sent, received,
 * lost: a probe not answered within the timeout; loss %; last, avg, best, worst; jitter, as ping), the addresses that
 * answered for it (most frequent first; several when the path is balanced), and its reverse name (ReverseNames: PTR
 * through the app's DNS client code, 1.5 s, cached, never blocking the probes). A hop that never answered is
 * `silent`, not an error. loss_only_here marks a hop with loss when every later hop that answered has none: the router
 * limits its ICMP answers (rate limiting), the path is fine.
 *
 * The destination: a literal address, /etc/hosts, or the app's DNS client (IPv4 first; IPv6 when the name has only
 * AAAA records, or for an IPv6 literal).
 */
#pragma once

#include "dns_client.hpp"
#include "netaddr.hpp"
#include "ping.hpp"
#include "worker.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

constexpr std::uint16_t kTraceSeqSpace = 16384;   /* probe numbers wrap here (UDP ports 33434 .. 49817) */
constexpr std::uint16_t kTraceUdpBasePort = 33434;
constexpr std::size_t kTraceHopAddresses = 4;     /* addresses kept per hop (the others are only counted) */

/* ---------------------------------------------------------------- the socket */

enum class ProbeMethod { Icmp, Udp };

struct ProbeReply {
    enum class Kind { TimeExceeded, Reached, Unreachable };
    std::uint16_t seq = 0;       /* the probe it answers */
    Kind kind = Kind::TimeExceeded;
    Address from;                /* who answered */
    int icmp_type = 0, icmp_code = 0;
    std::int64_t at_us = 0;      /* now_us() when it was read */
};

/* One socket towards one destination. send() never blocks; receive() waits at most timeout_ms. */
class ProbeSocket {
public:
    virtual ~ProbeSocket() = default;
    /* probe `seq` (< kTraceSeqSpace) with that TTL; 0 or the errno */
    virtual int send(int ttl, std::uint16_t seq) = 0;
    /* 1 with a reply, 0 on timeout, -errno on a socket error */
    virtual int receive(int timeout_ms, ProbeReply &out) = 0;
};

using ProbeFactory = std::function<std::unique_ptr<ProbeSocket>(const Address &target, ProbeMethod method, int *err)>;

/* The real one (ICMP datagram or UDP socket with IP_RECVERR). */
std::unique_ptr<ProbeSocket> open_probe_socket(const Address &target, ProbeMethod method, int *err);

/* "network unreachable", "host unreachable", "port unreachable", "communication prohibited", ... (pure) */
std::string unreachable_words(int family, int type, int code);

/* ---------------------------------------------------------------- the tool */

enum class TraceMethod { Auto, Icmp, Udp };

struct TraceConfig {
    std::string target;            /* a name or an address */
    int max_hops = 30;             /* 1 .. 64 */
    int interval_ms = 1000;        /* between the starts of two rounds: 100 .. 60000 */
    int rounds = 0;                /* 0 = until stopped */
    int timeout_ms = 1500;         /* per probe: 100 .. 10000 */
    TraceMethod method = TraceMethod::Auto; /* Auto: ICMP, else UDP */
    bool reverse_dns = true;       /* names of the hops, through resolve.server */
    ResolveOptions resolve;        /* the destination's name, and the server for the reverse names */
};

struct TraceHopAddress {
    std::string address;
    std::string name;              /* reverse name, "" while unknown or when there is none */
    int count = 0;                 /* answers from it */
};

struct TraceHop {
    int ttl = 0;
    PingStats stats;               /* transmitted, received, lost, loss %, last, avg, min = best, max = worst, jitter */
    std::vector<TraceHopAddress> addresses; /* most frequent first */
    int other_answers = 0;         /* answers from addresses not kept */
    bool multiple_addresses = false;
    bool silent = false;           /* probes timed out and none was ever answered */
    bool destination = false;      /* this hop is the destination */
    std::string unreachable;       /* a router said the destination cannot be reached: its words */
    bool loss_only_here = false;   /* loss here, none further on: likely ICMP rate limiting, not a real problem */
    std::string address() const { return addresses.empty() ? std::string() : addresses[0].address; }
    std::string name() const { return addresses.empty() ? std::string() : addresses[0].name; }
};

struct TraceSnapshot {
    enum class State { Idle, Resolving, Running, Done, Failed, Cancelled };
    State state = State::Idle;
    std::string target;            /* as typed */
    std::string address;           /* the destination's address */
    std::string resolved_by;       /* "", "hosts" or the DNS server */
    std::string error;             /* Failed: why */
    std::string method;            /* "ICMP" or "UDP" */
    std::string method_note;       /* why UDP is used */
    int max_hops = 30;
    int rounds = 0;                /* rounds finished (every probe answered or timed out) */
    bool reached = false;          /* the destination answered */
    bool unreachable = false;      /* a router answered "unreachable": the trace ends at dest_hop */
    int dest_hop = 0;              /* the destination's (or the unreachable router's) hop, 0 while unknown */
    std::vector<TraceHop> hops;    /* hop 1 first, as far as probed */
    std::size_t shown = 0;         /* hops for a summary: up to the destination, else up to the last that answered */
    bool running() const { return state == State::Resolving || state == State::Running; }
};

/* The derived fields (silent, multiple_addresses, loss_only_here, shown) from the counters (pure). */
void trace_derive(TraceSnapshot &s);

/* The engine (the session's thread; tests call it with a fake socket and a fake clock-free network). */
TraceSnapshot run_trace(const TraceConfig &cfg, const ProbeFactory &factory, const CancelToken &cancel,
                        const std::function<void(const TraceSnapshot &)> &publish);

class TraceSession {
public:
    TraceSession();                /* the real sockets */
    explicit TraceSession(ProbeFactory factory);
    bool start(const TraceConfig &cfg); /* LVGL thread; false (a Failed snapshot) for a bad target */
    void stop();
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const TraceSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    ProbeFactory factory_;
    Channel<TraceSnapshot> channel_;
    Worker worker_;
};

} // namespace nt
