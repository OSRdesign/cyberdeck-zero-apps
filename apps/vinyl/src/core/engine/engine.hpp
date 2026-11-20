/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl engine: one playback thread that decodes the current track and writes it to the chosen output (report 059 P1).
 *
 *   gapless   at the end of a track the engine continues with the track the controller named (set_next) on the SAME
 *             open stream when its format (rate, channels, sample size) is the same: no drain, no gap; another format
 *             drains the stream and opens a new one (a gap of tens of ms). In Automatic mode a changed default sink is
 *             also followed there (D8).
 *   position  what is heard: frames written minus the output's delay; a track change is reported when its first frame
 *             is heard (Started), the decoder's move earlier (Advanced, so the controller can name the next track).
 *   pause     the heard position is kept, the stream is dropped and closed; play opens it again there (Cassette's
 *             "pause by drop"), so Automatic picks the default sink of that moment.
 *   loss      when the output fails (a Bluetooth device switched off, a USB card pulled): the same device is looked for
 *             during 2.5 s (a Bluetooth profile switch re-creates the node: play on), else playback PAUSES at the frame
 *             heard and OutputLost is reported; the engine never moves to another output (D8).
 *   volume    an optional software gain (only for an output without a playback mixer: D11 as the owner changed it).
 *   P2a       ReplayGain (core/dsp/replaygain.hpp): each track's gain from its tags, read when the engine opens it, applied
 *             to that track's own samples (so a gapless change switches it exactly at the track boundary) and before the
 *             output's volume, which it never touches; the mode can change while a track plays. Gapless can be turned
 *             off (set_gapless: every change drains and reopens the stream). The sleep timer's end fade (set_end_fade):
 *             while no next track is named, the last 10 s of the track fade out (the decoded position, per period).
 *   P2d       The equalizer (set_eq, core/dsp/eq.hpp) and the crossfade (set_crossfade_s, core/dsp/crossfade.hpp), on
 *             every decoder (the small ones and FFmpeg: both work on the decoded samples) and every output. The chain:
 *             decoded samples -> float x the track's ReplayGain -> [the crossfade mix with the outgoing track, its own
 *             ReplayGain] -> EQ (no pre-gain: a boost raises its band, the rest stays at 0 dB) -> x the volume, boost
 *             and sleep fade -> limiter (it takes the peaks a boost pushes over -1 dBFS) -> the output's format. With
 *             the EQ flat or Off, no crossfade set and a net gain of 0 dB or less, the integer path of P1 stays
 *             (bit-exact); otherwise the float path with the limiter, latched for the open stream as the boost's is.
 *             The controller's Transition setting (core/settings) gives set_gapless (all but Normal) and
 *             set_crossfade_s (Crossfade 5 / 10 / 20 sec). A crossfade starts when the decoder reaches the fade's
 *             length before the end of a track whose length is known and whose next track was named with may_fade
 *             (the controller: every automatic change into another track, the same album or not; not repeat one; owner,
 *             2026-11-20), when the next track opens in the same format; the next track's decoder then becomes the
 *             engine's (its segment starts where the mix starts, Advanced is posted, so the controller names the one
 *             after it) and the outgoing one is read alongside until the fade ends. A load (a skip, a tap), a seek, a
 *             pause, an output change or loss ends a fade at once: the side that is heard goes on alone (a pause or a
 *             seek while the outgoing track is still the one heard goes back to it, the incoming one named next again).
 *
 * Commands come from the controller's thread (any thread), events and snapshots go back to it. LVGL-free.
 */
#pragma once

#include "../decode/track_decoder.hpp"
#include "../dsp/crossfade.hpp"
#include "../dsp/eq.hpp"
#include "../dsp/limiter.hpp"
#include "../dsp/replaygain.hpp"
#include "outputs.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vinyl {

enum class PlayState { Idle, Playing, Paused };
const char *play_state_name(PlayState s);

struct EngineEvent {
    enum class Kind {
        Started,     /* `pos` is heard now (a new track, by a load or a gapless change) */
        Advanced,    /* the decoder moved on to `pos` (name the following track with set_next) */
        QueueEnd,    /* the last track ended (nothing was named next): paused at its start */
        OpenFailed,  /* `pos` could not be opened (err, text); `automatic` when it was the next track at a track end */
        OutputLost,  /* the output in use went away: paused (text = its label) */
        OutputFailed /* the output could not be opened when playback started: paused (text = why) */
    };
    Kind kind = Kind::Started;
    int pos = -1;
    int err = 0;
    std::string text;
    std::string path;
    bool automatic = false;
};

struct EngineSnapshot {
    PlayState state = PlayState::Idle;
    int pos = -1;            /* the queue position heard */
    std::string path;
    TrackInfo info;
    uint64_t frame = 0;      /* heard position in the track */
    uint64_t total = 0;      /* the track's length in frames (0 unknown) */
    uint32_t rate = 0;
    std::string output_label; /* "Evolve 65", "Default output" ("" while nothing is open) */
    std::string output_name;  /* the sink or device opened */
    bool output_pulse = false;
    uint64_t underruns = 0;
    uint64_t gen = 0;        /* changes with every heard track change */
    ReplayGainTags rg;       /* the heard track's ReplayGain tags */
    bool fading = false;     /* P2d: a crossfade mixes two tracks now */
    int fade_s = 0;          /* the running fade's setting seconds (5 / 10 / 20; 0 when none runs): Now playing's chip */
    int eq_bands = 0;        /* P2d: the equalizer's bands at work (0: bypass) */
    uint64_t pos_ms() const { return rate ? frame * 1000 / rate : 0; }
    uint64_t total_ms() const { return rate ? total * 1000 / rate : 0; }
};

/* int16 / int32 interleaved frames of `in` (in_ch channels) to `out` with out_ch channels and out_fmt samples (S16, S24
 * packed or S32), times `gain` (1 = unchanged), clipped. Mono to stereo duplicates; more channels to stereo keeps the
 * first two; stereo or more to mono averages the first two. */
void convert_pcm(const void *in, unsigned in_bits, unsigned in_ch, void *out, SampleFormat out_fmt, unsigned out_ch,
                 size_t frames, float gain);
/* The software gain of an output without a mixer for a level in dB (core/volume: its model's dB of the level): the
 * amplitude 10^(dB / 20); -infinity (mute) is 0, 0 dB and above 1. */
float volume_gain_db(double db);
/* P2a gain boost (report 060 3e): as convert_pcm, but through the look-ahead limiter (core/dsp/limiter): the samples are
 * scaled by `gain` in float, limited (ceiling -1 dBFS) and delayed by lim.delay() frames (one frame out per frame in), then
 * written in out_fmt. `scratch` is the caller's float buffer. Used by the engine when the net digital gain is above 0 dB. */
void convert_pcm_limited(const void *in, unsigned in_bits, unsigned in_ch, void *out, SampleFormat out_fmt, unsigned out_ch,
                         size_t frames, float gain, Limiter &lim, std::vector<float> &scratch);
/* Writes the limiter's last frames (lim.flush) as out_fmt samples; returns the frames (lim.delay()). */
size_t limiter_tail_pcm(Limiter &lim, void *out, SampleFormat out_fmt, unsigned out_ch);
/* P2d: int16 / int32 interleaved frames (in_ch channels) to float interleaved stereo (full scale 1.0) times k (mono
 * duplicated, more than two channels: the first two); and float stereo frames to out_ch channels of out_fmt, clipped
 * (mono: the average of the two). */
void pcm_to_stereo_float(const void *in, unsigned in_bits, unsigned in_ch, size_t frames, float k, float *out);
void stereo_float_to_pcm(const float *st, size_t frames, unsigned out_ch, SampleFormat out_fmt, void *out);
/* The journal line of a track played with a boost or a positive net gain (owner's board test of 2026-11-15: "I don't have
 * the feeling it's working"): "[vinyl] gain: net +4.0 dB (boost +4 dB, replaygain +0.0 dB, app volume +0.0 dB) limiter on:
 * reduction avg 0.8 dB, max 4.5 dB, 48 % of frames limited, peak in -4.1 dBFS, after gain -0.1 dBFS, out -1.0 dBFS (first 2 s
 * of a.m4a)". `limiter` false: the net gain is under 0 dB, the integer path ("limiter off (net gain under 0 dB)"). */
std::string gain_report_text(double net_db, int boost_db, double replaygain_db, double volume_db, bool limiter,
                             const Limiter::Stats &st, const std::string &name, const char *when);
/* true when a net gain needs the limiter (above 0 dB, 0.001 dB of slack) */
inline bool gain_needs_limiter(float g) { return g > 1.0001f; }
/* The gain boost of 0 to +4 dB in whole dB (anything else is clamped) as an amplitude factor. */
float boost_factor(int db);

/* Reads a file's ReplayGain tags (the engine's thread, when it opens a track): core/tags read_replaygain, a fake in the
 * tests. false: none read. */
using ReplayGainReader = std::function<bool(const std::string &path, ReplayGainTags &out)>;

class Engine {
public:
    Engine(DecodeLibs libs, SinkProvider &sinks, ReplayGainReader rg = ReplayGainReader());
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /* Opens `path` (queue position `pos`) at start_ms; plays at once when `play`. */
    void load(int pos, const std::string &path, uint64_t start_ms, bool play);
    void play();
    void pause();
    void toggle();
    /* Pauses and goes back to the start of the track. */
    void stop();
    void seek_ms(uint64_t ms);
    /* The track to continue with when the current one ends (pos -1: none, the queue ends). may_fade (P2d): the change
     * may be a crossfade when the setting is on (the controller's rule, core/dsp crossfade_named: another track, not
     * repeat one, the sleep timer not stopping there). */
    void set_next(int pos, const std::string &path, bool may_fade = false);
    void set_output(const OutputTarget &t);
    void set_gain(float g);
    /* P2a: the ReplayGain mode (applied at once, to the playing track too), gapless on or off (the next change), the
     * sleep timer's end fade (the last 10 s of a track after which nothing is named) */
    void set_replaygain(ReplayGainMode m);
    /* the software gain boost (0..+4 dB, whole dB; clamped), at the end of the gain chain, with the limiter */
    void set_boost_db(int db);
    void set_gapless(bool on);
    void set_end_fade(bool on);
    /* P2d: the equalizer's gains in force (all 0: bypass; EqSettings::effective()), applied from the next period */
    void set_eq(const EqGains &gains);
    /* P2d: the crossfade in seconds (0 none, 5, 10, 20: the Transition's crossfades), for the next track change */
    void set_crossfade_s(int seconds);
    /* the gain ReplayGain applies to the heard track now, in dB (0: off or untagged) */
    double replaygain_db_now() const;

    EngineSnapshot snapshot() const;
    bool poll_event(EngineEvent &ev);
    /* Ends the thread (closes the output). Idempotent. */
    void shutdown();

    /* for the tests: the period the engine writes, in frames */
    static constexpr size_t kPeriodFrames = 2048;

private:
    struct Cmd {
        enum class Kind { Load, Play, Pause, Toggle, Stop, Seek, Next, Output };
        explicit Cmd(Kind k) : kind(k) {}
        Kind kind;
        int pos = -1;
        std::string path;
        uint64_t ms = 0;
        bool play = false;
        bool fade = false; /* Next: may crossfade */
        OutputTarget target;
    };
    struct Seg {
        uint64_t start_written = 0; /* written_ when this track's first frame was written */
        int pos = -1;
        std::string path;
        uint64_t start_frame = 0;
        TrackInfo info;
        ReplayGainTags rg;
    };

    void push(Cmd c, bool interrupt);
    void post(EngineEvent ev);
    void run();
    void handle(const Cmd &c);
    void step();
    bool ensure_open();
    void close_output(bool drain);
    bool write_all(const unsigned char *data, size_t frames);
    void end_of_track();
    void output_failed();
    void hold_at_heard(PlayState next); /* pause (or reposition) at the heard frame */
    void reposition(const Seg &cur, uint64_t frame, PlayState next);
    uint64_t heard_frame(Seg *seg);
    void update_snapshot();
    bool open_decoder(int pos, const std::string &path, std::unique_ptr<TrackDecoder> &dec, TrackInfo &info, int &err,
                      ReplayGainTags &rg);
    SinkFormat request_for(const TrackInfo &info) const;
    /* the factor applied to the decoded samples: the controller's gain, the decoder's track's ReplayGain, the end fade */
    float sample_gain(uint64_t frame_before) const;
    /* its two parts (P2d): a track's own ReplayGain; the rest (the controller's gain, the boost, the end fade) */
    float track_gain(const ReplayGainTags &rg) const;
    float common_gain(uint64_t frame_before) const;
    /* P2d: the float path from `st` (n stereo frames already at the track gain): EQ, the common gain, the limiter, then
     * obuf_ in the output's format */
    void float_out(float *st, size_t n, float common);
    void eq_sync();                       /* new gains (set_eq) or a new rate: the filters again */
    bool fade_maybe_start(uint64_t pos);  /* the crossfade's start: true when the decoders were swapped */
    void fade_step();                     /* one period of the mix */
    void fade_end();                      /* the outgoing decoder away (the fade done, or ended by a command) */

    DecodeLibs libs_;
    SinkProvider &sinks_;
    ReplayGainReader rg_reader_;
    std::atomic<int> rg_mode_{static_cast<int>(ReplayGainMode::Off)};
    std::atomic<bool> gapless_{true};
    std::atomic<bool> end_fade_{false};
    ReplayGainTags dec_rg_; /* the decoder's track's tags (the thread's) */

    /* commands */
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Cmd> cmds_;
    std::atomic<bool> interrupt_{false};
    std::atomic<bool> quit_{false};
    std::atomic<float> gain_{1.0f};
    std::atomic<float> boost_{1.0f};
    Limiter lim_;          /* the engine thread's: latched on for an open stream once the net gain went above 0 dB */
    bool lim_on_ = false;
    std::vector<float> fbuf_;
    void limiter_off() { lim_on_ = false; lim_.reset(); } /* the stream was dropped or closed: no tail to play */
    void limiter_drain(); /* the stream ends gracefully: the limiter's last frames are written */
    /* the gain journal line per track (the engine thread's) */
    std::string gstat_path_;
    uint64_t gstat_frames_ = 0;
    bool gstat_2s_ = false, gstat_on_ = false;
    float gstat_net_ = 1.0f;
    void gain_track_check(); /* a new track: the previous one's line when it was heard for a while, the counters reset */
    void gain_log(const char *when);
    std::deque<EngineEvent> events_;
    EngineSnapshot snap_;
    /* P2d: the equalizer (gains from the controller, applied by the thread) and the crossfade */
    std::mutex eq_mu_;
    EqGains eq_want_{};
    std::atomic<uint64_t> eq_ver_{0};
    uint64_t eq_have_ = UINT64_MAX;
    uint32_t eq_rate_ = 0;
    Equalizer eq_;
    std::atomic<int> fade_s_{0};
    bool next_fade_ = false;   /* the named next track may crossfade */
    bool dec_fade_ = false;    /* the decoder's track was named with may_fade (a pause back to the track before names it so again) */
    bool fade_tried_ = false;  /* the crossfade into the named next track was looked at (not again until another next or track) */
    struct Fade {
        std::unique_ptr<TrackDecoder> dec; /* the outgoing track */
        TrackInfo info;
        ReplayGainTags rg;
        std::string path;
        uint64_t done = 0, len = 0; /* frames mixed, the fade's length */
        bool ended = false;         /* the outgoing track has no more frames (silence for the rest of the fade) */
    } fade_;
    bool fading_ = false;
    int fading_s_ = 0; /* the setting's seconds the running fade started with */
    /* the incoming track opened at the setting's distance from the end, waiting until what is left of the outgoing one is
     * the fade's length (shorter than the setting when the incoming track is short) */
    struct Pending {
        std::unique_ptr<TrackDecoder> dec;
        TrackInfo info;
        ReplayGainTags rg;
        uint64_t len = 0;
    } pend_;
    std::vector<unsigned char> xbuf_; /* the outgoing track's period */
    std::vector<float> fin_, fout_;   /* the two tracks as float stereo */

    /* the thread's own state */
    PlayState state_ = PlayState::Idle;
    std::unique_ptr<TrackDecoder> dec_;
    TrackInfo info_;
    int dec_pos_ = -1;
    std::string dec_path_;
    int next_pos_ = -1;
    std::string next_path_;
    OutputTarget target_;
    OpenedSink out_;
    bool open_ = false;
    std::string last_label_, last_name_;
    bool last_pulse_ = false;
    SinkFormat req_, granted_;
    uint64_t written_ = 0;
    std::deque<Seg> segs_;
    int heard_pos_ = -1;
    uint64_t gen_ = 0;
    uint64_t underruns_ = 0;
    std::vector<unsigned char> dbuf_, obuf_;
    std::thread th_;
};

}  // namespace vinyl
