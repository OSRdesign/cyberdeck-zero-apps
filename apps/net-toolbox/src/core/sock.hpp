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
