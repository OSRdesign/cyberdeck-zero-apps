/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: Wake-on-LAN (see wol.hpp).
 */
#include "wol.hpp"

#include "netaddr.hpp"
#include "sock.hpp"
#include "text.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nt {
namespace {

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool fail(std::string *why, const char *words)
{
    if (why) *why = words;
    return false;
}

/* the IPv4 address of "a.b.c.d/nn" (or "a.b.c.d") and its prefix */
bool split_cidr(const std::string &cidr, Address &addr, int &prefix)
{
    const std::string t = trim(cidr);
    const std::size_t slash = t.find('/');
    prefix = 32;
    if (slash != std::string::npos && (!parse_int(t.substr(slash + 1), prefix) || prefix < 0 || prefix > 32)) return false;
    return parse_ip(t.substr(0, slash), addr) && addr.family == AF_INET;
}

} // namespace

std::string MacAddr::text() const
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return buf;
}

bool MacAddr::operator==(const MacAddr &o) const
{
    return std::memcmp(b, o.b, sizeof(b)) == 0;
}

bool parse_mac(const std::string &input, MacAddr &out, std::string *why)
{
    const std::string s = trim(input);
    if (s.empty()) return fail(why, "Type the machine's MAC address, like aa:bb:cc:dd:ee:ff.");
    if (s.size() != 12 && s.size() != 17)
        return fail(why, "A MAC address has 12 hex digits: aa:bb:cc:dd:ee:ff, aa-bb-cc-dd-ee-ff or aabbccddeeff.");
    MacAddr m;
    if (s.size() == 17) {
        const char sep = s[2];
        if (sep != ':' && sep != '-')
            return fail(why, "Separate the pairs with ':' or '-', like aa:bb:cc:dd:ee:ff.");
        for (int i = 0; i < 6; ++i) {
            const std::size_t at = static_cast<std::size_t>(i) * 3;
            if (i < 5 && s[at + 2] != sep) return fail(why, "Use one separator all along: aa:bb:cc:dd:ee:ff or aa-bb-cc-dd-ee-ff.");
            const int hi = hex_digit(s[at]), lo = hex_digit(s[at + 1]);
            if (hi < 0 || lo < 0) return fail(why, "A MAC address has hex digits only (0-9, a-f).");
            m.b[i] = static_cast<std::uint8_t>(hi * 16 + lo);
        }
    } else {
        for (int i = 0; i < 6; ++i) {
            const int hi = hex_digit(s[static_cast<std::size_t>(i) * 2]), lo = hex_digit(s[static_cast<std::size_t>(i) * 2 + 1]);
            if (hi < 0 || lo < 0) return fail(why, "A MAC address has hex digits only (0-9, a-f).");
            m.b[i] = static_cast<std::uint8_t>(hi * 16 + lo);
        }
    }
    bool zero = true, ones = true;
    for (const std::uint8_t x : m.b) {
        if (x) zero = false;
        if (x != 0xFF) ones = false;
    }
    if (zero) return fail(why, "00:00:00:00:00:00 is not a machine's address.");
    if (ones) return fail(why, "ff:ff:ff:ff:ff:ff is the broadcast address, not a machine's.");
    if (m.b[0] & 1) return fail(why, "That is a multicast address (its first byte is odd), not a network card's.");
    out = m;
    return true;
}

std::vector<std::uint8_t> magic_packet(const MacAddr &mac)
{
    std::vector<std::uint8_t> p(kMagicPacketSize, 0xFF);
    for (std::size_t i = 0; i < 16; ++i) std::memcpy(p.data() + 6 + i * 6, mac.b, 6);
    return p;
}

bool normalize_machine(WolMachine &m, std::string *why)
{
    WolMachine n;
    n.name = trim(m.name);
    if (n.name.empty()) return fail(why, "Give the machine a name.");
    if (n.name.size() > kWolNameMax) return fail(why, "The name is too long (32 characters at most).");
    for (const char c : n.name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return fail(why, "The name has a control character.");
        if (c == '|') return fail(why, "The name cannot hold '|'.");
    }
    MacAddr mac;
    std::string mac_why;
    if (!parse_mac(m.mac, mac, &mac_why)) {
        if (why) *why = mac_why;
        return false;
    }
    n.mac = mac.text();
    n.host = trim(m.host);
    if (!n.host.empty()) {
        std::string host_why;
        if (n.host.size() > 253 || !valid_host(n.host, &host_why)) {
            if (why) *why = host_why.empty() ? "The host to watch is not a valid name or address." : host_why;
            return false;
        }
    }
    n.broadcast = trim(m.broadcast);
    if (!n.broadcast.empty()) {
        Address a;
        if (!parse_ip(n.broadcast, a) || a.family != AF_INET)
            return fail(why, "The broadcast address must be an IPv4 address, like 192.168.1.255.");
        n.broadcast = a.text();
    }
    if (m.port < 1 || m.port > 65535) return fail(why, "The port must be between 1 and 65535 (usually 9).");
    n.port = m.port;
    m = n;
    return true;
}

std::string machine_to_setting(const WolMachine &m)
{
    return m.name + "|" + m.mac + "|" + m.host + "|" + m.broadcast + "|" + std::to_string(m.port);
}

bool machine_from_setting(const std::string &value, WolMachine &out)
{
    std::vector<std::string> f;
    std::size_t from = 0;
    for (;;) {
        const std::size_t bar = value.find('|', from);
        f.push_back(value.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
        if (bar == std::string::npos || f.size() > 5) break;
        from = bar + 1;
    }
    if (f.size() != 5) return false;
    WolMachine m;
    m.name = f[0];
    m.mac = f[1];
    m.host = f[2];
    m.broadcast = f[3];
    if (!parse_int(f[4], m.port)) return false;
    if (!normalize_machine(m)) return false;
    out = m;
    return true;
}

std::string WolDestination::label() const
{
    std::string s = address + " port " + std::to_string(port);
    if (!iface.empty()) s += " on " + iface;
    return s;
}

bool directed_broadcast(const std::string &cidr, std::string &out)
{
    Address a;
    int prefix = 32;
    if (!split_cidr(cidr, a, prefix) || prefix >= 31 || trim(cidr).find('/') == std::string::npos) return false;
    std::uint32_t ip = (static_cast<std::uint32_t>(a.bytes[0]) << 24) | (static_cast<std::uint32_t>(a.bytes[1]) << 16) |
                       (static_cast<std::uint32_t>(a.bytes[2]) << 8) | a.bytes[3];
    const std::uint32_t host_mask = prefix == 0 ? 0xFFFFFFFFu : (0xFFFFFFFFu >> prefix);
    ip |= host_mask;
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    out = buf;
    return true;
}

std::vector<WolDestination> plan_wol(const WolMachine &m, const NetState &net, const std::string &iface)
{
    std::vector<WolDestination> plan;
    std::vector<std::uint16_t> ports = {static_cast<std::uint16_t>(m.port)};
    if (m.port == 9) ports.push_back(7);
    if (!trim(m.broadcast).empty()) {
        for (const std::uint16_t p : ports) {
            WolDestination d;
            d.address = trim(m.broadcast);
            d.port = p;
            plan.push_back(d);
        }
        return plan;
    }
    for (const Iface &i : net.ifaces) {
        if (i.loopback || !i.up || i.ipv4.empty()) continue;
        if (!iface.empty() && i.name != iface) continue;
        for (const std::string &cidr : i.ipv4) {
            Address a;
            int prefix = 32;
            if (!split_cidr(cidr, a, prefix)) continue;
            std::vector<std::string> targets = {"255.255.255.255"};
            std::string directed;
            if (directed_broadcast(cidr, directed) && directed != targets[0]) targets.push_back(directed);
            for (const std::string &t : targets)
                for (const std::uint16_t p : ports) {
                    WolDestination d;
                    d.iface = i.name;
                    d.source = a.text();
                    d.address = t;
                    d.port = p;
                    plan.push_back(d);
                }
        }
    }
    return plan;
}

int UdpSender::send(const WolDestination &d, const std::vector<std::uint8_t> &payload)
{
    Address dst;
    if (!parse_ip(d.address, dst) || dst.family != AF_INET) return EDESTADDRREQ;
    Fd fd(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (!fd.valid()) return errno;
    const int on = 1;
    if (setsockopt(fd.get(), SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0) return errno;
    /* 0.3: the packet leaves by the interface it is planned for (no capability on Linux 5.7+; an older kernel says
     * EPERM and the source address below still steers it). The interface of the plan, not the chosen network: Automatic
     * sends on every interface that is up. */
    if (const int be = bind_to_device(fd.get(), d.iface)) {
        if (be != EPERM) return be;
    }
    if (!d.source.empty()) {
        Address src;
        if (!parse_ip(d.source, src) || src.family != AF_INET) return EADDRNOTAVAIL;
        sockaddr_storage sa{};
        const socklen_t len = src.to_sockaddr(0, sa);
        if (bind(fd.get(), reinterpret_cast<const sockaddr *>(&sa), len) != 0) return errno;
    }
    sockaddr_storage to{};
    const socklen_t to_len = dst.to_sockaddr(d.port, to);
    for (;;) {
        const ssize_t n = sendto(fd.get(), payload.data(), payload.size(), MSG_NOSIGNAL | MSG_DONTWAIT,
                                 reinterpret_cast<const sockaddr *>(&to), to_len);
        if (n == static_cast<ssize_t>(payload.size())) return 0;
        if (n < 0 && errno == EINTR) continue;
        return n < 0 ? errno : EIO;
    }
}

WolSendResult send_magic(const MacAddr &mac, const std::vector<WolDestination> &plan, DatagramSender &sender)
{
    WolSendResult r;
    const std::vector<std::uint8_t> packet = magic_packet(mac);
    std::vector<std::string> ifaces;
    std::string first_error;
    for (const WolDestination &d : plan) {
        WolPacket p;
        p.dest = d;
        p.error = sender.send(d, packet);
        p.words = p.error ? errno_words(p.error) : "sent";
        if (p.error) {
            ++r.errors;
            if (first_error.empty()) first_error = p.words;
        } else {
            ++r.sent;
            const std::string where = d.iface.empty() ? std::string() : d.iface;
            if (!where.empty() && std::find(ifaces.begin(), ifaces.end(), where) == ifaces.end()) ifaces.push_back(where);
        }
        r.packets.push_back(p);
    }
    if (plan.empty()) {
        r.summary = "Nothing to send on: no interface is up with an IPv4 address.";
    } else if (r.sent == 0) {
        r.summary = "Nothing could be sent: " + first_error + ".";
    } else {
        r.summary = std::to_string(r.sent) + (r.sent == 1 ? " packet sent" : " packets sent");
        if (!ifaces.empty()) {
            r.summary += " on";
            for (std::size_t i = 0; i < ifaces.size(); ++i) r.summary += (i ? ", " : " ") + ifaces[i];
        }
        if (r.errors) r.summary += "; " + std::to_string(r.errors) + " failed (" + first_error + ")";
        r.summary += ".";
    }
    return r;
}

/* ---------------------------------------------------------------- the tool */

WolSnapshot run_wol(const WolRequest &req, DatagramSender &sender, const EchoFactory &echo, const CancelToken &cancel,
                    const std::function<void(const WolSnapshot &)> &publish)
{
    WolSnapshot snap;
    snap.state = WolSnapshot::State::Sending;
    snap.machine = trim(req.machine.name);
    const auto emit = [&] {
        if (publish) publish(snap);
    };
    emit();
    WolMachine m = req.machine;
    std::string why;
    MacAddr mac;
    if (!normalize_machine(m, &why) || !parse_mac(m.mac, mac)) {
        snap.state = WolSnapshot::State::Failed;
        snap.error = why;
        emit();
        return snap;
    }
    snap.machine = m.name;
    snap.mac = m.mac;
    const std::vector<WolDestination> plan = plan_wol(m, req.net, req.iface);
    if (plan.empty()) {
        snap.state = WolSnapshot::State::Failed;
        snap.error = req.iface.empty() ? "No interface is up with an IPv4 address: there is nothing to send on."
                                       : req.iface + " is not up or has no IPv4 address.";
        emit();
        return snap;
    }
    snap.send = send_magic(mac, plan, sender);
    if (snap.send.sent == 0) {
        snap.state = WolSnapshot::State::Failed;
        snap.error = snap.send.summary;
        emit();
        return snap;
    }
    const std::int64_t sent_at = now_ms();
    if (!req.watch || m.host.empty()) {
        snap.state = WolSnapshot::State::Done;
        emit();
        return snap;
    }

    /* the wake watch */
    snap.state = WolSnapshot::State::Watching;
    snap.watch = true;
    snap.watch_target = m.host;
    snap.watch_words = "no answer yet (0 s)";
    emit();
    const int interval = std::min(10000, std::max(20, req.watch_interval_ms));
    const int max_ms = std::min(600000, std::max(100, req.watch_max_ms));
    const auto done = [&](WolSnapshot::State state) {
        snap.state = state;
        emit();
        return snap;
    };
    const Resolved res = resolve_host(m.host, req.resolve, cancel);
    if (cancel.cancelled()) return done(WolSnapshot::State::Cancelled);
    if (!res.ok) {
        snap.watch_words = "cannot watch: cannot find " + m.host + " (" + res.error + ")";
        return done(WolSnapshot::State::Done);
    }
    snap.watch_address = res.address.text();
    int err = 0;
    std::unique_ptr<EchoSocket> sock = echo ? echo(res.address, 56, &err) : nullptr;
    if (!sock) {
        snap.watch_words = "cannot watch: " + (err == EACCES || err == EPERM
                                                    ? std::string("ping is not allowed for this user")
                                                    : "the ping socket cannot be opened (" + errno_words(err ? err : EIO) + ")");
        return done(WolSnapshot::State::Done);
    }
    std::uint16_t seq = 0;
    std::int64_t next = now_ms();
    while (!cancel.cancelled()) {
        const std::int64_t now = now_ms();
        const std::int64_t waited = now - sent_at;
        const int waited_s = static_cast<int>(waited / 1000);
        if (waited_s != snap.waited_s) {
            snap.waited_s = waited_s;
            snap.watch_words = "no answer yet (" + std::to_string(waited_s) + " s)";
            emit();
        }
        if (waited >= max_ms) {
            snap.watch_words = "no answer after " + std::to_string(max_ms / 1000) + " s";
            snap.hint = kWolHint;
            return done(WolSnapshot::State::Done);
        }
        if (now >= next) {
            ++seq;
            ++snap.attempts;
            sock->send(seq); /* a send error: no answer this time, the next try follows */
            next += interval;
            if (next <= now) next = now + interval;
            emit();
        }
        const int wait = static_cast<int>(std::max<std::int64_t>(1, std::min<std::int64_t>(kSliceMs, next - now_ms())));
        std::uint16_t got = 0;
        const int rc = sock->receive(wait, got);
        if (rc == 1 && got >= 1 && got <= seq) {
            snap.answered = true;
            snap.answered_after_s = static_cast<double>(now_ms() - sent_at) / 1000.0;
            snap.answered_at_once = got == 1 && snap.answered_after_s < 2.0;
            if (snap.answered_at_once) {
                snap.watch_words = "answers at once: it was probably awake already";
            } else {
                const long s = std::max(1L, std::lround(snap.answered_after_s));
                snap.watch_words = "answers after " + std::to_string(s) + " s";
            }
            return done(WolSnapshot::State::Done);
        }
        if (rc < 0 && !cancel.sleep_ms(wait)) break;
    }
    return done(WolSnapshot::State::Cancelled);
}

WolSession::WolSession()
    : sender_(std::make_shared<UdpSender>()),
      echo_([](const Address &target, int payload, int *err) { return open_icmp_echo(target, payload, err); })
{
}

WolSession::WolSession(std::shared_ptr<DatagramSender> sender, EchoFactory echo)
    : sender_(std::move(sender)), echo_(std::move(echo))
{
}

bool WolSession::start(const WolRequest &req)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    WolSnapshot first;
    first.state = WolSnapshot::State::Sending;
    first.machine = trim(req.machine.name);
    channel_.begin(gen, first);
    const std::shared_ptr<DatagramSender> sender = sender_;
    const EchoFactory echo = echo_;
    const std::uint64_t started = worker_.start([this, gen, req, sender, echo](const CancelToken &cancel) {
        run_wol(req, *sender, echo, cancel, [this, gen](const WolSnapshot &s) { channel_.publish(gen, s); });
    });
    if (!started) {
        first.state = WolSnapshot::State::Failed;
        first.error = "Could not start (no thread).";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void WolSession::stop()
{
    worker_.cancel();
}

} // namespace nt
