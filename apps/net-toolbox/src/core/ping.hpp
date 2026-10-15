/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: ping (no LVGL; tests/test_ping.cpp: the statistics, the engine on a fake socket, a real ping of
 * 127.0.0.1 when the host allows it).
 *
 * No root: an unprivileged ICMP datagram socket (socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP), ICMPv6 likewise), which
 * the kernel allows to the groups in net.ipv4.ping_group_range (Raspberry Pi OS: everyone). The kernel fills in the
 * identifier and delivers only the replies to this socket; the app matches them by sequence number and by an 8-byte
 * cookie in the payload. ping_available() tells at start whether the socket opens, and why not.
 *
 * Per packet: Ok, Slow (rtt > max(100 ms, 3 x the average of the replies before it), amber in the UI) or Lost (no reply
 * within the timeout; a send error counts as lost too). The last kRingSize packets are kept for the strip and the
 * graph, Pending while they wait. Statistics: transmitted, received, lost (timed out; packets still waiting are not
 * lost yet), loss % = lost / (received + lost), min / avg / max, jitter = mean absolute difference of successive RTTs
 * (successive replies, a lost packet in between does not reset it).
 */
#pragma once

#include "dns_client.hpp"
#include "netaddr.hpp"
#include "worker.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

constexpr std::size_t kRingSize = 120;
constexpr double kSlowFloorMs = 100.0;
constexpr double kSlowFactor = 3.0;

enum class PacketState { Pending, Ok, Slow, Lost };

struct PacketResult {
    std::uint32_t number = 0; /* 1, 2, 3 ... (the ICMP sequence is number & 0xFFFF) */
    PacketState state = PacketState::Pending;
    double rtt_ms = 0;        /* Ok / Slow */
};

struct PingStats {
    int transmitted = 0;
    int received = 0;
    int lost = 0;
    double min_ms = 0, max_ms = 0, sum_ms = 0;
    double jitter_sum = 0;
    int jitter_n = 0;
    bool has_last = false;
    double last_ms = 0;

    double avg_ms() const { return received ? sum_ms / received : 0; }
    double jitter_ms() const { return jitter_n ? jitter_sum / jitter_n : 0; }
    double loss_pct() const { return received + lost ? 100.0 * lost / (received + lost) : 0; }
    int pending() const { return transmitted - received - lost; }
    /* max(100 ms, 3 x the average so far) */
    double slow_threshold_ms() const;
    void add_sent() { ++transmitted; }
    /* a reply: Ok or Slow, judged against the average BEFORE this reply */
    PacketState add_reply(double rtt_ms);
    void add_lost() { ++lost; }
};

/* The last kRingSize packets, oldest first. */
class PacketRing {
public:
    void push(const PacketResult &p);
    /* the entry of that packet number, nullptr when it left the ring */
    PacketResult *find(std::uint32_t number);
    std::vector<PacketResult> items() const;
    std::size_t size() const { return count_; }
    void clear() { head_ = count_ = 0; }

private:
    PacketResult buf_[kRingSize];
    std::size_t head_ = 0;  /* next write */
    std::size_t count_ = 0;
};

/* ---------------------------------------------------------------- the socket */

struct PingAvailability {
    bool ipv4 = false;
    bool ipv6 = false;
    int error = 0;      /* errno of the IPv4 socket */
    std::string why;    /* plain words and the fix when IPv4 ping is not available */
};

/* Opens and closes one ICMP datagram socket of each family. Cheap: call at start. */
PingAvailability ping_available();
/* The words for an IPv4 socket that failed with err; range_text = /proc/sys/net/ipv4/ping_group_range (pure) */
std::string ping_unavailable_words(int err, const std::string &range_text);
/* "1 0" (nobody) -> false; "0 2147483647" with gid 1000 -> true (pure) */
bool ping_group_allows(const std::string &range_text, unsigned gid);

/* One echo socket to one address. send() and receive() never block longer than asked. */
class EchoSocket {
public:
    virtual ~EchoSocket() = default;
    /* sends echo request `seq`; 0 or the errno */
    virtual int send(std::uint16_t seq) = 0;
    /* waits up to timeout_ms for one echo reply: 1 with its sequence, 0 on timeout, -errno on a socket error */
    virtual int receive(int timeout_ms, std::uint16_t &seq) = 0;
};

using EchoFactory = std::function<std::unique_ptr<EchoSocket>(const Address &target, int payload_bytes, int *err)>;

/* The real one: an ICMP (or ICMPv6) datagram socket, payload_bytes of payload (8 to 1400). */
std::unique_ptr<EchoSocket> open_icmp_echo(const Address &target, int payload_bytes, int *err);

/* ---------------------------------------------------------------- the ping tool */

struct PingConfig {
    std::string target;          /* a name or an address */
    int interval_ms = 1000;      /* 200 .. 60000 */
    int timeout_ms = 2000;       /* 100 .. 60000 */
    int count = 0;               /* 0 = until stopped */
    int payload_bytes = 56;
    ResolveOptions resolve;      /* the resolver for a name */
};

struct PingSnapshot {
    enum class State { Idle, Resolving, Running, Done, Failed, Cancelled };
    State state = State::Idle;
    std::string target;          /* as typed */
    std::string address;         /* the address pinged */
    std::string resolved_by;     /* "", "hosts", or the DNS server */
    std::string error;           /* Failed: why; Running: the last send error ("" when none) */
    PingStats stats;
    std::vector<PacketResult> recent; /* oldest first, at most kRingSize */
    bool running() const { return state == State::Resolving || state == State::Running; }
};

/* The engine (the session's thread; tests call it directly with a fake socket). Publishes a snapshot after every event
 * (a send, a reply, a loss) through `publish`; returns the last one. */
PingSnapshot run_ping(const PingConfig &cfg, const EchoFactory &factory, const CancelToken &cancel,
                      const std::function<void(const PingSnapshot &)> &publish);

class PingSession {
public:
    PingSession(); /* the real ICMP socket */
    explicit PingSession(EchoFactory factory);
    bool start(const PingConfig &cfg); /* LVGL thread; a running ping is stopped first */
    void stop();                      /* the snapshot goes to Cancelled within kSliceMs */
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const PingSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    EchoFactory factory_;
    Channel<PingSnapshot> channel_;
    Worker worker_;
};

} // namespace nt
