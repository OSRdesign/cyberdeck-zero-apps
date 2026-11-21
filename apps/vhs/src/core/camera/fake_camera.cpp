/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the fake camera (see fake_camera.hpp).
 */
#include "fake_camera.hpp"

#include "controls.hpp"
#include "jpeg/turbojpeg.hpp"
#include "live/clock.hpp"
#include "live/convert.hpp"

#include <linux/videodev2.h>
#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

namespace vhs {
namespace {

constexpr int kFakeFd = FakeCamera::kFd;
constexpr off_t kOffsetStep = 0x1000000; /* QUERYBUF offsets: index * step */
constexpr int kMaxBufs = 8;

CamMode cm(std::uint32_t f, int w, int h, std::vector<int> fps)
{
    CamMode m;
    m.fourcc = f;
    m.w = w;
    m.h = h;
    m.fps = std::move(fps);
    return m;
}

std::uint64_t key(std::uint32_t f, int w, int h)
{
    return (static_cast<std::uint64_t>(f) << 32) | (static_cast<std::uint64_t>(w & 0xFFFF) << 16) |
           static_cast<std::uint64_t>(h & 0xFFFF);
}

void copy_name(std::uint8_t *dst, std::size_t n, const char *s)
{
    std::memset(dst, 0, n);
    std::memcpy(dst, s, std::min(std::strlen(s), n - 1)); /* the rest stays 0: always terminated */
}

class FakeFinder final : public CameraFinder {
public:
    explicit FakeFinder(FakeCamera &cam) : cam_(cam) {}
    bool find(CameraRef &out) override
    {
        if (!cam_.plugged()) return false;
        out = CameraRef();
        out.path = cam_.path();
        out.name = "fake-camera-video-index0";
        out.id = cam_.identity();
        out.card = cam_.card();
        return true;
    }

private:
    FakeCamera &cam_;
};

class FakeBusFinder final : public CameraFinder {
public:
    explicit FakeBusFinder(std::vector<FakeCamera *> cams) : cams_(std::move(cams)) {}
    bool find(CameraRef &out) override
    {
        const std::vector<Candidate> c = candidates();
        std::string pref;
        {
            std::lock_guard<std::mutex> lock(m_);
            pref = prefer_;
        }
        const int i = pick_camera(c, pref);
        if (i < 0) return false;
        out = c[static_cast<std::size_t>(i)].ref;
        if (camera_fell_back(c, pref)) {
            out.fallback = true;
            out.wanted = pref;
        }
        return true;
    }
    void prefer(const std::string &key) override
    {
        std::lock_guard<std::mutex> lock(m_);
        prefer_ = key;
    }
    std::vector<CameraEntry> list() override { return camera_entries(candidates()); }

private:
    std::vector<Candidate> candidates() const
    {
        std::vector<Candidate> out;
        for (FakeCamera *cam : cams_) {
            if (!cam->plugged()) continue;
            Candidate c;
            c.ref.path = cam->path();
            c.ref.name = "fake-camera-" + std::to_string(out.size()) + "-video-index0";
            c.ref.id = cam->identity();
            c.ref.card = cam->card();
            c.ref.orientation = cam->orientation();
            c.by_id = true;
            c.capture = true;
            c.mjpg = lists_format(cam->modes(), kMJPG);
            c.yuyv = lists_format(cam->modes(), kYUYV);
            out.push_back(c);
        }
        return out;
    }
    std::vector<FakeCamera *> cams_;
    std::mutex m_;
    std::string prefer_;
};

} // namespace

void fake_test_card(int w, int h, int k, int n, std::vector<std::uint8_t> &out)
{
    static const std::uint8_t kBars[8][3] = {/* B G R */ {235, 235, 235}, {16, 235, 235}, {235, 235, 16}, {16, 235, 16},
                                             {235, 16, 235}, {16, 16, 235}, {235, 16, 16}, {16, 16, 16}};
    out.assign(static_cast<std::size_t>(w) * h * 4, 0);
    const int bars_h = h * 7 / 10;
    const int side = std::max(4, std::min(w, h) / 5);
    const int span = std::max(1, w - side);
    const int bx = n > 1 ? (k % n) * span / (n - 1) : 0;
    const int by = h * 35 / 100;
    std::uint32_t seed = 2463534242u + static_cast<std::uint32_t>(k) * 7919u;
    for (int y = 0; y < h; ++y) {
        std::uint8_t *p = out.data() + static_cast<std::size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x, p += 4) {
            int b, g, r;
            if (y < bars_h) {
                const std::uint8_t *c = kBars[std::min(7, x * 8 / std::max(1, w))];
                b = c[0];
                g = c[1];
                r = c[2];
            } else {
                b = g = r = 16 + (x * 219) / std::max(1, w - 1);
            }
            if (x >= bx && x < bx + side && y >= by && y < by + side) b = g = r = 250;
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            const int noise = static_cast<int>(seed % 21) - 10;
            p[0] = static_cast<std::uint8_t>(std::max(0, std::min(255, b + noise)));
            p[1] = static_cast<std::uint8_t>(std::max(0, std::min(255, g + noise)));
            p[2] = static_cast<std::uint8_t>(std::max(0, std::min(255, r + noise)));
            p[3] = 0xFF;
        }
    }
}

bool parse_fake_profile(const std::string &s, FakeOptions &o)
{
    if (s == "1" || s == "finecam") {
        o.profile = FakeProfile::FineCam;
        o.portrait = false;
    } else if (s == "portrait") {
        o.profile = FakeProfile::FineCam;
        o.portrait = true;
    } else if (s == "c270") {
        o.profile = FakeProfile::C270;
        o.portrait = false;
    } else if (s == "yuyv") {
        o.profile = FakeProfile::YuyvOnly;
        o.portrait = false;
    } else if (s == "usbfhd") {
        o.profile = FakeProfile::UsbFhd;
        o.portrait = false;
    } else if (s == "usb2") {
        o.profile = FakeProfile::Usb2;
        o.portrait = false;
    } else {
        return false;
    }
    return true;
}

std::unique_ptr<CameraFinder> make_fake_finder(FakeCamera &cam)
{
    return std::make_unique<FakeFinder>(cam);
}

std::unique_ptr<CameraFinder> make_fake_bus_finder(std::vector<FakeCamera *> cams)
{
    return std::make_unique<FakeBusFinder>(std::move(cams));
}

FakeCamera *FakeCameraBus::of(int fd) const
{
    for (FakeCamera *c : cams_) {
        const int base = FakeCamera::kFd + c->options_index();
        if (fd == base || fd == base + FakeCamera::kProbeFd) return c;
    }
    return nullptr;
}

int FakeCameraBus::open(const char *path)
{
    for (FakeCamera *c : cams_)
        if (path && c->path() == path) return c->open(path);
    errno = ENOENT;
    return -1;
}

int FakeCameraBus::close(int fd)
{
    FakeCamera *c = of(fd);
    if (!c) {
        errno = EBADF;
        return -1;
    }
    return c->close(fd);
}

int FakeCameraBus::ioctl(int fd, unsigned long req, void *arg)
{
    FakeCamera *c = of(fd);
    if (!c) {
        errno = EBADF;
        return -1;
    }
    return c->ioctl(fd, req, arg);
}

void *FakeCameraBus::map(std::size_t len, int fd, off_t offset)
{
    FakeCamera *c = of(fd);
    if (!c) {
        errno = EBADF;
        return nullptr;
    }
    return c->map(len, fd, offset);
}

int FakeCameraBus::unmap(void *addr, std::size_t len)
{
    return cams_.empty() ? -1 : cams_.front()->unmap(addr, len);
}

int FakeCameraBus::poll_in(int fd, int timeout_ms)
{
    FakeCamera *c = of(fd);
    if (!c) {
        errno = EBADF;
        return -1;
    }
    return c->poll_in(fd, timeout_ms);
}

FakeCamera::FakeCamera(FakeOptions o) : opt_(std::move(o))
{
    if (opt_.frames < 1) opt_.frames = 1;
    /* the encoder is made here, on the constructing thread: libjpeg-turbo reads the environment (JPEGMEM, JSIMD_*) when
     * a handle is made and on its first use, which must not race with a setenv of the main thread (the harness's
     * lifecycle scenes) */
    enc_ = std::make_unique<JpegEncoder>();
    if (enc_->ok()) {
        std::vector<std::uint8_t> card, jpeg;
        fake_test_card(16, 16, 0, 1, card);
        enc_->encode_bgrx(card.data(), 16, 16, 50, jpeg);
    }
    if (opt_.profile == FakeProfile::C270) {
        id_.vid = "046d";
        id_.pid = "0825";
        id_.serial = "FAKEC270";
        id_.manufacturer = "Logitech";
        id_.product = "C270 HD WEBCAM";
        ctrl_ = FakeCtrl::Present;
    } else if (opt_.profile == FakeProfile::YuyvOnly) {
        id_.vid = "1234";
        id_.pid = "5678";
        id_.product = "Fake YUYV Camera";
    } else if (opt_.profile == FakeProfile::UsbFhd) {
        id_.vid = "13d3";
        id_.pid = "5224";
        id_.manufacturer = "IMC Networks";
        id_.product = "USB FHD Webcam";
    } else if (opt_.profile == FakeProfile::Usb2) {
        id_.vid = "5986";
        id_.pid = "0656";
        id_.manufacturer = "Bison Electronics";
        id_.product = "USB2.0 Camera";
    } else {
        id_.vid = "a108";
        id_.pid = "2240";
        id_.serial = "FAKE0001";
        id_.manufacturer = "Ingenic Semiconductor Co.,Ltd";
        id_.product = "UGREEN FineCam 1080P CM969";
    }
    id_.devpath = "1-1.2";
    if (opt_.profile == FakeProfile::UsbFhd) id_.devpath = "1-9.3";
    if (opt_.profile == FakeProfile::Usb2) id_.devpath = "1-9.4";
    id_.hc_driver = opt_.hc_driver;
    path_ = opt_.path.empty() ? std::string(kPath) : opt_.path;
    if (opt_.ctrl >= 0 && opt_.ctrl <= 3) ctrl_ = static_cast<FakeCtrl>(opt_.ctrl);
    if (!opt_.canned_file.empty()) {
        std::ifstream f(opt_.canned_file, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        canned_ = split_mjpeg(bytes);
        if (canned_.empty()) canned_error_ = "no JPEG frame in " + opt_.canned_file;
    }
    build_modes();
}

FakeCamera::~FakeCamera() = default;

void FakeCamera::build_modes()
{
    modes_.clear();
    int cw = 0, ch = 0;
    bool dht = false;
    if (!canned_.empty() && jpeg_peek(canned_[0].data(), canned_[0].size(), cw, ch, dht)) {
        modes_.push_back(cm(kMJPG, cw, ch, {30}));
        mjpeg_ok_ = true;
        if (!opt_.portrait) {
            modes_.push_back(cm(kYUYV, 640, 480, {30}));
            modes_.push_back(cm(kYUYV, 640, 360, {30}));
        }
        return;
    }
    if (!canned_.empty()) canned_error_ = "the first frame of " + opt_.canned_file + " has no readable size";
    canned_.clear();
    mjpeg_ok_ = TurboJpeg::get().can_encode();
    switch (opt_.profile) {
    case FakeProfile::C270:
        for (const auto &s : {std::make_pair(1280, 960), std::make_pair(1280, 720), std::make_pair(1024, 576),
                              std::make_pair(960, 720), std::make_pair(800, 600), std::make_pair(640, 480),
                              std::make_pair(640, 360), std::make_pair(320, 240)})
            modes_.push_back(cm(kMJPG, s.first, s.second, {30, 25, 20, 15, 10, 5}));
        modes_.push_back(cm(kYUYV, 640, 480, {30, 25, 20, 15, 10, 5}));
        modes_.push_back(cm(kYUYV, 1280, 960, {7, 5}));
        break;
    case FakeProfile::YuyvOnly:
        modes_.push_back(cm(kYUYV, 640, 480, {30, 15}));
        modes_.push_back(cm(kYUYV, 320, 240, {30, 15}));
        break;
    case FakeProfile::UsbFhd:
        for (const auto &s : {std::make_pair(1920, 1080), std::make_pair(1280, 720), std::make_pair(640, 480),
                              std::make_pair(640, 360), std::make_pair(320, 240), std::make_pair(160, 120)})
            modes_.push_back(cm(kMJPG, s.first, s.second, {30}));
        for (const auto &s : {std::make_pair(1920, 1080), std::make_pair(1280, 720), std::make_pair(800, 600),
                              std::make_pair(640, 480), std::make_pair(640, 360)})
            modes_.push_back(cm(kYUYV, s.first, s.second, {30}));
        break;
    case FakeProfile::Usb2:
        for (const auto &s : {std::make_pair(640, 480), std::make_pair(320, 240), std::make_pair(160, 120)})
            modes_.push_back(cm(kMJPG, s.first, s.second, {30}));
        for (const auto &s : {std::make_pair(1280, 720), std::make_pair(640, 480)})
            modes_.push_back(cm(kYUYV, s.first, s.second, {10}));
        break;
    case FakeProfile::FineCam:
        if (opt_.portrait) {
            for (const auto &s : {std::make_pair(1080, 1920), std::make_pair(960, 1280), std::make_pair(720, 1280),
                                  std::make_pair(600, 800), std::make_pair(576, 1024), std::make_pair(480, 640)})
                modes_.push_back(cm(kMJPG, s.first, s.second, {30}));
        } else {
            for (const auto &s : {std::make_pair(1920, 1080), std::make_pair(1280, 960), std::make_pair(1280, 720),
                                  std::make_pair(1024, 576), std::make_pair(800, 600), std::make_pair(640, 480)})
                modes_.push_back(cm(kMJPG, s.first, s.second, {60, 30}));
            modes_.push_back(cm(kYUYV, 640, 480, {30}));
            modes_.push_back(cm(kYUYV, 640, 360, {30}));
        }
        break;
    }
}

int FakeCamera::fail(int err) const
{
    errno = err;
    return -1;
}

std::vector<CamMode> FakeCamera::modes() const
{
    std::lock_guard<std::mutex> lock(m_);
    return modes_;
}

bool FakeCamera::is_open() const
{
    std::lock_guard<std::mutex> lock(m_);
    return fd_ >= 0;
}

bool FakeCamera::plugged() const
{
    std::lock_guard<std::mutex> lock(m_);
    return !unplugged_;
}

CamIdentity FakeCamera::identity() const
{
    std::lock_guard<std::mutex> lock(m_);
    return id_;
}

std::string FakeCamera::card() const
{
    std::lock_guard<std::mutex> lock(m_);
    return (opt_.card_name.empty() ? id_.product : opt_.card_name).substr(0, 31); /* QUERYCAP's card holds 31 characters */
}

int FakeCamera::ctrl_value() const
{
    std::lock_guard<std::mutex> lock(m_);
    return ctrl_value_;
}

int FakeCamera::pace_fps() const
{
    /* the C270 left at exposure_auto_priority 1 slows itself to 15 fps (indoor light, C14) */
    if (opt_.profile == FakeProfile::C270 && ctrl_ != FakeCtrl::Absent && ctrl_value_ != 0) return std::min(fps_, 15);
    return fps_;
}

int FakeCamera::fps_now() const
{
    std::lock_guard<std::mutex> lock(m_);
    return pace_fps();
}

void FakeCamera::unplug()
{
    std::lock_guard<std::mutex> lock(m_);
    unplugged_ = true;
}

void FakeCamera::replug()
{
    std::lock_guard<std::mutex> lock(m_);
    if (unplugged_) replugged_ = true;
    unplugged_ = false;
}

void FakeCamera::set_portrait(bool on)
{
    std::lock_guard<std::mutex> lock(m_);
    if (opt_.portrait == on) return;
    opt_.portrait = on;
    cache_.clear();
    build_modes();
}

void FakeCamera::set_busy(bool on)
{
    std::lock_guard<std::mutex> lock(m_);
    busy_ = on;
}

void FakeCamera::set_no_access(bool on)
{
    std::lock_guard<std::mutex> lock(m_);
    no_access_ = on;
}

void FakeCamera::skip_sequence_every(int n)
{
    std::lock_guard<std::mutex> lock(m_);
    skip_every_ = std::max(0, n);
}

void FakeCamera::error_flag_every(int n)
{
    std::lock_guard<std::mutex> lock(m_);
    error_every_ = std::max(0, n);
}

void FakeCamera::eio_every_dqbuf(bool on)
{
    std::lock_guard<std::mutex> lock(m_);
    eio_ = on;
}

void FakeCamera::set_identity(const CamIdentity &id)
{
    std::lock_guard<std::mutex> lock(m_);
    id_ = id;
}

void FakeCamera::set_ctrl_value(int v)
{
    std::lock_guard<std::mutex> lock(m_);
    ctrl_value_ = v;
}

int FakeCamera::open(const char *path)
{
    std::lock_guard<std::mutex> lock(m_);
    if (!path || path_ != path || unplugged_) return fail(ENOENT);
    if (no_access_) return fail(EACCES);
    if (fd_ >= 0 && opt_.probe_ok && !probe_open_) { /* uvcvideo lets a second open ask (the chooser's scan) */
        probe_open_ = true;
        return kFakeFd + opt_.index + kProbeFd;
    }
    if (fd_ >= 0) return fail(EBUSY);
    fd_ = kFakeFd + opt_.index;
    opens_.fetch_add(1);
    return fd_;
}

int FakeCamera::close(int fd)
{
    std::lock_guard<std::mutex> lock(m_);
    if (probe_open_ && fd == kFakeFd + opt_.index + kProbeFd) {
        probe_open_ = false;
        return 0;
    }
    if (fd != fd_ || fd_ < 0) return fail(EBADF);
    fd_ = -1;
    streaming_ = false;
    bufs_.clear();
    queued_.clear();
    done_.clear();
    return 0;
}

std::size_t FakeCamera::buf_size() const
{
    return static_cast<std::size_t>(w_) * h_ * 2 + 4096; /* uvcvideo: the frame's worst case */
}

const std::vector<std::vector<std::uint8_t>> &FakeCamera::frames_for(std::uint32_t f, int w, int h)
{
    auto it = cache_.find(key(f, w, h));
    if (it != cache_.end()) return it->second;
    std::vector<std::vector<std::uint8_t>> frames;
    if (f == kMJPG && !canned_.empty()) {
        frames = canned_;
    } else if (f == kMJPG) {
        JpegEncoder &enc = *enc_;
        std::vector<std::uint8_t> card;
        for (int k = 0; k < opt_.frames; ++k) {
            std::vector<std::uint8_t> jpeg;
            if (enc.ok()) {
                fake_test_card(w, h, k, opt_.frames, card);
                enc.encode_bgrx(card.data(), w, h, opt_.jpeg_quality, jpeg);
            }
            if (jpeg.empty()) jpeg = {0xFF, 0xD8, 0xFF, 0xD9};
            frames.push_back(std::move(jpeg));
        }
    } else {
        std::vector<std::uint8_t> card;
        for (int k = 0; k < opt_.frames; ++k) {
            fake_test_card(w, h, k, opt_.frames, card);
            std::vector<std::uint8_t> yuyv(static_cast<std::size_t>(w) * h * 2);
            for (int y = 0; y < h; ++y) {
                const std::uint8_t *s = card.data() + static_cast<std::size_t>(y) * w * 4;
                std::uint8_t *d = yuyv.data() + static_cast<std::size_t>(y) * w * 2;
                for (int x = 0; x + 1 < w; x += 2, s += 8, d += 4) {
                    int y0, u0, v0, y1, u1, v1;
                    rgb_to_yuv601(s[2], s[1], s[0], y0, u0, v0);
                    rgb_to_yuv601(s[6], s[5], s[4], y1, u1, v1);
                    d[0] = static_cast<std::uint8_t>(y0);
                    d[1] = static_cast<std::uint8_t>((u0 + u1) / 2);
                    d[2] = static_cast<std::uint8_t>(y1);
                    d[3] = static_cast<std::uint8_t>((v0 + v1) / 2);
                }
            }
            frames.push_back(std::move(yuyv));
        }
    }
    return cache_.emplace(key(f, w, h), std::move(frames)).first->second;
}

void FakeCamera::produce(std::int64_t now)
{
    ++produced_;
    const std::uint32_t seq = seq_++;
    if (skip_every_ > 0 && produced_ % static_cast<std::uint64_t>(skip_every_) == 0) ++seq_; /* a lost frame */
    /* a frame goes to the oldest buffer queued before its time; none: the frame is lost (as the driver does) */
    if (queued_.empty() || queued_.front().second > now) return;
    const int i = queued_.front().first;
    queued_.pop_front();
    Buf &b = bufs_[static_cast<std::size_t>(i)];
    const auto &frames = frames_for(pf_, w_, h_);
    const auto &src = frames[static_cast<std::size_t>(produced_ - 1) % frames.size()];
    const std::size_t n = std::min(src.size(), b.mem.size());
    std::memcpy(b.mem.data(), src.data(), n);
    b.bytes = static_cast<std::uint32_t>(n);
    b.seq = seq;
    b.ts_us = now;
    b.flags = (error_every_ > 0 && produced_ % static_cast<std::uint64_t>(error_every_) == 0) ? V4L2_BUF_FLAG_ERROR : 0;
    b.queued = false;
    b.done = true;
    done_.push_back(i);
}

void FakeCamera::advance(std::int64_t now)
{
    if (!streaming_ || unplugged_) return;
    if (!opt_.realtime) {
        if (done_.empty() && !queued_.empty() && now >= next_due_us_) produce(now);
        return;
    }
    const std::int64_t period = 1000000 / std::max(1, pace_fps());
    if (now - next_due_us_ > 1000000) { /* a long stall (debugger, suspend): skip ahead */
        const std::int64_t missed = (now - next_due_us_) / period;
        seq_ += static_cast<std::uint32_t>(missed);
        next_due_us_ += missed * period;
    }
    while (next_due_us_ <= now) {
        produce(next_due_us_);
        next_due_us_ += period;
    }
}

int FakeCamera::poll_in(int fd, int timeout_ms)
{
    const std::int64_t deadline = mono_us() + static_cast<std::int64_t>(std::max(0, timeout_ms)) * 1000;
    for (;;) {
        std::int64_t wake;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (fd != fd_ || fd_ < 0) return fail(EBADF);
            if (unplugged_) return POLLERR | POLLHUP; /* uvcvideo after a disconnect */
            if (!streaming_) return POLLERR;          /* vb2: poll while not streaming */
            if (eio_) return POLLIN;                  /* DQBUF fails with EIO */
            const std::int64_t now = mono_us();
            advance(now);
            if (!done_.empty()) return POLLIN;
            if (now >= deadline) return 0;
            wake = opt_.realtime ? std::min(deadline, next_due_us_) : std::min(deadline, std::max(now + 2000, next_due_us_));
        }
        const std::int64_t now = mono_us();
        std::this_thread::sleep_for(std::chrono::microseconds(std::max<std::int64_t>(100, wake - now)));
    }
}

void *FakeCamera::map(std::size_t len, int fd, off_t offset)
{
    std::lock_guard<std::mutex> lock(m_);
    if (fd != fd_ || fd_ < 0 || offset % kOffsetStep != 0) {
        errno = EINVAL;
        return nullptr;
    }
    const std::size_t i = static_cast<std::size_t>(offset / kOffsetStep);
    if (i >= bufs_.size() || len > bufs_[i].mem.size()) {
        errno = EINVAL;
        return nullptr;
    }
    return bufs_[i].mem.data();
}

int FakeCamera::unmap(void *addr, std::size_t len)
{
    (void)len;
    return addr ? 0 : fail(EINVAL);
}

int FakeCamera::ioctl(int fd, unsigned long req, void *arg)
{
    std::lock_guard<std::mutex> lock(m_);
    const bool probe = probe_open_ && fd == kFakeFd + opt_.index + kProbeFd;
    if (!probe && (fd != fd_ || fd_ < 0)) return fail(EBADF);
    if (unplugged_) return fail(ENODEV);
    if (probe && req != VIDIOC_QUERYCAP && req != VIDIOC_ENUM_FMT && req != VIDIOC_ENUM_FRAMESIZES &&
        req != VIDIOC_ENUM_FRAMEINTERVALS && req != VIDIOC_G_CTRL && req != VIDIOC_QUERYCTRL)
        return fail(EBUSY);
    switch (req) {
    case VIDIOC_QUERYCAP: {
        auto *c = static_cast<struct v4l2_capability *>(arg);
        std::memset(c, 0, sizeof *c);
        copy_name(c->driver, sizeof c->driver, "uvcvideo");
        copy_name(c->card, sizeof c->card, (opt_.card_name.empty() ? id_.product : opt_.card_name).c_str());
        copy_name(c->bus_info, sizeof c->bus_info, "fake:usb-1.2");
        c->version = 0x060c00;
        c->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        c->capabilities = c->device_caps | V4L2_CAP_DEVICE_CAPS;
        return 0;
    }
    case VIDIOC_ENUM_FMT: {
        auto *f = static_cast<struct v4l2_fmtdesc *>(arg);
        if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) return fail(EINVAL);
        std::vector<std::uint32_t> fmts;
        for (const CamMode &m : modes_)
            if (std::find(fmts.begin(), fmts.end(), m.fourcc) == fmts.end()) fmts.push_back(m.fourcc);
        if (f->index >= fmts.size()) return fail(EINVAL);
        f->pixelformat = fmts[f->index];
        f->flags = f->pixelformat == kMJPG ? V4L2_FMT_FLAG_COMPRESSED : 0;
        copy_name(f->description, sizeof f->description, f->pixelformat == kMJPG ? "Motion-JPEG" : "YUYV 4:2:2");
        return 0;
    }
    case VIDIOC_ENUM_FRAMESIZES: {
        auto *s = static_cast<struct v4l2_frmsizeenum *>(arg);
        std::uint32_t n = 0;
        for (const CamMode &m : modes_) {
            if (m.fourcc != s->pixel_format) continue;
            if (n++ == s->index) {
                s->type = V4L2_FRMSIZE_TYPE_DISCRETE;
                s->discrete.width = static_cast<std::uint32_t>(m.w);
                s->discrete.height = static_cast<std::uint32_t>(m.h);
                return 0;
            }
        }
        return fail(EINVAL);
    }
    case VIDIOC_ENUM_FRAMEINTERVALS: {
        auto *iv = static_cast<struct v4l2_frmivalenum *>(arg);
        for (const CamMode &m : modes_) {
            if (m.fourcc != iv->pixel_format || m.w != static_cast<int>(iv->width) || m.h != static_cast<int>(iv->height))
                continue;
            if (iv->index >= m.fps.size()) return fail(EINVAL);
            iv->type = V4L2_FRMIVAL_TYPE_DISCRETE;
            iv->discrete.numerator = 1;
            iv->discrete.denominator = static_cast<std::uint32_t>(m.fps[iv->index]);
            return 0;
        }
        return fail(EINVAL);
    }
    case VIDIOC_G_FMT:
    case VIDIOC_S_FMT:
    case VIDIOC_TRY_FMT: {
        auto *f = static_cast<struct v4l2_format *>(arg);
        if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) return fail(EINVAL);
        if (req == VIDIOC_G_FMT) {
            if (!pf_) return fail(EINVAL);
        } else {
            if (req == VIDIOC_S_FMT && (busy_ || streaming_ || !bufs_.empty())) return fail(EBUSY);
            /* as uvcvideo: the nearest listed size of that format (the first format when it is not listed) */
            const CamMode *best = nullptr;
            for (const CamMode &m : modes_) {
                if (m.fourcc != f->fmt.pix.pixelformat) continue;
                if (!best || std::abs(m.w - static_cast<int>(f->fmt.pix.width)) + std::abs(m.h - static_cast<int>(f->fmt.pix.height)) <
                                 std::abs(best->w - static_cast<int>(f->fmt.pix.width)) + std::abs(best->h - static_cast<int>(f->fmt.pix.height)))
                    best = &m;
            }
            if (!best) best = &modes_.front();
            if (req == VIDIOC_TRY_FMT) {
                f->fmt.pix.pixelformat = best->fourcc;
                f->fmt.pix.width = static_cast<std::uint32_t>(best->w);
                f->fmt.pix.height = static_cast<std::uint32_t>(best->h);
                return 0;
            }
            pf_ = best->fourcc;
            w_ = best->w;
            h_ = best->h;
            fps_ = best->fps.empty() ? 30 : best->fps.back();
            for (int f2 : best->fps)
                if (f2 == 30) fps_ = 30;
        }
        f->fmt.pix.pixelformat = pf_;
        f->fmt.pix.width = static_cast<std::uint32_t>(w_);
        f->fmt.pix.height = static_cast<std::uint32_t>(h_);
        f->fmt.pix.field = V4L2_FIELD_NONE;
        f->fmt.pix.bytesperline = pf_ == kYUYV ? static_cast<std::uint32_t>(w_ * 2) : 0;
        f->fmt.pix.sizeimage = static_cast<std::uint32_t>(buf_size());
        f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
        return 0;
    }
    case VIDIOC_S_PARM:
    case VIDIOC_G_PARM: {
        auto *p = static_cast<struct v4l2_streamparm *>(arg);
        if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) return fail(EINVAL);
        if (req == VIDIOC_S_PARM) {
            const int want = interval_fps(p->parm.capture.timeperframe.numerator, p->parm.capture.timeperframe.denominator);
            for (const CamMode &m : modes_)
                if (m.fourcc == pf_ && m.w == w_ && m.h == h_)
                    for (int f2 : m.fps)
                        if (f2 == want) fps_ = want;
        }
        p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
        p->parm.capture.timeperframe.numerator = 1;
        p->parm.capture.timeperframe.denominator = static_cast<std::uint32_t>(fps_);
        return 0;
    }
    case VIDIOC_QUERYCTRL: {
        auto *q = static_cast<struct v4l2_queryctrl *>(arg);
        if (q->id != kCidExposureAutoPriority || ctrl_ == FakeCtrl::Absent) return fail(EINVAL);
        const std::uint32_t id = q->id;
        std::memset(q, 0, sizeof *q);
        q->id = id;
        q->type = V4L2_CTRL_TYPE_BOOLEAN;
        copy_name(q->name, sizeof q->name, "Exposure, Dynamic Framerate");
        q->minimum = 0;
        q->maximum = 1;
        q->step = 1;
        q->default_value = 0;
        if (ctrl_ == FakeCtrl::Inactive) q->flags |= V4L2_CTRL_FLAG_INACTIVE;
        if (ctrl_ == FakeCtrl::ReadOnly) q->flags |= V4L2_CTRL_FLAG_READ_ONLY;
        return 0;
    }
    case VIDIOC_G_CTRL:
    case VIDIOC_S_CTRL: {
        auto *c = static_cast<struct v4l2_control *>(arg);
        if (c->id == 0x009a0922 && req == VIDIOC_G_CTRL && opt_.orientation >= 0) { /* V4L2_CID_CAMERA_ORIENTATION */
            c->value = opt_.orientation;
            return 0;
        }
        if (c->id != kCidExposureAutoPriority || ctrl_ == FakeCtrl::Absent) return fail(EINVAL);
        if (req == VIDIOC_G_CTRL) {
            c->value = ctrl_value_;
            return 0;
        }
        if (ctrl_ == FakeCtrl::ReadOnly) return fail(EACCES);
        if (c->value < 0 || c->value > 1) return fail(ERANGE);
        ctrl_value_ = c->value;
        ctrl_sets_.fetch_add(1);
        return 0;
    }
    case VIDIOC_REQBUFS: {
        auto *r = static_cast<struct v4l2_requestbuffers *>(arg);
        if (r->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || r->memory != V4L2_MEMORY_MMAP) return fail(EINVAL);
        if (streaming_) return fail(EBUSY);
        if (busy_) return fail(EBUSY);
        bufs_.clear();
        queued_.clear();
        done_.clear();
        const std::uint32_t n = std::min<std::uint32_t>(r->count, kMaxBufs);
        for (std::uint32_t i = 0; i < n; ++i) {
            Buf b;
            b.mem.assign(buf_size(), 0);
            bufs_.push_back(std::move(b));
        }
        r->count = n;
        return 0;
    }
    case VIDIOC_QUERYBUF:
    case VIDIOC_QBUF:
    case VIDIOC_DQBUF: {
        auto *b = static_cast<struct v4l2_buffer *>(arg);
        if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || b->memory != V4L2_MEMORY_MMAP) return fail(EINVAL);
        if (req == VIDIOC_DQBUF) {
            if (!streaming_) return fail(EINVAL);
            if (eio_) return fail(EIO);
            advance(mono_us());
            if (done_.empty()) return fail(EAGAIN);
            const int i = done_.front();
            done_.pop_front();
            Buf &x = bufs_[static_cast<std::size_t>(i)];
            x.done = false;
            b->index = static_cast<std::uint32_t>(i);
            b->bytesused = x.bytes;
            b->sequence = x.seq;
            b->flags = x.flags | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
            b->field = V4L2_FIELD_NONE;
            b->timestamp.tv_sec = static_cast<time_t>(x.ts_us / 1000000);
            b->timestamp.tv_usec = static_cast<suseconds_t>(x.ts_us % 1000000);
            b->length = static_cast<std::uint32_t>(x.mem.size());
            return 0;
        }
        if (b->index >= bufs_.size()) return fail(EINVAL);
        Buf &x = bufs_[b->index];
        if (req == VIDIOC_QUERYBUF) {
            b->length = static_cast<std::uint32_t>(x.mem.size());
            b->m.offset = static_cast<std::uint32_t>(b->index * kOffsetStep);
            b->flags = (x.queued ? V4L2_BUF_FLAG_QUEUED : 0) | (x.done ? V4L2_BUF_FLAG_DONE : 0);
            return 0;
        }
        if (x.queued || x.done) return fail(EINVAL);
        x.queued = true;
        queued_.emplace_back(static_cast<int>(b->index), mono_us());
        return 0;
    }
    case VIDIOC_STREAMON: {
        if (bufs_.empty() || !pf_) return fail(EINVAL);
        if (busy_) return fail(EBUSY);
        if (!streaming_) {
            frames_for(pf_, w_, h_); /* made (encoded) now, not inside the first frame's time slot */
            streaming_ = true;
            streamons_.fetch_add(1);
            std::int64_t first = opt_.realtime ? 1000000 / std::max(1, pace_fps()) : 0;
            if (opt_.start_delay_ms > 0) first = static_cast<std::int64_t>(opt_.start_delay_ms) * 1000;
            if (replugged_ && opt_.restart_delay_ms > 0) first = static_cast<std::int64_t>(opt_.restart_delay_ms) * 1000;
            replugged_ = false;
            next_due_us_ = mono_us() + first;
        }
        return 0;
    }
    case VIDIOC_STREAMOFF: {
        streaming_ = false;
        for (Buf &x : bufs_) x.queued = x.done = false;
        queued_.clear();
        done_.clear();
        return 0;
    }
    default: return fail(ENOTTY);
    }
}

} // namespace vhs
