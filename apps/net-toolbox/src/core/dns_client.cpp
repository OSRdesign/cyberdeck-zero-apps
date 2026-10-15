/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the DNS client (see dns_client.hpp).
 */
#include "dns_client.hpp"

#include "sock.hpp"
#include "text.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <ctime>

namespace nt {
namespace {

std::uint16_t random_id()
{
    std::uint16_t id = 0;
    if (getrandom(&id, sizeof(id), GRND_NONBLOCK) != static_cast<ssize_t>(sizeof(id))) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        id = static_cast<std::uint16_t>(ts.tv_nsec ^ (ts.tv_nsec >> 16) ^ getpid());
    }
    return id;
}

/* the question of a reply is ours (true when it cannot be read: the id already matched and a truncated or error
 * answer may cut it) */
bool question_ok(const std::vector<std::uint8_t> &buf, std::size_t n, const std::string &qname, std::uint16_t type)
{
    dns::Header h;
    if (!dns::parse_header(buf.data(), n, h) || h.qdcount == 0) return true;
    std::string name;
    std::size_t next = 0;
    if (!dns::read_name(buf.data(), n, 12, name, &next) || next + 4 > n) return true;
    const std::uint16_t t = static_cast<std::uint16_t>((buf[next] << 8) | buf[next + 1]);
    return dns::names_equal(name, qname) && t == type;
}

int udp_exchange(const Address &server, std::uint16_t port, const std::vector<std::uint8_t> &query, std::uint16_t id,
                 const std::string &qname, std::uint16_t type, int timeout_ms, int tries, const CancelToken &cancel,
                 std::vector<std::uint8_t> &reply, int *tries_used)
{
    sockaddr_storage ss;
    const socklen_t sl = server.to_sockaddr(port, ss);
    if (!sl) return EAFNOSUPPORT;
    Fd fd(socket(server.family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd.valid()) return errno;
    /* connected: the kernel drops datagrams from anyone else, and a closed port comes back as ECONNREFUSED */
    if (connect(fd.get(), reinterpret_cast<const sockaddr *>(&ss), sl) != 0) return errno;
    std::vector<std::uint8_t> buf(4096);
    for (int t = 0; t < std::max(1, tries); ++t) {
        if (cancel.cancelled()) return ECANCELED;
        *tries_used = t + 1;
        if (send(fd.get(), query.data(), query.size(), MSG_NOSIGNAL) < 0) return errno;
        const std::int64_t deadline = now_ms() + std::max(1, timeout_ms);
        for (;;) {
            const int e = wait_fd(fd.get(), POLLIN, deadline, cancel);
            if (e == ETIMEDOUT) break; /* the next try */
            if (e) return e;
            const ssize_t n = recv(fd.get(), buf.data(), buf.size(), 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                return errno;
            }
            dns::Header h;
            const std::size_t len = static_cast<std::size_t>(n);
            if (!dns::parse_header(buf.data(), len, h) || h.id != id || !h.qr) continue; /* not ours */
            if (!question_ok(buf, len, qname, type)) continue;
            reply.assign(buf.begin(), buf.begin() + n);
            return 0;
        }
    }
    return ETIMEDOUT;
}

int tcp_exchange(const Address &server, std::uint16_t port, const std::vector<std::uint8_t> &query, std::uint16_t id,
                 int timeout_ms, const CancelToken &cancel, std::vector<std::uint8_t> &reply)
{
    const std::int64_t deadline = now_ms() + std::max(1, timeout_ms);
    int e = 0;
    Fd fd = tcp_connect(server, port, deadline, cancel, &e);
    if (!fd.valid()) return e;
    std::vector<std::uint8_t> msg;
    msg.reserve(query.size() + 2);
    msg.push_back(static_cast<std::uint8_t>(query.size() >> 8));
    msg.push_back(static_cast<std::uint8_t>(query.size() & 0xFF));
    msg.insert(msg.end(), query.begin(), query.end());
    if ((e = send_all(fd.get(), msg.data(), msg.size(), deadline, cancel))) return e;
    std::uint8_t head[2];
    std::size_t have = 0, got = 0;
    while (have < 2) {
        if ((e = recv_some(fd.get(), head + have, 2 - have, &got, deadline, cancel))) return e;
        if (!got) return ECONNRESET;
        have += got;
    }
    const std::size_t n = (static_cast<std::size_t>(head[0]) << 8) | head[1];
    if (n < 12) return EPROTO;
    reply.assign(n, 0);
    have = 0;
    while (have < n) {
        if ((e = recv_some(fd.get(), reply.data() + have, n - have, &got, deadline, cancel))) return e;
        if (!got) return ECONNRESET;
        have += got;
    }
    dns::Header h;
    if (!dns::parse_header(reply.data(), reply.size(), h) || h.id != id || !h.qr) return EPROTO;
    return 0;
}

std::string transport_words(int e, std::uint16_t port)
{
    if (e == ECONNREFUSED) return "refused (nothing answers on port " + std::to_string(port) + ")";
    if (e == EPROTO) return "an answer that is not DNS";
    return errno_words(e);
}

} // namespace

const std::vector<ResolverPreset> &resolver_presets()
{
    static const std::vector<ResolverPreset> presets = {
        {"System", ""}, {"Cloudflare", "1.1.1.1"}, {"Google", "8.8.8.8"}, {"Quad9", "9.9.9.9"}};
    return presets;
}

std::string pick_resolver(const std::string &choice, const std::vector<std::string> &system_servers)
{
    const std::string c = trim(choice);
    if (c.empty() || lower(c) == "system") {
        for (const std::string &s : system_servers)
            if (is_ip(s)) return s;
        return std::string();
    }
    return is_ip(c) ? c : std::string();
}

std::vector<std::string> DnsResult::addresses(std::uint16_t want) const
{
    std::vector<std::string> out;
    for (const dns::Record &r : answers)
        if (r.type == want && r.data.find('(') == std::string::npos) out.push_back(r.data);
    return out;
}

bool make_question(const std::string &input, std::uint16_t &type, std::string &qname, std::string *why)
{
    std::string text = trim(input);
    Address a;
    if (parse_ip(text, a)) {
        qname = reverse_name(a);
        type = dns::kPTR;
        return true;
    }
    if (text.size() > 1 && text.back() == '.') text.pop_back();
    if (!valid_hostname(text, why)) return false;
    qname = text;
    return true;
}

DnsResult dns_query(const DnsQuery &q, const CancelToken &cancel)
{
    DnsResult r;
    r.name = q.name;
    r.type = q.type;
    r.server = q.server;
    r.chain.push_back(q.name);
    Address server;
    if (!parse_ip(q.server, server)) {
        r.error = q.server.empty() ? "no DNS server" : "not an IP address: " + q.server;
        return r;
    }
    const std::int64_t start = now_ms();
    bool edns = q.edns;
    std::vector<std::uint8_t> reply;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::uint16_t id = random_id();
        dns::QueryOptions opt;
        opt.edns = edns;
        std::vector<std::uint8_t> query;
        std::string why;
        if (!dns::build_query(q.name, q.type, id, opt, query, &why)) {
            r.error = why;
            return r;
        }
        int tries = 0;
        reply.clear();
        int e = udp_exchange(server, q.port, query, id, q.name, q.type, q.timeout_ms, q.tries, cancel, reply, &tries);
        r.tries_used += tries;
        r.via_tcp = false;
        dns::Header h;
        if (!e && dns::parse_header(reply.data(), reply.size(), h) && h.tc && q.tcp_fallback) {
            std::vector<std::uint8_t> full;
            const int te = tcp_exchange(server, q.port, query, id, q.timeout_ms, cancel, full);
            if (!te) {
                reply.swap(full);
                r.via_tcp = true;
            } else if (te == ECANCELED) {
                e = te;
            } /* else keep the truncated answer: what it holds is still true */
        }
        r.elapsed_ms = static_cast<int>(now_ms() - start);
        if (e) {
            r.error = transport_words(e, q.port);
            return r;
        }
        dns::Message m;
        if (!dns::parse_message(reply.data(), reply.size(), m, &why)) {
            r.error = "an unreadable answer (" + why + ")";
            return r;
        }
        if (edns && attempt == 0 && (m.header.rcode == dns::kFormErr || m.header.rcode == dns::kNotImp)) {
            edns = false; /* an old server that does not know EDNS: once more without it */
            continue;
        }
        r.answered = true;
        r.rcode = m.header.rcode;
        r.rcode_name = dns::rcode_name(r.rcode);
        r.rcode_words = dns::rcode_words(r.rcode);
        r.authoritative = m.header.aa;
        r.answers = std::move(m.answers);
        r.authority = std::move(m.authority);
        dns::Message chain_view;
        chain_view.answers = r.answers;
        r.chain = dns::cname_chain(chain_view, q.name);
        return r;
    }
    return r;
}

std::vector<DnsResult> dns_compare(const DnsQuery &base, const std::vector<std::string> &servers,
                                   const CancelToken &cancel, const std::function<void(const DnsResult &)> &each)
{
    std::vector<DnsResult> out;
    for (const std::string &s : servers) {
        if (cancel.cancelled()) break;
        DnsQuery q = base;
        q.server = s;
        out.push_back(dns_query(q, cancel));
        if (each) each(out.back());
    }
    return out;
}

std::vector<Address> hosts_lookup(const std::string &hosts_text, const std::string &name)
{
    std::vector<Address> v4, v6;
    std::string want = lower(trim(name));
    if (want.size() > 1 && want.back() == '.') want.pop_back();
    if (want.empty()) return v4;
    for (const std::string &raw : split_lines(hosts_text)) {
        std::string line = raw;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const std::vector<std::string> f = split_ws(line);
        if (f.size() < 2) continue;
        Address a;
        if (!parse_ip(f[0], a)) continue;
        for (std::size_t i = 1; i < f.size(); ++i) {
            if (lower(f[i]) != want) continue;
            std::vector<Address> &list = a.family == AF_INET ? v4 : v6;
            if (std::find(list.begin(), list.end(), a) == list.end()) list.push_back(a);
            break;
        }
    }
    v4.insert(v4.end(), v6.begin(), v6.end());
    return v4;
}

Resolved resolve_host(const std::string &input, const ResolveOptions &opt, const CancelToken &cancel)
{
    Resolved out;
    std::string host = trim(input);
    if (parse_ip(host, out.address)) {
        out.ok = true;
        out.literal = true;
        out.all.push_back(out.address);
        return out;
    }
    if (host.size() > 1 && host.back() == '.') host.pop_back();
    std::string why;
    if (!valid_hostname(host, &why)) {
        out.error = why;
        return out;
    }
    if (!opt.hosts_path.empty()) {
        int fd = open(opt.hosts_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            std::string text;
            char buf[4096];
            ssize_t n;
            while (text.size() < 256 * 1024 && (n = read(fd, buf, sizeof(buf))) > 0) text.append(buf, static_cast<std::size_t>(n));
            close(fd);
            out.all = hosts_lookup(text, host);
            if (!out.all.empty()) {
                out.ok = true;
                out.from_hosts = true;
                out.address = out.all.front();
                return out;
            }
        }
    }
    if (opt.server.empty()) {
        out.error = "No DNS server to ask (resolv.conf lists none)";
        return out;
    }
    const std::int64_t start = now_ms();
    DnsQuery q;
    q.name = host;
    q.server = opt.server;
    q.port = opt.port;
    q.timeout_ms = opt.timeout_ms;
    q.tries = opt.tries;
    out.server = opt.server;
    std::string first_error;
    for (const std::uint16_t type : {dns::kA, dns::kAAAA}) {
        q.type = type;
        const DnsResult r = dns_query(q, cancel);
        if (cancel.cancelled()) {
            out.error = "cancelled";
            break;
        }
        if (!r.answered) {
            out.error = "The DNS server " + opt.server + " did not answer (" + r.error + ")";
            break;
        }
        if (r.rcode == dns::kNxDomain) {
            out.error = host + " does not exist (NXDOMAIN)";
            break;
        }
        if (r.rcode != dns::kNoError) {
            if (first_error.empty())
                first_error = "The DNS server " + opt.server + " answered " + r.rcode_name + " (" + r.rcode_words + ")";
            continue;
        }
        for (const std::string &text : r.addresses(type)) {
            Address a;
            if (parse_ip(text, a)) out.all.push_back(a);
        }
        if (!out.all.empty()) {
            out.ok = true;
            out.address = out.all.front();
            out.error.clear();
            break;
        }
    }
    out.elapsed_ms = static_cast<int>(now_ms() - start);
    if (!out.ok && out.error.empty()) out.error = first_error.empty() ? "No address for " + host : first_error;
    return out;
}

bool DnsSession::start(const DnsRequest &req)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    DnsSnapshot first;
    first.type = req.type;
    std::string why;
    if (!make_question(req.input, first.type, first.question, &why)) {
        first.state = DnsSnapshot::State::Failed;
        first.error = why;
        channel_.begin(gen, first);
        return false;
    }
    for (const std::string &s : req.servers)
        if (is_ip(trim(s)) && std::find(first.servers.begin(), first.servers.end(), trim(s)) == first.servers.end())
            first.servers.push_back(trim(s));
    if (first.servers.empty()) {
        first.state = DnsSnapshot::State::Failed;
        first.error = "No DNS server to ask: the system lists none. Pick one (1.1.1.1, 8.8.8.8, 9.9.9.9) or type one";
        channel_.begin(gen, first);
        return false;
    }
    first.state = DnsSnapshot::State::Running;
    channel_.begin(gen, first);
    DnsQuery base;
    base.name = first.question;
    base.type = first.type;
    base.port = req.port;
    base.timeout_ms = req.timeout_ms;
    base.tries = req.tries;
    const std::uint64_t started = worker_.start([this, gen, base, first](const CancelToken &cancel) {
        DnsSnapshot snap = first;
        dns_compare(base, first.servers, cancel, [&](const DnsResult &r) {
            if (cancel.cancelled() && !r.answered) return;
            snap.results.push_back(r);
            channel_.publish(gen, snap);
        });
        snap.state = cancel.cancelled() ? DnsSnapshot::State::Cancelled : DnsSnapshot::State::Done;
        channel_.publish(gen, snap);
    });
    if (!started) {
        first.state = DnsSnapshot::State::Failed;
        first.error = "Could not start the lookup (no thread)";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void DnsSession::stop()
{
    worker_.cancel();
}

} // namespace nt
