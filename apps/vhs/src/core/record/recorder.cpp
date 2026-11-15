/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: one recording session (see recorder.hpp).
 */
#include "recorder.hpp"

#include "journal.hpp"
#include "live/clock.hpp"
#include "scenes/fileops.hpp"
#include "scenes/naming.hpp"
#include "scenes/photos.hpp"

#include <fcntl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace vhs {
namespace {

std::int64_t now_ms()
{
    return mono_us() / 1000;
}

} // namespace

const char *rec_stop_name(RecStop s)
{
    switch (s) {
    case RecStop::None: return "none";
    case RecStop::User: return "user";
    case RecStop::DiskFull: return "disk-full";
    case RecStop::CameraLost: return "camera-lost";
    case RecStop::Released: return "released";
    case RecStop::Error: return "error";
    }
    return "?";
}

Recorder::Recorder(RecorderConfig cfg) : cfg_(std::move(cfg)), sync_(0)
{
    const int fps = std::max(1, cfg_.video.fps);
    ring_cap_ = static_cast<std::size_t>(std::max(2, cfg_.ring_seconds) * fps);
    audio_cap_frames_ = static_cast<std::size_t>(std::max(2, cfg_.ring_seconds) * std::max(0, cfg_.audio_rate));
    audio_on_ = cfg_.audio_rate > 0;
}

Recorder::~Recorder()
{
    request_stop(RecStop::Released);
    join();
}

void Recorder::start()
{
    if (th_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        st_.running = true;
        st_.audio = cfg_.audio_rate > 0;
    }
    last_audio_ms_ = now_ms();
    start_ms_ = now_ms();
    th_ = std::thread([this] { run(); });
}

void Recorder::push_video(const std::uint8_t *data, std::size_t n, std::int64_t ts_us)
{
    if (stop_req_.load()) return;
    VFrame f;
    f.data.assign(data, data + n); /* the copy outside the lock */
    f.ts_us = ts_us;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (ring_.size() >= ring_cap_) {
            ++st_.ring_drops; /* a full ring drops the frame (brief 6.1: "drop rec") */
            return;
        }
        ring_.push_back(std::move(f));
    }
    cv_.notify_one();
}

std::size_t Recorder::write(const void *frames, std::size_t n)
{
    if (stop_req_.load() || cfg_.audio_rate <= 0 || n == 0) return n;
    const std::size_t ch = static_cast<std::size_t>(std::max(1, cfg_.audio_channels));
    AChunk c;
    c.kind = AChunk::Samples;
    const auto *s = static_cast<const std::int16_t *>(frames);
    c.samples.assign(s, s + n * ch);
    c.frames = n;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (audio_queued_frames_ + n > audio_cap_frames_) {
            /* full: the samples are lost, the timeline is kept (silence of that length) */
            st_.audio_drops += n;
            AChunk g;
            g.kind = AChunk::Gap;
            g.frames = n;
            audio_.push_back(std::move(g));
            return n;
        }
        audio_queued_frames_ += n;
        audio_.push_back(std::move(c));
    }
    last_audio_ms_ = now_ms();
    cv_.notify_one();
    return n;
}

void Recorder::audio_gap(std::uint64_t frames)
{
    AChunk g;
    g.kind = AChunk::Gap;
    g.frames = frames;
    std::lock_guard<std::mutex> lock(m_);
    st_.gap_frames += frames;
    audio_.push_back(std::move(g));
}

void Recorder::audio_clock(std::int64_t mono_ns, std::uint64_t position)
{
    AChunk c;
    c.kind = AChunk::Clock;
    c.ns = mono_ns;
    c.frames = position;
    std::lock_guard<std::mutex> lock(m_);
    audio_.push_back(std::move(c));
}

void Recorder::audio_end()
{
    AChunk c;
    c.kind = AChunk::End;
    {
        std::lock_guard<std::mutex> lock(m_);
        audio_.push_back(std::move(c));
    }
    cv_.notify_one();
}

void Recorder::request_stop(RecStop why)
{
    {
        std::lock_guard<std::mutex> lock(m_);
        if (reason_ == RecStop::None) reason_ = why;
    }
    stop_req_ = true;
    cv_.notify_all();
}

void Recorder::join()
{
    if (th_.joinable()) th_.join();
}

RecorderStatus Recorder::status() const
{
    std::lock_guard<std::mutex> lock(m_);
    RecorderStatus s = st_;
    s.stop = reason_;
    return s;
}

void Recorder::fail(const std::string &why)
{
    std::fprintf(stderr, "[vhs] recorder: %s\n", why.c_str());
    {
        std::lock_guard<std::mutex> lock(m_);
        st_.error = why;
        if (reason_ == RecStop::None) reason_ = RecStop::Error;
    }
    stop_req_ = true;
}

void Recorder::sync_file()
{
    if (fd_ >= 0) cfg_.ops->fdatasync(fd_);
}

bool Recorder::open_part(int part, std::int64_t offset_ms)
{
    const std::string name = stem_ + (part >= 2 ? "_p" + std::to_string(part) : std::string()) + ".mkv";
    part_path_ = cfg_.folder + "/" + name;
    mux_ = std::make_unique<MkvMuxer>();
    std::string err;
    if (!mux_->open(*cfg_.av, part_path_, cfg_.video, enc_.get(), &err)) {
        mux_.reset();
        fail(err);
        return false;
    }
    fd_ = cfg_.ops->open(part_path_, O_WRONLY, 0);
    part_ = part;
    part_offset_ms_ = offset_ms;
    part_bytes_ = 0;
    files_.push_back(name);
    Journal j;
    j.file = files_.front();
    j.parts = files_;
    j.started = cfg_.started_unix;
    journal_write(*cfg_.ops, cfg_.folder, stem_, j);
    {
        std::lock_guard<std::mutex> lock(m_);
        st_.parts = part;
        st_.files = files_;
    }
    std::fprintf(stderr, "[vhs] recorder: part %d %s (%s %dx%d%s)\n", part, part_path_.c_str(), video_codec_name(cfg_.video.codec),
                 cfg_.video.w, cfg_.video.h, enc_ ? ", AAC" : ", no sound");
    return true;
}

bool Recorder::close_part()
{
    if (!mux_) return true;
    std::string err;
    const int r = mux_->close(&err);
    sync_file();
    if (fd_ >= 0) cfg_.ops->close(fd_);
    fd_ = -1;
    cfg_.ops->fsync_dir(cfg_.folder);
    mux_.reset();
    if (r < 0) fail(err);
    return r >= 0;
}

void Recorder::feed_audio(const std::int16_t *s, std::size_t frames)
{
    if (!enc_) {
        audio_pos_ += frames;
        return;
    }
    const std::size_t ch = static_cast<std::size_t>(enc_->channels());
    if (!sync_.started()) {
        /* before t0: kept (3 s at most) until the first frame fixes the scene's sample 0 */
        pre_.emplace_back(static_cast<std::int64_t>(audio_pos_), std::vector<std::int16_t>(s, s + frames * ch));
        audio_pos_ += frames;
        std::size_t kept = 0;
        for (const auto &e : pre_) kept += e.second.size() / ch;
        while (!pre_.empty() && kept > static_cast<std::size_t>(enc_->rate()) * 3) {
            kept -= pre_.front().second.size() / ch;
            pre_.pop_front();
        }
        return;
    }
    std::int64_t idx = sync_.audio_index(audio_pos_);
    audio_pos_ += frames;
    if (idx + static_cast<std::int64_t>(frames) <= 0) return; /* audio before t0 is dropped */
    std::size_t skip = idx < 0 ? static_cast<std::size_t>(-idx) : 0;
    if (idx < 0) idx = 0;
    /* a hole (samples trimmed before t0): silence keeps the timeline */
    if (idx > enc_->samples_in()) {
        std::vector<std::int16_t> zeros(static_cast<std::size_t>(idx - enc_->samples_in()) * ch, 0);
        enc_->push(zeros.data(), zeros.size() / ch);
    }
    if (enc_->push(s + skip * ch, frames - skip) < 0) fail("AAC encoding failed");
    EncodedAudio p;
    while (enc_->pop(p)) apkts_.push_back(std::move(p));
}

void Recorder::write_audio_packets(std::int64_t before_ms, bool all)
{
    if (!enc_ || !mux_) {
        apkts_.clear();
        return;
    }
    const int rate = enc_->rate();
    while (!apkts_.empty()) {
        EncodedAudio &p = apkts_.front();
        const std::int64_t pts = rescale(p.pts, 1000, rate);
        if (!all && pts >= before_ms) break;
        const std::int64_t dur = std::max<std::int64_t>(1, rescale(p.duration, 1000, rate));
        if (mux_->write_audio(p.data.data(), p.data.size(), pts - part_offset_ms_, dur) >= 0) {
            part_bytes_ += p.data.size();
            bytes_done_ += p.data.size();
        }
        apkts_.pop_front();
    }
}

void Recorder::run()
{
    FileOps &ops = *cfg_.ops;
    /* the name: the shared counter (brief 8) */
    stem_ = cfg_.stem;
    if (stem_.empty()) {
        int number = 0;
        const int r = ensure_folder(ops, cfg_.folder) == 0 ? next_scene_number(ops, cfg_.folder, number) : -1;
        if (r != 0) {
            fail("the folder " + cfg_.folder + " cannot be written");
            std::lock_guard<std::mutex> lock(m_);
            st_.running = false;
            st_.finished = true;
            return;
        }
        SceneName n;
        n.prefix = cfg_.prefix;
        n.number = number;
        n.when = cfg_.when.empty() ? "nodate" : cfg_.when;
        n.ext = "mkv";
        stem_ = format_scene_name(n);
        stem_ = stem_.substr(0, stem_.size() - 4);
    } else {
        ensure_folder(ops, cfg_.folder);
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        st_.stem = stem_;
    }
    if (cfg_.audio_rate > 0) {
        enc_ = std::make_unique<AacEncoder>();
        std::string err;
        if (!enc_->open(*cfg_.av, cfg_.audio_rate, std::max(1, cfg_.audio_channels), &err)) {
            std::fprintf(stderr, "[vhs] recorder: no sound: %s\n", err.c_str());
            enc_.reset();
        }
    }
    sync_ = AvSync(enc_ ? cfg_.audio_rate : 0);
    audio_on_ = enc_ != nullptr;
    {
        std::lock_guard<std::mutex> lock(m_);
        st_.audio = enc_ != nullptr;
    }
    const bool opened = open_part(1, 0);
    {
        std::lock_guard<std::mutex> lock(m_);
        st_.open_ms = static_cast<int>(now_ms() - start_ms_);
    }
    DiskGuard guard(cfg_.min_free_bytes);
    const int fps = std::max(1, cfg_.video.fps);
    const std::int64_t dur_ms = 1000 / fps;
    std::int64_t last_flush = now_ms(), last_guard = now_ms(), first_pending = 0;
    std::deque<VFrame> pending;
    /* the capture rate (brief 5.3: under 27 fps for 5 s while recording is logged) */
    std::int64_t sec_start = -1;
    int sec_frames = 0, slow_secs = 0;
    bool slow_logged = false;
    if (!opened) stop_req_ = true;
    for (;;) {
        std::deque<VFrame> vin;
        std::deque<AChunk> ain;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait_for(lock, std::chrono::milliseconds(10),
                         [this] { return stop_req_.load() || !ring_.empty() || !audio_.empty(); });
            vin.swap(ring_);
            ain.swap(audio_);
            audio_queued_frames_ = 0;
        }
        /* the clock pairs and the end first: the scene can start (t0) before this batch's samples are placed, so a
         * large batch (a slow first open of the files) never trims audio after t0 from the pre-t0 buffer */
        std::uint64_t batch_frames = 0;
        for (const AChunk &c : ain) {
            if (c.kind == AChunk::Clock) sync_.pair(c.ns, c.frames);
            else if (c.kind == AChunk::End) audio_on_ = false;
            else batch_frames += c.frames;
        }
        for (VFrame &f : vin) {
            if (pending.empty()) first_pending = now_ms();
            pending.push_back(std::move(f));
        }
        const bool stopping = stop_req_.load();
        if (opened && !sync_.started() && !pending.empty()) {
            const bool wait_clock = enc_ && !sync_.ready() && now_ms() - first_pending < cfg_.audio_wait_ms && !stopping;
            if (!wait_clock) {
                if (enc_ && !sync_.ready()) {
                    /* no clock pair yet: now and the samples received so far, until the first real pair */
                    sync_.seed(mono_us() * 1000, audio_pos_ + batch_frames);
                    std::fprintf(stderr, "[vhs] recorder: no audio clock after %d ms, provisional start\n", cfg_.audio_wait_ms);
                }
                sync_.start(pending.front().ts_us);
                const std::uint64_t cur = audio_pos_;
                std::deque<std::pair<std::int64_t, std::vector<std::int16_t>>> pre;
                pre.swap(pre_);
                for (auto &e : pre) {
                    audio_pos_ = static_cast<std::uint64_t>(e.first);
                    feed_audio(e.second.data(), e.second.size() / static_cast<std::size_t>(std::max(1, enc_ ? enc_->channels() : 1)));
                }
                audio_pos_ = cur;
            }
        }
        for (AChunk &c : ain) { /* the samples, in order (before t0: the pre-t0 buffer) */
            if (c.kind == AChunk::Gap) {
                if (!enc_) {
                    audio_pos_ += c.frames;
                    continue;
                }
                std::vector<std::int16_t> zeros(static_cast<std::size_t>(c.frames) * static_cast<std::size_t>(enc_->channels()), 0);
                feed_audio(zeros.data(), static_cast<std::size_t>(c.frames));
            } else if (c.kind == AChunk::Samples) {
                feed_audio(c.samples.data(), static_cast<std::size_t>(c.frames));
            }
        }
        while (opened && mux_ && sync_.started() && !pending.empty()) {
            VFrame &f = pending.front();
            std::int64_t pts = sync_.video_ms(f.ts_us);
            if (pts <= last_pts_) pts = last_pts_ + 1; /* never backwards (a refitted clock line) */
            if (enc_ && audio_on_.load() && !stopping) {
                const std::int64_t enc_ms = rescale(enc_->samples_in(), 1000, enc_->rate());
                const bool stalled = now_ms() - last_audio_ms_.load() > cfg_.audio_stall_ms;
                if (enc_ms < pts + dur_ms && !stalled) break; /* the audio has not reached this frame yet */
            }
            write_audio_packets(pts, false); /* the audio before this frame */
            if (split_due(part_bytes_, f.data.size(), cfg_.split_bytes, cfg_.split_margin)) {
                close_part();
                if (!open_part(part_ + 1, pts)) break;
            }
            if (mux_->write_video(f.data.data(), f.data.size(), pts - part_offset_ms_, dur_ms, true) < 0) {
                fail("a video frame could not be written");
                break;
            }
            part_bytes_ += f.data.size();
            bytes_done_ += f.data.size();
            last_pts_ = pts;
            /* the rate */
            if (sec_start < 0) sec_start = pts;
            ++sec_frames;
            if (pts - sec_start >= 1000) {
                if (sec_frames < 27) ++slow_secs;
                else slow_secs = 0;
                if (slow_secs >= 5 && !slow_logged) {
                    std::fprintf(stderr, "[vhs] recorder: the camera delivers %d fps (under 27 for 5 s)\n", sec_frames);
                    slow_logged = true;
                }
                if (slow_secs == 0) slow_logged = false;
                sec_start = pts;
                sec_frames = 0;
            }
            {
                std::lock_guard<std::mutex> lock(m_);
                st_.started = true;
                st_.frames += 1;
                st_.written_ms = pts + dur_ms;
                st_.bytes = bytes_done_;
            }
            pending.pop_front();
        }
        const std::int64_t t = now_ms();
        if (mux_ && t - last_flush >= cfg_.flush_ms) {
            mux_->flush();
            sync_file();
            last_flush = t;
        }
        if (t - last_guard >= cfg_.guard_ms) {
            last_guard = t;
            std::uint64_t free_b = 0;
            if (cfg_.free_space) free_b = cfg_.free_space();
            else if (ops.free_space(cfg_.folder, free_b) != 0) free_b = UINT64_MAX;
            const DiskGuard::Verdict v = guard.step(t, free_b, bytes_done_);
            {
                std::lock_guard<std::mutex> lock(m_);
                st_.free_bytes = free_b;
                st_.seconds_left = guard.seconds_left();
            }
            if (v != DiskGuard::Verdict::Ok && !stopping) {
                std::fprintf(stderr, "[vhs] recorder: disk guard: %s, stopping\n",
                             v == DiskGuard::Verdict::Floor ? "under the floor" : "under 2 minutes left");
                request_stop(RecStop::DiskFull);
            }
        }
        if (stopping) {
            bool more;
            {
                std::lock_guard<std::mutex> lock(m_);
                more = !ring_.empty() || !audio_.empty();
            }
            if (!more && (pending.empty() || !mux_ || !sync_.started())) break;
        }
    }
    /* finish: the encoder's tail, the trailer, fdatasync, the journal closed */
    if (enc_ && mux_) {
        enc_->finish();
        EncodedAudio p;
        while (enc_->pop(p)) apkts_.push_back(std::move(p));
        write_audio_packets(0, true);
    }
    close_part();
    std::uint64_t frames;
    {
        std::lock_guard<std::mutex> lock(m_);
        frames = st_.frames;
        st_.bytes = bytes_done_;
    }
    if (frames == 0) {
        /* nothing was recorded: no scene */
        for (const std::string &f : files_) ops.unlink(cfg_.folder + "/" + f);
        ops.unlink(journal_path(cfg_.folder, stem_));
        files_.clear();
    } else {
        Journal j;
        j.state = "closed";
        j.file = files_.front();
        j.parts = files_;
        j.started = cfg_.started_unix;
        journal_write(ops, cfg_.folder, stem_, j);
    }
    std::fprintf(stderr, "[vhs] recorder: finished %s: %llu frames, %d part(s), %llu bytes, %llu dropped (ring), stop %s\n",
                 stem_.c_str(), static_cast<unsigned long long>(frames), part_, static_cast<unsigned long long>(bytes_done_),
                 static_cast<unsigned long long>(status().ring_drops), rec_stop_name(status().stop));
    std::lock_guard<std::mutex> lock(m_);
    st_.files = files_;
    st_.running = false;
    st_.finished = true;
}

} // namespace vhs
