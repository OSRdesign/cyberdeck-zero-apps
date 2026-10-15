/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: ping (see ping.hpp).
 */
#include "ping.hpp"

#include "sock.hpp"
#include "text.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace nt {

/* ---------------------------------------------------------------- statistics */

double PingStats::slow_threshold_ms() const
{
    return std::max(kSlowFloorMs, kSlowFactor * avg_ms());
}

PacketState PingStats::add_reply(double rtt_ms)
{
    if (rtt_ms < 0) rtt_ms = 0;
    const PacketState state = rtt_ms > slow_threshold_ms() ? PacketState::Slow : PacketState::Ok;
    if (!received || rtt_ms < min_ms) min_ms = rtt_ms;
    if (!received || rtt_ms > max_ms) max_ms = rtt_ms;
    sum_ms += rtt_ms;
    ++received;
    if (has_last) {
        jitter_sum += rtt_ms > last_ms ? rtt_ms - last_ms : last_ms - rtt_ms;
        ++jitter_n;
    }
    has_last = true;
    last_ms = rtt_ms;
    return state;
}

void PacketRing::push(const PacketResult &p)
{
    buf_[head_] = p;
    head_ = (head_ + 1) % kRingSize;
    if (count_ < kRingSize) ++count_;
}

PacketResult *PacketRing::find(std::uint32_t number)
{
    for (std::size_t i = 0; i < count_; ++i) {
        const std::size_t at = (head_ + kRingSize - 1 - i) % kRingSize; /* newest first: the usual case */
        if (buf_[at].number == number) return &buf_[at];
    }
    return nullptr;
}

std::vector<PacketResult> PacketRing::items() const
{
    std::vector<PacketResult> out;
    out.reserve(count_);
    for (std::size_t i = 0; i < count_; ++i) out.push_back(buf_[(head_ + kRingSize - count_ + i) % kRingSize]);
    return out;
}

/* ---------------------------------------------------------------- the socket */

namespace {

std::string read_small(const char *path)
{
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::string();
    char buf[256];
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
}

std::uint16_t checksum(const std::uint8_t *p, std::size_t n)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) sum += static_cast<std::uint32_t>((p[i] << 8) | p[i + 1]);
    if (n & 1) sum += static_cast<std::uint32_t>(p[n - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum & 0xFFFF);
}

class IcmpEcho : public EchoSocket {
public:
    IcmpEcho(int fd, const Address &target, int payload) : fd_(fd), payload_(payload)
    {
        len_ = target.to_sockaddr(0, addr_);
        v6_ = target.family == AF_INET6;
        if (getrandom(cookie_, sizeof(cookie_), GRND_NONBLOCK) != static_cast<ssize_t>(sizeof(cookie_))) {
            const std::int64_t t = now_us() ^ (static_cast<std::int64_t>(getpid()) << 20);
            std::memcpy(cookie_, &t, sizeof(cookie_));
        }
        packet_.assign(8 + static_cast<std::size_t>(payload_), 0);
        for (std::size_t i = 16; i < packet_.size(); ++i) packet_[i] = static_cast<std::uint8_t>(i);
        std::memcpy(packet_.data() + 8, cookie_, sizeof(cookie_));
        reply_.assign(packet_.size() + 64, 0);
    }
    int send(std::uint16_t seq) override
    {
        packet_[0] = v6_ ? 128 : 8; /* echo request */
        packet_[1] = 0;
        packet_[2] = packet_[3] = 0;
        packet_[4] = packet_[5] = 0; /* identifier: the kernel's */
        packet_[6] = static_cast<std::uint8_t>(seq >> 8);
        packet_[7] = static_cast<std::uint8_t>(seq & 0xFF);
        if (!v6_) { /* the kernel computes it for ICMPv6 */
            const std::uint16_t c = checksum(packet_.data(), packet_.size());
            packet_[2] = static_cast<std::uint8_t>(c >> 8);
            packet_[3] = static_cast<std::uint8_t>(c & 0xFF);
        }
        for (;;) {
            const ssize_t n = sendto(fd_.get(), packet_.data(), packet_.size(), MSG_NOSIGNAL,
                                     reinterpret_cast<const sockaddr *>(&addr_), len_);
            if (n >= 0) return 0;
            if (errno == EINTR) continue;
            return errno;
        }
    }
    int receive(int timeout_ms, std::uint16_t &seq) override
    {
        const std::int64_t deadline = now_ms() + std::max(0, timeout_ms);
        for (;;) {
            const ssize_t n = recv(fd_.get(), reply_.data(), reply_.size(), 0);
            if (n >= 16) {
                const std::uint8_t want = v6_ ? 129 : 0; /* echo reply */
                if (reply_[0] == want && std::memcmp(reply_.data() + 8, cookie_, sizeof(cookie_)) == 0) {
                    seq = static_cast<std::uint16_t>((reply_[6] << 8) | reply_[7]);
                    return 1;
                }
                continue; /* not ours: look again */
            }
            if (n >= 0) continue; /* too short to be ours */
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return -errno;
            const std::int64_t left = deadline - now_ms();
            if (left <= 0) return 0;
            struct pollfd p = {fd_.get(), POLLIN, 0};
            const int rc = poll(&p, 1, static_cast<int>(left));
            if (rc == 0) return 0;
            if (rc < 0 && errno != EINTR) return -errno;
        }
    }

private:
    Fd fd_;
    int payload_;
    sockaddr_storage addr_{};
    socklen_t len_ = 0;
    bool v6_ = false;
    std::uint8_t cookie_[8] = {};
    std::vector<std::uint8_t> packet_, reply_;
};

int open_icmp_socket(int family)
{
    const int proto = family == AF_INET6 ? static_cast<int>(IPPROTO_ICMPV6) : static_cast<int>(IPPROTO_ICMP);
    const int fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, proto);
    return fd >= 0 ? fd : -errno;
}

} // namespace

bool ping_group_allows(const std::string &range_text, unsigned gid)
{
    const std::vector<std::string> f = split_ws(range_text);
    std::uint64_t lo = 0, hi = 0;
    if (f.size() < 2 || !parse_u64(f[0], lo) || !parse_u64(f[1], hi)) return false;
    return lo <= gid && gid <= hi;
}

std::string ping_unavailable_words(int err, const std::string &range_text)
{
    if (err == EACCES || err == EPERM) {
        std::string range = trim(range_text);
        for (char &c : range)
            if (c == '\t') c = ' ';
        std::string s = "Ping is not allowed for this user";
        if (!range.empty()) s += " (net.ipv4.ping_group_range is \"" + range + "\")";
        return s + ". Fix: sudo sysctl -w net.ipv4.ping_group_range=\"0 2147483647\", and add that line to a file in "
                   "/etc/sysctl.d/ to keep it after a restart.";
    }
    if (err == EPROTONOSUPPORT || err == EAFNOSUPPORT || err == ESOCKTNOSUPPORT)
        return "This system has no unprivileged ping socket (the kernel does not offer ICMP datagram sockets).";
    return "The ping socket cannot be opened: " + errno_words(err) + ".";
}

PingAvailability ping_available()
{
    PingAvailability a;
    int fd = open_icmp_socket(AF_INET);
    if (fd >= 0) {
        a.ipv4 = true;
        close(fd);
    } else {
        a.error = -fd;
        a.why = ping_unavailable_words(a.error, read_small("/proc/sys/net/ipv4/ping_group_range"));
    }
    fd = open_icmp_socket(AF_INET6);
    if (fd >= 0) {
        a.ipv6 = true;
        close(fd);
    }
    return a;
}

std::unique_ptr<EchoSocket> open_icmp_echo(const Address &target, int payload_bytes, int *err)
{
    if (!target.valid()) {
        if (err) *err = EAFNOSUPPORT;
        return nullptr;
    }
    const int fd = open_icmp_socket(target.family);
    if (fd < 0) {
        if (err) *err = -fd;
        return nullptr;
    }
    if (const int be = bind_to_active_device(fd)) { /* the chosen network (sock.hpp) */
        close(fd);
        if (err) *err = be;
        return nullptr;
    }
    if (err) *err = 0;
    return std::make_unique<IcmpEcho>(fd, target, std::min(1400, std::max(8, payload_bytes)));
}

/* ---------------------------------------------------------------- the engine */

PingSnapshot run_ping(const PingConfig &input, const EchoFactory &factory, const CancelToken &cancel,
                      const std::function<void(const PingSnapshot &)> &publish)
{
    PingConfig cfg = input;
    cfg.interval_ms = std::min(60000, std::max(200, cfg.interval_ms));
    cfg.timeout_ms = std::min(60000, std::max(100, cfg.timeout_ms));
    cfg.count = std::max(0, cfg.count);
    PingSnapshot snap;
    snap.target = trim(cfg.target);
    snap.state = PingSnapshot::State::Resolving;
    const auto emit = [&] {
        if (publish) publish(snap);
    };
    emit();

    const Resolved res = resolve_host(snap.target, cfg.resolve, cancel);
    if (cancel.cancelled()) {
        snap.state = PingSnapshot::State::Cancelled;
        emit();
        return snap;
    }
    if (!res.ok) {
        snap.state = PingSnapshot::State::Failed;
        snap.error = "Cannot find " + snap.target + ": " + res.error;
        emit();
        return snap;
    }
    snap.address = res.address.text();
    snap.resolved_by = res.literal ? "" : res.from_hosts ? "hosts" : res.server;
    int err = 0;
    std::unique_ptr<EchoSocket> sock = factory ? factory(res.address, cfg.payload_bytes, &err) : nullptr;
    if (!sock) {
        snap.state = PingSnapshot::State::Failed;
        snap.error = err == EACCES || err == EPERM
                         ? ping_unavailable_words(err, read_small("/proc/sys/net/ipv4/ping_group_range"))
                         : "The ping socket cannot be opened: " + errno_words(err ? err : EIO);
        emit();
        return snap;
    }
    snap.state = PingSnapshot::State::Running;
    emit();

    struct Waiting {
        std::uint32_t number;
        std::int64_t sent_us;
    };
    std::vector<Waiting> waiting;
    PacketRing ring;
    std::uint32_t number = 0;
    std::int64_t next_send = now_ms();
    const auto more_to_send = [&] { return cfg.count == 0 || number < static_cast<std::uint32_t>(cfg.count); };

    while (!cancel.cancelled()) {
        bool changed = false;
        std::int64_t now = now_ms();
        if (more_to_send() && now >= next_send) {
            ++number;
            ring.push({number, PacketState::Pending, 0});
            snap.stats.add_sent();
            const int e = sock->send(static_cast<std::uint16_t>(number & 0xFFFF));
            if (e) {
                ring.find(number)->state = PacketState::Lost;
                snap.stats.add_lost();
                snap.error = "Send failed: " + errno_words(e);
            } else {
                waiting.push_back({number, now_us()});
                snap.error.clear();
            }
            next_send += cfg.interval_ms;
            if (next_send <= now) next_send = now + cfg.interval_ms; /* after a stall: no burst */
            changed = true;
        }
        const std::int64_t now_u = now_us();
        for (auto it = waiting.begin(); it != waiting.end();) {
            if (now_u - it->sent_us >= static_cast<std::int64_t>(cfg.timeout_ms) * 1000) {
                if (PacketResult *p = ring.find(it->number)) p->state = PacketState::Lost;
                snap.stats.add_lost();
                it = waiting.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) {
            snap.recent = ring.items();
            emit();
        }
        if (!more_to_send() && waiting.empty()) {
            snap.state = PingSnapshot::State::Done;
            break;
        }
        now = now_ms();
        std::int64_t wait = kSliceMs;
        if (more_to_send()) wait = std::min(wait, next_send - now);
        for (const Waiting &w : waiting)
            wait = std::min(wait, (w.sent_us + static_cast<std::int64_t>(cfg.timeout_ms) * 1000 - now_u) / 1000 + 1);
        wait = std::max<std::int64_t>(wait, 0);
        std::uint16_t seq = 0;
        const int r = sock->receive(static_cast<int>(wait), seq);
        if (r == 1) {
            const std::int64_t at = now_us();
            for (auto it = waiting.begin(); it != waiting.end(); ++it) {
                if ((it->number & 0xFFFF) != seq) continue;
                const double rtt = static_cast<double>(at - it->sent_us) / 1000.0;
                const PacketState state = snap.stats.add_reply(rtt);
                if (PacketResult *p = ring.find(it->number)) {
                    p->state = state;
                    p->rtt_ms = rtt;
                }
                waiting.erase(it);
                snap.recent = ring.items();
                emit();
                break;
            } /* a duplicate or a reply after its timeout: ignored */
        } else if (r < 0) {
            snap.error = "Receive failed: " + errno_words(-r);
            if (!cancel.sleep_ms(static_cast<int>(std::max<std::int64_t>(wait, 10)))) break;
        }
    }
    if (snap.state != PingSnapshot::State::Done) snap.state = PingSnapshot::State::Cancelled;
    snap.recent = ring.items();
    emit();
    return snap;
}

PingSession::PingSession()
    : factory_([](const Address &target, int payload, int *err) { return open_icmp_echo(target, payload, err); })
{
}

PingSession::PingSession(EchoFactory factory) : factory_(std::move(factory)) {}

bool PingSession::start(const PingConfig &cfg)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    PingSnapshot first;
    first.target = trim(cfg.target);
    std::string why;
    if (!valid_host(first.target, &why)) {
        first.state = PingSnapshot::State::Failed;
        first.error = why;
        channel_.begin(gen, first);
        return false;
    }
    first.state = PingSnapshot::State::Resolving;
    channel_.begin(gen, first);
    const EchoFactory factory = factory_;
    const std::uint64_t started = worker_.start([this, gen, cfg, factory](const CancelToken &cancel) {
        run_ping(cfg, factory, cancel, [this, gen](const PingSnapshot &s) { channel_.publish(gen, s); });
    });
    if (!started) {
        first.state = PingSnapshot::State::Failed;
        first.error = "Could not start the ping (no thread)";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void PingSession::stop()
{
    worker_.cancel();
}

} // namespace nt
