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
 *
 * Commands come from the controller's thread (any thread), events and snapshots go back to it. LVGL-free.
 */
#pragma once

#include "../decode/track_decoder.hpp"
#include "outputs.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
    uint64_t pos_ms() const { return rate ? frame * 1000 / rate : 0; }
    uint64_t total_ms() const { return rate ? total * 1000 / rate : 0; }
};

/* int16 / int32 interleaved frames of `in` (in_ch channels) to `out` with out_ch channels and out_fmt samples (S16, S24
 * packed or S32), times `gain` (1 = unchanged), clipped. Mono to stereo duplicates; more channels to stereo keeps the
 * first two; stereo or more to mono averages the first two. */
void convert_pcm(const void *in, unsigned in_bits, unsigned in_ch, void *out, SampleFormat out_fmt, unsigned out_ch,
                 size_t frames, float gain);
/* The software gain 0..100 of an output without a mixer, on the sound server's cubic curve (50 = -18.06 dB). */
float volume_gain(int volume);

class Engine {
public:
    Engine(DecodeLibs libs, SinkProvider &sinks);
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
    /* The track to continue with when the current one ends (pos -1: none, the queue ends). */
    void set_next(int pos, const std::string &path);
    void set_output(const OutputTarget &t);
    void set_gain(float g);

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
        OutputTarget target;
    };
    struct Seg {
        uint64_t start_written = 0; /* written_ when this track's first frame was written */
        int pos = -1;
        std::string path;
        uint64_t start_frame = 0;
        TrackInfo info;
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
    bool open_decoder(int pos, const std::string &path, std::unique_ptr<TrackDecoder> &dec, TrackInfo &info, int &err);
    SinkFormat request_for(const TrackInfo &info) const;

    DecodeLibs libs_;
    SinkProvider &sinks_;

    /* commands */
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Cmd> cmds_;
    std::atomic<bool> interrupt_{false};
    std::atomic<bool> quit_{false};
    std::atomic<float> gain_{1.0f};
    std::deque<EngineEvent> events_;
    EngineSnapshot snap_;

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
