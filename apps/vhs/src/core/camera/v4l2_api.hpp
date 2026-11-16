/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the camera seam. V4l2Api is every kernel call the capture code makes on a V4L2 device node, with libc's
 * conventions (-1 and errno on failure). make_real_v4l2() is the kernel (plain syscalls, no libv4l); FakeCamera
 * (fake_camera.hpp) answers the same calls for the --fake camera and the unit tests (the fake ioctl backend).
 */
#pragma once

#include <sys/types.h>

#include <cstddef>
#include <memory>

namespace vhs {

class V4l2Api {
public:
    virtual ~V4l2Api() = default;
    /* open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC): a descriptor, or -1 (errno ENOENT, EACCES, EBUSY, ENODEV...) */
    virtual int open(const char *path) = 0;
    virtual int close(int fd) = 0;
    /* ioctl(fd, req, arg), EINTR retried: 0 (or a positive value), or -1 with errno */
    virtual int ioctl(int fd, unsigned long req, void *arg) = 0;
    /* mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset): the mapping, or nullptr with errno */
    virtual void *map(std::size_t len, int fd, off_t offset) = 0;
    virtual int unmap(void *addr, std::size_t len) = 0;
    /* poll() for one descriptor (POLLIN): its revents (> 0), 0 on timeout, -1 with errno (EINTR is returned as 0) */
    virtual int poll_in(int fd, int timeout_ms) = 0;
};

std::unique_ptr<V4l2Api> make_real_v4l2();

} // namespace vhs
