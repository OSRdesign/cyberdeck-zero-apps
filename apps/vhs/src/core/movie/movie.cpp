/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the movie thread (see movie.hpp).
 */
#include "movie.hpp"

#include "live/clock.hpp"

#include <chrono>
#include <cstdio>

namespace vhs {

Movie::Movie(const AvLibs &av, std::function<std::int64_t()> clock_ms, std::function<bool()> playing)
    : av_(av), clock_(std::move(clock_ms)), playing_(std::move(playing))
{
}

Movie::~Movie()
{
    close();
}

bool Movie::open(const std::string &path, int box_w, int box_h, PixFmt fmt, std::string *why)
{
    close();
    std::string err;
    if (!dec_.open(av_, path, box_w, box_h, fmt, &err)) {
        std::lock_guard<std::mutex> lock(m_);
        st_ = MovieStatus();
        st_.error = err;
        if (why) *why = err;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        st_ = MovieStatus();
        st_.open = true;
        st_.duration_ms = dec_.duration_ms();
        st_.codec = dec_.codec_name();
        st_.w = dec_.width();
        st_.h = dec_.height();
        st_.frame_ms = dec_.frame_ms();
        stop_ = false;
        seek_to_ = -1;
        seek_gen_ = 0;
    }
    ready_gen_ = -1;
    pres_.clear();
    pres_.set_late_ms(static_cast<std::int64_t>(2 * dec_.frame_ms()));
    th_ = std::thread([this] { run(); });
    return true;
}

std::uint64_t Movie::seek(std::int64_t ms)
{
    std::uint64_t n;
    {
        std::lock_guard<std::mutex> lock(m_);
        seek_to_ = ms;
        n = ++seek_gen_;
        st_.ended = false;
    }
    pres_.clear();
    cv_.notify_all();
    return n;
}

void Movie::close()
{
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    dec_.close();
    pres_.clear();
    std::lock_guard<std::mutex> lock(m_);
    st_.open = false;
}

MovieStatus Movie::status() const
{
    std::lock_guard<std::mutex> lock(m_);
    MovieStatus s = st_;
    s.shown_dropped = pres_.dropped();
    return s;
}

void Movie::run()
{
    const double frame_ms = dec_.frame_ms();
    std::int64_t last_pts = -static_cast<std::int64_t>(frame_ms);
    DropWindow drops;
    double decode_total = 0;
    std::uint64_t decode_n = 0;
    bool ended = false;
    std::uint64_t gen = 0; /* the seek whose pictures this loop decodes */
    for (;;) {
        std::int64_t seek = -1;
        {
            std::unique_lock<std::mutex> lock(m_);
            if (ended && seek_to_ < 0) cv_.wait_for(lock, std::chrono::milliseconds(50));
            if (stop_) break;
            seek = seek_to_;
            seek_to_ = -1;
            if (seek >= 0) gen = seek_gen_;
        }
        if (seek >= 0) {
            dec_.seek(seek);
            pres_.clear();
            last_pts = seek - static_cast<std::int64_t>(frame_ms);
            ended = false;
        }
        if (ended) continue;
        if (pres_.full()) {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait_for(lock, std::chrono::milliseconds(4));
            continue;
        }
        /* the drop rule (brief 10): the next frame is due at last_pts + one period; more than two periods behind the
         * clock while playing (G6: a little late is still shown): dropped before decoding */
        const std::int64_t clock = clock_();
        const bool late = playing_() && clock - (last_pts + static_cast<std::int64_t>(frame_ms)) > static_cast<std::int64_t>(2 * frame_ms);
        auto pic = std::make_shared<Picture>();
        std::int64_t pts = 0;
        std::string why;
        const std::int64_t t0 = mono_us();
        const int r = dec_.next(*pic, pts, late, &why);
        if (r == VideoDecoder::Picture_) {
            decode_total += static_cast<double>(mono_us() - t0) / 1000.0;
            ++decode_n;
            last_pts = pts;
            bool stale;
            {
                std::lock_guard<std::mutex> lock(m_);
                stale = seek_to_ >= 0; /* a seek came while it decoded: the picture is from before it */
            }
            if (!stale) {
                pres_.push(TimedPicture{pic, pts});
                ready_gen_ = static_cast<std::int64_t>(gen);
            }
        } else if (r == VideoDecoder::Skipped) {
            if (late) {
                last_pts += static_cast<std::int64_t>(frame_ms);
                if (drops.drop(mono_us() / 1000)) notice_ = true;
            }
        } else if (r == VideoDecoder::End) {
            ended = true;
        } else {
            std::lock_guard<std::mutex> lock(m_);
            st_.error = why.empty() ? "decode error" : why;
            ended = true;
        }
        std::lock_guard<std::mutex> lock(m_);
        st_.ended = ended;
        st_.decoded = dec_.decoded();
        st_.dropped = dec_.dropped();
        st_.decode_ms = decode_n ? decode_total / static_cast<double>(decode_n) : 0;
        st_.drops_notice = drops.fired();
    }
}

} // namespace vhs
