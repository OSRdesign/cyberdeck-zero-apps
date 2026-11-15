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

void Capture::rechoose()
{
    {
        std::lock_guard<std::mutex> lock(status_m_);
        mode_change_ = true; /* an in-place switch to what the chooser says now (nothing when it is the same) */
    }
    {
        std::lock_guard<std::mutex> lock(sleep_m_);
    }
    sleep_cv_.notify_all();
}

void Capture::reopen()
{
    {
        std::lock_guard<std::mutex> lock(status_m_);
        mode_change_ = true;
        reopen_full_ = true; /* the settings changed: the full close and open */
    }
    {
        std::lock_guard<std::mutex> lock(sleep_m_);
    }
    sleep_cv_.notify_all();
}

void Capture::hold_mode(const ModeReq &m)
{
    {
        std::lock_guard<std::mutex> lock(status_m_);
        hold_ = m;
        hold_on_ = true;
        const bool need = !(status_.mode.fourcc == m.fourcc && status_.mode.w == m.w && status_.mode.h == m.h);
        hold_switched_ = need;
        if (need) mode_change_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(sleep_m_);
    }
    sleep_cv_.notify_all();
}

void Capture::release_hold()
{
    {
        std::lock_guard<std::mutex> lock(status_m_);
        if (!hold_on_) return;
        hold_on_ = false;
        if (hold_switched_) mode_change_ = true; /* back to the preview's mode */
        hold_switched_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(sleep_m_);
    }
    sleep_cv_.notify_all();
}

bool Capture::holding() const
{
    std::lock_guard<std::mutex> lock(status_m_);
    return hold_on_;
}

void Capture::request_photo(const ModeReq &expect, std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(photo_m_);
    photo_expect_ = expect;
    photo_id_ = id;
    photo_ready_ = false;
    photo_ = PhotoShot();
    photo_armed_ = true;
}

void Capture::cancel_photo()
{
    std::lock_guard<std::mutex> lock(photo_m_);
    photo_armed_ = false;
    photo_ready_ = false;
}

bool Capture::take_photo(PhotoShot &out)
{
    std::lock_guard<std::mutex> lock(photo_m_);
    if (!photo_ready_) return false;
    out = std::move(photo_);
    photo_ = PhotoShot();
    photo_ready_ = false;
    return true;
}

void Capture::photo_from(const Frame &f)
{
    if (!photo_armed_.load() || since_on_ <= static_cast<std::uint64_t>(kWarmupFrames)) return;
    std::lock_guard<std::mutex> lock(photo_m_);
    if (!photo_armed_.load()) return;
    const ModeReq &e = photo_expect_;
    if (e.valid() && !(e.fourcc == f.mode.fourcc && e.w == f.mode.w && e.h == f.mode.h)) return;
    photo_.id = photo_id_;
    photo_.ok = true;
    photo_.data.assign(f.data.begin(), f.data.begin() + static_cast<std::ptrdiff_t>(f.size));
    photo_.mode = f.mode;
    photo_.ts_us = f.ts_us;
    photo_.seq = f.seq;
    photo_ready_ = true;
    photo_armed_ = false;
}

void Capture::set_tap(FrameTap *tap, const ModeReq &expect)
{
    std::lock_guard<std::mutex> lock(tap_m_);
    tap_ = tap;
    tap_expect_ = expect;
    tap_on_ = tap != nullptr;
}

void Capture::clear_tap()
{
    std::lock_guard<std::mutex> lock(tap_m_); /* waits for a tap_frame() in progress */
    tap_ = nullptr;
    tap_on_ = false;
}

void Capture::tap_from(const Frame &f)
{
    if (!tap_on_.load() || since_on_ <= static_cast<std::uint64_t>(kWarmupFrames)) return;
    std::lock_guard<std::mutex> lock(tap_m_);
    if (!tap_) return;
    const ModeReq &e = tap_expect_;
    if (e.valid() && (e.fourcc != f.mode.fourcc || e.w != f.mode.w || e.h != f.mode.h)) return;
    tap_->tap_frame(f.data.data(), f.size, f.ts_us, f.mode);
    tapped_.fetch_add(1);
}

void Capture::photo_fail(const std::string &why)
{
    if (!photo_armed_.load()) return;
    std::lock_guard<std::mutex> lock(photo_m_);
    if (!photo_armed_.load()) return;
    photo_ = PhotoShot();
    photo_.id = photo_id_;
    photo_.error = why;
    photo_ready_ = true;
    photo_armed_ = false;
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
                reopen_full_ = false;
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
        bool change, full;
        {
            std::lock_guard<std::mutex> lock(status_m_);
            change = mode_change_;
            full = reopen_full_;
        }
        if (change) {
            if (!full && streaming_ && switch_in_place()) continue;
            const std::int64_t t0 = mono_us();
            close_device();
            pending_close_ms_ = static_cast<int>((mono_us() - t0) / 1000);
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
            ++since_on_;
            if (since_on_ == 1 || since_on_ == static_cast<std::uint64_t>(kWarmupFrames) + 1) { /* the start's timing */
                std::lock_guard<std::mutex> lock(status_m_);
                if (since_on_ == 1) {
                    t_first_us_ = now;
                    timing_.first_ms = static_cast<int>((now - t_on_us_) / 1000);
                } else {
                    timing_.warm_ms = static_cast<int>((now - t_first_us_) / 1000);
                }
            }
            photo_from(f); /* before publish: back() is still this frame */
            tap_from(f);
            out_.publish();
            frames_.fetch_add(1);
            if (state() == CamState::Turned) set_state(CamState::Streaming, ""); /* held until the first frame */
        }
        if (b.index < maps_.size() && cfg_.api->ioctl(fd_, VIDIOC_QBUF, &b) != 0) lost(errno, "QBUF");
    }
    close_device();
    photo_fail("the camera was released");
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
        photo_fail("the camera went away");
        absent();
    } else {
        set_state(CamState::Failed, std::string(where) + ": " + std::strerror(err));
    }
    sleep_ms(cfg_.lost_retry_ms);
}

bool Capture::open_device(const CameraRef &ref)
{
    V4l2Api &api = *cfg_.api;
    const std::int64_t t_open = mono_us();
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
    const int open_ms = static_cast<int>((mono_us() - t_open) / 1000);
    const bool portrait = camera_portrait(listed);
    const std::int64_t now = mono_ms();
    const bool returning = lost_ms_ != 0 && now - lost_ms_ <= cfg_.back_window_ms && same_camera(ref, lost_ref_);
    const bool turned = returning && portrait != lost_portrait_;
    {
        std::lock_guard<std::mutex> lock(status_m_);
        status_.card = info.card;
        status_.listed = listed;
        status_.device = ref.path;
        status_.id = ref.id;
        status_.bus_driver = ref.id.hc_driver;
    }
    std::string note;
    const ModeReq mode = pick_mode(listed, ref.id, &note);
    if (!mode.valid()) return fail(CamState::Failed, "the camera lists no MJPG or YUYV mode");
    StreamTiming t;
    t.close_ms = pending_close_ms_;
    t.open_ms = open_ms;
    pending_close_ms_ = -1;
    std::string why;
    CamState bad = CamState::Failed;
    if (!start_stream(mode, true, t, why, bad)) return fail(bad, why);
    cur_ = ref;
    cur_portrait_ = portrait;
    lost_ms_ = 0;
    {
        std::lock_guard<std::mutex> lock(status_m_);
        status_.lost_ms = 0;
        if (turned) ++status_.turns;
    }
    std::fprintf(stderr, "[vhs] camera: %s (%s, %s, id %s, bus %s) streaming %s, %zu buffers%s%s%s\n", ref.path.c_str(),
                 info.card.c_str(), info.bus.c_str(), ref.id.valid() ? ref.id.key().c_str() : "-",
                 ref.id.hc_driver.empty() ? "?" : ref.id.hc_driver.c_str(), mode_text(mode_).c_str(), maps_.size(),
                 note.empty() ? "" : ", ", note.c_str(), turned ? ", turned" : (returning ? ", back" : ""));
    std::fprintf(stderr, "[vhs] %s\n", controls_log(exposure_).c_str());
    set_state(turned ? CamState::Turned : CamState::Streaming, note);
    return true;
}

ModeReq Capture::pick_mode(const std::vector<CamMode> &listed, const CamIdentity &id, std::string *note)
{
    ModeReq want, hold;
    bool holding = false;
    {
        std::lock_guard<std::mutex> lock(status_m_);
        want = want_;
        hold = hold_;
        holding = hold_on_;
    }
    ModeReq mode;
    if (holding && hold.valid() && resolve_mode(listed, hold, mode)) return mode; /* the hold (a Light photo, a scene) */
    if (want.valid() && resolve_mode(listed, want, mode)) return mode;
    if (want.valid() && note) *note = mode_text(want) + " not offered";
    mode = cfg_.choose ? cfg_.choose(listed, id) : ModeReq();
    if (!mode.valid() || !resolve_mode(listed, mode, mode)) mode = fallback_mode(listed);
    return mode;
}

bool Capture::start_stream(const ModeReq &want_mode, bool controls, StreamTiming t, std::string &why, CamState &bad)
{
    V4l2Api &api = *cfg_.api;
    ModeReq mode = want_mode;
    std::int64_t t0 = mono_us();
    struct v4l2_format f;
    std::memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = static_cast<std::uint32_t>(mode.w);
    f.fmt.pix.height = static_cast<std::uint32_t>(mode.h);
    f.fmt.pix.pixelformat = mode.fourcc;
    f.fmt.pix.field = V4L2_FIELD_ANY;
    if (api.ioctl(fd_, VIDIOC_S_FMT, &f) != 0) {
        const int e = errno;
        bad = e == EBUSY ? CamState::Busy : CamState::Failed;
        why = e == EBUSY ? "" : std::string("S_FMT: ") + std::strerror(e);
        return false;
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
    t.set_ms = static_cast<int>((mono_us() - t0) / 1000);
    if (controls) { /* every P1 mode is a fixed 30 fps mode (brief 5.3); a switch in place keeps what the open set */
        t0 = mono_us();
        exposure_ = apply_exposure_priority(api, fd_);
        t.controls_ms = static_cast<int>((mono_us() - t0) / 1000);
    }
    t0 = mono_us();
    struct v4l2_requestbuffers rb;
    std::memset(&rb, 0, sizeof rb);
    rb.count = static_cast<std::uint32_t>(cfg_.buffers);
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (api.ioctl(fd_, VIDIOC_REQBUFS, &rb) != 0) {
        const int e = errno;
        bad = e == EBUSY ? CamState::Busy : CamState::Failed;
        why = e == EBUSY ? "" : std::string("REQBUFS: ") + std::strerror(e);
        return false;
    }
    if (rb.count < 2) {
        why = "the driver gave fewer than 2 buffers";
        return false;
    }
    for (std::uint32_t i = 0; i < rb.count; ++i) {
        struct v4l2_buffer b;
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (api.ioctl(fd_, VIDIOC_QUERYBUF, &b) != 0) {
            why = std::string("QUERYBUF: ") + std::strerror(errno);
            return false;
        }
        Mapped m;
        m.len = b.length;
        m.p = api.map(b.length, fd_, static_cast<off_t>(b.m.offset));
        if (!m.p) {
            why = std::string("mmap: ") + std::strerror(errno);
            return false;
        }
        maps_.push_back(m);
        if (api.ioctl(fd_, VIDIOC_QBUF, &b) != 0) {
            why = std::string("QBUF: ") + std::strerror(errno);
            return false;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (api.ioctl(fd_, VIDIOC_STREAMON, &type) != 0) {
        const int e = errno;
        bad = e == EBUSY ? CamState::Busy : CamState::Failed;
        if (e == ENOSPC) {
            std::fprintf(stderr, "[vhs] camera: STREAMON ENOSPC: the USB bus has not enough bandwidth for %s\n",
                         mode_text(mode).c_str());
            why = "Not enough USB bandwidth for " + mode_text(mode) + ".";
        } else if (e != EBUSY) {
            why = std::string("STREAMON: ") + std::strerror(e);
        }
        return false;
    }
    t_on_us_ = mono_us();
    t.bufs_ms = static_cast<int>((t_on_us_ - t0) / 1000);
    streaming_ = true;
    had_stream_ = true;
    have_seq_ = false;
    since_on_ = 0;
    eio_run_ = err_polls_ = 0;
    mode_ = mode;
    ++gen_;
    t.gen = gen_;
    std::lock_guard<std::mutex> lock(status_m_);
    status_.mode = mode;
    status_.gen = gen_;
    status_.exposure = exposure_;
    timing_ = t;
    return true;
}

bool Capture::switch_in_place()
{
    /* a mode change on the open device (STREAMOFF, buffers freed, S_FMT, STREAMON): no close, no USB resume, no new
     * mode listing, the controls kept; any refusal falls back to the full close and reopen */
    std::vector<CamMode> listed;
    {
        std::lock_guard<std::mutex> lock(status_m_);
        listed = status_.listed;
        mode_change_ = false;
    }
    const ModeReq mode = pick_mode(listed, cur_.id, nullptr);
    if (!mode.valid()) return false;
    if (mode.fourcc == mode_.fourcc && mode.w == mode_.w && mode.h == mode_.h && mode.fps == mode_.fps) return true;
    V4l2Api &api = *cfg_.api;
    const std::int64_t t0 = mono_us();
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    api.ioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;
    for (const Mapped &m : maps_) api.unmap(m.p, m.len);
    maps_.clear();
    struct v4l2_requestbuffers rb;
    std::memset(&rb, 0, sizeof rb);
    rb.count = 0;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (api.ioctl(fd_, VIDIOC_REQBUFS, &rb) != 0) return false;
    StreamTiming t;
    t.in_place = true;
    t.close_ms = static_cast<int>((mono_us() - t0) / 1000);
    t.open_ms = 0;
    std::string why;
    CamState bad = CamState::Failed;
    if (!start_stream(mode, false, t, why, bad)) {
        std::fprintf(stderr, "[vhs] camera: switch in place to %s refused (%s): reopening\n", mode_text(mode).c_str(),
                     why.empty() ? "busy" : why.c_str());
        return false;
    }
    std::fprintf(stderr, "[vhs] camera: switched in place to %s\n", mode_text(mode_).c_str());
    return true;
}

StreamTiming Capture::stream_timing() const
{
    std::lock_guard<std::mutex> lock(status_m_);
    return timing_;
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
