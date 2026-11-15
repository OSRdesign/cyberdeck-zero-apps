/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the live-view pipeline, LVGL-free (plan 062 2.4, brief 3.2; from the spike).
 *
 *   capture thread   DQBUF, copy, QBUF  ->  frame mailbox (one slot: an untaken frame is dropped, "drop dec")
 *   decode thread    takes the newest frame; MJPEG: TurboJPEG at the fit scale into BGRX, resampled to the fit size in
 *                    the display format (the whole frame, never cropped: convert.hpp); YUYV: sampled to the fit size
 *                    ->  picture mailbox (one slot: a picture the UI did not take in time is dropped, "drop ui")
 *   UI thread        (the app's LVGL timer at the display refresh, or the bench's taker) takes the newest picture and
 *                    reports note_shown() / note_refresh()
 *   sampler thread   once a second: the interval's counters, stage times, CPU per thread, RSS, temperature, the
 *                    firmware flags and the rpi_volt alarm; the stats line ("[vhs] stats ...") every log_every intervals
 *                    (10 s; 1 s with VHS_STATS=1)
 *
 * No thread ever waits for a slower one: the two mailboxes drop the older item instead (counted).
 */
#pragma once

#include "camera/capture.hpp"
#include "convert.hpp"
#include "jpeg/turbojpeg.hpp"
#include "mailbox.hpp"
#include "stats.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vhs {

struct Picture {
    std::vector<std::uint8_t> px; /* the pixels (fmt), w x h, stride bytes per row: the whole frame at the fit size */
    PixFmt fmt = PixFmt::RGB565;
    int w = 0, h = 0, stride = 0;
    int box_w = 0, box_h = 0;     /* the picture box it was made for (it goes centred in it) */
    ModeReq mode;                 /* the camera mode of its frame */
    int denom = 1;                /* the decode scale used (1/denom) */
    int dec_w = 0, dec_h = 0;     /* the decoded (or source) size before the resample */
    std::uint32_t seq = 0;
    std::int64_t ts_us = 0, dq_us = 0, done_us = 0;
    std::uint64_t n = 0;          /* the decoder's picture count */
    std::uint64_t gen = 0;        /* the capture session of its frame */
};

/* One JPEG (a photo file) decoded to fit a box (the photo review, brief 9.5): the fit rule and the resample of the live
 * view (convert.hpp). Its own TurboJPEG handle (the scenes worker's call). False with err when it cannot be decoded. */
bool decode_jpeg_fit(const std::uint8_t *jpeg, std::size_t n, int box_w, int box_h, PixFmt fmt, Picture &out,
                     std::string *err);

struct PipelineConfig {
    V4l2Api *api = nullptr;
    CameraFinder *finder = nullptr;
    ModeChooser choose;          /* the mode at each open (brief section 4) */
    ModeReq mode;                /* an explicit mode (--mode); not valid: the chooser's */
    int denom = 0;               /* 0 = the fit rule, else 1, 2, 4, 8 forced (bench) */
    PixFmt fmt = PixFmt::RGB565;
    int box_w = 352, box_h = 198;
    bool fastdct = true;
    bool log_stats = true;       /* the stats line to stderr */
    int log_every = 10;          /* every n intervals */
    int stats_ms = 1000;
    int ui_tid = 0;              /* the thread that takes the pictures (its CPU is the "ui" column) */
    int poll_ms = 200, retry_ms = 1000, lost_retry_ms = 250;
    int buffers = 4;             /* REQBUFS (3 when ram_mb <= 512) */
    bool keep_totals = false;    /* the bench: every sample after the first interval kept for its summary */
};

/* Everything after the first interval (the warm-up), for the bench's summary. */
struct Totals {
    int intervals = 0;
    double secs = 0;
    std::uint64_t cap = 0, dec = 0, shown = 0, drop_dec = 0, drop_ui = 0, gaps = 0, errors = 0, dec_fail = 0;
    std::vector<double> dec_ms, conv_ms, ui_ms, render_ms, lat_ms;
    double core_secs = 0, cap_core_secs = 0, dec_core_secs = 0, ui_core_secs = 0; /* sums of cores x seconds */
    bool have_cores = false;
    double rss_max = -1, rss_last = -1;
    bool have_temp = false;
    double temp_max = 0, temp_last = 0;
    bool have_thr = false;
    std::uint32_t thr_or = 0, thr_last = 0;
    std::string mode, scale, state;
};

class Pipeline {
public:
    explicit Pipeline(PipelineConfig cfg);
    ~Pipeline();
    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;

    void start();
    void stop(); /* joins every thread; the camera is closed when it returns */
    bool running() const { return started_.load(); }

    /* controls, any thread */
    void set_mode(const ModeReq &m);
    void reopen();
    void set_scale(int denom);       /* 0 = fit */
    int scale() const { return denom_.load(); }
    void set_box(int w, int h);
    PixFmt fmt() const { return cfg_.fmt; }
    bool fastdct() const { return cfg_.fastdct; }

    /* the UI side */
    LatestMailbox<Picture> &pictures() { return pictures_; }
    void note_shown(const Picture &p);              /* the UI took p and set it on the screen */
    /* the redraw that showed it ended: ui_ms = handoff -> end of that redraw, render_ms = that redraw alone (start ->
     * end), lat_ms = the driver's timestamp -> end of that redraw; a negative value is not recorded */
    void note_refresh(double ui_ms, double render_ms, double lat_ms);

    /* the capture thread's photo slot and mode hold (capture.hpp) */
    Capture &capture() { return capture_; }

    /* facts, any thread */
    CaptureStatus camera() const { return capture_.status(); }
    bool decoder_ok() const { return TurboJpeg::get().available(); }
    std::string decoder_error() const { return TurboJpeg::get().error(); }
    std::string last_decode_error() const;
    bool has_interval() const;
    Interval last_interval() const;
    std::uint64_t captured() const { return capture_.frames(); }
    std::uint64_t decoded() const { return decoded_.load(); }
    std::uint64_t shown() const { return shown_.load(); }
    std::uint64_t decode_failures() const { return dec_fail_.load(); }
    int capture_tid() const { return capture_.tid(); }
    int decode_tid() const { return decode_tid_.load(); }

    Totals totals() const;

    /* every interval, on the sampler thread */
    void set_on_interval(std::function<void(const Interval &)> fn);

private:
    void decode_loop();
    void sample_loop();
    void decode_one(const Frame &f, JpegDecoder *dec, std::vector<std::uint8_t> &scratch);

    PipelineConfig cfg_;
    LatestMailbox<Frame> frames_;
    LatestMailbox<Picture> pictures_;
    Capture capture_;
    std::thread decode_thread_, sample_thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> started_{false};
    std::atomic<int> denom_{0};
    std::atomic<int> box_w_{0}, box_h_{0};
    std::atomic<std::uint64_t> decoded_{0}, shown_{0}, dec_fail_{0};
    std::atomic<int> decode_tid_{0};
    std::atomic<int> last_denom_{1}, last_dec_w_{0}, last_dec_h_{0}, last_fit_w_{0}, last_fit_h_{0};
    SampleSet dec_ms_, conv_ms_, ui_ms_, render_ms_, lat_ms_;
    Axis ax_, ay_; /* the decode thread's resample tables */
    std::unique_ptr<JpegDecoder> decoder_; /* made by start(), then the decode thread's */

    mutable std::mutex m_;
    std::string last_decode_error_;
    Interval last_;
    bool has_last_ = false;
    std::function<void(const Interval &)> on_interval_;
    Totals totals_;
    int interval_index_ = 0;
    std::uint64_t interval_count_ = 0; /* the sampler thread's */
    std::mutex sample_m_;
    std::condition_variable sample_cv_;
};

} // namespace vhs
