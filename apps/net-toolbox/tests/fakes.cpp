/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: hand-made DNS packets and loopback servers (see fakes.hpp).
 */
#include "fakes.hpp"

#include "dns.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace fake {
namespace {

void put16(Bytes &b, std::uint16_t v)
{
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void put32(Bytes &b, std::uint32_t v)
{
    put16(b, static_cast<std::uint16_t>(v >> 16));
    put16(b, static_cast<std::uint16_t>(v & 0xFFFF));
}

std::string lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

int bound_socket(int type, const char *ip, std::uint16_t port)
{
    const int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

std::uint16_t local_port(int fd)
{
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
    return ntohs(a.sin_port);
}

bool read_exact(int fd, std::uint8_t *p, std::size_t n)
{
    std::size_t have = 0;
    while (have < n) {
        const ssize_t r = recv(fd, p + have, n - have, 0);
        if (r <= 0) return false;
        have += static_cast<std::size_t>(r);
    }
    return true;
}

} // namespace

Bytes wire_name(const std::string &name)
{
    Bytes b;
    std::size_t start = 0;
    while (start < name.size()) {
        std::size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        b.push_back(static_cast<std::uint8_t>(dot - start));
        b.insert(b.end(), name.begin() + static_cast<std::ptrdiff_t>(start), name.begin() + static_cast<std::ptrdiff_t>(dot));
        start = dot + 1;
    }
    b.push_back(0);
    return b;
}

Bytes rd_a(const char *ip)
{
    Bytes b(4);
    inet_pton(AF_INET, ip, b.data());
    return b;
}

Bytes rd_aaaa(const char *ip)
{
    Bytes b(16);
    inet_pton(AF_INET6, ip, b.data());
    return b;
}

Bytes rd_name(const std::string &name)
{
    return wire_name(name);
}

Bytes rd_mx(std::uint16_t pref, const std::string &name)
{
    Bytes b;
    put16(b, pref);
    const Bytes n = wire_name(name);
    b.insert(b.end(), n.begin(), n.end());
    return b;
}

Bytes rd_txt(const std::vector<std::string> &strings)
{
    Bytes b;
    for (const std::string &s : strings) {
        b.push_back(static_cast<std::uint8_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
    return b;
}

Bytes make_response(const Bytes &query, int rcode, const std::vector<Record> &answers, bool tc, int id_delta,
                    const std::vector<Record> &authority)
{
    nt::dns::Message q;
    Bytes out;
    if (!nt::dns::parse_message(query.data(), query.size(), q) || q.questions.empty()) return out;
    put16(out, static_cast<std::uint16_t>(q.header.id + id_delta));
    std::uint16_t flags = 0x8000 | 0x0100 | 0x0080 | static_cast<std::uint16_t>(rcode & 0xF);
    if (tc) flags |= 0x0200;
    put16(out, flags);
    put16(out, 1);
    put16(out, static_cast<std::uint16_t>(answers.size()));
    put16(out, static_cast<std::uint16_t>(authority.size()));
    put16(out, 0);
    const Bytes qn = wire_name(q.questions[0].name);
    out.insert(out.end(), qn.begin(), qn.end());
    put16(out, q.questions[0].type);
    put16(out, 1);
    for (const std::vector<Record> *list : {&answers, &authority}) {
        for (const Record &r : *list) {
            if (lower(r.name) == lower(q.questions[0].name)) put16(out, 0xC00C);
            else {
                const Bytes n = wire_name(r.name);
                out.insert(out.end(), n.begin(), n.end());
            }
            put16(out, r.type);
            put16(out, 1);
            put32(out, r.ttl);
            put16(out, static_cast<std::uint16_t>(r.rdata.size()));
            out.insert(out.end(), r.rdata.begin(), r.rdata.end());
        }
    }
    return out;
}

Dns::Dns(const char *ip, std::uint16_t port)
{
    udp_ = bound_socket(SOCK_DGRAM, ip, port);
    if (udp_ >= 0) {
        port_ = local_port(udp_);
        tcp_ = bound_socket(SOCK_STREAM, ip, port_);
        if (tcp_ >= 0 && listen(tcp_, 8) != 0) {
            close(tcp_);
            tcp_ = -1;
        }
    }
    if (ok()) thread_ = std::thread([this] { loop(); });
}

Dns::~Dns()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (udp_ >= 0) close(udp_);
    if (tcp_ >= 0) close(tcp_);
}

void Dns::add(const std::string &name, std::uint16_t type, const Record &r)
{
    std::lock_guard<std::mutex> lock(mu_);
    zone_[{lower(name), type}].push_back(r);
}

Bytes Dns::answer(const Bytes &query, bool over_tcp)
{
    nt::dns::Message q;
    if (!nt::dns::parse_message(query.data(), query.size(), q) || q.questions.empty()) return Bytes();
    const Mode m = static_cast<Mode>(mode.load());
    if (m == Mode::FormErrOnEdns && q.edns) return make_response(query, nt::dns::kFormErr, {});
    if (m == Mode::ServFail) return make_response(query, nt::dns::kServFail, {});
    std::vector<Record> recs;
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const std::string name = lower(q.questions[0].name);
        for (const auto &kv : zone_)
            if (kv.first.first == name) known = true;
        const auto it = zone_.find({name, q.questions[0].type});
        if (it != zone_.end()) recs = it->second;
    }
    if (m == Mode::Truncate && !over_tcp) return make_response(query, 0, {}, true);
    return make_response(query, known ? 0 : nt::dns::kNxDomain, recs);
}

void Dns::loop()
{
    Bytes buf(4096);
    while (!stop_) {
        pollfd p[2] = {{udp_, POLLIN, 0}, {tcp_, POLLIN, 0}};
        if (poll(p, 2, 20) <= 0) continue;
        if (p[0].revents & POLLIN) {
            sockaddr_storage from{};
            socklen_t fl = sizeof(from);
            const ssize_t n = recvfrom(udp_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr *>(&from), &fl);
            if (n > 0) {
                ++udp_queries;
                const Bytes query(buf.begin(), buf.begin() + n);
                if (static_cast<Mode>(mode.load()) == Mode::Silent) continue;
                if (static_cast<Mode>(mode.load()) == Mode::WrongIdFirst) {
                    const Bytes wrong = make_response(query, 0, {}, false, 1);
                    sendto(udp_, wrong.data(), wrong.size(), 0, reinterpret_cast<sockaddr *>(&from), fl);
                }
                const Bytes reply = answer(query, false);
                if (!reply.empty()) sendto(udp_, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&from), fl);
            }
        }
        if (p[1].revents & POLLIN) {
            const int c = accept4(tcp_, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) continue;
            ++tcp_queries;
            timeval tv{1, 0};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::uint8_t head[2];
            if (static_cast<Mode>(mode.load()) != Mode::Silent && read_exact(c, head, 2)) {
                Bytes query((static_cast<std::size_t>(head[0]) << 8) | head[1]);
                if (read_exact(c, query.data(), query.size())) {
                    const Bytes reply = answer(query, true);
                    Bytes framed;
                    put16(framed, static_cast<std::uint16_t>(reply.size()));
                    framed.insert(framed.end(), reply.begin(), reply.end());
                    send(c, framed.data(), framed.size(), MSG_NOSIGNAL);
                }
            }
            close(c);
        }
    }
}

Tcp::Tcp(std::string reply) : reply_(std::move(reply))
{
    fd_ = bound_socket(SOCK_STREAM, "127.0.0.1", 0);
    if (fd_ >= 0 && listen(fd_, 8) != 0) {
        close(fd_);
        fd_ = -1;
    }
    if (fd_ >= 0) {
        port_ = local_port(fd_);
        thread_ = std::thread([this] { loop(); });
    }
}

Tcp::~Tcp()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) close(fd_);
}

std::string Tcp::last_request() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return last_;
}

void Tcp::loop()
{
    while (!stop_) {
        pollfd p = {fd_, POLLIN, 0};
        if (poll(&p, 1, 20) <= 0) continue;
        const int c = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) continue;
        ++accepted;
        if (!reply_.empty()) {
            timeval tv{1, 0};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::string req;
            char buf[512];
            while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
                const ssize_t n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<std::size_t>(n));
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                last_ = req;
            }
            send(c, reply_.data(), reply_.size(), MSG_NOSIGNAL);
        }
        close(c);
    }
}

std::uint16_t closed_port()
{
    const int fd = bound_socket(SOCK_STREAM, "127.0.0.1", 0);
    if (fd < 0) return 9;
    const std::uint16_t port = local_port(fd);
    close(fd);
    return port;
}

} // namespace fake
