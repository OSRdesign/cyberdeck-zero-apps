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
 * 0.1.2: a held mode (hold_mode: the Light photo) the camera refuses (S_FMT / REQBUFS / STREAMON error such as ENOSPC, the
 * shared USB bandwidth) falls back to the next smaller listed MJPEG size at once, on the open device, and the photo is
 * taken at that size (PhotoShot::mode says which); the refused sizes are forgotten at release_hold(). Exposure
 * (set_exposure: Manual with a value, or Auto) is written by this thread, at its next turn and again after every start of
 * streaming (an in-place switch or a reopen), so a photo at another size has the preview's shutter; a camera that was
 * left on Manual goes back to Aperture Priority at close. 0.1.2 draft 3: the request carries the PREVIEW's shutter and gain
 * (the shutter capped near 1/15 s so the preview stays smooth, the gain making up the brightness) and the PHOTO's true
 * shutter: request_photo() writes the photo's (and the default gain), waits for it to take effect (kExposureSettleFrames
 * frames, at most kPhotoExposureCapMs) and writes the preview's again once the photo is taken. A camera with continuous autofocus makes a photo wait until its
 * frames' sizes settle (FocusSettle), at most kFocusCapMs.
 * A mode change (request_mode, hold_mode, release_hold) switches on the open device: STREAMOFF, the buffers freed
 * (REQBUFS 0: uvcvideo refuses S_FMT while buffers exist), S_FMT, S_PARM, new buffers, STREAMON; the controls stay as
 * the open set them. Any refusal falls back to the full close and reopen, which reopen() (the settings changed) always
 * does. Each start's phases are timed (stream_timing(): the rec-start breakdown). The bus is never touched (C12: no
 * unbind, no reset, no authorized toggles).
 */
#pragma once

#include "camera.hpp"
#include "controls.hpp"
#include "exposure.hpp"
#include "live/mailbox.hpp"
#include "modes/select.hpp"
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
    std::string key;              /* its stable id (identity.hpp stable_key()), "" when it has none */
    bool fallback = false;        /* 0.1.2: opened instead of the remembered camera `wanted` (not plugged in; 2+ present) */
    std::string wanted;
    std::string bus_driver;       /* the host controller driver of the last camera opened ("dwc_otg" ...) */
    ModeReq mode;                 /* in use while streaming */
    std::vector<CamMode> listed;  /* the camera's list at the last open */
    ExposurePriority exposure;    /* the control rule at the last open */
    ExposureCaps exp_caps;        /* 0.1.2: manual exposure and autofocus of the camera open (or the last one) */
    int exp_value = -1;           /* 0.1.2: exposure_time_absolute written (manual) now, -1: automatic */
    std::uint64_t exp_apply_n = 0; /* +1 at every write of the exposure (a request, or again after a start of streaming) */
    std::uint64_t exp_apply_gen = 0; /* the streaming session the last write belongs to */
    std::uint32_t exp_apply_seq = 0; /* the driver's sequence number of the last frame taken when it was written (0: none yet) */
    std::uint64_t exp_failed = 0; /* +1 at every write that failed */
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
    int focus_wait_ms = 0;           /* 0.1.2: how long the focus wait held it (autofocus cameras) */
    bool focus_capped = false;       /* the wait ended at kFocusCapMs, not settled */
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

/* The phases of the last stream start in ms (-1: not measured): the rec-start breakdown (M4). */
struct StreamTiming {
    std::uint64_t gen = 0;   /* the streaming session they belong to */
    bool in_place = false;   /* a switch on the open device; false: a close and an open */
    int close_ms = -1;       /* STREAMOFF + unmap (+ close) of the previous stream */
    int open_ms = -1;        /* open + QUERYCAP + the mode list (0 in place) */
    int set_ms = -1;         /* S_FMT + S_PARM */
    int controls_ms = -1;    /* exposure_auto_priority (an open only) */
    int bufs_ms = -1;        /* REQBUFS + mmap + QBUF + STREAMON */
    int first_ms = -1;       /* STREAMON to the first frame */
    int warm_ms = -1;        /* the first frame to the end of the warm-up (kWarmupFrames more) */
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
    /* Ask the chooser again and switch in place when its answer differs (the Photo / Video mode changed). Any thread. */
    void rechoose();
    /* Stream m until release_hold() (the Light photo, the self-timer's countdown in Light: brief 4.1 rule 3, 6.7); the
     * device is reopened only when m is not the mode streaming. Any thread. */
    void hold_mode(const ModeReq &m);
    void release_hold();
    bool holding() const;
    /* The photo slot: the next good frame streamed at `expect` (format and size; any mode when invalid) with at least
     * kWarmupFrames frames since its STREAMON goes to the slot; take_photo() hands the result over once. A new request
     * replaces an older one; a loss of the camera ends it with an error. Any thread. */
    void request_photo(const ModeReq &expect, std::uint64_t id, bool settle_focus = true);
    void cancel_photo();
    bool take_photo(PhotoShot &out);
    /* 0.1.2: the exposure: manual with `value` (exposure_time_absolute, 100 us) or automatic. Written by the capture thread
     * (never blocks the caller); kept across reopens of the same camera, dropped (Automatic) when another one opens. A
     * camera that cannot do it ignores it. Any thread. */
    void set_exposure(bool manual, int value, int gain = -1, int photo_value = -1);
    /* The tap: frames of `expect` (format and size) after kWarmupFrames since their STREAMON go to `tap` until
     * clear_tap(), which returns only when no call is in progress (the tap may be destroyed then). Any thread. */
    void set_tap(FrameTap *tap, const ModeReq &expect);
    void clear_tap();
    std::uint64_t tapped() const { return tapped_.load(); }
    StreamTiming stream_timing() const;
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
    ModeReq pick_mode(const std::vector<CamMode> &listed, const CamIdentity &id, std::string *note, bool *held);
    bool start_stream_held(const std::vector<CamMode> &listed, const CamIdentity &id, ModeReq mode, bool held, bool controls,
                           StreamTiming t, std::string &why, CamState &bad);
    void release_buffers();
    void apply_exposure();
    bool start_stream(const ModeReq &mode, bool controls, StreamTiming t, std::string &why, CamState &bad);
    bool switch_in_place();
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
    bool reopen_full_ = false;  /* status_m_ */
    StreamTiming timing_;       /* status_m_ */
    std::int64_t t_on_us_ = 0, t_first_us_ = 0; /* the thread's */
    int pending_close_ms_ = -1;
    ModeReq hold_;            /* status_m_ */
    bool hold_on_ = false;    /* status_m_ */
    bool hold_switched_ = false;
    std::vector<Size> hold_refused_; /* status_m_: the held sizes the camera refused */
    struct ExpReq {
        bool manual = false;
        int value = 0;       /* the preview's exposure_time_absolute */
        int gain = -1;       /* the preview's gain (-1: not touched) */
        int photo = -1;      /* the photo's exposure_time_absolute (-1: the preview's) */
    };
    ExpReq exp_req_;                  /* status_m_ */
    std::atomic<bool> exp_dirty_{false};
    std::atomic<bool> photo_true_{false}; /* a photo is wanted with its own (longer) shutter: written, then the preview's again */

    /* the photo slot */
    void photo_from(const Frame &f);
    void photo_fail(const std::string &why);
    std::atomic<bool> photo_armed_{false};
    mutable std::mutex photo_m_;
    ModeReq photo_expect_;
    std::uint64_t photo_id_ = 0;
    bool photo_settle_ = false;       /* photo_m_: wait for the focus */
    std::int64_t photo_t0_ms_ = 0;    /* photo_m_: the first frame that would do (0: none yet) */
    std::int64_t photo_req_ms_ = 0;   /* photo_m_: the request */
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
    ExposureCaps exp_caps_;           /* the thread's */
    bool exp_manual_ = false;         /* the thread's: Manual is written on the camera now */
    bool exp_photo_applied_ = false;  /* the thread's: the photo's shutter is the one written */
    std::uint64_t since_exp_ = 0;     /* the thread's: good frames since the exposure was written */
    CameraRef exp_ref_;               /* the camera the request belongs to */
    FocusSettle focus_;               /* the thread's: sizes of the latest frames (autofocus cameras) */
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
