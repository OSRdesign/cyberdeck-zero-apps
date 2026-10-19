/*
 * SPDX-License-Identifier: MIT
 */

#include "tcp_transport.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace meshzero {

namespace {

uint64_t mono_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string endpoint(const std::string &host, int port)
{
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

} // namespace

struct TcpTransport::Resolve {
    std::mutex m;
    bool done = false;
    bool ok = false;
    sockaddr_storage addr{};
    socklen_t len = 0;
    int family = AF_INET;
    std::string error;
};

bool valid_host(const std::string &host)
{
    if (host.empty() || host.size() > 253) return false;
    in6_addr a6{};
    in_addr a4{};
    if (::inet_pton(AF_INET, host.c_str(), &a4) == 1 || ::inet_pton(AF_INET6, host.c_str(), &a6) == 1) return true;
    // a host name: labels of letters, digits and dashes, separated by dots (not all digits: that was a bad IPv4)
    bool alpha = false;
    size_t label = 0;
    for (size_t i = 0; i < host.size(); ++i) {
        const char c = host[i];
        if (c == '.') {
            if (label == 0 || host[i - 1] == '-') return false;
            label = 0;
            continue;
        }
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && c != '-') return false;
        if (c == '-' && label == 0) return false;
        alpha = alpha || letter;
        if (++label > 63) return false;
    }
    return label > 0 && host.back() != '-' && alpha;
}

TcpTransport::TcpTransport(std::string host, int port, std::string name, int connect_timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(connect_timeout_ms)
{
    info_.path = "tcp:" + endpoint(host_, port_);
    info_.product = std::move(name);
    info_.board_guess = "MeshCore companion over TCP";
}

TcpTransport::~TcpTransport()
{
    close();
}

void TcpTransport::fail(const std::string &why, OpenStatus status)
{
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    resolve_.reset();
    error_ = endpoint(host_, port_) + ": " + why;
    failure_ = why;
    fail_status_ = status;
    state_ = State::Failed;
}

void TcpTransport::start_connect(const void *addr, unsigned addr_len, int family)
{
    const int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fail(std::string("socket: ") + std::strerror(errno), OpenStatus::Error);
        return;
    }
    fd_ = fd;
    const int r = ::connect(fd, static_cast<const sockaddr *>(addr), static_cast<socklen_t>(addr_len));
    if (r == 0 || errno == EINPROGRESS) {
        state_ = State::Connecting;
        deadline_ = mono_ms() + static_cast<uint64_t>(timeout_ms_);
        return;
    }
    const int e = errno;
    fail(e == ECONNREFUSED ? "connection refused (no MeshCore Wi-Fi interface on this port?)"
         : e == ENETUNREACH || e == EHOSTUNREACH ? "network unreachable (is the deck on that network?)"
                                                 : std::strerror(e),
         OpenStatus::Error);
}

void TcpTransport::poll()
{
    if (state_ == State::Resolving) {
        std::shared_ptr<Resolve> r = resolve_;
        bool done = false, ok = false;
        sockaddr_storage addr{};
        socklen_t len = 0;
        int family = AF_INET;
        std::string err;
        {
            std::lock_guard<std::mutex> lock(r->m);
            done = r->done;
            ok = r->ok;
            addr = r->addr;
            len = r->len;
            family = r->family;
            err = r->error;
        }
        if (done) {
            resolve_.reset();
            if (ok) start_connect(&addr, len, family);
            else fail("unknown host (" + err + ")", OpenStatus::Error);
        } else if (mono_ms() >= deadline_) {
            fail("the name lookup took too long", OpenStatus::Error);      // the thread ends by itself, its answer is dropped
        }
        return;
    }
    if (state_ != State::Connecting) return;
    pollfd p{fd_, POLLOUT, 0};
    const int n = ::poll(&p, 1, 0);
    if (n > 0) {
        int err = 0;
        socklen_t l = sizeof(err);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &l) != 0) err = errno;
        if (err == 0) {
            const int on = 1, idle = 10, intvl = 5, cnt = 3;
            ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
            ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
            ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
            ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
            state_ = State::Open;
            lost_ = false;
            tx_.clear();
            error_.clear();
            failure_.clear();
            return;
        }
        fail(err == ECONNREFUSED ? "connection refused (no MeshCore Wi-Fi interface on this port?)"
             : err == ENETUNREACH || err == EHOSTUNREACH ? "network unreachable (is the deck on that network?)"
             : err == ETIMEDOUT ? "no answer"
                                : std::strerror(err),
             OpenStatus::Error);
        return;
    }
    if (mono_ms() >= deadline_) fail("no answer in " + std::to_string(std::max(1, (timeout_ms_ + 500) / 1000)) + " s", OpenStatus::Error);
}

OpenStatus TcpTransport::open()
{
    if (state_ == State::Open) return OpenStatus::Ok;
    if (state_ == State::Failed) {                       // the failure was reported once: this call starts again
        const OpenStatus s = fail_status_;
        state_ = State::Idle;
        return s;
    }
    if (state_ == State::Idle) {
        lost_ = false;
        error_.clear();
        if (port_ < 1 || port_ > 65535 || !valid_host(host_)) {
            fail("not a host and port", OpenStatus::Error);
            state_ = State::Idle;
            return OpenStatus::Error;
        }
        sockaddr_in a4{};
        sockaddr_in6 a6{};
        if (::inet_pton(AF_INET, host_.c_str(), &a4.sin_addr) == 1) {
            a4.sin_family = AF_INET;
            a4.sin_port = htons(static_cast<uint16_t>(port_));
            start_connect(&a4, sizeof(a4), AF_INET);
        } else if (::inet_pton(AF_INET6, host_.c_str(), &a6.sin6_addr) == 1) {
            a6.sin6_family = AF_INET6;
            a6.sin6_port = htons(static_cast<uint16_t>(port_));
            start_connect(&a6, sizeof(a6), AF_INET6);
        } else {
            // a name: looked up on a thread (getaddrinfo blocks); the thread keeps its own share of the result
            auto r = std::make_shared<Resolve>();
            resolve_ = r;
            state_ = State::Resolving;
            deadline_ = mono_ms() + static_cast<uint64_t>(timeout_ms_);
            const std::string host = host_, service = std::to_string(port_);
            std::thread([r, host, service] {
                addrinfo hints{};
                hints.ai_socktype = SOCK_STREAM;
                hints.ai_family = AF_UNSPEC;
                addrinfo *res = nullptr;
                const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
                std::lock_guard<std::mutex> lock(r->m);
                r->done = true;
                if (rc != 0 || !res) {
                    r->error = rc ? ::gai_strerror(rc) : "no address";
                } else {
                    const addrinfo *pick = res;                  // IPv4 first: a Wi-Fi board has no IPv6 server
                    for (const addrinfo *a = res; a; a = a->ai_next)
                        if (a->ai_family == AF_INET) { pick = a; break; }
                    std::memcpy(&r->addr, pick->ai_addr, pick->ai_addrlen);
                    r->len = pick->ai_addrlen;
                    r->family = pick->ai_family;
                    r->ok = true;
                }
                if (res) ::freeaddrinfo(res);
            }).detach();
        }
    }
    poll();
    if (state_ == State::Open) return OpenStatus::Ok;
    if (state_ == State::Failed) {
        const OpenStatus s = fail_status_;
        state_ = State::Idle;
        return s;
    }
    error_ = "connecting to " + endpoint(host_, port_);
    return OpenStatus::NotFound;
}

void TcpTransport::close()
{
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    resolve_.reset();
    tx_.clear();
    if (state_ != State::Failed) state_ = State::Idle;
}

void TcpTransport::flush_tx()
{
    while (fd_ >= 0 && !tx_.empty()) {
        const ssize_t n = ::send(fd_, tx_.data(), tx_.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            tx_.erase(0, static_cast<size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        } else {
            lost_ = true;
            error_ = endpoint(host_, port_) + ": write failed (" + std::strerror(errno) + ")";
            return;
        }
    }
}

long TcpTransport::read(uint8_t *buf, size_t max)
{
    if (state_ != State::Open) return -1;
    flush_tx();
    if (lost_) return -1;
    const ssize_t n = ::recv(fd_, buf, max, MSG_DONTWAIT);
    if (n > 0) return n;
    if (n == 0) {
        error_ = endpoint(host_, port_) + ": the node closed the connection";
        lost_ = true;
        return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    error_ = endpoint(host_, port_) + ": link lost (" + std::strerror(errno) + ")";
    lost_ = true;
    return -1;
}

bool TcpTransport::write(const uint8_t *data, size_t len)
{
    if (state_ != State::Open || lost_) return false;
    if (tx_.size() + len > 8192) return false;
    tx_.append(reinterpret_cast<const char *>(data), len);
    flush_tx();
    return !lost_;
}

} // namespace meshzero
