/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette playback: a streaming WAV reader and the Player (play, pause, seek, the 5x tape-cue scan, the 5 s jump).
 * LVGL-free; files through the FileOps seam, sound through the PlaybackSink seam.
 *
 * Threads. In Threaded mode the Player owns one playback thread: it alone reads the file and touches the sink; every
 * command (open, play, pause, seek, scan ...) is queued to it and the caller waits until it has been applied (at most
 * kCommandWaitMs; the thread blocks at most one period inside a write). Queries (state, position, events) read a snapshot
 * under a mutex and never wait for the device. In Manual mode (tests) there is no thread: commands apply at once and
 * step() plays one block.
 *
 * Positions are source frames. position() is the frame being heard: the next frame to send minus what is still queued in
 * the device. Pause, seek, scan changes and stop drop the queued audio and rewind by it, so pause and resume lose and
 * repeat nothing that was heard.
 *
 * Scan (tape cue). Forward: every 5 source frames are averaged into one output frame (a box filter, so the cue sounds
 * like fast tape with raised pitch, not a skipping click) and the position moves 5 frames per output frame. Reverse: blocks
 * are read backwards and the groups are emitted last-first with the same averaging. Only the last group before an end may
 * hold fewer than 5 frames (it is averaged over what it holds).
 * Reaching an end (decided for T5, the plan left it open):
 *   - the END while scanning forward: the take is over, exactly as when normal play reaches the end: the queued cue plays
 *     out, the state becomes Stopped with position() == length(), the event ReachedEnd is posted (the app feeds the deck
 *     TAKE_ENDED for both TakeEnded and ReachedEnd). end_scan() afterwards is a no-op.
 *   - the START while scanning reverse: the queued cue plays out and the Player holds at frame 0, still Scanning (the key
 *     is still down), posting ReachedStart once. end_scan() then returns to the state before the scan from frame 0: Playing
 *     plays the take from its start (a tape rewound to the start and released), Paused stays paused at 0.
 * Stop rewinds to 0; play() from Stopped at the end (after the take ended) starts again from 0.
 */
#pragma once

#include "fileops.hpp"
#include "frame_sink.hpp"
#include "sink.hpp"
#include "wav.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cassette {

/* Reads the audio of a WAV file in blocks (never the whole file) through FileOps. The playable length is the declared data
 * size when it fits the file; a placeholder or oversized header (a take whose header was never updated: a crash, a power
 * cut, or a take still open) uses the real file length instead. A partial last frame is ignored. */
class WavReader {
public:
    explicit WavReader(FileOps &ops) : ops_(ops) {}
    ~WavReader() { close(); }
    WavReader(const WavReader &) = delete;
    WavReader &operator=(const WavReader &) = delete;

    int open(const std::string &path); /* 0, or a negative errno (-EINVAL: not a PCM WAV we can play) */
    void close();
    bool is_open() const { return fd_ >= 0; }
    const AudioFormat &format() const { return fmt_; }
    uint32_t block_align() const { return align_; }
    uint64_t frames() const { return frames_; }
    bool used_file_length() const { return used_file_length_; } /* the header's sizes were not trusted */
    /* Reads up to n frames from `frame` into buf (n * block_align bytes): frames read (0 at the end) or a negative errno. */
    long read(uint64_t frame, size_t n, void *buf);

private:
    FileOps &ops_;
    int fd_ = -1;
    AudioFormat fmt_;
    uint32_t align_ = 0;
    uint64_t data_offset_ = 0, frames_ = 0;
    bool used_file_length_ = false;
};

enum class PlayerState { Stopped, Playing, Paused, Scanning };
enum class Direction { Reverse = -1, Forward = 1 };
enum class PlayerEvent { TakeEnded, ReachedStart, ReachedEnd, DeviceLost };

const char *player_state_name(PlayerState s);
const char *player_event_name(PlayerEvent e);

constexpr unsigned kScanSpeed = 5;
constexpr unsigned kJumpSeconds = 5;
constexpr int kCommandWaitMs = 1000;

class Player {
public:
    enum class Mode { Threaded, Manual };
    Player(FileOps &ops, PlaybackSink &sink, Mode mode = Mode::Threaded);
    ~Player(); /* stops, closes the sink, joins the thread */
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    /* Every command returns 0 or a negative errno (-ETIMEDOUT when the thread did not answer in time: it still applies). */
    int open(const std::string &path); /* stops what plays; Stopped at 0 */
    int close();
    int play();   /* Stopped: from position() (0 after stop; from 0 again when at the end); Paused: resume; Scanning: end it */
    int pause();  /* Playing or Scanning -> Paused at the frame heard */
    int resume(); /* Paused (or Stopped, a cued take) -> Playing */
    int stop();   /* -> Stopped at 0; the sink is closed */
    int seek(uint64_t frame); /* clamped to [0, length]; keeps the state */
    /* From Playing, Paused or Stopped (a cued take: it counts as Paused); while scanning, changes the direction. */
    int scan(Direction dir, unsigned speed = kScanSpeed);
    int end_scan(); /* back to the state before scan(): Playing or Paused */
    int jump(Direction dir, unsigned seconds = kJumpSeconds); /* seek by +-seconds, clamped */

    /* Manual mode: applies queued commands and plays one block; true when something was done. */
    bool step();

    PlayerState state() const;
    PlayerState state_before_scan() const;
    Direction scan_direction() const;
    bool at_start_hold() const; /* scanning reverse, held at frame 0 */
    uint64_t position() const;
    uint64_t length() const;
    double position_s() const;
    double length_s() const;
    AudioFormat format() const;
    SinkFormat sink_format() const;
    unsigned underruns() const;
    bool poll_event(PlayerEvent &e);

private:
    struct Cmd;
    int submit(std::shared_ptr<Cmd> c);
    void process_commands();
    int apply(Cmd &c);
    bool pump();
    void run();
    bool running() const { return st_ == PlayerState::Playing || (st_ == PlayerState::Scanning && !held_); }
    int ensure_sink();
    void close_sink();
    void drop_and_rewind();
    void do_stop(uint64_t pos);
    bool write_out(size_t frames, size_t *written);
    void device_lost();
    void post(PlayerEvent e);
    void publish();

    FileOps &ops_;
    PlaybackSink &sink_;
    const Mode mode_;

    /* owned by the playback thread (or by the caller in Manual mode) */
    WavReader reader_;
    PlayerState st_ = PlayerState::Stopped, prev_ = PlayerState::Paused;
    Direction dir_ = Direction::Forward;
    unsigned speed_ = kScanSpeed;
    bool held_ = false;
    bool sink_open_ = false;
    SinkFormat sfmt_;
    uint64_t pos_ = 0, len_ = 0;
    unsigned underruns_ = 0;
    std::vector<unsigned char> in_, out_;
    std::vector<int32_t> tmp_;

    /* shared */
    mutable std::mutex mu_;
    std::condition_variable cv_, done_cv_;
    std::deque<std::shared_ptr<Cmd>> queue_;
    std::deque<PlayerEvent> events_;
    bool quit_ = false;
    struct Snap {
        PlayerState st = PlayerState::Stopped, prev = PlayerState::Paused;
        Direction dir = Direction::Forward;
        bool held = false;
        uint64_t heard = 0, len = 0;
        AudioFormat fmt;
        SinkFormat sfmt;
        unsigned underruns = 0;
    } snap_;
    std::thread th_;
};

} // namespace cassette
