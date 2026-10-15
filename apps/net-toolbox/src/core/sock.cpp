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
#include <mutex>

namespace nt {

namespace {

std::mutex g_iface_mu;
std::string g_iface;

} // namespace

bool valid_iface_name(const std::string &name)
{
    if (name.empty() || name.size() > 15) return false;
    if (name[0] == '-') return false; /* never an option for a command */
    if (name.find_first_not_of("0123456789.") == std::string::npos) return false; /* an address, not a name (curl would read it as one) */
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == ':' || c == '-'))
            return false;
    return true;
}

int bind_to_device(int fd, const std::string &iface)
{
    if (iface.empty()) return 0;
    if (!valid_iface_name(iface)) return EINVAL;
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface.c_str(), static_cast<socklen_t>(iface.size() + 1)) != 0)
        return errno;
    return 0;
}

void set_active_interface(const std::string &iface)
{
    const std::lock_guard<std::mutex> lock(g_iface_mu);
    g_iface = valid_iface_name(iface) ? iface : std::string();
}

std::string active_interface()
{
    const std::lock_guard<std::mutex> lock(g_iface_mu);
    return g_iface;
}

int bind_to_active_device(int fd)
{
    return bind_to_device(fd, active_interface());
}

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
        else e = bind_to_active_device(fd.get());
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
    case ENODEV: return "network interface not found";
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
