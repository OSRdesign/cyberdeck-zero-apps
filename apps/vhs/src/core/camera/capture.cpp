/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the capture thread (see capture.hpp).
 */
#include "capture.hpp"

#include "live/clock.hpp"
#include "modes/select.hpp"

#include <linux/videodev2.h>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace vhs {
namespace {

bool same_camera(const CameraRef &a, const CameraRef &b)
{
    if (a.id.valid() || b.id.valid()) return a.id.same(b.id);
    return !a.path.empty() && a.path == b.path; /* no USB identity (a fixed node without sysfs, the fake) */
}

ModeReq fallback_mode(const std::vector<CamMode> &listed)
{
    ModeReq m;
    for (std::uint32_t f : {kMJPG, kYUYV})
        for (const CamMode &c : listed)
            if (c.fourcc == f && (c.fps.empty() || c.has_fps(30))) {
                m.fourcc = c.fourcc;
                m.w = c.w;
                m.h = c.h;
                m.fps = 30;
                return m;
            }
    return m;
}

} // namespace

int current_tid()
{
    return static_cast<int>(syscall(SYS_gettid));
}

std::int64_t mono_ms()
{
    return mono_us() / 1000;
}

const char *state_name(CamState s)
{
    switch (s) {
    case CamState::Searching: return "searching";
    case CamState::NoCamera: return "no-camera";
    case CamState::Streaming: return "streaming";
    case CamState::Lost: return "lost";
    case CamState::Turned: return "turned";
    case CamState::NotBack: return "not-back";
    case CamState::Busy: return "busy";
    case CamState::NoAccess: return "no-access";
    case CamState::Failed: return "failed";
    }
    return "?";
}

Capture::Capture(CaptureConfig cfg, LatestMailbox<Frame> &out) : cfg_(std::move(cfg)), out_(out)
{
    want_ = cfg_.want;
    if (cfg_.buffers < 2) cfg_.buffers = 2;
    if (cfg_.poll_ms < 1) cfg_.poll_ms = 1;
}

Capture::~Capture()
{
    stop();
}

void Capture::start()
{
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void Capture::stop()
{
    if (!thread_.joinable()) return;
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(sleep_m_);
    }
    sleep_cv_.notify_all();
    thread_.join();
}

void Capture::request_mode(const ModeReq &m)
{
    {
        std::lock_guard<std::mutex> lock(status_m_);
        want_ = m;
        mode_change_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(sleep_m_); /* no lost wake-up between its check and its wait */
    }
    sleep_cv_.notify_all();
}

void Capture::reopen()
{
    {
        std::lock_guard<std::mutex> lock(status_m_);
        mode_change_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(sleep_m_);
    }
    sleep_cv_.notify_all();
}

CaptureStatus Capture::status() const
{
    std::lock_guard<std::mutex> lock(status_m_);
    return status_;
}

CamState Capture::state() const
{
    std::lock_guard<std::mutex> lock(status_m_);
    return status_.state;
}

void Capture::set_state(CamState s, const std::string &detail)
{
    std::lock_guard<std::mutex> lock(status_m_);
    if (status_.state != s || status_.detail != detail)
        std::fprintf(stderr, "[vhs] camera: %s%s%s\n", state_name(s), detail.empty() ? "" : ": ", detail.c_str());
    status_.state = s;
    status_.detail = detail;
}

bool Capture::sleep_ms(int ms)
{
    std::unique_lock<std::mutex> lock(sleep_m_);
    sleep_cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] {
        if (stop_) return true;
        std::lock_guard<std::mutex> l2(status_m_);
        return mode_change_;
    });
    return !stop_;
}

void Capture::run()
{
    tid_ = current_tid();
    while (!stop_) {
        if (fd_ < 0) {
            {
                std::lock_guard<std::mutex> lock(status_m_);
                mode_change_ = false;
            }
            CameraRef ref;
            if (!cfg_.finder || !cfg_.finder->find(ref)) {
                absent();
                sleep_ms(lost_ms_ ? cfg_.lost_retry_ms : cfg_.retry_ms);
                continue;
            }
            if (!open_device(ref)) sleep_ms(lost_ms_ ? cfg_.lost_retry_ms : cfg_.retry_ms);
            continue;
        }
        bool change;
        {
            std::lock_guard<std::mutex> lock(status_m_);
            change = mode_change_;
        }
        if (change) {
            close_device();
            continue; /* reopened at once with the new mode */
        }
        const int r = cfg_.api->poll_in(fd_, cfg_.poll_ms);
        if (r < 0) {
            lost(errno, "poll");
            continue;
        }
        if (r == 0) {
            timeouts_.fetch_add(1);
            continue;
        }
        struct v4l2_buffer b;
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (cfg_.api->ioctl(fd_, VIDIOC_DQBUF, &b) != 0) {
            const int e = errno;
            if (e == EAGAIN) {
                if ((r & (POLLERR | POLLHUP)) && ++err_polls_ > 10) lost(ENODEV, "poll error");
                else if (r & (POLLERR | POLLHUP)) sleep_ms(10);
                continue;
            }
            if (e == EIO) {
                errors_.fetch_add(1);
                if (++eio_run_ >= 10) lost(EIO, "DQBUF");
                continue;
            }
            lost(e, "DQBUF");
            continue;
        }
        eio_run_ = 0;
        err_polls_ = 0;
        const std::int64_t now = mono_us();
        if (have_seq_ && b.sequence > last_seq_ + 1) gaps_.fetch_add(b.sequence - last_seq_ - 1);
        last_seq_ = b.sequence;
        have_seq_ = true;
        const bool bad = (b.flags & V4L2_BUF_FLAG_ERROR) || b.bytesused == 0 || b.index >= maps_.size();
        if (bad) {
            errors_.fetch_add(1);
        } else {
            Frame &f = out_.back();
            const std::size_t n = std::min<std::size_t>(b.bytesused, maps_[b.index].len);
            if (f.data.size() < n) f.data.resize(n);
            std::memcpy(f.data.data(), maps_[b.index].p, n);
            f.size = n;
            f.mode = mode_;
            f.stride = stride_;
            f.seq = b.sequence;
            f.ts_us = static_cast<std::int64_t>(b.timestamp.tv_sec) * 1000000 + b.timestamp.tv_usec;
            if (f.ts_us <= 0 || f.ts_us > now) f.ts_us = now;
            f.dq_us = now;
            f.gen = gen_;
            out_.publish();
            frames_.fetch_add(1);
            if (state() == CamState::Turned) set_state(CamState::Streaming, ""); /* held until the first frame */
        }
        if (b.index < maps_.size() && cfg_.api->ioctl(fd_, VIDIOC_QBUF, &b) != 0) lost(errno, "QBUF");
    }
    close_device();
}

void Capture::absent()
{
    if (!had_stream_) {
        set_state(CamState::NoCamera, "");
        return;
    }
    if (lost_ms_ == 0) { /* gone between two opens without having streamed again: count it from now */
        lost_ms_ = mono_ms();
    }
    const bool recent = mono_ms() - lost_ms_ < cfg_.lost_hold_ms;
    set_state(recent ? CamState::Lost : CamState::NotBack, "");
}

void Capture::lost(int err, const char *where)
{
    const bool was_streaming = streaming_;
    close_device();
    if (err == ENODEV || err == ENXIO || err == ENOENT || err == EIO || err == EBADF || err == EPIPE || err == ESHUTDOWN) {
        if (was_streaming) {
            lost_ms_ = mono_ms();
            lost_ref_ = cur_;
            lost_portrait_ = cur_portrait_;
            std::lock_guard<std::mutex> lock(status_m_);
            ++status_.losses;
            status_.lost_ms = lost_ms_;
        }
        std::fprintf(stderr, "[vhs] camera: gone (%s: %s)\n", where, std::strerror(err));
        absent();
    } else {
        set_state(CamState::Failed, std::string(where) + ": " + std::strerror(err));
    }
    sleep_ms(cfg_.lost_retry_ms);
}

bool Capture::open_device(const CameraRef &ref)
{
    V4l2Api &api = *cfg_.api;
    fd_ = api.open(ref.path.c_str());
    if (fd_ < 0) {
        const int e = errno;
        if (e == EACCES || e == EPERM) set_state(CamState::NoAccess, ref.path);
        else if (e == EBUSY) set_state(CamState::Busy, "");
        else if (e == ENOENT || e == ENODEV || e == ENXIO) absent();
        else set_state(CamState::Failed, std::string("open: ") + std::strerror(e));
        return false;
    }
    opens_.fetch_add(1);
    auto fail = [this](CamState s, const std::string &why) {
        close_device();
        set_state(s, why);
        return false;
    };
    const DeviceInfo info = query_device(api, fd_);
    if (!info.capture) return fail(CamState::Failed, "not a video capture device: " + ref.path);
    const std::vector<CamMode> listed = enumerate_modes(api, fd_);
    const bool portrait = camera_portrait(listed);
    const std::int64_t now = mono_ms();
    const bool returning = lost_ms_ != 0 && now - lost_ms_ <= cfg_.back_window_ms && same_camera(ref, lost_ref_);
    const bool turned = returning && portrait != lost_portrait_;
    ModeReq want;
    {
        std::lock_guard<std::mutex> lock(status_m_);
        want = want_;
        status_.card = info.card;
        status_.listed = listed;
        status_.device = ref.path;
        status_.id = ref.id;
        status_.bus_driver = ref.id.hc_driver;
    }
    std::string note;
    ModeReq mode;
    if (!(want.valid() && resolve_mode(listed, want, mode))) {
        if (want.valid()) note = mode_text(want) + " not offered";
        mode = cfg_.choose ? cfg_.choose(listed, ref.id) : ModeReq();
        if (!mode.valid() || !resolve_mode(listed, mode, mode)) mode = fallback_mode(listed);
        if (!mode.valid()) return fail(CamState::Failed, "the camera lists no MJPG or YUYV mode");
    }
    struct v4l2_format f;
    std::memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = static_cast<std::uint32_t>(mode.w);
    f.fmt.pix.height = static_cast<std::uint32_t>(mode.h);
    f.fmt.pix.pixelformat = mode.fourcc;
    f.fmt.pix.field = V4L2_FIELD_ANY;
    if (api.ioctl(fd_, VIDIOC_S_FMT, &f) != 0) {
        const int e = errno;
        return fail(e == EBUSY ? CamState::Busy : CamState::Failed, e == EBUSY ? "" : std::string("S_FMT: ") + std::strerror(e));
    }
    mode.w = static_cast<int>(f.fmt.pix.width);
    mode.h = static_cast<int>(f.fmt.pix.height);
    mode.fourcc = f.fmt.pix.pixelformat;
    stride_ = f.fmt.pix.bytesperline ? static_cast<int>(f.fmt.pix.bytesperline) : mode.w * 2;
    struct v4l2_streamparm p;
    std::memset(&p, 0, sizeof p);
    p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    p.parm.capture.timeperframe.numerator = 1;
    p.parm.capture.timeperframe.denominator = static_cast<std::uint32_t>(mode.fps);
    if (api.ioctl(fd_, VIDIOC_S_PARM, &p) == 0) {
        const int got = interval_fps(p.parm.capture.timeperframe.numerator, p.parm.capture.timeperframe.denominator);
        if (got > 0) mode.fps = got;
    }
    exposure_ = apply_exposure_priority(api, fd_); /* every P1 mode is a fixed 30 fps mode (brief 5.3) */
    struct v4l2_requestbuffers rb;
    std::memset(&rb, 0, sizeof rb);
    rb.count = static_cast<std::uint32_t>(cfg_.buffers);
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (api.ioctl(fd_, VIDIOC_REQBUFS, &rb) != 0) {
        const int e = errno;
        return fail(e == EBUSY ? CamState::Busy : CamState::Failed, e == EBUSY ? "" : std::string("REQBUFS: ") + std::strerror(e));
    }
    if (rb.count < 2) return fail(CamState::Failed, "the driver gave fewer than 2 buffers");
    for (std::uint32_t i = 0; i < rb.count; ++i) {
        struct v4l2_buffer b;
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (api.ioctl(fd_, VIDIOC_QUERYBUF, &b) != 0) return fail(CamState::Failed, std::string("QUERYBUF: ") + std::strerror(errno));
        Mapped m;
        m.len = b.length;
        m.p = api.map(b.length, fd_, static_cast<off_t>(b.m.offset));
        if (!m.p) return fail(CamState::Failed, std::string("mmap: ") + std::strerror(errno));
        maps_.push_back(m);
        if (api.ioctl(fd_, VIDIOC_QBUF, &b) != 0) return fail(CamState::Failed, std::string("QBUF: ") + std::strerror(errno));
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (api.ioctl(fd_, VIDIOC_STREAMON, &type) != 0) {
        const int e = errno;
        if (e == EBUSY) return fail(CamState::Busy, "");
        if (e == ENOSPC) {
            std::fprintf(stderr, "[vhs] camera: STREAMON ENOSPC: the USB bus has not enough bandwidth for %s\n",
                         mode_text(mode).c_str());
            return fail(CamState::Failed, "Not enough USB bandwidth for " + mode_text(mode) + ".");
        }
        return fail(CamState::Failed, std::string("STREAMON: ") + std::strerror(e));
    }
    streaming_ = true;
    had_stream_ = true;
    have_seq_ = false;
    eio_run_ = err_polls_ = 0;
    mode_ = mode;
    cur_ = ref;
    cur_portrait_ = portrait;
    lost_ms_ = 0;
    ++gen_;
    {
        std::lock_guard<std::mutex> lock(status_m_);
        status_.mode = mode;
        status_.gen = gen_;
        status_.exposure = exposure_;
        status_.lost_ms = 0;
        if (turned) ++status_.turns;
    }
    std::fprintf(stderr, "[vhs] camera: %s (%s, %s, id %s, bus %s) streaming %s, %u buffers%s%s%s\n", ref.path.c_str(),
                 info.card.c_str(), info.bus.c_str(), ref.id.valid() ? ref.id.key().c_str() : "-",
                 ref.id.hc_driver.empty() ? "?" : ref.id.hc_driver.c_str(), mode_text(mode).c_str(), rb.count,
                 note.empty() ? "" : ", ", note.c_str(), turned ? ", turned" : (returning ? ", back" : ""));
    std::fprintf(stderr, "[vhs] %s\n", controls_log(exposure_).c_str());
    set_state(turned ? CamState::Turned : CamState::Streaming, note);
    return true;
}

void Capture::close_device()
{
    if (fd_ < 0) return;
    V4l2Api &api = *cfg_.api;
    restore_exposure_priority(api, fd_, exposure_);
    exposure_ = ExposurePriority();
    if (streaming_) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        api.ioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }
    for (const Mapped &m : maps_) api.unmap(m.p, m.len);
    maps_.clear();
    api.close(fd_);
    fd_ = -1;
    std::lock_guard<std::mutex> lock(status_m_);
    status_.mode = ModeReq(); /* fourcc 0: not streaming */
}

} // namespace vhs
