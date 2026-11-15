/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the real V4L2 calls (see v4l2_api.hpp).
 */
#include "v4l2_api.hpp"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace vhs {
namespace {

class RealV4l2 final : public V4l2Api {
public:
    int open(const char *path) override { return ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC); }
    int close(int fd) override { return ::close(fd); }
    int ioctl(int fd, unsigned long req, void *arg) override
    {
        int r;
        do {
            r = ::ioctl(fd, req, arg);
        } while (r == -1 && errno == EINTR);
        return r;
    }
    void *map(std::size_t len, int fd, off_t offset) override
    {
        void *p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
        return p == MAP_FAILED ? nullptr : p;
    }
    int unmap(void *addr, std::size_t len) override { return ::munmap(addr, len); }
    int poll_in(int fd, int timeout_ms) override
    {
        struct pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        const int r = ::poll(&p, 1, timeout_ms);
        if (r < 0) return errno == EINTR ? 0 : -1;
        return r == 0 ? 0 : (p.revents ? p.revents : POLLIN);
    }
};

} // namespace

std::unique_ptr<V4l2Api> make_real_v4l2()
{
    return std::make_unique<RealV4l2>();
}

} // namespace vhs
