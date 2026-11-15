/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the V4L2 layer on the fake ioctl backend (core/fake_camera.hpp): QUERYCAP, the mode enumeration of
 * both switch positions, S_FMT / S_PARM, the buffer dance (REQBUFS, QUERYBUF, mmap, QBUF, STREAMON, poll, DQBUF), the
 * driver's sequence numbers and the faults (unplug, busy, no access).
 */
#include "check.hpp"

#include "camera/camera.hpp"
#include "camera/fake_camera.hpp"
#include "modes/select.hpp"

#include <linux/videodev2.h>
#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

using namespace vhs;

namespace {

FakeOptions fast()
{
    FakeOptions o;
    o.realtime = false;
    o.frames = 2;
    return o;
}

void enumerate()
{
    FakeCamera cam(fast());
    CHECK(cam.open("/dev/video0") < 0);
    CHECK_EQ(errno, ENOENT);
    const int fd = cam.open(FakeCamera::kPath);
    CHECK(fd >= 0);
    CHECK(cam.open(FakeCamera::kPath) < 0); /* one opener */
    CHECK_EQ(errno, EBUSY);
    const DeviceInfo info = query_device(cam, fd);
    CHECK(info.capture);
    CHECK_EQ(info.card, cam.card());
    CHECK_EQ(info.card, std::string("UGREEN FineCam 1080P CM969"));
    CHECK_EQ(info.driver, std::string("uvcvideo"));
    const std::vector<CamMode> modes = enumerate_modes(cam, fd);
    CHECK_EQ(modes.size(), static_cast<std::size_t>(8)); /* position B: 6 MJPEG + 2 YUYV */
    if (modes.size() == 8) {
        CHECK_EQ(modes[0].fourcc, kMJPG);
        CHECK_EQ(modes[0].w, 1920);
        CHECK_EQ(modes[0].fps.size(), static_cast<std::size_t>(2));
        CHECK_EQ(modes[0].fps[0], 60); /* highest first */
        CHECK_EQ(modes[7].fourcc, kYUYV);
        CHECK_EQ(modes[7].h, 360);
        CHECK_EQ(modes[7].fps.size(), static_cast<std::size_t>(1));
    }
    CHECK_EQ(cam.ioctl(fd, 0xDEAD, nullptr), -1);
    CHECK_EQ(errno, ENOTTY);
    CHECK_EQ(cam.close(fd), 0);
    CHECK(cam.close(fd) < 0);

    FakeOptions po = fast();
    po.portrait = true;
    FakeCamera pcam(po);
    const int pfd = pcam.open(FakeCamera::kPath);
    const std::vector<CamMode> pm = enumerate_modes(pcam, pfd);
    CHECK_EQ(pm.size(), static_cast<std::size_t>(6));
    bool any_yuyv = false;
    for (const CamMode &m : pm) any_yuyv |= m.fourcc == kYUYV;
    CHECK(!any_yuyv);
    CHECK(camera_portrait(pm));
    CHECK(!camera_portrait(modes));
    pcam.close(pfd);
}

/* the capture sequence by hand: S_FMT, S_PARM, 3 buffers, frames with consecutive sequence numbers */
void buffer_dance()
{
    FakeCamera cam(fast());
    const int fd = cam.open(FakeCamera::kPath);
    struct v4l2_format f;
    std::memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = 650; /* not listed: the nearest (640x360) */
    f.fmt.pix.height = 370;
    f.fmt.pix.pixelformat = kYUYV;
    CHECK_EQ(cam.ioctl(fd, VIDIOC_S_FMT, &f), 0);
    CHECK_EQ(f.fmt.pix.width, 640u);
    CHECK_EQ(f.fmt.pix.height, 360u);
    CHECK_EQ(f.fmt.pix.bytesperline, 1280u);
    struct v4l2_streamparm p;
    std::memset(&p, 0, sizeof p);
    p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    p.parm.capture.timeperframe.numerator = 1;
    p.parm.capture.timeperframe.denominator = 30;
    CHECK_EQ(cam.ioctl(fd, VIDIOC_S_PARM, &p), 0);
    CHECK_EQ(p.parm.capture.timeperframe.denominator, 30u);
    struct v4l2_requestbuffers rb;
    std::memset(&rb, 0, sizeof rb);
    rb.count = 3;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    CHECK_EQ(cam.ioctl(fd, VIDIOC_REQBUFS, &rb), 0);
    CHECK_EQ(rb.count, 3u);
    CHECK_EQ(cam.ioctl(fd, VIDIOC_S_FMT, &f), -1); /* buffers exist: EBUSY, as uvcvideo */
    CHECK_EQ(errno, EBUSY);
    std::vector<std::uint8_t *> maps;
    for (std::uint32_t i = 0; i < rb.count; ++i) {
        struct v4l2_buffer b;
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        CHECK_EQ(cam.ioctl(fd, VIDIOC_QUERYBUF, &b), 0);
        void *m = cam.map(b.length, fd, static_cast<off_t>(b.m.offset));
        CHECK(m != nullptr);
        maps.push_back(static_cast<std::uint8_t *>(m));
        CHECK_EQ(cam.ioctl(fd, VIDIOC_QBUF, &b), 0);
        CHECK_EQ(cam.ioctl(fd, VIDIOC_QBUF, &b), -1); /* twice: EINVAL */
    }
    CHECK(cam.map(16, fd, 12345) == nullptr);
    struct v4l2_buffer b;
    std::memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    CHECK_EQ(cam.ioctl(fd, VIDIOC_DQBUF, &b), -1); /* not streaming */
    CHECK_EQ(cam.poll_in(fd, 0) & POLLERR, POLLERR);
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    CHECK_EQ(cam.ioctl(fd, VIDIOC_STREAMON, &type), 0);
    CHECK_EQ(cam.streamon_count(), 1);
    std::uint32_t last_seq = 0;
    bool consecutive = true;
    for (int n = 0; n < 10; ++n) {
        CHECK(cam.poll_in(fd, 100) & POLLIN);
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        CHECK_EQ(cam.ioctl(fd, VIDIOC_DQBUF, &b), 0);
        CHECK_EQ(b.bytesused, 640u * 360 * 2);
        CHECK(b.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC);
        if (n > 0 && b.sequence != last_seq + 1) consecutive = false;
        last_seq = b.sequence;
        CHECK_EQ(cam.ioctl(fd, VIDIOC_QBUF, &b), 0);
    }
    CHECK(consecutive);
    /* nothing queued: no frame, EAGAIN, poll times out */
    std::vector<v4l2_buffer> held;
    for (;;) {
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (cam.ioctl(fd, VIDIOC_DQBUF, &b) != 0) break;
        held.push_back(b);
    }
    CHECK_EQ(errno, EAGAIN);
    CHECK_EQ(held.size(), static_cast<std::size_t>(3));
    CHECK_EQ(cam.poll_in(fd, 20), 0);
    /* the faults */
    for (v4l2_buffer &h : held) cam.ioctl(fd, VIDIOC_QBUF, &h);
    cam.skip_sequence_every(2);
    std::uint32_t seqs[4] = {};
    for (int n = 0; n < 4; ++n) {
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        cam.poll_in(fd, 100);
        CHECK_EQ(cam.ioctl(fd, VIDIOC_DQBUF, &b), 0);
        seqs[n] = b.sequence;
        cam.ioctl(fd, VIDIOC_QBUF, &b);
    }
    CHECK(seqs[3] - seqs[0] > 3); /* numbers skipped */
    cam.skip_sequence_every(0);
    cam.error_flag_every(1);
    cam.poll_in(fd, 100);
    std::memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    CHECK_EQ(cam.ioctl(fd, VIDIOC_DQBUF, &b), 0);
    CHECK(b.flags & V4L2_BUF_FLAG_ERROR);
    cam.ioctl(fd, VIDIOC_QBUF, &b);
    cam.error_flag_every(0);
    cam.unplug();
    CHECK_EQ(cam.poll_in(fd, 100) & POLLERR, POLLERR);
    CHECK_EQ(cam.ioctl(fd, VIDIOC_DQBUF, &b), -1);
    CHECK_EQ(errno, ENODEV);
    CHECK_EQ(cam.ioctl(fd, VIDIOC_STREAMOFF, &type), -1);
    CHECK_EQ(cam.close(fd), 0);
    CHECK(cam.open(FakeCamera::kPath) < 0);
    CHECK_EQ(errno, ENOENT);
    cam.replug();
    const int fd2 = cam.open(FakeCamera::kPath);
    CHECK(fd2 >= 0);
    cam.set_busy(true);
    CHECK_EQ(cam.ioctl(fd2, VIDIOC_S_FMT, &f), -1);
    CHECK_EQ(errno, EBUSY);
    cam.close(fd2);
    cam.set_no_access(true);
    CHECK(cam.open(FakeCamera::kPath) < 0);
    CHECK_EQ(errno, EACCES);
    CHECK_EQ(cam.open_count(), 2);
}

/* the realtime fake: about 30 frames a second; a frame finding no buffer is lost (the sequence jumps) */
void realtime()
{
    FakeOptions o;
    o.frames = 2;
    FakeCamera cam(o);
    const int fd = cam.open(FakeCamera::kPath);
    struct v4l2_format f;
    std::memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = 640;
    f.fmt.pix.height = 360;
    f.fmt.pix.pixelformat = kYUYV;
    cam.ioctl(fd, VIDIOC_S_FMT, &f);
    struct v4l2_requestbuffers rb;
    std::memset(&rb, 0, sizeof rb);
    rb.count = 2;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    cam.ioctl(fd, VIDIOC_REQBUFS, &rb);
    for (std::uint32_t i = 0; i < rb.count; ++i) {
        struct v4l2_buffer b;
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        cam.ioctl(fd, VIDIOC_QBUF, &b);
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cam.ioctl(fd, VIDIOC_STREAMON, &type);
    int frames = 0;
    std::uint32_t first = 0, last = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < end) {
        if (!(cam.poll_in(fd, 50) & POLLIN)) continue;
        struct v4l2_buffer b;
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (cam.ioctl(fd, VIDIOC_DQBUF, &b) != 0) continue;
        if (frames == 0) first = b.sequence;
        last = b.sequence;
        ++frames;
        cam.ioctl(fd, VIDIOC_QBUF, &b);
    }
    CHECK(frames >= 10); /* about 15 in 0.5 s */
    CHECK(frames <= 20);
    /* hold both buffers for 200 ms: about 6 frames are lost */
    std::vector<v4l2_buffer> held;
    while (held.size() < 2) {
        cam.poll_in(fd, 100);
        struct v4l2_buffer b;
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (cam.ioctl(fd, VIDIOC_DQBUF, &b) == 0) held.push_back(b);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (v4l2_buffer &h : held) cam.ioctl(fd, VIDIOC_QBUF, &h);
    cam.poll_in(fd, 200);
    struct v4l2_buffer b;
    std::memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    CHECK_EQ(cam.ioctl(fd, VIDIOC_DQBUF, &b), 0);
    CHECK(b.sequence > held.back().sequence + 3);
    (void)first;
    (void)last;
    cam.close(fd);
}

} // namespace

void test_v4l2()
{
    enumerate();
    buffer_dance();
    realtime();
}
