/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the movie thread (brief 3.2, 10): owns the VideoDecoder, decodes ahead into the presenter (2-3 pictures), applies
 * the drop rule (a frame more than one frame period behind the clock is dropped before decoding; MJPEG: any frame;
 * others: the decoder skips non-reference frames), seeks on request (the presenter is emptied, the decoder lands on the
 * exact frame for MJPEG) and reports its status. The clock comes from the controller (the engine's heard position, else
 * the monotonic PlayClock), read through a thread-safe function. LVGL-free.
 */
#pragma once

#include "movie/presenter.hpp"
#include "movie/video_decoder.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace vhs {

struct MovieStatus {
    bool open = false;
    bool ended = false;          /* the last frame was decoded */
    std::string error;           /* "This video cannot be played" ... */
    std::int64_t duration_ms = -1;
    std::string codec;
    int w = 0, h = 0;
    double frame_ms = 33.3;
    std::uint64_t decoded = 0, dropped = 0, shown_dropped = 0;
    double decode_ms = 0;        /* the mean time to decode and fit one frame */
    bool drops_notice = false;   /* 2 s of drops (once per movie) */
};

class Movie {
public:
    /* clock_ms: the playback clock (any thread); playing: whether it runs (late frames matter only then). */
    Movie(const AvLibs &av, std::function<std::int64_t()> clock_ms, std::function<bool()> playing);
    ~Movie();
    Movie(const Movie &) = delete;
    Movie &operator=(const Movie &) = delete;

    bool open(const std::string &path, int box_w, int box_h, PixFmt fmt, std::string *why);
    void seek(std::int64_t ms);
    void close();
    Presenter &presenter() { return pres_; }
    MovieStatus status() const;
    /* True once when the drop notice fires. */
    bool take_drops_notice() { return notice_.exchange(false); }

private:
    void run();
    const AvLibs &av_;
    std::function<std::int64_t()> clock_;
    std::function<bool()> playing_;
    VideoDecoder dec_;
    Presenter pres_;
    std::thread th_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::int64_t seek_to_ = -1; /* m_ */
    MovieStatus st_;            /* m_ */
    std::atomic<bool> notice_{false};
};

} // namespace vhs
