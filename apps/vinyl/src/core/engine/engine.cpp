/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl engine (see engine.hpp).
 */
#include "engine.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vinyl {

const char *play_state_name(PlayState s) {
    switch (s) {
    case PlayState::Playing: return "playing";
    case PlayState::Paused: return "paused";
    default: return "idle";
    }
}

float volume_gain(int volume) {
    if (volume <= 0) return 0.0f;
    if (volume >= 100) return 1.0f;
    const float v = volume / 100.0f;
    return v * v;
}

void convert_pcm(const void *in, unsigned in_bits, unsigned in_ch, void *out, SampleFormat out_fmt, unsigned out_ch,
                 size_t frames, float gain) {
    if (in_ch == 0 || out_ch == 0) return;
    const int16_t *i16 = static_cast<const int16_t *>(in);
    const int32_t *i32 = static_cast<const int32_t *>(in);
    unsigned char *o = static_cast<unsigned char *>(out);
    const bool scale = gain < 0.9999f || gain > 1.0001f;
    const int64_t gq = static_cast<int64_t>(std::lround(std::max(0.0f, gain) * 65536.0f));
    const unsigned ob = format_bytes(out_fmt);
    auto sample = [&](size_t f, unsigned c) -> int32_t {
        const size_t k = f * in_ch + c;
        return in_bits == 16 ? static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(i16[k])) << 16) : i32[k];
    };
    auto put = [&](int32_t v) {
        if (scale) {
            const int64_t x = (static_cast<int64_t>(v) * gq) >> 16;
            v = static_cast<int32_t>(std::clamp<int64_t>(x, INT32_MIN, INT32_MAX));
        }
        switch (out_fmt) {
        case SampleFormat::S16_LE: {
            const int16_t s = static_cast<int16_t>(v >> 16);
            std::memcpy(o, &s, 2);
            break;
        }
        case SampleFormat::S24_3LE: {
            const uint32_t u = static_cast<uint32_t>(v) >> 8;
            o[0] = static_cast<unsigned char>(u & 0xFF);
            o[1] = static_cast<unsigned char>((u >> 8) & 0xFF);
            o[2] = static_cast<unsigned char>((u >> 16) & 0xFF);
            break;
        }
        default: std::memcpy(o, &v, 4); break;
        }
        o += ob;
    };
    for (size_t f = 0; f < frames; ++f) {
        const int32_t l = sample(f, 0);
        const int32_t r = in_ch > 1 ? sample(f, 1) : l;
        if (out_ch == 1) {
            put(static_cast<int32_t>((static_cast<int64_t>(l) + r) / 2));
            continue;
        }
        put(l);
        put(r);
        for (unsigned c = 2; c < out_ch; ++c) put(0);
    }
}

namespace {
bool same_format(const SinkFormat &a, const SinkFormat &b) {
    return a.rate == b.rate && a.channels == b.channels && a.format == b.format;
}
}  // namespace

Engine::Engine(DecodeLibs libs, SinkProvider &sinks) : libs_(std::move(libs)), sinks_(sinks) {
    th_ = std::thread([this] { run(); });
}

Engine::~Engine() { shutdown(); }

void Engine::shutdown() {
    {
        std::lock_guard<std::mutex> g(mu_);
        quit_.store(true);
        interrupt_.store(true);
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void Engine::push(Cmd c, bool interrupt) {
    {
        std::lock_guard<std::mutex> g(mu_);
        cmds_.push_back(std::move(c));
        if (interrupt) interrupt_.store(true);
    }
    cv_.notify_all();
}

void Engine::post(EngineEvent ev) {
    std::lock_guard<std::mutex> g(mu_);
    if (events_.size() > 64) events_.pop_front();
    events_.push_back(std::move(ev));
}

void Engine::load(int pos, const std::string &path, uint64_t start_ms, bool play) {
    Cmd c{Cmd::Kind::Load};
    c.pos = pos;
    c.path = path;
    c.ms = start_ms;
    c.play = play;
    push(std::move(c), true);
}
void Engine::play() { push(Cmd{Cmd::Kind::Play}, false); }
void Engine::pause() { push(Cmd{Cmd::Kind::Pause}, true); }
void Engine::toggle() { push(Cmd{Cmd::Kind::Toggle}, true); }
void Engine::stop() { push(Cmd{Cmd::Kind::Stop}, true); }
void Engine::seek_ms(uint64_t ms) {
    Cmd c{Cmd::Kind::Seek};
    c.ms = ms;
    push(std::move(c), true);
}
void Engine::set_next(int pos, const std::string &path) {
    Cmd c{Cmd::Kind::Next};
    c.pos = pos;
    c.path = path;
    push(std::move(c), false);
}
void Engine::set_output(const OutputTarget &t) {
    Cmd c{Cmd::Kind::Output};
    c.target = t;
    push(std::move(c), true);
}
void Engine::set_gain(float g) { gain_.store(g); }

EngineSnapshot Engine::snapshot() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_;
}

bool Engine::poll_event(EngineEvent &ev) {
    std::lock_guard<std::mutex> g(mu_);
    if (events_.empty()) return false;
    ev = std::move(events_.front());
    events_.pop_front();
    return true;
}

/* ------------------------------------------------------------------------------------------------ the thread */

void Engine::run() {
    while (!quit_.load()) {
        Cmd c{Cmd::Kind::Play};
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (cmds_.empty() && state_ != PlayState::Playing)
                cv_.wait_for(lk, std::chrono::milliseconds(200), [&] { return quit_.load() || !cmds_.empty(); });
            if (!cmds_.empty()) {
                c = std::move(cmds_.front());
                cmds_.pop_front();
                have = true;
                if (cmds_.empty()) interrupt_.store(false);
            }
        }
        if (quit_.load()) break;
        if (have) {
            handle(c);
            update_snapshot();
            continue;
        }
        if (state_ == PlayState::Playing) step();
        else update_snapshot();
    }
    close_output(false);
    dec_.reset();
}

bool Engine::open_decoder(int pos, const std::string &path, std::unique_ptr<TrackDecoder> &dec, TrackInfo &info, int &err) {
    (void)pos;
    err = 0;
    dec = open_track(path, libs_, info, &err);
    if (!dec) return false;
    if (info.out.rate == 0 || info.out.channels == 0 || (info.out.bits != 16 && info.out.bits != 32)) {
        dec.reset();
        err = kDecodeFailed;
        return false;
    }
    return true;
}

SinkFormat Engine::request_for(const TrackInfo &info) const {
    SinkFormat f;
    f.rate = info.out.rate;
    f.channels = std::min<unsigned>(info.out.channels, 2);
    f.format = info.out.bits > 16 ? SampleFormat::S32_LE : SampleFormat::S16_LE;
    return f;
}

void Engine::handle(const Cmd &c) {
    switch (c.kind) {
    case Cmd::Kind::Load: {
        std::unique_ptr<TrackDecoder> d;
        TrackInfo inf;
        int err = 0;
        if (!open_decoder(c.pos, c.path, d, inf, err)) {
            close_output(false);
            dec_.reset();
            info_ = TrackInfo();
            dec_pos_ = c.pos;
            dec_path_ = c.path;
            next_pos_ = -1;
            next_path_.clear();
            segs_.clear();
            segs_.push_back(Seg{written_, c.pos, c.path, 0, TrackInfo()});
            state_ = PlayState::Idle;
            heard_pos_ = c.pos;
            ++gen_;
            EngineEvent ev;
            ev.kind = EngineEvent::Kind::OpenFailed;
            ev.pos = c.pos;
            ev.err = err;
            ev.path = c.path;
            ev.text = track_error_text(err, c.path);
            post(ev);
            return;
        }
        uint64_t start = c.ms * inf.out.rate / 1000;
        if (inf.total_frames && start >= inf.total_frames) start = 0;
        if (start && d->seek(start) != 0) {
            start = 0;
            d->seek(0);
        }
        if (open_) {
            if (!same_format(request_for(inf), req_)) close_output(false);
            else out_.sink->drop();
        }
        dec_ = std::move(d);
        info_ = inf;
        dec_pos_ = c.pos;
        dec_path_ = c.path;
        next_pos_ = -1;
        next_path_.clear();
        segs_.clear();
        segs_.push_back(Seg{written_, c.pos, c.path, start, inf});
        state_ = c.play ? PlayState::Playing : PlayState::Paused;
        if (!c.play) close_output(false);
        heard_pos_ = c.pos;
        ++gen_;
        EngineEvent s;
        s.kind = EngineEvent::Kind::Started;
        s.pos = c.pos;
        s.path = c.path;
        post(s);
        s.kind = EngineEvent::Kind::Advanced;
        post(s);
        return;
    }
    case Cmd::Kind::Play:
        if (dec_ && state_ != PlayState::Playing) state_ = PlayState::Playing;
        return;
    case Cmd::Kind::Pause:
        if (state_ == PlayState::Playing) hold_at_heard(PlayState::Paused);
        return;
    case Cmd::Kind::Toggle:
        if (state_ == PlayState::Playing) hold_at_heard(PlayState::Paused);
        else if (dec_) state_ = PlayState::Playing;
        return;
    case Cmd::Kind::Stop:
        if (!dec_) return;
        if (state_ == PlayState::Playing) hold_at_heard(PlayState::Paused);
        if (dec_) {
            dec_->seek(0);
            segs_.clear();
            segs_.push_back(Seg{written_, dec_pos_, dec_path_, 0, info_});
            state_ = PlayState::Paused;
        }
        return;
    case Cmd::Kind::Seek: {
        if (!dec_) return;
        const PlayState was = state_;
        if (was == PlayState::Playing) hold_at_heard(PlayState::Playing);
        if (!dec_) return;
        uint64_t f = c.ms * info_.out.rate / 1000;
        if (info_.total_frames && f >= info_.total_frames) f = info_.total_frames > 1 ? info_.total_frames - 1 : 0;
        if (dec_->seek(f) != 0) {
            dec_->seek(0);
            f = 0;
        }
        f = dec_->position();
        segs_.clear();
        segs_.push_back(Seg{written_, dec_pos_, dec_path_, f, info_});
        return;
    }
    case Cmd::Kind::Next:
        next_pos_ = c.pos;
        next_path_ = c.path;
        return;
    case Cmd::Kind::Output: {
        if (c.target == target_ && open_) return;
        const PlayState was = state_;
        if (open_) {
            if (was == PlayState::Playing) hold_at_heard(PlayState::Playing);
            close_output(false);
        }
        target_ = c.target;
        state_ = was;
        return;
    }
    }
}

uint64_t Engine::heard_frame(Seg *seg) {
    if (segs_.empty()) {
        if (seg) *seg = Seg();
        return 0;
    }
    long d = open_ ? out_.sink->delay() : 0;
    if (d < 0) d = 0;
    const uint64_t heard = written_ > static_cast<uint64_t>(d) ? written_ - static_cast<uint64_t>(d) : 0;
    size_t i = 0;
    for (size_t k = 0; k < segs_.size(); ++k)
        if (segs_[k].start_written <= heard) i = k;
    while (i > 0) {
        segs_.pop_front();
        --i;
    }
    const Seg &s = segs_.front();
    uint64_t f = s.start_frame + (heard > s.start_written ? heard - s.start_written : 0);
    if (s.info.total_frames && f > s.info.total_frames) f = s.info.total_frames;
    if (seg) *seg = s;
    return f;
}

void Engine::reposition(const Seg &cur, uint64_t frame, PlayState next) {
    if (cur.pos != dec_pos_ || cur.path != dec_path_) {
        /* the decoder already moved on to the next track (gapless look-ahead): go back to the one heard, and name the
         * one it had moved to as the next again */
        std::unique_ptr<TrackDecoder> d;
        TrackInfo inf;
        int err = 0;
        const int was_pos = dec_pos_;
        const std::string was_path = dec_path_;
        if (open_decoder(cur.pos, cur.path, d, inf, err)) {
            dec_ = std::move(d);
            info_ = inf;
            dec_pos_ = cur.pos;
            dec_path_ = cur.path;
            next_pos_ = was_pos;
            next_path_ = was_path;
        }
    }
    if (open_) {
        out_.sink->drop();
        if (next != PlayState::Playing) close_output(false);
    }
    if (dec_) {
        if (dec_->seek(frame) != 0) {
            dec_->seek(0);
            frame = 0;
        }
    }
    segs_.clear();
    segs_.push_back(Seg{written_, dec_pos_, dec_path_, frame, info_});
    state_ = dec_ ? next : PlayState::Idle;
}

void Engine::hold_at_heard(PlayState next) {
    Seg cur;
    const uint64_t f = heard_frame(&cur);
    reposition(cur, f, next);
}

bool Engine::ensure_open() {
    OpenedSink o;
    std::string why;
    int r = sinks_.open(target_, o, why);
    const SinkFormat want = request_for(info_);
    if (r == 0) {
        AudioFormat af;
        af.rate = want.rate;
        af.channels = static_cast<uint16_t>(want.channels);
        af.bits = static_cast<uint16_t>(want.format == SampleFormat::S32_LE ? 32 : 16);
        r = o.sink->open(af, granted_);
        if (r != 0 && why.empty()) why = r == kNoAlsa ? "no sound library (libasound)" : std::strerror(r < 0 && r > -4096 ? -r : EIO);
    } else if (why.empty()) {
        why = r == kNoAlsa ? "no sound library (libasound)" : "the output cannot be opened";
    }
    if (r != 0) {
        std::fprintf(stderr, "[vinyl] output %s cannot be opened: %s\n", target_.key().c_str(), why.c_str());
        state_ = PlayState::Paused;
        EngineEvent ev;
        ev.kind = EngineEvent::Kind::OutputFailed;
        ev.pos = dec_pos_;
        ev.err = r;
        ev.text = why;
        post(ev);
        return false;
    }
    if (granted_.channels == 0) granted_.channels = want.channels;
    out_ = std::move(o);
    open_ = true;
    req_ = want;
    last_label_ = out_.label;
    last_name_ = out_.name;
    last_pulse_ = out_.pulse;
    std::fprintf(stderr, "[vinyl] output open: %s (%s), %u Hz, %u ch, %s\n", out_.label.c_str(), out_.name.c_str(),
                 granted_.rate, granted_.channels, format_name(granted_.format));
    return true;
}

void Engine::close_output(bool drain) {
    if (!open_) return;
    if (drain) out_.sink->drain();
    out_.sink->close();
    out_.sink.reset();
    open_ = false;
}

bool Engine::write_all(const unsigned char *data, size_t frames) {
    const size_t fb = granted_.channels * format_bytes(granted_.format);
    size_t off = 0;
    while (off < frames) {
        if (interrupt_.load() || quit_.load()) return true; /* a command waits: it repositions, the rest is dropped */
        const long r = out_.sink->write(data + off * fb, frames - off);
        if (r > 0) {
            off += static_cast<size_t>(r);
            written_ += static_cast<uint64_t>(r);
            continue;
        }
        if (r == 0) continue;
        if (r == -EPIPE || r == -ESTRPIPE || r == -EINTR) {
            ++underruns_;
            if (out_.sink->recover(static_cast<int>(r)) == 0) continue;
        }
        output_failed();
        return false;
    }
    return true;
}

void Engine::output_failed() {
    Seg cur;
    const uint64_t f = heard_frame(&cur);
    OpenedSink lost;
    lost.name = out_.name;
    lost.device = out_.device;
    lost.label = out_.label;
    lost.pulse = out_.pulse;
    std::fprintf(stderr, "[vinyl] output lost: %s; looking for it again\n", lost.label.c_str());
    if (open_) {
        out_.sink->close();
        out_.sink.reset();
        open_ = false;
    }
    for (int i = 0; i < 5 && !quit_.load(); ++i) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (cv_.wait_for(lk, std::chrono::milliseconds(500), [&] { return quit_.load() || !cmds_.empty(); })) break;
        }
        if (sinks_.still_there(lost)) {
            std::fprintf(stderr, "[vinyl] output %s is back (a profile switch): playing on\n", lost.label.c_str());
            reposition(cur, f, PlayState::Playing); /* reopened by the next step */
            return;
        }
    }
    reposition(cur, f, PlayState::Paused);
    EngineEvent ev;
    ev.kind = EngineEvent::Kind::OutputLost;
    ev.pos = cur.pos;
    ev.text = lost.label;
    post(ev);
}

void Engine::step() {
    if (!dec_) {
        state_ = PlayState::Idle;
        return;
    }
    if (!open_ && !ensure_open()) {
        update_snapshot();
        return;
    }
    const size_t fb = info_.out.frame_bytes();
    dbuf_.resize(kPeriodFrames * fb);
    long n = dec_->read(dbuf_.data(), kPeriodFrames);
    if (n < 0) {
        std::fprintf(stderr, "[vinyl] decode error %ld in %s: the track ends here\n", n, dec_path_.c_str());
        n = 0;
    }
    if (n == 0) {
        end_of_track();
        update_snapshot();
        return;
    }
    obuf_.resize(static_cast<size_t>(n) * granted_.channels * format_bytes(granted_.format));
    convert_pcm(dbuf_.data(), info_.out.bits, info_.out.channels, obuf_.data(), granted_.format, granted_.channels,
                static_cast<size_t>(n), gain_.load());
    write_all(obuf_.data(), static_cast<size_t>(n));
    update_snapshot();
}

void Engine::end_of_track() {
    if (next_pos_ >= 0) {
        std::unique_ptr<TrackDecoder> d;
        TrackInfo inf;
        int err = 0;
        const int npos = next_pos_;
        const std::string npath = next_path_;
        next_pos_ = -1;
        next_path_.clear();
        if (!open_decoder(npos, npath, d, inf, err)) {
            close_output(true);
            dec_.reset();
            info_ = TrackInfo();
            dec_pos_ = npos;
            dec_path_ = npath;
            segs_.clear();
            segs_.push_back(Seg{written_, npos, npath, 0, TrackInfo()});
            state_ = PlayState::Idle;
            EngineEvent ev;
            ev.kind = EngineEvent::Kind::OpenFailed;
            ev.pos = npos;
            ev.err = err;
            ev.path = npath;
            ev.text = track_error_text(err, npath);
            ev.automatic = true;
            post(ev);
            return;
        }
        bool same = open_ && same_format(request_for(inf), req_);
        if (same && target_.kind == OutputTarget::Kind::Auto && sinks_.default_moved(out_)) same = false;
        if (!same) close_output(true); /* the previous track's tail is played out first */
        dec_ = std::move(d);
        info_ = inf;
        dec_pos_ = npos;
        dec_path_ = npath;
        segs_.push_back(Seg{written_, npos, npath, 0, inf});
        EngineEvent ev;
        ev.kind = EngineEvent::Kind::Advanced;
        ev.pos = npos;
        ev.path = npath;
        post(ev);
        return;
    }
    close_output(true);
    dec_->seek(0);
    segs_.clear();
    segs_.push_back(Seg{written_, dec_pos_, dec_path_, 0, info_});
    state_ = PlayState::Paused;
    EngineEvent ev;
    ev.kind = EngineEvent::Kind::QueueEnd;
    ev.pos = dec_pos_;
    ev.path = dec_path_;
    post(ev);
}

void Engine::update_snapshot() {
    Seg s;
    const uint64_t f = heard_frame(&s);
    if (!segs_.empty() && s.pos != heard_pos_) {
        heard_pos_ = s.pos;
        ++gen_;
        EngineEvent ev;
        ev.kind = EngineEvent::Kind::Started;
        ev.pos = s.pos;
        ev.path = s.path;
        post(ev);
    }
    std::lock_guard<std::mutex> g(mu_);
    snap_.state = state_;
    snap_.pos = heard_pos_;
    snap_.path = s.path;
    snap_.info = s.info;
    snap_.frame = f;
    snap_.total = s.info.total_frames;
    snap_.rate = s.info.out.rate;
    snap_.output_label = last_label_;
    snap_.output_name = last_name_;
    snap_.output_pulse = last_pulse_;
    snap_.underruns = underruns_;
    snap_.gen = gen_;
}

}  // namespace vinyl
