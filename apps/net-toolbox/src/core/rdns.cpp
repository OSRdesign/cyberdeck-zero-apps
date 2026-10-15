/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: reverse names for the traceroute (see rdns.hpp).
 */
#include "rdns.hpp"

#include "dns.hpp"
#include "worker.hpp"

#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace nt {

ReverseNames::ReverseNames(const std::string &server, std::uint16_t port, int timeout_ms)
    : timeout_ms_(std::max(100, timeout_ms)), next_id_(0)
{
    if (getrandom(&next_id_, sizeof(next_id_), GRND_NONBLOCK) != static_cast<ssize_t>(sizeof(next_id_)))
        next_id_ = static_cast<std::uint16_t>(now_us() ^ getpid());
    Address a;
    if (!parse_ip(server, a)) return;
    sockaddr_storage ss;
    const socklen_t sl = a.to_sockaddr(port, ss);
    Fd fd(socket(a.family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd.valid() || bind_to_active_device(fd.get()) != 0 || connect(fd.get(), reinterpret_cast<const sockaddr *>(&ss), sl) != 0) return;
    fd_ = std::move(fd);
}

void ReverseNames::want(const Address &a)
{
    if (!fd_.valid() || !a.valid() || entries_.size() >= kMaxEntries) return;
    for (const Entry &e : entries_)
        if (e.addr == a) return;
    Entry e;
    e.addr = a;
    e.qname = reverse_name(a);
    if (e.qname.empty()) return;
    entries_.push_back(e);
    service();
}

void ReverseNames::send(Entry &e)
{
    e.id = next_id_++;
    std::vector<std::uint8_t> query;
    e.sent_ms = now_ms();
    if (!dns::build_query(e.qname, dns::kPTR, e.id, dns::QueryOptions(), query) ||
        ::send(fd_.get(), query.data(), query.size(), MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
        e.state = State::Done; /* no name: the hop shows its address */
        ++changes_;
        return;
    }
    e.state = State::Sent;
}

void ReverseNames::service()
{
    if (!fd_.valid()) return;
    std::uint8_t buf[1500];
    for (;;) {
        const ssize_t n = recv(fd_.get(), buf, sizeof(buf), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            break; /* EAGAIN, or an error from an earlier send (ECONNREFUSED): nothing more to read now */
        }
        dns::Message m;
        if (!dns::parse_message(buf, static_cast<std::size_t>(n), m)) continue;
        for (Entry &e : entries_) {
            if (e.state != State::Sent || e.id != m.header.id || !dns::response_matches(m, e.id, e.qname, dns::kPTR))
                continue;
            e.state = State::Done;
            ++changes_;
            for (const dns::Record &r : m.answers) {
                if (r.type != dns::kPTR || r.data.empty() || r.data.find('(') != std::string::npos) continue;
                e.name = r.data.size() > 253 ? r.data.substr(0, 253) : r.data;
                break;
            }
            break;
        }
    }
    const std::int64_t now = now_ms();
    std::size_t pending = 0;
    for (Entry &e : entries_) {
        if (e.state == State::Sent && now - e.sent_ms >= timeout_ms_) { /* one try */
            e.state = State::Done;
            ++changes_;
        }
        if (e.state == State::Sent) ++pending;
    }
    for (Entry &e : entries_) {
        if (pending >= kMaxPending) break;
        if (e.state != State::Queued) continue;
        send(e);
        if (e.state == State::Sent) ++pending;
    }
}

std::string ReverseNames::name(const Address &a, bool *done) const
{
    for (const Entry &e : entries_) {
        if (!(e.addr == a)) continue;
        if (done) *done = e.state == State::Done;
        return e.name;
    }
    if (done) *done = true; /* never asked */
    return std::string();
}

bool ReverseNames::busy() const
{
    for (const Entry &e : entries_)
        if (e.state != State::Done) return true;
    return false;
}

} // namespace nt
