/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core: the writer. It drains the ring in blocks into the take file (BWF header first), fdatasyncs on a
 * timer and rewrites the header sizes after each sync, checks free space, splits at 4 GiB (or switches to RF64),
 * pauses and resumes, and keeps the journal. All file calls and the clock go through FileOps and Clock.
 *
 * Compressed takes (FLAC, Ogg Opus; phase B of the formats, docs/FORMATS.md): with WriterConfig::make_encoder set, each
 * part gets a fresh encoder writing through a FileByteSink on the writer's own descriptor (pwrite through FileOps). The
 * cadence is the WAV one: blocks from the ring go to Encoder::write(); at each sync Encoder::flush() pushes out what the
 * library lets go and then the writer calls fdatasync, so after a sync the file holds whole FLAC frames / Ogg pages up
 * to the encoder's frames_out (the frames counted as synced); stop and a split end the part with Encoder::finish()
 * (FLAC rewrites STREAMINFO, Opus writes its end-of-stream page). No header patches, no RF64; with auto_split a part
 * ends before its file could pass split_bytes (checked before each block: see split_slack) and the next part is a new
 * stream with a new encoder. After a write failure the file is mended in place at once
 * (the cut frame or page removed, STREAMINFO / end-of-stream set: codec/stream_scan.hpp), as the WAV header is patched.
 * The encoder runs on the writer thread: the capture thread only ever touches the ring.
 *
 * It runs either on its own thread (start_thread) or step by step (pump, for the tests on a fake clock).
 * Stopping: stop the producer first, then stop(): the writer writes what is still in the ring and finishes the file.
 * LVGL-free.
 */
#pragma once

#include "codec/byte_sink.hpp"
#include "codec/encoder.hpp"
#include "fileops.hpp"
#include "journal.hpp"
#include "ring.hpp"
#include "wav.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cassette {

enum class WriterState { Idle, Recording, Paused, Stopped, Failed };
enum class StopReason { None, User, LowSpace, IoError };

struct WriterConfig {
    AudioFormat fmt;
    std::string take;                              /* the take name (journal file name) */
    std::string recordings_dir;                    /* the journal lives in <recordings_dir>/.journal; empty: no journal */
    std::function<std::string(int)> namer;         /* full path of part 0, 1, 2 ... (the naming scheme is the caller's) */
    BwfInfo bwf;
    std::string device_name, device_id;
    uint32_t sync_interval_s = 10;                 /* clamped to 5..30 unless sync_interval_ms is set */
    uint32_t sync_interval_ms = 0;                 /* when not 0, overrides sync_interval_s (tests, crash tests) */
    size_t block_bytes = 128 * 1024;               /* 64..256 KB */
    uint32_t flush_ms = 1000;                      /* a block smaller than block_bytes is written after this long */
    bool auto_split = true;                        /* split into parts at split_bytes; false: RF64 for one big file */
    uint64_t split_bytes = 0xFFFFFFFFull - (1u << 20); /* largest file (header and audio) of a part */
    uint64_t riff_limit = kRiffMax;                /* RIFF size above which the file switches to RF64 (tests lower it) */
    uint64_t min_free_bytes = 200ull << 20;        /* stop cleanly when free space falls under this */
    uint32_t space_check_ms = 1000;
    const std::atomic<uint64_t> *lost_frames = nullptr; /* the caller's running estimate of audio lost to capture overruns: kept in the journal */
    std::function<void(uint64_t)> on_sync;         /* called after each successful fdatasync with the total synced frames */

    /* Compressed takes: a fresh encoder per part (0, 1, ...), for the capture format fmt. Null: WAV (BWF + PCM). */
    std::function<std::unique_ptr<Encoder>(int part)> make_encoder;
    std::function<Tags(int part)> tags;            /* the Vorbis comments of each part; null: none */
    uint64_t split_slack = 256 * 1024;             /* encoder parts: a part ends when its size + the PCM size of the frames
                                                      the encoder holds and of the next block + this would pass split_bytes
                                                      (it covers frame and page headers; compressed audio is never larger
                                                      than its PCM by more) */
    /* the journal's "codec" record (journal.hpp), used with make_encoder */
    std::string codec, codec_preset, encoder_name;
    int bitrate_bps = 0;
    bool force_mono = false;
};

struct WriterStatus {
    WriterState state = WriterState::Idle;
    StopReason reason = StopReason::None;
    std::string reason_text;      /* for the screen: why it stopped, empty while recording */
    int error = 0;                /* errno of an I/O failure */
    uint64_t frames_written = 0;  /* to files, all parts (encoder takes: given to the encoders) */
    uint64_t frames_synced = 0;   /* durable at the last sync (encoder takes: whole frames / pages in the file) */
    uint64_t bytes_out = 0;       /* encoder takes: compressed bytes of all parts, headers included; WAV: 0 */
    uint64_t paused_frames = 0;
    uint64_t dropped_frames = 0;  /* the ring's count */
    int part = 0;                 /* the part being written */
    uint64_t syncs = 0;
    uint64_t blocks = 0;
    std::string path;             /* the part being written */
};

class Writer {
public:
    Writer(Ring &ring, FileOps &ops, Clock &clk, WriterConfig cfg);
    ~Writer();

    /* Opens part 0, writes the (empty, valid) header and the journal. 0 or -errno (-ENOSPC: below the reserve). */
    int start();
    /* One iteration of the writer loop. Returns false once the writer is stopped or failed. */
    bool pump(bool *busy = nullptr);
    void start_thread();
    /* Finishes the take: the rest of the ring, a last sync, the header sizes, the journal. Joins the thread. */
    void stop();
    /* Asks the thread to finish (non-blocking); stop() joins. */
    void request_stop() { stop_req_.store(true); }
    /* Frames offered before the call are written; frames after it are discarded until resume (ring position exact). */
    void pause();
    void resume();

    WriterStatus status() const;

private:
    struct Cmd {
        uint64_t pos;
        bool pause;
    };
    bool drain_step(bool force);
    bool write_frames(const unsigned char *p, size_t nframes);
    bool encode_frames(const unsigned char *p, size_t nframes);
    int open_part(int part);
    int open_encoder(int fd, int part);
    int finalize_part();
    int apply_patches(const PatchSet &ps);
    int do_sync();
    int switch_rf64();
    bool rotate();
    void fail(int err);
    void finish(StopReason r);
    void abandon_file();
    int journal_update(const std::string &state, const std::string &reason);
    void publish();

    Ring &ring_;
    FileOps &ops_;
    Clock &clk_;
    WriterConfig cfg_;
    uint32_t ba_ = 0;
    size_t fb_ = 0;
    int fd_ = -1;
    int part_ = 0;
    std::string path_;
    uint64_t part_data_ = 0;   /* audio bytes written to this part */
    uint64_t total_bytes_ = 0; /* audio bytes written, all parts */
    uint64_t unsynced_ = 0;
    uint64_t synced_frames_ = 0;
    uint64_t paused_frames_ = 0;
    uint64_t syncs_ = 0, blocks_ = 0;
    bool rf64_ = false;
    bool paused_ = false;
    bool finishing_ = false;
    uint64_t last_sync_ms_ = 0, last_write_ms_ = 0, last_space_ms_ = 0;
    uint32_t sync_ms_ = 10000;
    JournalEntry je_;
    std::vector<unsigned char> scratch_;
    /* encoder takes (the sink is declared first so that it outlives the encoder that points at it) */
    std::unique_ptr<FileByteSink> sink_;
    std::unique_ptr<Encoder> enc_;
    uint64_t done_frames_ = 0;  /* frames of the parts already finished */
    uint64_t part_frames_ = 0;  /* frames given to this part's encoder */
    uint64_t done_bytes_ = 0;   /* compressed bytes of the finished parts */

    WriterState state_ = WriterState::Idle;
    StopReason reason_ = StopReason::None;
    std::string reason_text_;
    int error_ = 0;

    mutable std::mutex mtx_; /* guards cmds_ and the published status */
    std::deque<Cmd> cmds_;
    WriterStatus pub_;
    std::atomic<bool> stop_req_{false};
    std::thread thread_;
};

}  // namespace cassette
