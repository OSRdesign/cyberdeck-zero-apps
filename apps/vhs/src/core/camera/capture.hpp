/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the capture thread (plan 062 2.4, brief 3.2 and section 5), from the spike. It asks the finder for the camera,
 * opens it (QUERYCAP and the mode list; the mode from the chooser; S_FMT and S_PARM 1/30 with the granted interval read
 * back; the exposure_auto_priority rule; REQBUFS with 4 mmap buffers, 3 on boards with 512 MB or less; STREAMON), then:
 * poll() with a timeout, DQBUF, copy the frame out, QBUF at once, hand the copy to a latest-frame mailbox (an older frame
 * not yet taken is dropped there: the decoder always gets the newest one and capture never waits for it).
 *
 * States (brief 5.2 and 5.5):
 *   NoCamera  no qualifying node (and none streamed in this session); looked for again every retry_ms
 *   Streaming frames arrive
 *   Lost      the streaming camera went away (ENODEV / ENXIO / a poll error / 10 EIO in a row), 0..lost_hold_ms ago
 *   NotBack   still away after lost_hold_ms
 *   Turned    it came back within back_window_ms with the same identity and its largest mode's orientation flipped
 *             (the switch of the CM969 moved): held until the first frame, then Streaming. Back unflipped: a replug,
 *             Streaming at once (silently)
 *   Busy      EBUSY at open / S_FMT / REQBUFS / STREAMON;  NoAccess  EACCES / EPERM at open
 *   Failed    anything else, with the reason (ENOSPC at STREAMON: the USB bandwidth)
 * A mode change closes and reopens the device (uvcvideo refuses S_FMT while buffers exist). The bus is never touched
 * (C12: no unbind, no reset, no authorized toggles).
 */
#pragma once

#include "camera.hpp"
#include "controls.hpp"
#include "live/mailbox.hpp"
#include "v4l2_api.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vhs {

struct Frame {
    std::vector<std::uint8_t> data; /* the frame's bytes (data.size() >= size) */
    std::size_t size = 0;
    ModeReq mode;                   /* format, size and rate it was captured at */
    int stride = 0;                 /* YUYV bytes per row */
    std::uint32_t seq = 0;          /* the driver's sequence number */
    std::int64_t ts_us = 0;         /* the driver's timestamp (CLOCK_MONOTONIC: uvcvideo's default) */
    std::int64_t dq_us = 0;         /* when it was dequeued */
    std::uint64_t gen = 0;          /* the streaming session it belongs to */
};

enum class CamState { Searching, NoCamera, Streaming, Lost, Turned, NotBack, Busy, NoAccess, Failed };
const char *state_name(CamState s); /* "streaming", "no-camera" ... (log) */

struct CaptureStatus {
    CamState state = CamState::Searching;
    std::string device;           /* the node opened (or last opened) */
    std::string card;             /* QUERYCAP card name */
    std::string detail;           /* the reason of Failed, or a note ("MJPG 640x360 30 fps not offered") */
    CamIdentity id;               /* the camera open, or the last one */
    std::string bus_driver;       /* the host controller driver of the last camera opened ("dwc_otg" ...) */
    ModeReq mode;                 /* in use while streaming */
    std::vector<CamMode> listed;  /* the camera's list at the last open */
    ExposurePriority exposure;    /* the control rule at the last open */
    std::uint64_t gen = 0;        /* +1 at every start of streaming */
    std::uint64_t losses = 0;     /* +1 every time a streaming camera went away */
    std::uint64_t turns = 0;      /* +1 every time it came back turned */
    std::int64_t lost_ms = 0;     /* when it went away (monotonic ms), 0 while it is not away */
};

/* The photo slot's result (brief 3.2, 7 step 1): one good frame, after the warm-up, of the mode asked for. */
struct PhotoShot {
    std::uint64_t id = 0;            /* the request's */
    bool ok = false;
    std::string error;               /* when not ok: "the camera went away" ... */
    std::vector<std::uint8_t> data;  /* the frame's bytes (the camera's JPEG for MJPEG) */
    ModeReq mode;
    std::int64_t ts_us = 0;
    std::uint32_t seq = 0;
};

/* Frames dropped after STREAMON before a photo (or a recording's first frame) is taken: the exposure settles in about
 * 15 frames (0.5 s, plan 1.2; brief 5.1 step 5). */
constexpr int kWarmupFrames = 15;

/* The recorder's tap (brief 6.1): every good frame streamed at the mode asked for, after the warm-up, is handed over on
 * the capture thread; the receiver copies it and never blocks (a full ring drops the frame and counts it). */
class FrameTap {
public:
    virtual ~FrameTap() = default;
    virtual void tap_frame(const std::uint8_t *data, std::size_t n, std::int64_t ts_us, const ModeReq &mode) = 0;
};

/* The mode to stream for a camera's list (brief section 4), at every open; an invalid result means "the first MJPEG
 * mode at 30 fps, else the first YUYV one". */
using ModeChooser = std::function<ModeReq(const std::vector<CamMode> &listed, const CamIdentity &id)>;

struct CaptureConfig {
    V4l2Api *api = nullptr;
    CameraFinder *finder = nullptr;
    int buffers = 4;
    int poll_ms = 200;
    int retry_ms = 1000;        /* looking for a camera that is not there (and Busy / NoAccess / Failed) */
    int lost_retry_ms = 250;    /* looking for one that just went away */
    int lost_hold_ms = 5000;    /* Lost -> NotBack */
    int back_window_ms = 10000; /* back within this with the same identity: a replug or a turn */
    ModeReq want;               /* an explicit mode (--mode); not valid or not listed: the chooser */
    ModeChooser choose;
};

class Capture {
public:
    Capture(CaptureConfig cfg, LatestMailbox<Frame> &out);
    ~Capture();
    Capture(const Capture &) = delete;
    Capture &operator=(const Capture &) = delete;

    void start();
    void stop(); /* joins the thread: the device is closed when it returns */

    /* The thread switches to m (close, reopen) at its next turn. Any thread. */
    void request_mode(const ModeReq &m);
    /* Close and open again with the chooser's mode (settings changed). Any thread. */
    void reopen();
    /* Stream m until release_hold() (the Light photo, the self-timer's countdown in Light: brief 4.1 rule 3, 6.7); the
     * device is reopened only when m is not the mode streaming. Any thread. */
    void hold_mode(const ModeReq &m);
    void release_hold();
    bool holding() const;
    /* The photo slot: the next good frame streamed at `expect` (format and size; any mode when invalid) with at least
     * kWarmupFrames frames since its STREAMON goes to the slot; take_photo() hands the result over once. A new request
     * replaces an older one; a loss of the camera ends it with an error. Any thread. */
    void request_photo(const ModeReq &expect, std::uint64_t id);
    void cancel_photo();
    bool take_photo(PhotoShot &out);
    /* The tap: frames of `expect` (format and size) after kWarmupFrames since their STREAMON go to `tap` until
     * clear_tap(), which returns only when no call is in progress (the tap may be destroyed then). Any thread. */
    void set_tap(FrameTap *tap, const ModeReq &expect);
    void clear_tap();
    std::uint64_t tapped() const { return tapped_.load(); }
    CaptureStatus status() const;

    std::uint64_t frames() const { return frames_.load(); }   /* frames handed to the mailbox */
    std::uint64_t gaps() const { return gaps_.load(); }       /* sequence numbers the driver skipped */
    std::uint64_t errors() const { return errors_.load(); }   /* EIO, error-flagged or empty buffers */
    std::uint64_t timeouts() const { return timeouts_.load(); }
    std::uint64_t opens() const { return opens_.load(); }
    int tid() const { return tid_.load(); }

private:
    struct Mapped {
        void *p = nullptr;
        std::size_t len = 0;
    };
    void run();
    bool open_device(const CameraRef &ref);
    void close_device();
    void absent();
    void lost(int err, const char *where);
    void set_state(CamState s, const std::string &detail);
    CamState state() const;
    bool sleep_ms(int ms);

    CaptureConfig cfg_;
    LatestMailbox<Frame> &out_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex sleep_m_;
    std::condition_variable sleep_cv_;

    mutable std::mutex status_m_;
    CaptureStatus status_;
    ModeReq want_;
    bool mode_change_ = false;
    ModeReq hold_;            /* status_m_ */
    bool hold_on_ = false;    /* status_m_ */
    bool hold_switched_ = false;

    /* the photo slot */
    void photo_from(const Frame &f);
    void photo_fail(const std::string &why);
    std::atomic<bool> photo_armed_{false};
    mutable std::mutex photo_m_;
    ModeReq photo_expect_;
    std::uint64_t photo_id_ = 0;
    bool photo_ready_ = false;
    PhotoShot photo_;
    std::uint64_t since_on_ = 0; /* the thread's: good frames since STREAMON */

    /* the tap */
    void tap_from(const Frame &f);
    std::atomic<bool> tap_on_{false};
    std::mutex tap_m_;
    FrameTap *tap_ = nullptr; /* tap_m_ */
    ModeReq tap_expect_;      /* tap_m_ */
    std::atomic<std::uint64_t> tapped_{0};

    /* the thread's own */
    int fd_ = -1;
    std::vector<Mapped> maps_;
    bool streaming_ = false;
    bool had_stream_ = false;
    bool have_seq_ = false;
    std::uint32_t last_seq_ = 0;
    int eio_run_ = 0, err_polls_ = 0;
    ModeReq mode_;
    int stride_ = 0;
    std::uint64_t gen_ = 0;
    ExposurePriority exposure_;
    CameraRef cur_;
    bool cur_portrait_ = false;
    /* the camera that went away */
    std::int64_t lost_ms_ = 0;
    CameraRef lost_ref_;
    bool lost_portrait_ = false;

    std::atomic<std::uint64_t> frames_{0}, gaps_{0}, errors_{0}, timeouts_{0}, opens_{0};
    std::atomic<int> tid_{0};
};

int current_tid();
std::int64_t mono_ms();

} // namespace vhs
