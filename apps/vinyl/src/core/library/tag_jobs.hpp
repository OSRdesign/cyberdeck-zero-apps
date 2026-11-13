/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the background reader of tags and covers. One thread at a lower priority (nice +10) reads the tags of
 * the files the browser and the queue show (titles, durations) and the cover of the track playing, so the UI thread
 * never waits for a disk (a cold SD card). The cover request always goes first; a newer cover request replaces an older
 * one. The cache keeps at most kMaxCached files. LVGL-free.
 */
#pragma once

#include "../art/cover.hpp"
#include "../tags/tag_reader.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vinyl {

class TagJobs {
public:
    static constexpr size_t kMaxCached = 20000;
    TagJobs();
    ~TagJobs();
    TagJobs(const TagJobs &) = delete;
    TagJobs &operator=(const TagJobs &) = delete;

    /* Queues the files not read yet (in this order, after the ones already waiting). */
    void want_tags(const std::vector<std::string> &paths);
    /* The cover of `path` at side x side; replaces a request not started yet. */
    void want_cover(const std::string &path, int side);
    /* The tags when read (false: not yet). */
    bool tags(const std::string &path, TrackTags &out) const;
    /* Changes each time tags arrive. */
    uint64_t gen() const { return gen_.load(); }
    struct CoverResult {
        std::string path;
        CoverStatus status = CoverStatus::None;
        std::shared_ptr<const PixelImage> image;
        int pic_w = 0, pic_h = 0;
    };
    bool take_cover(CoverResult &out);
    /* Stops the thread (the destructor does). */
    void stop();

private:
    void run();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::set<std::string> queued_;
    std::unordered_map<std::string, TrackTags> cache_;
    std::string cover_path_;
    int cover_side_ = 0;
    bool cover_wanted_ = false;
    bool cover_ready_ = false;
    CoverResult cover_;
    bool quit_ = false;
    std::atomic<uint64_t> gen_{0};
    std::thread th_;
};

}  // namespace vinyl
