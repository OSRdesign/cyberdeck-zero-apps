/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: non-blocking socket steps shared by the DNS, port-check and HTTP clients (no LVGL). Every wait is a
 * poll() of at most kSliceMs between two looks at the cancel flag; deadlines are CLOCK_MONOTONIC milliseconds
 * (now_ms()). Errors come back as errno values: ETIMEDOUT for a deadline, ECANCELED for a cancel.
 */
#pragma once

#include "netaddr.hpp"
#include "worker.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace nt {

/* ---------------------------------------------------------------- the interface a socket is tied to (0.3)
 *
 * SO_BINDTODEVICE needs no capability on Linux 5.7 and later (the boards run 6.18; checked on a Pi 3A+: an ICMP
 * datagram socket and a TCP connect, bound to wlan0, go out of wlan0; bound to a name that does not exist the call
 * fails with ENODEV). A kernel older than 5.7 refuses with EPERM: the socket then stays unbound (the system's route)
 * and the caller says so only through bind_to_device()'s result.
 *
 * The chosen network (scope.hpp) is process-wide state, set by the UI when the user picks an interface or when the one
 * picked appears or vanishes: every socket the tools open (ICMP echo, traceroute probes, DNS over UDP and TCP, TCP
 * port check, whois, the captive-portal HTTP) and every curl command line (cmdrun.hpp) takes it from here, so a tool
 * needs no parameter for it. "" = Automatic: the system's routing, as before. */

/* [A-Za-z0-9_.:-]{1,15}: what the kernel allows in an interface name, less the characters a command line could
 * misread */
bool valid_iface_name(const std::string &name);

/* SO_BINDTODEVICE on fd. 0 on success or for an empty name (no binding); else the errno (ENODEV: no such interface,
 * EPERM: kernel older than 5.7, EINVAL: invalid name). */
int bind_to_device(int fd, const std::string &iface);

/* The interface of the chosen network ("" = Automatic). An invalid name is stored as "". */
void set_active_interface(const std::string &iface);
std::string active_interface();
/* bind_to_device(fd, active_interface()); a failure is a real error here (ENODEV: the adapter was unplugged) */
int bind_to_active_device(int fd);

/* The chosen interface for the lifetime of the object (tests). */
class ScopedActiveInterface {
public:
    explicit ScopedActiveInterface(const std::string &iface) : old_(active_interface()) { set_active_interface(iface); }
    ~ScopedActiveInterface() { set_active_interface(old_); }
    ScopedActiveInterface(const ScopedActiveInterface &) = delete;
    ScopedActiveInterface &operator=(const ScopedActiveInterface &) = delete;

private:
    std::string old_;
};

/* Closes the descriptor when it goes out of scope. */
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&o) noexcept : fd_(o.release()) {}
    Fd &operator=(Fd &&o) noexcept
    {
        if (this != &o) {
            reset();
            fd_ = o.release();
        }
        return *this;
    }
    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release()
    {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1);

private:
    int fd_ = -1;
};

/* Waits until fd is ready for `events` (POLLIN / POLLOUT). 0 when ready, else ETIMEDOUT, ECANCELED or the poll
 * error; POLLERR / POLLHUP count as ready (the next call tells the error). */
int wait_fd(int fd, short events, std::int64_t deadline_ms, const CancelToken &cancel);

/* A TCP connection to addr:port by the deadline. The descriptor (non-blocking), or an invalid one and *err:
 * ECONNREFUSED (a reset: closed port), ETIMEDOUT (no answer), EHOSTUNREACH / ENETUNREACH, ECANCELED, ... */
Fd tcp_connect(const Address &addr, std::uint16_t port, std::int64_t deadline_ms, const CancelToken &cancel, int *err);

/* All of data by the deadline (MSG_NOSIGNAL). 0 or the errno. */
int send_all(int fd, const void *data, std::size_t len, std::int64_t deadline_ms, const CancelToken &cancel);

/* Up to cap bytes once something is there. 0 with *got (0 = the peer closed), else the errno. */
int recv_some(int fd, void *buf, std::size_t cap, std::size_t *got, std::int64_t deadline_ms, const CancelToken &cancel);

/* Plain words for an errno of these steps: "timed out", "connection refused", "network unreachable", ... */
std::string errno_words(int err);

} // namespace nt
