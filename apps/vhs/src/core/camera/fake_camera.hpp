/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the fake camera (from the spike), a V4l2Api that behaves like a UVC camera: QUERYCAP, the ENUM_FMT /
 * ENUM_FRAMESIZES / ENUM_FRAMEINTERVALS lists, S_FMT / S_PARM, QUERYCTRL / G_CTRL / S_CTRL (exposure_auto_priority),
 * REQBUFS / QUERYBUF / mmap, QBUF / DQBUF on a non-blocking descriptor, STREAMON / OFF, poll. It is the --fake camera of
 * the app and the bench (no camera needed), the scenes' camera (VHS_FAKE), and the unit tests' fake ioctl backend.
 *
 * Profiles:
 *   FineCam    the UGREEN FineCam CM969, switch position B: MJPG 1920x1080 ... 640x480 at 60 and 30 fps, YUYV 640x480
 *              and 640x360 at 30; position A (`portrait`): the portrait MJPEG sizes at 30 fps only. No exposure control
 *              unless asked (its state on the real camera is board test B29's question)
 *   C270       the Logitech C270: MJPG up to 1280x960 (no 1080p), exposure_auto_priority present and 1; while it is 1
 *              the camera delivers 15 fps (it slows itself down indoors, C14)
 *   YuyvOnly   a camera without MJPEG (YUYV 640x480 / 320x240 at 30)
 *
 * Frames: a moving test card (colour bars, a grey ramp, a white box crossing the picture, some noise so JPEG has work to
 * do). YUYV frames are made directly; MJPEG frames are encoded once per mode with TurboJPEG (Huffman tables included, as
 * the FineCam sends them), or taken from a canned .mjpg file looped (`canned_file`); then its frame size is the only MJPEG
 * mode. Without TurboJPEG and without a canned file the MJPEG frames are empty JPEGs (SOI + EOI).
 *
 * Timing: `realtime` delivers a frame every 1/fps s (a frame finding no queued buffer is lost and its sequence number
 * skipped, as the driver does); otherwise a frame is ready whenever a buffer is queued (tests).
 * Fault injection: unplug / replug (ENODEV, then ENOENT at open; the finder reports nothing meanwhile), a turn
 * (set_portrait while unplugged), busy (EBUSY at S_FMT), no access (EACCES at open), a sequence gap every n frames, an
 * error-flagged buffer every n frames, EIO at DQBUF, a slow first frame after a replug (restart_delay_ms).
 */
#pragma once

#include "camera.hpp"
#include "v4l2_api.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace vhs {

enum class FakeProfile { FineCam, C270, YuyvOnly };

/* The exposure_auto_priority control as the fake camera offers it. */
enum class FakeCtrl { Absent, Present, Inactive, ReadOnly };

struct FakeOptions {
    FakeProfile profile = FakeProfile::FineCam;
    bool portrait = false;   /* FineCam switch position A: the portrait MJPEG sizes only, no YUYV */
    std::string canned_file; /* loop these MJPEG frames (its frame size becomes the MJPEG mode) */
    bool realtime = true;    /* frames at the mode's rate; false: a frame per DQBUF while a buffer is queued */
    int frames = 6;          /* synthetic frames per mode (looped) */
    int jpeg_quality = 85;
    int ctrl = -1;           /* FakeCtrl as int; -1: the profile's (FineCam absent, C270 present = 1) */
    int restart_delay_ms = 0;/* the first frame after a replug comes this much later (a camera re-enumerating) */
    std::string hc_driver = "xhci_hcd"; /* the identity's bus */
};

class FakeCamera final : public V4l2Api {
public:
    static constexpr const char *kPath = "fake:camera";

    explicit FakeCamera(FakeOptions options = FakeOptions());
    ~FakeCamera() override;

    int open(const char *path) override;
    int close(int fd) override;
    int ioctl(int fd, unsigned long req, void *arg) override;
    void *map(std::size_t len, int fd, off_t offset) override;
    int unmap(void *addr, std::size_t len) override;
    int poll_in(int fd, int timeout_ms) override;

    /* fault injection (any thread) */
    void unplug();
    void replug();
    void set_portrait(bool on);        /* the switch: takes effect at the next open (call it while unplugged) */
    void set_busy(bool on);
    void set_no_access(bool on);
    void skip_sequence_every(int n);   /* 0: never */
    void error_flag_every(int n);      /* 0: never */
    void eio_every_dqbuf(bool on);     /* every DQBUF fails with EIO */
    void set_identity(const CamIdentity &id);
    void set_ctrl_value(int v);        /* what the camera holds now (a test sets it back to 1, as another app would) */

    /* facts (any thread) */
    std::vector<CamMode> modes() const;
    int open_count() const { return opens_.load(); }
    int streamon_count() const { return streamons_.load(); }
    bool is_open() const;
    bool plugged() const;
    bool can_make_mjpeg() const { return mjpeg_ok_; }
    std::string canned_error() const { return canned_error_; }
    CamIdentity identity() const;
    std::string card() const;
    int ctrl_value() const;            /* exposure_auto_priority now */
    int ctrl_sets() const { return ctrl_sets_.load(); }
    int fps_now() const;               /* the pace frames come at (15 for a C270 left at priority 1) */

private:
    struct Buf {
        std::vector<std::uint8_t> mem;
        bool queued = false;
        bool done = false;
        std::uint32_t bytes = 0, seq = 0, flags = 0;
        std::int64_t ts_us = 0;
    };
    void build_modes();
    const std::vector<std::vector<std::uint8_t>> &frames_for(std::uint32_t fourcc, int w, int h);
    void advance(std::int64_t now);
    void produce(std::int64_t now);
    std::size_t buf_size() const;
    int pace_fps() const;
    int fail(int err) const;

    mutable std::mutex m_;
    FakeOptions opt_;
    std::vector<CamMode> modes_;
    std::vector<std::vector<std::uint8_t>> canned_;
    std::string canned_error_;
    bool mjpeg_ok_ = false;
    std::map<std::uint64_t, std::vector<std::vector<std::uint8_t>>> cache_;
    CamIdentity id_;
    FakeCtrl ctrl_ = FakeCtrl::Absent;
    int ctrl_value_ = 1;

    int fd_ = -1;
    bool streaming_ = false;
    bool unplugged_ = false, busy_ = false, no_access_ = false, eio_ = false;
    bool replugged_ = false; /* the next STREAMON is the first after a replug */
    int skip_every_ = 0, error_every_ = 0;
    std::uint32_t pf_ = 0;
    int w_ = 0, h_ = 0, fps_ = 30;
    std::vector<Buf> bufs_;
    std::deque<std::pair<int, std::int64_t>> queued_; /* buffers given to the "driver" and when, in order */
    std::deque<int> done_;   /* filled, waiting for DQBUF */
    std::uint32_t seq_ = 0;
    std::uint64_t produced_ = 0;
    std::int64_t next_due_us_ = 0;
    std::atomic<int> opens_{0}, streamons_{0}, ctrl_sets_{0};
};

/* The fake camera's test card (BGRX, w x h): frame k of n. */
void fake_test_card(int w, int h, int k, int n, std::vector<std::uint8_t> &bgrx);

/* The finder of the fake camera: FakeCamera::kPath with its identity while it is plugged in. */
std::unique_ptr<CameraFinder> make_fake_finder(FakeCamera &cam);

/* "finecam", "portrait", "c270", "yuyv" (VHS_FAKE values; "1" = finecam) -> options; false when unknown. */
bool parse_fake_profile(const std::string &s, FakeOptions &out);

} // namespace vhs
