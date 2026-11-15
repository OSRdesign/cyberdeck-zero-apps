/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: one recording session (brief 3.2, section 6). The capture thread pushes the frames streamed after the warm-up
 * into a bounded ring (2 s of frames on boards with 512 MB or less, 4 s elsewhere; a full ring drops the frame and
 * counts it: capture never blocks); the audio thread pushes the camera microphone's S16 periods, its gaps (an overrun:
 * silence of the lost length) and its clock pairs into a queue (a short lock, never held across I/O). The writer thread
 * owns the files: it names the scene (the shared counter), writes the journal, maps the timestamps onto the audio
 * clock (avsync.hpp), encodes AAC, muxes MKV (mkv_writer.hpp), interleaves (a video frame waits until the audio has
 * reached it, unless the audio stalls), flushes and fdatasyncs every 5 s, asks the disk guard every second, cuts parts
 * at split_mib (_p2, _p3 ...: each part starts at 0; audio before the cut stays in the old part), and finishes the file
 * (trailer, fdatasync, journal "closed") at every stop. LVGL-free.
 */
#pragma once

#include "audio/frame_sink.hpp"
#include "avsync.hpp"
#include "disk_guard.hpp"
#include "mkv_writer.hpp"

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

namespace vhs {

class FileOps;

enum class RecStop { None, User, DiskFull, CameraLost, Released, Error };
const char *rec_stop_name(RecStop s);

struct RecorderConfig {
    const AvLibs *av = nullptr;
    FileOps *ops = nullptr;
    std::string folder;
    std::string prefix = "scene";   /* Settings > Name prefix */
    std::string when;               /* "YYYYMMDD-HHMMSS" or "nodate" (naming.hpp) */
    std::string stem;               /* tests: a fixed stem instead of prefix_NNN_when */
    VideoFormat video;
    int audio_rate = 0, audio_channels = 0; /* 0: video only */
    std::uint64_t split_bytes = 4096ull << 20; /* 0: Split Off */
    std::uint64_t split_margin = 16ull << 20;
    std::uint64_t min_free_bytes = 500ull << 20;
    int ring_seconds = 4;
    int flush_ms = 5000, guard_ms = 1000;
    int audio_wait_ms = 1500;       /* the first frame waits this long for the audio clock */
    int audio_stall_ms = 600;       /* no audio for this long: video is written without waiting */
    std::function<std::uint64_t()> free_space; /* null: statvfs of the folder */
    std::int64_t started_unix = 0;  /* for the journal */
};

struct RecorderStatus {
    bool running = false;   /* the writer thread runs */
    bool started = false;   /* the first frame is written (the REC timer runs from it) */
    bool finished = false;  /* the writer ended (by a stop, the disk guard or an error): join() it */
    std::int64_t written_ms = 0; /* the last frame's end on the scene's timeline */
    std::uint64_t bytes = 0;     /* all parts */
    std::uint64_t frames = 0, ring_drops = 0, audio_drops = 0, gap_frames = 0;
    int parts = 0;
    std::vector<std::string> files;
    std::string stem;
    bool audio = false;
    RecStop stop = RecStop::None;
    std::string error;
    std::uint64_t free_bytes = 0;
    std::int64_t seconds_left = -1;
};

class Recorder : public FrameSink {
public:
    explicit Recorder(RecorderConfig cfg);
    ~Recorder() override;
    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;

    /* Starts the writer thread (it opens the files). */
    void start();
    /* The capture thread: one frame (bytes copied), its driver timestamp (us, CLOCK_MONOTONIC). Never blocks. */
    void push_video(const std::uint8_t *data, std::size_t n, std::int64_t ts_us);
    /* The audio thread (FrameSink): interleaved S16 frames; the frames accepted. */
    std::size_t write(const void *frames, std::size_t nframes) override;
    void audio_gap(std::uint64_t frames);
    void audio_clock(std::int64_t mono_ns, std::uint64_t position);
    /* The microphone is gone: video goes on alone. */
    void audio_end();
    /* Asks the writer to finish (non-blocking); the first reason wins. */
    void request_stop(RecStop why);
    /* Waits for the writer (after request_stop or when finished). */
    void join();
    RecorderStatus status() const;

private:
    struct VFrame {
        std::vector<std::uint8_t> data;
        std::int64_t ts_us = 0;
    };
    struct AChunk {
        enum Kind { Samples, Gap, Clock, End } kind = Samples;
        std::vector<std::int16_t> samples;
        std::uint64_t frames = 0;   /* Gap: frames lost; Clock: the position */
        std::int64_t ns = 0;        /* Clock */
    };
    void run();
    bool open_part(int part, std::int64_t offset_ms);
    bool close_part();
    void feed_audio(const std::int16_t *s, std::size_t frames);
    void write_audio_packets(std::int64_t before_ms, bool all);
    void fail(const std::string &why);
    void sync_file();

    RecorderConfig cfg_;
    std::thread th_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<VFrame> ring_;
    std::size_t ring_cap_ = 120;
    std::deque<AChunk> audio_;
    std::size_t audio_cap_frames_ = 0, audio_queued_frames_ = 0;
    std::atomic<bool> stop_req_{false};
    std::atomic<bool> audio_on_{false};
    std::atomic<std::int64_t> last_audio_ms_{0};
    RecStop reason_ = RecStop::None; /* m_ */
    RecorderStatus st_;              /* m_ */

    /* the writer thread's */
    AvSync sync_;
    std::unique_ptr<AacEncoder> enc_;
    std::unique_ptr<MkvMuxer> mux_;
    std::deque<EncodedAudio> apkts_;
    std::deque<std::pair<std::int64_t, std::vector<std::int16_t>>> pre_; /* audio before t0: (position, samples) */
    std::uint64_t audio_pos_ = 0;    /* the capture position of the next sample */
    std::int64_t part_offset_ms_ = 0;
    std::uint64_t part_bytes_ = 0, bytes_done_ = 0;
    int part_ = 0;
    int fd_ = -1;
    std::string part_path_;
    std::int64_t last_pts_ = -1;
    std::vector<std::string> files_;
    std::string stem_;
};

} // namespace vhs
