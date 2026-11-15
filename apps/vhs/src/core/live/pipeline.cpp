/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the live-view pipeline (see pipeline.hpp).
 */
#include "pipeline.hpp"

#include "clock.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace vhs {
namespace {

CaptureConfig capture_config(const PipelineConfig &c)
{
    CaptureConfig cc;
    cc.api = c.api;
    cc.finder = c.finder;
    cc.choose = c.choose;
    cc.want = c.mode;
    cc.poll_ms = c.poll_ms;
    cc.retry_ms = c.retry_ms;
    cc.lost_retry_ms = c.lost_retry_ms;
    cc.buffers = c.buffers;
    return cc;
}

double ms_between(std::int64_t a, std::int64_t b)
{
    return static_cast<double>(b - a) / 1000.0;
}

} // namespace

Pipeline::Pipeline(PipelineConfig cfg) : cfg_(std::move(cfg)), capture_(capture_config(cfg_), frames_)
{
    denom_ = cfg_.denom;
    box_w_ = cfg_.box_w;
    box_h_ = cfg_.box_h;
    if (cfg_.stats_ms < 100) cfg_.stats_ms = 100;
    if (cfg_.log_every < 1) cfg_.log_every = 1;
}

Pipeline::~Pipeline()
{
    stop();
}

void Pipeline::start()
{
    if (started_.exchange(true)) return;
    stop_ = false;
    capture_.start();
    decode_thread_ = std::thread([this] { decode_loop(); });
    sample_thread_ = std::thread([this] { sample_loop(); });
}

void Pipeline::stop()
{
    if (!started_.exchange(false)) return;
    stop_ = true;
    frames_.wake();
    {
        std::lock_guard<std::mutex> lock(sample_m_);
    }
    sample_cv_.notify_all();
    capture_.stop();
    if (decode_thread_.joinable()) decode_thread_.join();
    if (sample_thread_.joinable()) sample_thread_.join();
}

void Pipeline::set_mode(const ModeReq &m)
{
    capture_.request_mode(m);
}

void Pipeline::reopen()
{
    capture_.reopen();
}

void Pipeline::set_scale(int denom)
{
    if (denom != 0 && denom != 1 && denom != 2 && denom != 4 && denom != 8) denom = 0;
    denom_ = denom;
}

void Pipeline::set_box(int w, int h)
{
    box_w_ = std::max(1, w);
    box_h_ = std::max(1, h);
}

void Pipeline::note_shown(const Picture &p)
{
    (void)p;
    shown_.fetch_add(1);
}

void Pipeline::note_refresh(double ui_ms, double render_ms, double lat_ms)
{
    if (ui_ms >= 0) ui_ms_.add(ui_ms);
    if (render_ms >= 0) render_ms_.add(render_ms);
    if (lat_ms >= 0) lat_ms_.add(lat_ms);
}

std::string Pipeline::last_decode_error() const
{
    std::lock_guard<std::mutex> lock(m_);
    return last_decode_error_;
}

bool Pipeline::has_interval() const
{
    std::lock_guard<std::mutex> lock(m_);
    return has_last_;
}

Interval Pipeline::last_interval() const
{
    std::lock_guard<std::mutex> lock(m_);
    return last_;
}

Totals Pipeline::totals() const
{
    std::lock_guard<std::mutex> lock(m_);
    return totals_;
}

void Pipeline::set_on_interval(std::function<void(const Interval &)> fn)
{
    std::lock_guard<std::mutex> lock(m_);
    on_interval_ = std::move(fn);
}

void Pipeline::decode_loop()
{
    decode_tid_ = current_tid();
    std::unique_ptr<JpegDecoder> dec;
    if (TurboJpeg::get().available()) dec = std::make_unique<JpegDecoder>();
    std::vector<std::uint8_t> scratch;
    while (!stop_) {
        if (!frames_.wait_take(100)) continue;
        if (stop_) break;
        decode_one(frames_.front(), dec.get(), scratch);
    }
}

void Pipeline::decode_one(const Frame &f, JpegDecoder *dec, std::vector<std::uint8_t> &scratch)
{
    const int box_w = box_w_.load(), box_h = box_h_.load();
    const int want = denom_.load();
    const PixFmt fmt = cfg_.fmt;
    const int bpp = pix_bytes(fmt);
    Picture &p = pictures_.back();
    const std::int64_t t0 = mono_us();
    std::int64_t t1 = t0, t2 = t0;
    int denom = 1, dw = 0, dh = 0, fw = 0, fh = 0;
    if (f.mode.fourcc == kMJPG) {
        auto failed = [this](const std::string &why) {
            dec_fail_.fetch_add(1);
            std::lock_guard<std::mutex> lock(m_);
            if (last_decode_error_ != why) std::fprintf(stderr, "[vhs] decode: %s\n", why.c_str());
            last_decode_error_ = why;
        };
        if (!dec || !dec->ok()) {
            failed(TurboJpeg::get().error().empty() ? "no TurboJPEG decoder" : TurboJpeg::get().error());
            return;
        }
        JpegInfo info;
        if (!dec->header(f.data.data(), f.size, info)) {
            failed("not a JPEG frame (" + std::to_string(f.size) + " bytes): " + dec->last_error());
            return;
        }
        fit_size(info.w, info.h, box_w, box_h, fw, fh);
        denom = want ? want : fit_denom(info.w, info.h, fw, fh);
        dw = scaled_up(info.w, denom);
        dh = scaled_up(info.h, denom);
        int r;
        if (fmt == PixFmt::XRGB8888 && dw == fw && dh == fh) {
            /* TurboJPEG writes LVGL's XRGB8888 directly: the picture is the decoded buffer */
            p.px.resize(static_cast<std::size_t>(dw) * dh * 4);
            r = dec->decode_bgrx(f.data.data(), f.size, denom, p.px.data(), dw * 4, cfg_.fastdct);
            t1 = t2 = mono_us();
        } else {
            scratch.resize(static_cast<std::size_t>(dw) * dh * 4);
            r = dec->decode_bgrx(f.data.data(), f.size, denom, scratch.data(), dw * 4, cfg_.fastdct);
            t1 = mono_us();
            if (r >= 0) {
                p.px.resize(static_cast<std::size_t>(fw) * fh * bpp);
                resample_bgrx(scratch.data(), dw, dh, dw * 4, fmt, p.px.data(), fw, fh, fw * bpp, ax_, ay_);
            }
            t2 = mono_us();
        }
        if (r < 0) {
            failed(dec->last_error());
            return;
        }
        if (r > 0) {
            std::lock_guard<std::mutex> lock(m_);
            last_decode_error_ = "warning: " + dec->last_error();
        }
        dec_ms_.add(ms_between(t0, t1));
        conv_ms_.add(ms_between(t1, t2));
    } else if (f.mode.fourcc == kYUYV) {
        const int stride = f.stride > 0 ? f.stride : f.mode.w * 2;
        if (f.size < static_cast<std::size_t>(stride) * f.mode.h) {
            dec_fail_.fetch_add(1);
            return;
        }
        fit_size(f.mode.w, f.mode.h, box_w, box_h, fw, fh);
        dw = f.mode.w;
        dh = f.mode.h;
        p.px.resize(static_cast<std::size_t>(fw) * fh * bpp);
        yuyv_fit(fmt, f.data.data(), f.mode.w, f.mode.h, stride, p.px.data(), fw, fh, fw * bpp);
        t1 = t0; /* no decode: the conversion is the whole cost */
        t2 = mono_us();
        dec_ms_.add(0);
        conv_ms_.add(ms_between(t1, t2));
    } else {
        dec_fail_.fetch_add(1);
        return;
    }
    p.fmt = fmt;
    p.w = fw;
    p.h = fh;
    p.stride = fw * bpp;
    p.box_w = box_w;
    p.box_h = box_h;
    p.mode = f.mode;
    p.denom = denom;
    p.dec_w = dw;
    p.dec_h = dh;
    p.seq = f.seq;
    p.ts_us = f.ts_us;
    p.dq_us = f.dq_us;
    p.done_us = t2;
    p.gen = f.gen;
    p.n = decoded_.fetch_add(1) + 1;
    last_denom_ = denom;
    last_dec_w_ = dw;
    last_dec_h_ = dh;
    last_fit_w_ = fw;
    last_fit_h_ = fh;
    pictures_.publish();
}

void Pipeline::sample_loop()
{
    SystemProbe sys;
    std::fprintf(stderr, "[vhs] measuring: temperature %s, firmware flags %s, under-voltage alarm %s, %ld ticks/s\n",
                 sys.thermal_path().empty() ? "(none)" : sys.thermal_path().c_str(),
                 sys.throttle_source().empty() ? "(none: not a Raspberry Pi)" : sys.throttle_source().c_str(),
                 sys.undervolt_path().empty() ? "(none)" : sys.undervolt_path().c_str(), sys.hz());
    std::uint64_t cap0 = capture_.frames(), dec0 = decoded_.load(), shown0 = shown_.load();
    std::uint64_t dropd0 = frames_.drops(), dropu0 = pictures_.drops(), gaps0 = capture_.gaps();
    std::uint64_t err0 = capture_.errors(), fail0 = dec_fail_.load();
    std::uint64_t pt0 = 0, ct0 = 0, dt0 = 0, ut0 = 0;
    bool have_pt = sys.process_ticks(pt0);
    bool have_ct = false, have_dt = false, have_ut = sys.thread_ticks(cfg_.ui_tid, ut0);
    std::int64_t t0 = mono_us();
    while (!stop_) {
        {
            std::unique_lock<std::mutex> lock(sample_m_);
            sample_cv_.wait_for(lock, std::chrono::milliseconds(cfg_.stats_ms), [this] { return stop_.load(); });
        }
        if (stop_) break;
        const std::int64_t now = mono_us();
        const double secs = static_cast<double>(now - t0) / 1e6;
        if (secs <= 0) continue;
        Interval iv;
        iv.secs = secs;
        const CaptureStatus cam = capture_.status();
        iv.state = state_name(cam.state);
        if (cam.mode.valid()) iv.mode = mode_text(cam.mode);
        const int want = denom_.load();
        char sc[96];
        std::snprintf(sc, sizeof sc, "%s%s %dx%d>%dx%d", want ? "forced " : "", scale_text(last_denom_.load()).c_str(),
                      last_dec_w_.load(), last_dec_h_.load(), last_fit_w_.load(), last_fit_h_.load());
        iv.scale = sc;
        iv.fmt = pix_name(cfg_.fmt);
        iv.box = std::to_string(box_w_.load()) + "x" + std::to_string(box_h_.load());
        const std::uint64_t cap = capture_.frames(), dec = decoded_.load(), shown = shown_.load();
        const std::uint64_t dropd = frames_.drops(), dropu = pictures_.drops(), gaps = capture_.gaps();
        const std::uint64_t err = capture_.errors(), fail = dec_fail_.load();
        iv.cap_fps = static_cast<double>(cap - cap0) / secs;
        iv.dec_fps = static_cast<double>(dec - dec0) / secs;
        iv.shown_fps = static_cast<double>(shown - shown0) / secs;
        iv.drop_dec = dropd - dropd0;
        iv.drop_ui = dropu - dropu0;
        iv.gaps = gaps - gaps0;
        iv.errors = err - err0;
        iv.dec_fail = fail - fail0;
        cap0 = cap;
        dec0 = dec;
        shown0 = shown;
        dropd0 = dropd;
        dropu0 = dropu;
        gaps0 = gaps;
        err0 = err;
        fail0 = fail;
        std::vector<double> dms = dec_ms_.take(), cms = conv_ms_.take(), ums = ui_ms_.take(), lms = lat_ms_.take();
        std::vector<double> rms = render_ms_.take();
        iv.index = ++interval_count_;
        iv.dec_ms = summarize(dms);
        iv.conv_ms = summarize(cms);
        iv.ui_ms = summarize(ums);
        iv.render_ms = summarize(rms);
        iv.lat_ms = summarize(lms);
        std::uint64_t pt = 0, ct = 0, dt = 0, ut = 0;
        if (sys.process_ticks(pt)) {
            if (have_pt) iv.cores = cores_between(pt0, pt, t0, now, sys.hz());
            pt0 = pt;
            have_pt = true;
        }
        if (sys.thread_ticks(capture_.tid(), ct)) {
            if (have_ct) iv.cap_cores = cores_between(ct0, ct, t0, now, sys.hz());
            ct0 = ct;
            have_ct = true;
        }
        if (sys.thread_ticks(decode_tid_.load(), dt)) {
            if (have_dt) iv.dec_cores = cores_between(dt0, dt, t0, now, sys.hz());
            dt0 = dt;
            have_dt = true;
        }
        if (sys.thread_ticks(cfg_.ui_tid, ut)) {
            if (have_ut) iv.ui_cores = cores_between(ut0, ut, t0, now, sys.hz());
            ut0 = ut;
            have_ut = true;
        }
        iv.rss_mb = sys.rss_mb();
        iv.have_temp = sys.temp_c(iv.temp_c);
        iv.have_thr = sys.throttled(iv.thr, now);
        iv.have_uv = sys.undervolt_alarm(iv.uv);
        t0 = now;
        if (cfg_.log_stats && iv.index % static_cast<std::uint64_t>(cfg_.log_every) == 0)
            std::fprintf(stderr, "%s\n", format_log(iv).c_str());
        std::function<void(const Interval &)> fn;
        {
            std::lock_guard<std::mutex> lock(m_);
            last_ = iv;
            has_last_ = true;
            fn = on_interval_;
            if (cfg_.keep_totals && interval_index_++ > 0) {
                Totals &t = totals_;
                auto add = [](std::vector<double> &to, const std::vector<double> &from) {
                    if (to.size() < 500000) to.insert(to.end(), from.begin(), from.end());
                };
                ++t.intervals;
                t.secs += secs;
                t.cap += static_cast<std::uint64_t>(iv.cap_fps * secs + 0.5);
                t.dec += static_cast<std::uint64_t>(iv.dec_fps * secs + 0.5);
                t.shown += static_cast<std::uint64_t>(iv.shown_fps * secs + 0.5);
                t.drop_dec += iv.drop_dec;
                t.drop_ui += iv.drop_ui;
                t.gaps += iv.gaps;
                t.errors += iv.errors;
                t.dec_fail += iv.dec_fail;
                add(t.dec_ms, dms);
                add(t.conv_ms, cms);
                add(t.ui_ms, ums);
                add(t.render_ms, rms);
                add(t.lat_ms, lms);
                if (iv.cores >= 0) {
                    t.have_cores = true;
                    t.core_secs += iv.cores * secs;
                    t.cap_core_secs += std::max(0.0, iv.cap_cores) * secs;
                    t.dec_core_secs += std::max(0.0, iv.dec_cores) * secs;
                    t.ui_core_secs += std::max(0.0, iv.ui_cores) * secs;
                }
                if (iv.rss_mb >= 0) {
                    t.rss_last = iv.rss_mb;
                    t.rss_max = std::max(t.rss_max, iv.rss_mb);
                }
                if (iv.have_temp) {
                    t.temp_max = t.have_temp ? std::max(t.temp_max, iv.temp_c) : iv.temp_c;
                    t.temp_last = iv.temp_c;
                    t.have_temp = true;
                }
                if (iv.have_thr) {
                    t.have_thr = true;
                    t.thr_or |= iv.thr;
                    t.thr_last = iv.thr;
                }
                t.mode = iv.mode;
                t.scale = iv.scale;
                t.state = iv.state;
            }
        }
        if (fn) fn(iv);
    }
}

} // namespace vhs
