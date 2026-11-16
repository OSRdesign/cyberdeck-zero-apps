/*
 * SPDX-License-Identifier: MIT
 *
 * VHS playback, the audio side (brief 3.1, 10): Vinyl 0.3.0's audio stack (play/engine, outputs, volume, input, pulse, decode:
 * copied from apps/vinyl at fdad829, trimmed to the FFmpeg audio decoder) behind one small, LVGL-free facade. The video side
 * (movie thread, presenter) takes its time from the clock this class reports.
 *
 *   clock       the HEARD position of the scene's audio stream: frames written to the output minus the output's delay (Vinyl's
 *               engine), extrapolated between two engine snapshots (the engine writes a period at a time: at 16 kHz a
 *               snapshot every 128 ms), never more than 250 ms ahead of the last one, and never going backwards while
 *               playing. Millisecond 0 is movie time 0: the container's start time when that is positive, else 0 (a scene's
 *               AAC priming before 0 is dropped), so a scene's video pts 0 meets it and a video demuxer that subtracts the
 *               same start time shares the origin.
 *   no audio    a file that opens and has no audio stream (a video-only scene): open() is true, clock().has_audio false; the
 *               video side then runs its own monotonic clock. The same when the output fails: has_audio stays true,
 *               playing goes false and `error` says why (no device, a Bluetooth headset gone: output_lost).
 *   volume      Vinyl's output_volume model of the output the sound goes to: the sound server's sink volume, the sound card's
 *               playback mixer, or a software gain for a card without a mixer (kept per output in ~/.config/vhs/output-levels).
 *   keys        the transport keys of headsets, speakers and keyboards (Vinyl's media-key reader): poll them from the LVGL
 *               thread; the caller acts (the facade never plays or pauses by itself).
 *
 * Threads: the engine thread (decode and output), the media-key reader, the sound-server worker; all calls below may come from
 * any thread (one mutex), none blocks on I/O except open(), which waits up to five seconds for the engine thread's open: the
 * first one maps FFmpeg (+22 MB, a few hundred ms on a Pi Zero 2 W), so call it off the LVGL thread. Everything else only reads
 * what the other threads left (engine snapshot, server scan, mixer read at most every 300 ms). Construct the player when
 * playback is entered and shutdown() it when it is left: it holds three threads and, on a board with a sound server, scans the
 * server every 5 s.
 *
 * Tests build it over fakes with the second constructor (audio_player_deps.hpp): a fake SinkProvider (no sound device in WSL),
 * a fake mixer, the real FFmpeg. LVGL-free.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vhs {

struct AudioClock {
    bool open = false;          /* a file is open (also a video-only one) */
    bool has_audio = false;     /* it has an audio stream */
    bool playing = false;       /* the engine plays (false: paused, ended, no audio, or the output failed) */
    std::int64_t heard_ms = 0;  /* the heard position */
    std::int64_t total_ms = 0;  /* the audio's length (the container's when the stream has none), 0 unknown */
    std::string output_label;   /* "USB Audio", "Evolve 65", "Default output" ("" when not known yet) */
    bool output_lost = false;   /* the output went away while playing (a Bluetooth device switched off, a card pulled): paused */
    std::string error;          /* the open failed, or the output could not be opened or was lost: a sentence for the notice */
    /* beyond the first list: the audio reached its end (heard_ms = total_ms, playing false); play() starts again */
    bool ended = false;
    bool seeking = false;       /* G6: a seek not applied yet (heard_ms is its target, standing still) */
    bool dummy = false;         /* G6: the sound server's null sink ("Dummy Output", auto_null): nothing is heard */
    std::int64_t delay_ms = 0;  /* G6: the output's delay at the last position (the stats line) */
};

/* One row of the output chooser (Settings > Playback output): Vinyl's rows. `key` is the stored output target string. */
struct AudioOutputRow {
    std::string key;     /* "auto", "bt:<MAC>", "pulse:<sink>", "alsa:<device>" */
    std::string label;   /* "Automatic", "Evolve 65", "USB sound card UC02", "HDMI" */
    std::string value;   /* "follows system: Evolve 65", "music quality", "wired", "paired, not connected" */
    bool chosen = false;
    bool dimmed = false; /* paired, not connected; or a choice made earlier that is not there now */
    bool bluetooth = false;
};

struct AudioPlayerDeps;

class AudioPlayer {
public:
    /* The real world: the sound server through libpulse when one runs (Bluetooth, USB, HDMI sinks), else ALSA through
     * libasound, the launcher's status files, the media-key devices. `output_target` is Settings.output, Vinyl's output
     * target string: "" or "auto" = Automatic. FFmpeg is mapped at the first open(). */
    explicit AudioPlayer(std::string output_target);
    /* The same over `deps` (tests). */
    AudioPlayer(std::string output_target, std::unique_ptr<AudioPlayerDeps> deps);
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;

    /* Opens `path` at start_ms (paused, or playing at once when `play`). false with clock().error when the file cannot be
     * opened; true with has_audio false for a video-only file. Waits for the engine thread (see above). */
    bool open(const std::string &path, std::int64_t start_ms, bool play);
    void play();
    void pause();
    void seek_ms(std::int64_t ms);
    /* Lets the file go: the output is closed. */
    void close();
    AudioClock clock() const;

    /* The level of the output as the volume group shows it: the percent of the output's own levels (Vinyl's model), 0 when
     * muted, -1 when not known (no output yet, the server reports none). */
    int volume_percent() const;
    /* One level up (+1) or down (-1) of the output's own steps: "-" at the lowest level mutes, "+" while muted brings the
     * sound back at its level. Nothing when the level is not known. */
    void volume_step(int dir);
    /* The output's label for the volume group ("USB Audio"); "" when not known. */
    std::string volume_label() const;
    /* G6: "was off" after VHS raised a silent output at its first use (the volume line says so), else "" */
    std::string volume_note() const;
    /* beyond the first list: the group can change the level, and the output is muted now */
    bool volume_adjustable() const;
    bool volume_muted() const;

    /* The transport keys (a headset's play / pause and stop, a keyboard's media keys), one press per call. */
    enum class Key { None, PlayPause, Play, Pause, Stop, Next, Prev };
    Key poll_key();

    /* beyond the first list, for Settings > Playback output (M6): the chooser's rows, the choice, and whether Bluetooth audio
     * is not available on this board (no sound server, or the launcher says none). set_output() applies at the next play. */
    std::vector<AudioOutputRow> output_rows() const;
    std::string output_key() const;
    void set_output(const std::string &key);
    bool bluetooth_not_set_up() const;

    /* Ends the threads and closes the output. Idempotent; the destructor does it too. */
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace vhs
