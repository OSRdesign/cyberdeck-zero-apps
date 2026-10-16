/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: device names (see lannames.hpp).
 */
#include "lannames.hpp"

#include "dns.hpp"
#include "text.hpp"
#include "worker.hpp"

#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace nt {

const char *name_source_label(NameSource s)
{
    switch (s) {
    case NameSource::Mdns: return "mDNS";
    case NameSource::Netbios: return "NetBIOS";
    case NameSource::ReverseDns: return "reverse DNS";
    case NameSource::None: break;
    }
    return "";
}

std::string trim_local_suffix(std::string name)
{
    static const char *suffixes[] = {".local", ".lan", ".home", ".fritz.box", ".home.arpa"};
    for (const char *suffix : suffixes) {
        const std::size_t n = std::strlen(suffix);
        if (name.size() > n && lower(name.substr(name.size() - n)) == suffix) {
            name.erase(name.size() - n);
            break;
        }
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    return printable(name);
}

/* ---------------------------------------------------------------- mDNS */

std::vector<std::uint8_t> build_mdns_reverse_query(const Address &a, std::uint16_t id)
{
    std::vector<std::uint8_t> out;
    const std::string qname = reverse_name(a);
    if (qname.empty()) return out;
    dns::QueryOptions opt;
    opt.rd = false;  /* mDNS questions do not set recursion desired */
    opt.edns = false;
    if (!dns::build_query(qname, dns::kPTR, id, opt, out) || out.size() < 4) {
        out.clear();
        return out;
    }
    out[out.size() - 2] |= 0x80; /* the QU bit (unicast response wanted) on the question class */
    return out;
}

std::string parse_mdns_name(const std::uint8_t *buf, std::size_t len)
{
    dns::Message m;
    if (!dns::parse_message(buf, len, m)) return std::string();
    for (const dns::Record &r : m.answers)
        if (r.type == dns::kPTR && !r.data.empty() && r.data.find('(') == std::string::npos)
            return trim_local_suffix(r.data);
    return std::string();
}

/* ---------------------------------------------------------------- NetBIOS */

std::vector<std::uint8_t> build_nbstat_query(std::uint16_t id)
{
    std::vector<std::uint8_t> q;
    q.reserve(50);
    q.push_back(static_cast<std::uint8_t>(id >> 8));
    q.push_back(static_cast<std::uint8_t>(id & 0xFF));
    q.push_back(0);
    q.push_back(0); /* flags */
    q.push_back(0);
    q.push_back(1); /* qdcount = 1 */
    for (int i = 0; i < 6; ++i) q.push_back(0); /* an, ns, ar counts */
    /* the first-level-encoded wildcard name "*": length 0x20, then "CK" (for '*') and "AA" for each of 15 nulls */
    q.push_back(0x20);
    q.push_back('C');
    q.push_back('K');
    for (int i = 0; i < 30; ++i) q.push_back('A');
    q.push_back(0);    /* root label terminator */
    q.push_back(0);    /* type 0x0021 NBSTAT */
    q.push_back(0x21);
    q.push_back(0);    /* class 0x0001 IN */
    q.push_back(1);
    return q;
}

std::string parse_nbstat_name(const std::uint8_t *buf, std::size_t len)
{
    if (len < 12) return std::string();
    const int qd = (buf[4] << 8) | buf[5];
    const int an = (buf[6] << 8) | buf[7];
    if (an < 1) return std::string();
    std::size_t pos = 12;
    std::string skip;
    for (int i = 0; i < qd; ++i) {
        std::size_t next = 0;
        if (!dns::read_name(buf, len, pos, skip, &next)) return std::string();
        pos = next + 4; /* qtype + qclass */
        if (pos > len) return std::string();
    }
    /* the first answer's name */
    std::size_t next = 0;
    if (!dns::read_name(buf, len, pos, skip, &next)) return std::string();
    pos = next;
    if (pos + 11 > len) return std::string(); /* type(2) class(2) ttl(4) rdlength(2) + count(1) */
    pos += 10;                                /* skip type, class, ttl, rdlength */
    const int count = buf[pos++];
    for (int i = 0; i < count && pos + 18 <= len; ++i, pos += 18) {
        const int suffix = buf[pos + 15];
        const int flags = (buf[pos + 16] << 8) | buf[pos + 17];
        if (suffix == 0x00 && !(flags & 0x8000)) { /* a unique (not group) workstation name */
            std::string host(reinterpret_cast<const char *>(buf + pos), 15);
            while (!host.empty() && (host.back() == ' ' || host.back() == '\0')) host.pop_back();
            if (!host.empty()) return printable(host);
        }
    }
    return std::string();
}

/* ---------------------------------------------------------------- priority */

NamePick best_name(const std::string &mdns, const std::string &netbios, const std::string &rdns)
{
    NamePick p;
    const std::string m = trim(mdns);
    const std::string n = trim(netbios);
    const std::string r = trim_local_suffix(rdns);
    if (!m.empty()) {
        p.name = m;
        p.source = NameSource::Mdns;
    } else if (!n.empty()) {
        p.name = n;
        p.source = NameSource::Netbios;
    } else if (!r.empty()) {
        p.name = r;
        p.source = NameSource::ReverseDns;
    }
    return p;
}

/* ---------------------------------------------------------------- the driver */

namespace {

Fd open_unicast_udp()
{
    Fd fd(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd.valid()) return Fd();
    if (bind_to_active_device(fd.get()) != 0) return Fd(); /* a chosen interface that is gone */
    return fd;
}

void send_to(const Fd &fd, const Address &a, std::uint16_t port, const std::vector<std::uint8_t> &data)
{
    if (!fd.valid() || data.empty()) return;
    sockaddr_storage ss;
    const socklen_t sl = a.to_sockaddr(port, ss);
    if (!sl) return;
    ::sendto(fd.get(), data.data(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT,
             reinterpret_cast<const sockaddr *>(&ss), sl);
}

} // namespace

LocalNames::LocalNames(int timeout_ms) : timeout_ms_(std::max(100, timeout_ms)), next_id_(0)
{
    if (getrandom(&next_id_, sizeof(next_id_), GRND_NONBLOCK) != static_cast<ssize_t>(sizeof(next_id_)))
        next_id_ = static_cast<std::uint16_t>(now_us() ^ getpid());
    mdns_ = open_unicast_udp();
    nbns_ = open_unicast_udp();
}

void LocalNames::want(const Address &a)
{
    if (!ready() || a.family != AF_INET || entries_.size() >= kMaxEntries) return;
    const std::uint32_t h = ipv4_to_host_order(a);
    if (!range_.contains(h) || h == range_.self) return; /* only hosts of the scan range, unicast */
    for (const Entry &e : entries_)
        if (e.addr == a) return;
    Entry e;
    e.addr = a;
    e.id = next_id_++;
    entries_.push_back(e);
    send(entries_.back());
}

void LocalNames::send(Entry &e)
{
    e.sent_ms = now_ms();
    e.sent = true;
    send_to(mdns_, e.addr, 5353, build_mdns_reverse_query(e.addr, e.id));
    send_to(nbns_, e.addr, 137, build_nbstat_query(e.id));
}

void LocalNames::absorb_mdns(const std::uint8_t *buf, std::size_t len, const Address &from)
{
    const std::string name = parse_mdns_name(buf, len);
    if (name.empty()) return;
    for (Entry &e : entries_)
        if (e.addr == from && e.mdns_name.empty()) e.mdns_name = name;
}

void LocalNames::absorb_nbns(const std::uint8_t *buf, std::size_t len, const Address &from)
{
    const std::string name = parse_nbstat_name(buf, len);
    if (name.empty()) return;
    for (Entry &e : entries_)
        if (e.addr == from && e.nbns_name.empty()) e.nbns_name = name;
}

void LocalNames::service()
{
    if (!ready()) return;
    std::uint8_t buf[1500];
    for (int which = 0; which < 2; ++which) {
        const Fd &fd = which == 0 ? mdns_ : nbns_;
        if (!fd.valid()) continue;
        for (;;) {
            sockaddr_storage ss;
            socklen_t sl = sizeof(ss);
            const ssize_t n = recvfrom(fd.get(), buf, sizeof(buf), MSG_DONTWAIT,
                                       reinterpret_cast<sockaddr *>(&ss), &sl);
            if (n < 0) {
                if (errno == EINTR) continue;
                break; /* EAGAIN or an earlier error: nothing more now */
            }
            const Address from = Address::from_sockaddr(reinterpret_cast<const sockaddr *>(&ss));
            if (which == 0) absorb_mdns(buf, static_cast<std::size_t>(n), from);
            else absorb_nbns(buf, static_cast<std::size_t>(n), from);
        }
    }
    const std::int64_t now = now_ms();
    for (Entry &e : entries_)
        if (e.sent && !e.done && now - e.sent_ms >= timeout_ms_) e.done = true; /* one try */
}

std::string LocalNames::name(const Address &a, NameSource *source) const
{
    for (const Entry &e : entries_) {
        if (!(e.addr == a)) continue;
        const NamePick p = best_name(e.mdns_name, e.nbns_name, std::string());
        if (source) *source = p.source;
        return p.name;
    }
    if (source) *source = NameSource::None;
    return std::string();
}

bool LocalNames::busy() const
{
    for (const Entry &e : entries_)
        if (e.sent && !e.done) return true;
    return false;
}

} // namespace nt
