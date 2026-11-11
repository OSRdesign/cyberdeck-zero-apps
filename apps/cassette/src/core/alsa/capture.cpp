/* SPDX-License-Identifier: MIT */
#include "capture.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>

namespace cassette {

const char *capture_error_name(CaptureError e) {
    switch (e) {
    case CaptureError::None: return "none";
    case CaptureError::Unplugged: return "unplugged";
    case CaptureError::Overrun: return "overrun";
    case CaptureError::Busy: return "busy";
    case CaptureError::Other: return "other";
    }
    return "?";
}

CaptureError classify_error(int err) {
    switch (err) {
    case -ENODEV:
    case -EBADFD: /* "File descriptor in bad state": exactly what arecord printed at the unplug in P0 */
    case -ENOTTY:
        return CaptureError::Unplugged;
    case -EPIPE: return CaptureError::Overrun;
    case -EBUSY: return CaptureError::Busy;
    default: return err < 0 ? CaptureError::Other : CaptureError::None;
    }
}

namespace {
double steady_now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
void nap(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
} // namespace

Capture::Capture(OpenResult &&opened, FrameSink &sink, CaptureOptions opt)
    : pcm_(std::move(opened.pcm)),
      sink_(sink),
      opt_(std::move(opt)),
      fmt_(opened.format),
      conv_(opened.conv),
      hw_(opened.hw),
      via_server_(opened.via_server) {
    if (!opt_.now) opt_.now = steady_now;
    if (opt_.period_frames == 0) opt_.period_frames = 1024;
}

Capture::~Capture() {
    stop();
    if (pcm_) pcm_->close();
}

bool Capture::start() {
    if (th_.joinable()) {
        if (running_.load()) return false;
        th_.join(); /* ended by itself earlier */
    }
    if (!pcm_) return false; /* the card was lost: open again through open_capture */
    if (int r = pcm_->prepare(); r < 0) {
        note(classify_error(r), r, "Cannot prepare the card: " + alsa_error_text(r));
        if (classify_error(r) == CaptureError::Unplugged) fatal(CaptureError::Unplugged, r, "The microphone was disconnected");
        return false;
    }
    stop_ = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        st_.stop_reason = CaptureError::None;
    }
    running_ = true;
    th_ = std::thread([this] { run(); });
    return true;
}

void Capture::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    running_ = false;
}

CaptureStats Capture::stats() const {
    CaptureStats s;
    {
        std::lock_guard<std::mutex> g(mu_);
        s = st_;
    }
    s.frames_captured = captured_.load();
    s.frames_dropped = dropped_.load();
    s.frames_lost_xrun = lost_.load();
    s.overruns = overruns_.load();
    s.running = running_.load();
    return s;
}

void Capture::note(CaptureError e, int err, const std::string &msg) {
    std::lock_guard<std::mutex> g(mu_);
    st_.last_error = e;
    st_.last_errno = err;
    st_.message = msg;
}

/* Ends the thread's work because of an error: close the PCM, record why. Runs on the capture thread (or in start()). */
void Capture::fatal(CaptureError why, int err, const std::string &msg) {
    note(why, err, msg);
    {
        std::lock_guard<std::mutex> g(mu_);
        st_.stop_reason = why;
    }
    if (pcm_) {
        pcm_->close();
        pcm_.reset();
    }
}

void Capture::run() {
    const uint32_t period = opt_.period_frames;
    const size_t in_bytes_per_frame = static_cast<size_t>(hw_.channels) * format_bytes(hw_.format);
    const size_t out_bytes_per_frame = static_cast<size_t>(fmt_.channels) * (fmt_.bits / 8);
    std::vector<uint8_t> raw(period * in_bytes_per_frame), cvt;
    if (conv_ != Conversion::None) cvt.resize(period * out_bytes_per_frame);

    double last = opt_.now(); /* when the last read returned */
    int other_in_a_row = 0;
    while (!stop_.load()) {
        long n = pcm_->readi(raw.data(), period);
        if (n > 0) {
            other_in_a_row = 0;
            const void *out = raw.data();
            if (conv_ == Conversion::S32_to_S24_3) {
                convert_s32_to_s24_3(raw.data(), cvt.data(), static_cast<size_t>(n) * hw_.channels);
                out = cvt.data();
            }
            size_t got = static_cast<size_t>(n);
            captured_ += got;
            size_t took = sink_.write(out, got);
            if (took > got) took = got;
            if (took < got) dropped_ += got - took;
            if (opt_.on_period) opt_.on_period(out, got, fmt_);
            last = opt_.now();
            continue;
        }
        if (n == 0) { /* no data and no error: do not spin */
            nap(2);
            continue;
        }
        int e = static_cast<int>(n);
        if (e == -EINTR) continue;
        if (e == -EAGAIN) {
            nap(2);
            continue;
        }
        CaptureError c = classify_error(e);
        if (c == CaptureError::Unplugged) {
            fatal(c, e, "The microphone was disconnected (" + alsa_error_text(e) + ")");
            break;
        }
        if (c == CaptureError::Busy) {
            fatal(c, e, "The card is in use by another program");
            break;
        }
        if (c == CaptureError::Overrun) {
            /* The card kept producing while we were not reading, and recover() discards what was in its buffer: the loss is
             * about the time since the last good read at the stream rate. An estimate, not a measurement. */
            double gap = opt_.now() - last;
            uint64_t lost = gap > 0 ? static_cast<uint64_t>(gap * fmt_.rate + 0.5) : 0;
            ++overruns_;
            lost_ += lost;
            note(CaptureError::Overrun, e, "Overrun: about " + std::to_string(lost) + " frames lost");
            int r = pcm_->recover(e);
            if (r < 0) {
                CaptureError rc = classify_error(r);
                fatal(rc == CaptureError::Overrun ? CaptureError::Other : rc, r,
                      "Cannot recover from an overrun: " + alsa_error_text(r));
                break;
            }
            last = opt_.now();
            continue;
        }
        /* Other (for example -ESTRPIPE after a suspend): one recover attempt, a few in a row at most */
        note(CaptureError::Other, e, "Read error: " + alsa_error_text(e));
        int r = pcm_->recover(e);
        if (r < 0 || ++other_in_a_row > 20) {
            fatal(classify_error(r) == CaptureError::Unplugged ? CaptureError::Unplugged : CaptureError::Other,
                  r < 0 ? r : e, "Read error: " + alsa_error_text(r < 0 ? r : e));
            break;
        }
        nap(5);
        last = opt_.now();
    }
    running_ = false;
}

} // namespace cassette
