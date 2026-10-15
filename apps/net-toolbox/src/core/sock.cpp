/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: non-blocking socket steps (see sock.hpp).
 */
#include "sock.hpp"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace nt {

void Fd::reset(int fd)
{
    if (fd_ >= 0) close(fd_);
    fd_ = fd;
}

int wait_fd(int fd, short events, std::int64_t deadline_ms, const CancelToken &cancel)
{
    for (;;) {
        if (cancel.cancelled()) return ECANCELED;
        const std::int64_t left = deadline_ms - now_ms();
        if (left <= 0) return ETIMEDOUT;
        struct pollfd p = {fd, events, 0};
        const int rc = poll(&p, 1, static_cast<int>(std::min<std::int64_t>(left, kSliceMs)));
        if (rc > 0) return 0;
        if (rc < 0 && errno != EINTR) return errno;
    }
}

Fd tcp_connect(const Address &addr, std::uint16_t port, std::int64_t deadline_ms, const CancelToken &cancel, int *err)
{
    int e = 0;
    Fd fd;
    sockaddr_storage ss;
    const socklen_t sl = addr.to_sockaddr(port, ss);
    if (!sl) e = EAFNOSUPPORT;
    if (!e) {
        fd.reset(socket(addr.family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP));
        if (!fd.valid()) e = errno;
    }
    if (!e && connect(fd.get(), reinterpret_cast<const sockaddr *>(&ss), sl) != 0) {
        if (errno != EINPROGRESS) {
            e = errno;
        } else {
            e = wait_fd(fd.get(), POLLOUT, deadline_ms, cancel);
            if (!e) {
                int so = 0;
                socklen_t len = sizeof(so);
                if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &so, &len) != 0) so = errno;
                e = so;
            }
        }
    }
    if (err) *err = e;
    if (e) fd.reset();
    return fd;
}

int send_all(int fd, const void *data, std::size_t len, std::int64_t deadline_ms, const CancelToken &cancel)
{
    const char *p = static_cast<const char *>(data);
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = send(fd, p + done, len - done, MSG_NOSIGNAL);
        if (n > 0) {
            done += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const int e = wait_fd(fd, POLLOUT, deadline_ms, cancel);
            if (e) return e;
            continue;
        }
        return n < 0 ? errno : EIO;
    }
    return 0;
}

int recv_some(int fd, void *buf, std::size_t cap, std::size_t *got, std::int64_t deadline_ms, const CancelToken &cancel)
{
    *got = 0;
    for (;;) {
        const ssize_t n = recv(fd, buf, cap, 0);
        if (n >= 0) {
            *got = static_cast<std::size_t>(n);
            return 0;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return errno;
        const int e = wait_fd(fd, POLLIN, deadline_ms, cancel);
        if (e) return e;
    }
}

std::string errno_words(int err)
{
    switch (err) {
    case 0: return "ok";
    case ETIMEDOUT: return "timed out";
    case ECANCELED: return "cancelled";
    case ECONNREFUSED: return "connection refused";
    case ECONNRESET: return "connection reset";
    case ENETUNREACH: return "network unreachable";
    case EHOSTUNREACH: return "no route to host";
    case EHOSTDOWN: return "host is down";
    case EACCES:
    case EPERM: return "not allowed";
    case EADDRNOTAVAIL: return "address not available";
    case EAFNOSUPPORT: return "address family not supported";
    case EPROTONOSUPPORT: return "protocol not supported";
    case EMFILE:
    case ENFILE: return "too many open files";
    case EIO: return "input/output error";
    default: {
        std::string s = std::strerror(err);
        if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z') s[0] = static_cast<char>(s[0] - 'A' + 'a');
        return s;
    }
    }
}

} // namespace nt
