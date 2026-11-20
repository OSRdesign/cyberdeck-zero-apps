/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl engine (see engine.hpp).
 */
#include "engine.hpp"

#include "../sleep/sleep_timer.hpp"
#include "../tags/tag_reader.hpp"

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

float volume_gain_db(double db) {
    if (!(db > -200.0)) return 0.0f; /* mute (and NaN) */
    if (db >= 0.0) return 1.0f;
    return static_cast<float>(std::pow(10.0, db / 20.0)); /* linear in dB (Dr. Lex: exponential amplitude) */
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
double db_of_factor(double f) { return 20.0 * std::log10(std::max(f, 1e-6)); }
std::string base_name(const std::string &p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}
}  // namespace

std::string gain_report_text(double net_db, int boost_db, double replaygain_db, double volume_db, bool limiter,
                             const Limiter::Stats &st, const std::string &name, const char *when) {
    char b[512];
    int n = std::snprintf(b, sizeof b, "gain: net %+.1f dB (boost %+d dB, replaygain %+.1f dB, app volume %+.1f dB) ", net_db,
                          boost_db, replaygain_db, volume_db);
    if (!limiter || st.frames == 0) {
        std::snprintf(b + n, sizeof b - static_cast<size_t>(n), "limiter off (net gain under 0 dB) (%s of %s)", when, name.c_str());
        return b;
    }
    const double avg = st.red_sum_db / static_cast<double>(st.frames);
    const double max_red = st.min_gain < 1.0f ? -db_of_factor(st.min_gain) : 0.0;
    const double pct = 100.0 * static_cast<double>(st.limited) / static_cast<double>(st.frames);
    const double after = db_of_factor(st.peak_in), out = db_of_factor(st.peak_out);
    std::snprintf(b + n, sizeof b - static_cast<size_t>(n),
                  "limiter on: reduction avg %.1f dB, max %.1f dB, %.0f %% of frames limited, peak in %+.1f dBFS, after gain %+.1f "
                  "dBFS, out %+.1f dBFS (%s of %s)",
                  avg, max_red, pct, after - net_db, after, out, when, name.c_str());
    return b;
}

void Engine::gain_log(const char *when) {
    if (!gstat_on_) return;
    const float rgf = replaygain_factor(static_cast<ReplayGainMode>(rg_mode_.load()), dec_rg_);
    const float bf = boost_.load();
    const double boost_db = db_of_factor(bf);
    std::string line = gain_report_text(db_of_factor(gstat_net_), static_cast<int>(std::lround(boost_db)), db_of_factor(rgf),
                                        db_of_factor(gain_.load()), lim_on_, lim_.stats(), base_name(gstat_path_), when);
    std::fprintf(stderr, "[vinyl] %s\n", line.c_str());
}

void Engine::gain_track_check() {
    if (dec_path_ == gstat_path_) return;
    if (gstat_frames_ > 0 && info_.out.rate && gstat_frames_ >= info_.out.rate / 2) gain_log("track left");
    gstat_path_ = dec_path_;
    gstat_frames_ = 0;
    gstat_2s_ = false;
    gstat_on_ = false;
    lim_.clear_stats();
}

float boost_factor(int db) {
    const int d = std::max(0, std::min(kMaxBoostDb, db));
    return d == 0 ? 1.0f : static_cast<float>(std::pow(10.0, d / 20.0));
}

namespace {
/* one sample (an int32 at the top of its range) into out_fmt, advancing `o` */
void pack_sample(unsigned char *&o, SampleFormat fmt, int32_t v) {
    switch (fmt) {
    case SampleFormat::S16_LE: {
        const int16_t s = static_cast<int16_t>(v >> 16);
        std::memcpy(o, &s, 2);
        o += 2;
        break;
    }
    case SampleFormat::S24_3LE: {
        const uint32_t u = static_cast<uint32_t>(v) >> 8;
        o[0] = static_cast<unsigned char>(u & 0xFF);
        o[1] = static_cast<unsigned char>((u >> 8) & 0xFF);
        o[2] = static_cast<unsigned char>((u >> 16) & 0xFF);
        o += 3;
        break;
    }
    default: std::memcpy(o, &v, 4); o += 4; break;
    }
}
int32_t float_to_s32(float x) {
    const double v = std::floor(static_cast<double>(x) * 2147483648.0 + 0.5);
    return static_cast<int32_t>(std::max(-2147483648.0, std::min(2147483647.0, v)));
}
/* stereo float frames to out_ch channels of out_fmt (mono: the average, more: zeros after the first two) */
void put_frames(const float *st, size_t frames, unsigned out_ch, SampleFormat out_fmt, unsigned char *o) {
    for (size_t f = 0; f < frames; ++f) {
        const float l = st[2 * f], r = st[2 * f + 1];
        if (out_ch == 1) {
            pack_sample(o, out_fmt, float_to_s32(0.5f * (l + r)));
            continue;
        }
        pack_sample(o, out_fmt, float_to_s32(l));
        pack_sample(o, out_fmt, float_to_s32(r));
        for (unsigned c = 2; c < out_ch; ++c) pack_sample(o, out_fmt, 0);
    }
}
}  // namespace

void pcm_to_stereo_float(const void *in, unsigned in_bits, unsigned in_ch, size_t frames, float k, float *out) {
    if (in_ch == 0) return;
    const int16_t *i16 = static_cast<const int16_t *>(in);
    const int32_t *i32 = static_cast<const int32_t *>(in);
    const float s = std::max(0.0f, k) * (in_bits == 16 ? 1.0f / 32768.0f : 1.0f / 2147483648.0f);
    for (size_t f = 0; f < frames; ++f) {
        const size_t a = f * in_ch, b = in_ch > 1 ? a + 1 : a;
        const float l = in_bits == 16 ? static_cast<float>(i16[a]) : static_cast<float>(i32[a]);
        const float r = in_bits == 16 ? static_cast<float>(i16[b]) : static_cast<float>(i32[b]);
        out[2 * f] = l * s;
        out[2 * f + 1] = r * s;
    }
}

void stereo_float_to_pcm(const float *st, size_t frames, unsigned out_ch, SampleFormat out_fmt, void *out) {
    if (out_ch == 0) return;
    put_frames(st, frames, out_ch, out_fmt, static_cast<unsigned char *>(out));
}

void convert_pcm_limited(const void *in, unsigned in_bits, unsigned in_ch, void *out, SampleFormat out_fmt, unsigned out_ch,
                         size_t frames, float gain, Limiter &lim, std::vector<float> &scratch) {
    if (in_ch == 0 || out_ch == 0) return;
    scratch.resize(frames * 2);
    pcm_to_stereo_float(in, in_bits, in_ch, frames, gain, scratch.data());
    lim.process(scratch.data(), frames);
    put_frames(scratch.data(), frames, out_ch, out_fmt, static_cast<unsigned char *>(out));
}

size_t limiter_tail_pcm(Limiter &lim, void *out, SampleFormat out_fmt, unsigned out_ch) {
    std::vector<float> tail(2 * lim.delay());
    const size_t n = lim.delay();
    lim.flush(tail.data());
    put_frames(tail.data(), n, out_ch, out_fmt, static_cast<unsigned char *>(out));
    return n;
}

namespace {
bool same_format(const SinkFormat &a, const SinkFormat &b) {
    return a.rate == b.rate && a.channels == b.channels && a.format == b.format;
}
}  // namespace

Engine::Engine(DecodeLibs libs, SinkProvider &sinks, ReplayGainReader rg)
    : libs_(std::move(libs)), sinks_(sinks), rg_reader_(std::move(rg)) {
    if (!rg_reader_) rg_reader_ = [](const std::string &path, ReplayGainTags &out) { return read_replaygain(path, out); };
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
void Engine::set_next(int pos, const std::string &path, bool may_fade) {
    Cmd c{Cmd::Kind::Next};
    c.pos = pos;
    c.path = path;
    c.fade = may_fade;
    push(std::move(c), false);
}
void Engine::set_output(const OutputTarget &t) {
    Cmd c{Cmd::Kind::Output};
    c.target = t;
    push(std::move(c), true);
}
void Engine::set_gain(float g) { gain_.store(g); }
void Engine::set_replaygain(ReplayGainMode m) { rg_mode_.store(static_cast<int>(m)); }
void Engine::set_boost_db(int db) { boost_.store(boost_factor(db)); }
void Engine::set_gapless(bool on) { gapless_.store(on); }
void Engine::set_end_fade(bool on) { end_fade_.store(on); }
void Engine::set_eq(const EqGains &gains) {
    {
        std::lock_guard<std::mutex> g(eq_mu_);
        if (gains == eq_want_ && eq_ver_.load() != 0) return;
        eq_want_ = gains;
    }
    eq_ver_.fetch_add(1);
}
void Engine::set_crossfade_s(int seconds) { fade_s_.store(crossfade_valid(seconds)); }

double Engine::replaygain_db_now() const {
    std::lock_guard<std::mutex> g(mu_);
    return replaygain_db(static_cast<ReplayGainMode>(rg_mode_.load()), snap_.rg);
}

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

bool Engine::open_decoder(int pos, const std::string &path, std::unique_ptr<TrackDecoder> &dec, TrackInfo &info, int &err,
                          ReplayGainTags &rg) {
    (void)pos;
    err = 0;
    rg = ReplayGainTags();
    dec = open_track(path, libs_, info, &err);
    if (!dec) return false;
    if (info.out.rate == 0 || info.out.channels == 0 || (info.out.bits != 16 && info.out.bits != 32)) {
        dec.reset();
        err = kDecodeFailed;
        return false;
    }
    /* the track's ReplayGain tags (a small read: no picture, the MP3 tag walked frame by frame) */
    if (rg_reader_ && rg_reader_(path, rg) && rg.any()) {
        const size_t slash = path.rfind('/');
        std::fprintf(stderr, "[vinyl] replaygain %s: track %s%.2f dB (peak %.3f), album %s%.2f dB (peak %.3f)\n",
                     slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1, rg.has_track ? "" : "none ",
                     rg.track_db, rg.has_track_peak ? rg.track_peak : 0.0f, rg.has_album ? "" : "none ", rg.album_db,
                     rg.has_album_peak ? rg.album_peak : 0.0f);
    }
    return true;
}

float Engine::track_gain(const ReplayGainTags &rg) const {
    return replaygain_factor(static_cast<ReplayGainMode>(rg_mode_.load()), rg);
}

float Engine::common_gain(uint64_t frame_before) const {
    float g = gain_.load() * boost_.load();
    /* the sleep timer's end fade: the last 10 s of the decoded track when nothing is named next */
    if (end_fade_.load() && next_pos_ < 0 && info_.total_frames && info_.out.rate) {
        const uint64_t left = info_.total_frames > frame_before ? info_.total_frames - frame_before : 0;
        g *= sleep_fade_gain(left * 1000 / info_.out.rate);
    }
    return g;
}

float Engine::sample_gain(uint64_t frame_before) const { return track_gain(dec_rg_) * common_gain(frame_before); }

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
        /* a load (a skip, a tap on a track) changes at once: a crossfade running ends, nothing named next fades */
        fade_end();
        next_fade_ = dec_fade_ = fade_tried_ = false;
        std::unique_ptr<TrackDecoder> d;
        TrackInfo inf;
        ReplayGainTags rg;
        int err = 0;
        if (!open_decoder(c.pos, c.path, d, inf, err, rg)) {
            close_output(false);
            dec_.reset();
            info_ = TrackInfo();
            dec_rg_ = ReplayGainTags();
            dec_pos_ = c.pos;
            dec_path_ = c.path;
            next_pos_ = -1;
            next_path_.clear();
            segs_.clear();
            segs_.push_back(Seg{written_, c.pos, c.path, 0, TrackInfo(), ReplayGainTags()});
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
            else {
                out_.sink->drop();
                limiter_off();
                eq_.reset();
            }
        }
        dec_ = std::move(d);
        info_ = inf;
        dec_rg_ = rg;
        dec_pos_ = c.pos;
        dec_path_ = c.path;
        next_pos_ = -1;
        next_path_.clear();
        segs_.clear();
        segs_.push_back(Seg{written_, c.pos, c.path, start, inf, rg});
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
        fade_end();
        if (dec_) {
            dec_->seek(0);
            segs_.clear();
            segs_.push_back(Seg{written_, dec_pos_, dec_path_, 0, info_, dec_rg_});
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
        segs_.push_back(Seg{written_, dec_pos_, dec_path_, f, info_, dec_rg_});
        return;
    }
    case Cmd::Kind::Next:
        next_pos_ = c.pos;
        next_path_ = c.path;
        next_fade_ = c.fade && c.pos >= 0;
        fade_tried_ = false;
        pend_ = Pending(); /* an incoming track opened for the one named before */
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
    /* a crossfade ends here (a pause, a seek, an output change or loss): the side heard goes on alone */
    if (fading_) std::fprintf(stderr, "[vinyl] crossfade: ended early (a pause, a seek or the output), %s plays on alone\n",
                              base_name(cur.path).c_str());
    fade_end();
    fade_tried_ = false;
    if (cur.pos != dec_pos_ || cur.path != dec_path_) {
        /* the decoder already moved on to the next track (gapless look-ahead, or a crossfade not heard yet): go back to
         * the one heard, and name the one it had moved to as the next again (with the crossfade it was named with) */
        std::unique_ptr<TrackDecoder> d;
        TrackInfo inf;
        ReplayGainTags rg;
        int err = 0;
        const int was_pos = dec_pos_;
        const std::string was_path = dec_path_;
        const bool was_fade = dec_fade_;
        if (open_decoder(cur.pos, cur.path, d, inf, err, rg)) {
            dec_ = std::move(d);
            info_ = inf;
            dec_rg_ = rg;
            dec_pos_ = cur.pos;
            dec_path_ = cur.path;
            next_pos_ = was_pos;
            next_path_ = was_path;
            next_fade_ = was_fade;
            dec_fade_ = false;
        }
    }
    if (open_) {
        out_.sink->drop();
        limiter_off();
        eq_.reset();
        if (next != PlayState::Playing) close_output(false);
    }
    if (dec_) {
        if (dec_->seek(frame) != 0) {
            dec_->seek(0);
            frame = 0;
        }
    }
    segs_.clear();
    segs_.push_back(Seg{written_, dec_pos_, dec_path_, frame, info_, dec_rg_});
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
    lim_.init(granted_.rate ? granted_.rate : want.rate);
    lim_on_ = false;
    eq_.reset();
    req_ = want;
    last_label_ = out_.label;
    last_name_ = out_.name;
    last_pulse_ = out_.pulse;
    std::fprintf(stderr, "[vinyl] output open: %s (%s), %u Hz, %u ch, %s\n", out_.label.c_str(), out_.name.c_str(),
                 granted_.rate, granted_.channels, format_name(granted_.format));
    return true;
}

void Engine::limiter_drain() {
    if (!lim_on_ || !open_) return;
    obuf_.resize(lim_.delay() * granted_.channels * format_bytes(granted_.format));
    const size_t n = limiter_tail_pcm(lim_, obuf_.data(), granted_.format, granted_.channels);
    write_all(obuf_.data(), n);
}

void Engine::close_output(bool drain) {
    if (!open_) return;
    if (drain) limiter_drain(); /* the last 5 ms the limiter holds */
    limiter_off();
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
        limiter_off();
    }
    fade_end(); /* reposition() below goes on with the side heard */
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
    eq_sync();
    /* P2d: the crossfade: its start (the decoders swapped, the incoming track is the engine's now), then its periods */
    if (!fading_) fade_maybe_start(dec_->position());
    gain_track_check();
    if (fading_) {
        fade_step();
        return;
    }
    const size_t fb = info_.out.frame_bytes();
    dbuf_.resize(kPeriodFrames * fb);
    const uint64_t frame_before = dec_->position();
    /* a crossfade coming: the period stops where it starts, so the fade begins exactly its length before the end */
    size_t want = kPeriodFrames;
    if (fade_s_.load() > 0 && next_fade_ && next_pos_ >= 0 && info_.total_frames && info_.out.rate && (pend_.dec || !fade_tried_)) {
        const uint64_t len = pend_.dec ? pend_.len : crossfade_frames(fade_s_.load(), info_.out.rate, info_.total_frames, 0);
        const uint64_t at = len && len < info_.total_frames ? info_.total_frames - len : 0;
        if (at > frame_before && at - frame_before < want) want = static_cast<size_t>(at - frame_before);
    }
    long n = dec_->read(dbuf_.data(), want);
    if (n < 0) {
        std::fprintf(stderr, "[vinyl] decode error %ld in %s: the track ends here\n", n, dec_path_.c_str());
        n = 0;
    }
    if (n == 0) {
        gain_log("track end");
        gstat_on_ = false;
        gstat_frames_ = 0;
        end_of_track();
        update_snapshot();
        return;
    }
    obuf_.resize(static_cast<size_t>(n) * granted_.channels * format_bytes(granted_.format));
    const float gain = sample_gain(frame_before);
    if (lim_on_ || gain_needs_limiter(gain) || eq_.active() || fade_s_.load() > 0) {
        /* above 0 dB net (a boost, a positive ReplayGain), the equalizer at work, or the crossfade on (so a fade never
         * latches the limiter, and its 5 ms delay, in the middle of a track): the float path with the look-ahead
         * limiter; it stays in until the stream closes */
        lim_on_ = true;
        fbuf_.resize(static_cast<size_t>(n) * 2);
        pcm_to_stereo_float(dbuf_.data(), info_.out.bits, info_.out.channels, static_cast<size_t>(n), track_gain(dec_rg_), fbuf_.data());
        float_out(fbuf_.data(), static_cast<size_t>(n), common_gain(frame_before));
    } else {
        convert_pcm(dbuf_.data(), info_.out.bits, info_.out.channels, obuf_.data(), granted_.format, granted_.channels,
                    static_cast<size_t>(n), gain);
    }
    /* the journal line: a boost or a positive net gain is in force: once after the first 2 s, again at the track's end */
    if (boost_.load() > 1.0001f || gain_needs_limiter(gain)) {
        gstat_on_ = true;
        gstat_net_ = gain;
    }
    gstat_frames_ += static_cast<uint64_t>(n);
    if (gstat_on_ && !gstat_2s_ && info_.out.rate && gstat_frames_ >= 2ull * info_.out.rate) {
        gstat_2s_ = true;
        gain_log("first 2 s");
    }
    write_all(obuf_.data(), static_cast<size_t>(n));
    update_snapshot();
}

void Engine::end_of_track() {
    if (next_pos_ >= 0) {
        std::unique_ptr<TrackDecoder> d;
        TrackInfo inf;
        ReplayGainTags rg;
        int err = 0;
        const int npos = next_pos_;
        const std::string npath = next_path_;
        const bool nfade = next_fade_;
        next_pos_ = -1;
        next_path_.clear();
        next_fade_ = fade_tried_ = false;
        pend_ = Pending(); /* the outgoing track ended before its fade (its length was shorter than said): a gapless change */
        if (!open_decoder(npos, npath, d, inf, err, rg)) {
            close_output(true);
            dec_.reset();
            info_ = TrackInfo();
            dec_rg_ = ReplayGainTags();
            dec_pos_ = npos;
            dec_path_ = npath;
            segs_.clear();
            segs_.push_back(Seg{written_, npos, npath, 0, TrackInfo(), ReplayGainTags()});
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
        /* gapless off (the Playback setting): every change drains the stream and opens it again */
        bool same = gapless_.load() && open_ && same_format(request_for(inf), req_);
        if (same && target_.kind == OutputTarget::Kind::Auto && sinks_.default_moved(out_)) same = false;
        if (!same) close_output(true); /* the previous track's tail is played out first */
        dec_ = std::move(d);
        info_ = inf;
        dec_rg_ = rg;
        dec_pos_ = npos;
        dec_path_ = npath;
        dec_fade_ = nfade;
        segs_.push_back(Seg{written_, npos, npath, 0, inf, rg});
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
    segs_.push_back(Seg{written_, dec_pos_, dec_path_, 0, info_, dec_rg_});
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
    snap_.rg = s.rg;
    snap_.fading = fading_;
    snap_.eq_bands = eq_.bands();
}

/* ------------------------------------------------------------------------------------------------ P2d */

void Engine::float_out(float *st, size_t n, float common) {
    if (eq_.active()) eq_.process(st, n);
    if (common < 0.9999f || common > 1.0001f) {
        const float g = std::max(0.0f, common);
        for (size_t i = 0; i < 2 * n; ++i) st[i] *= g;
    }
    lim_.process(st, n);
    stereo_float_to_pcm(st, n, granted_.channels, granted_.format, obuf_.data());
}

void Engine::eq_sync() {
    const uint64_t v = eq_ver_.load();
    const uint32_t rate = info_.out.rate;
    if (v == eq_have_ && rate == eq_rate_) return;
    EqGains g;
    {
        std::lock_guard<std::mutex> l(eq_mu_);
        g = eq_want_;
    }
    const int before = eq_.bands();
    eq_have_ = v;
    eq_rate_ = rate;
    eq_.configure(g, rate);
    if (eq_.bands() != before || eq_.bands() > 0)
        std::fprintf(stderr, "[vinyl] equalizer: %d band(s) at work, curve top %+.1f dB, no pre-gain (the limiter takes the peaks) (%u Hz)\n",
                     eq_.bands(), eq_max_gain_db(g, rate), rate);
}

bool Engine::fade_maybe_start(uint64_t pos) {
    const int s = fade_s_.load();
    if (s <= 0 || !next_fade_ || next_pos_ < 0 || !open_ || !info_.total_frames || !info_.out.rate) return false;
    const uint64_t total = info_.total_frames;
    if (!pend_.dec) {
        if (fade_tried_) return false;
        const uint64_t want = crossfade_frames(s, info_.out.rate, total, 0);
        if (want == 0 || pos + want < total) return false; /* not yet (or no fade for a track this short) */
        fade_tried_ = true;
        std::unique_ptr<TrackDecoder> d;
        TrackInfo inf;
        ReplayGainTags rg;
        int err = 0;
        if (!open_decoder(next_pos_, next_path_, d, inf, err, rg)) return false; /* the end of the track reports it */
        /* only between tracks of the same format (D29), on the same stream (Automatic: the default output did not move) */
        if (!same_format(request_for(inf), req_) || (target_.kind == OutputTarget::Kind::Auto && sinks_.default_moved(out_))) {
            std::fprintf(stderr, "[vinyl] crossfade: not into %s (another format or output): gapless change\n", base_name(next_path_).c_str());
            return false;
        }
        const uint64_t len = crossfade_frames(s, info_.out.rate, total, inf.total_frames); /* half of a short incoming track */
        if (len == 0) return false;
        pend_.dec = std::move(d);
        pend_.info = inf;
        pend_.rg = rg;
        pend_.len = len;
    }
    if (pos + pend_.len < total) return false; /* a short incoming track: its shorter fade starts later */
    const uint64_t left = total > pos ? total - pos : 0;
    const uint64_t len = std::min(pend_.len, left);
    if (len * 1000 < static_cast<uint64_t>(kCrossfadeMinMs) * info_.out.rate) { /* resumed too near the end: gapless */
        pend_ = Pending();
        return false;
    }
    std::unique_ptr<TrackDecoder> d = std::move(pend_.dec);
    const TrackInfo inf = pend_.info;
    const ReplayGainTags rg = pend_.rg;
    pend_ = Pending();
    /* the swap: the incoming track is the engine's (its segment starts with the mix), the outgoing one is read alongside */
    fade_.dec = std::move(dec_);
    fade_.info = info_;
    fade_.rg = dec_rg_;
    fade_.path = dec_path_;
    fade_.done = 0;
    fade_.len = len;
    fade_.ended = false;
    fading_ = true;
    std::fprintf(stderr, "[vinyl] crossfade: %.1f s from %s into %s\n", static_cast<double>(len) / info_.out.rate,
                 base_name(dec_path_).c_str(), base_name(next_path_).c_str());
    dec_ = std::move(d);
    info_ = inf;
    dec_rg_ = rg;
    dec_pos_ = next_pos_;
    dec_path_ = next_path_;
    dec_fade_ = true;
    next_pos_ = -1;
    next_path_.clear();
    next_fade_ = false;
    fade_tried_ = false;
    segs_.push_back(Seg{written_, dec_pos_, dec_path_, 0, inf, rg});
    EngineEvent ev;
    ev.kind = EngineEvent::Kind::Advanced;
    ev.pos = dec_pos_;
    ev.path = dec_path_;
    post(ev);
    return true;
}

namespace {
/* `frames` frames of a decoder (a decoder may give fewer than asked before its end: read again), fewer only at its end */
long read_full(TrackDecoder &d, unsigned char *buf, size_t frames, size_t frame_bytes) {
    size_t got = 0;
    while (got < frames) {
        const long r = d.read(buf + got * frame_bytes, frames - got);
        if (r <= 0) break;
        got += static_cast<size_t>(r);
    }
    return static_cast<long>(got);
}
}  // namespace

void Engine::fade_step() {
    const size_t m = static_cast<size_t>(std::min<uint64_t>(kPeriodFrames, fade_.len - fade_.done));
    /* the incoming track (the engine's decoder) */
    dbuf_.resize(m * info_.out.frame_bytes());
    const uint64_t frame_before = dec_->position();
    const long nb = read_full(*dec_, dbuf_.data(), m, info_.out.frame_bytes());
    /* the outgoing one: silence once it has no more frames (its length was a little longer than it is) */
    long na = 0;
    if (!fade_.ended && fade_.dec) {
        xbuf_.resize(m * fade_.info.out.frame_bytes());
        na = read_full(*fade_.dec, xbuf_.data(), m, fade_.info.out.frame_bytes());
        if (static_cast<size_t>(na) < m) fade_.ended = true;
    }
    fin_.assign(2 * m, 0.0f);
    fout_.assign(2 * m, 0.0f);
    pcm_to_stereo_float(dbuf_.data(), info_.out.bits, info_.out.channels, static_cast<size_t>(nb), track_gain(dec_rg_), fin_.data());
    if (na > 0)
        pcm_to_stereo_float(xbuf_.data(), fade_.info.out.bits, fade_.info.out.channels, static_cast<size_t>(na), track_gain(fade_.rg),
                            fout_.data());
    fbuf_.resize(2 * m);
    crossfade_mix(fout_.data(), fin_.data(), fbuf_.data(), m, fade_.done, fade_.len);
    fade_.done += m;
    lim_on_ = true; /* two tracks can add up over full scale: the limiter is in */
    obuf_.resize(m * granted_.channels * format_bytes(granted_.format));
    float_out(fbuf_.data(), m, common_gain(frame_before));
    gstat_frames_ += m;
    if (fade_.done >= fade_.len) {
        std::fprintf(stderr, "[vinyl] crossfade: done\n");
        fade_end();
    }
    write_all(obuf_.data(), m);
    update_snapshot();
}

void Engine::fade_end() {
    pend_ = Pending();
    if (!fading_ && !fade_.dec) return;
    fade_.dec.reset();
    fade_ = Fade();
    fading_ = false;
}

}  // namespace vinyl
