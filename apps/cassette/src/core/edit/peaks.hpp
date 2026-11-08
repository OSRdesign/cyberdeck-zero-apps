/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core: the peak pyramid of a take, for the waveform on the trim page. LVGL-free.
 *
 * Level 0 holds one (min, max) pair per block of 256 frames; every level above halves the resolution (min of the two
 * mins, max of the two maxes) until one block is left. The pyramid is built in ONE streaming pass over the take (bounded
 * memory: the take is read in chunks of at most kReadChunk bytes, the cache file is written with pwrite) and stored next
 * to the take as "<take>.peaks" (versioned header; the take's size, mtime, frame count and format are recorded; a
 * missing, stale, truncated or inconsistent cache is rebuilt; written to ".tmp-pk-<random>" then renamed, so a reader
 * never sees a half cache).
 *
 * Values are on a 16-bit scale: S16 as is, S24 shifted right by 8, S32 by 16.
 * Stereo and more channels: ONE envelope for the take, the min of all channels' mins and the max of all channels' maxes
 * (the louder side wins in each direction).
 * The audio length is the real length of the file (data offset to end of file, whole frames), so the placeholder header
 * of an unrepaired take (arecord) works; a header that declares LESS audio than the file holds is honoured.
 */
#pragma once

#include "fileops.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cassette {

constexpr uint32_t kPeakBlock = 256;            /* frames per level-0 block */
constexpr size_t kPeakReadChunk = 1u << 20;     /* the largest single read of the take while building */

struct PeakPair {
    int16_t min = 0, max = 0;
    bool operator==(const PeakPair &o) const { return min == o.min && max == o.max; }
};

struct PeakBuildOptions {
    std::function<void(uint64_t done, uint64_t total)> progress; /* frames of the take read so far (monotonic) */
    const std::atomic<bool> *cancel = nullptr;
};

class PeakPyramid {
public:
    ~PeakPyramid();
    PeakPyramid(const PeakPyramid &) = delete;
    PeakPyramid &operator=(const PeakPyramid &) = delete;

    /* Opens a valid cache or builds one (and uses it). nullptr with *error set on failure or cancel. */
    static std::unique_ptr<PeakPyramid> open_or_build(FileOps &ops, const std::string &take_path, std::string *error,
                                                      const PeakBuildOptions &opt = PeakBuildOptions());
    /* The cache file name of a take. */
    static std::string cache_path(const std::string &take_path) { return take_path + ".peaks"; }

    uint64_t frames() const { return frames_; }
    uint32_t rate() const { return rate_; }
    bool rebuilt() const { return rebuilt_; } /* this open had to build the cache */
    /* The cache file could not be written (read-only or full folder): the pyramid lives in memory (at most 64 MiB; a longer
     * take then fails to open). Same answers from window(). */
    bool in_memory() const { return cache_fd_ == kMemCacheFd; }
    size_t levels() const { return counts_.size(); }
    uint64_t level_blocks(size_t level) const { return level < counts_.size() ? counts_[level] : 0; }

    /* `bars` (min,max) pairs; bar i covers frames [start + i*fpb, start + (i+1)*fpb), fpb = frames_per_bar >= 1.
     * fpb < 256: exact, read from the take itself (at most 256 frames per bar).
     * fpb >= 256: from the coarsest level whose block (256 << level frames) is not larger than fpb, so a bar is the
     *   min/max over the blocks that overlap it (aligned bars are exact; others may reach up to one block either side).
     * Bars past the end of the take, or an empty take, give (0,0). One pread from the cache (or one from the take). */
    std::vector<PeakPair> window(uint64_t start_frame, uint64_t frames_per_bar, size_t bars);

    /* The level window() uses for fpb >= 256 (exposed for tests). */
    size_t level_for(uint64_t frames_per_bar) const;

private:
    static constexpr int kMemCacheFd = 0x3fffff01;
    PeakPyramid() = default;
    int64_t cache_read(void *buf, size_t n, uint64_t off);
    std::vector<unsigned char> mem_;
    FileOps *ops_ = nullptr;
    int cache_fd_ = -1, take_fd_ = -1;
    uint64_t frames_ = 0, data_offset_ = 0;
    uint32_t rate_ = 0, block_align_ = 0, channels_ = 0, bits_ = 0;
    std::vector<uint64_t> counts_, offsets_; /* per level: blocks, byte offset in the cache file */
    bool rebuilt_ = false;
    friend struct PeakBuilder;
};

}  // namespace cassette
