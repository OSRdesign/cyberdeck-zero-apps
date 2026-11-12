/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the background reader (see tag_jobs.hpp).
 */
#include "tag_jobs.hpp"

#include <cstdio>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace vinyl {

TagJobs::TagJobs() { th_ = std::thread([this] { run(); }); }

TagJobs::~TagJobs() { stop(); }

void TagJobs::stop() {
    {
        std::lock_guard<std::mutex> g(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void TagJobs::want_tags(const std::vector<std::string> &paths) {
    {
        std::lock_guard<std::mutex> g(mu_);
        for (const std::string &p : paths) {
            if (p.empty() || cache_.count(p) || queued_.count(p)) continue;
            queue_.push_back(p);
            queued_.insert(p);
        }
    }
    cv_.notify_all();
}

void TagJobs::want_cover(const std::string &path, int side) {
    {
        std::lock_guard<std::mutex> g(mu_);
        cover_path_ = path;
        cover_side_ = side;
        cover_wanted_ = true;
    }
    cv_.notify_all();
}

bool TagJobs::tags(const std::string &path, TrackTags &out) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = cache_.find(path);
    if (it == cache_.end()) return false;
    out = it->second;
    return true;
}

bool TagJobs::take_cover(CoverResult &out) {
    std::lock_guard<std::mutex> g(mu_);
    if (!cover_ready_) return false;
    out = std::move(cover_);
    cover_ready_ = false;
    return true;
}

void TagJobs::run() {
    /* below the UI and the playback thread: the decoder must never wait behind a tag scan */
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
    for (;;) {
        std::string path, cover;
        int side = 0;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return quit_ || cover_wanted_ || !queue_.empty(); });
            if (quit_) return;
            if (cover_wanted_) {
                cover = cover_path_;
                side = cover_side_;
                cover_wanted_ = false;
            } else {
                path = queue_.front();
                queue_.pop_front();
            }
        }
        if (!cover.empty() || side > 0) {
            CoverResult r;
            r.path = cover;
            TrackTags t;
            Picture pic;
            if (!cover.empty() && read_tags(cover, t, &pic, 12u << 20) && !pic.data.empty()) {
                auto img = std::make_shared<PixelImage>();
                cover_size(pic, r.pic_w, r.pic_h);
                r.status = decode_cover(pic, side, *img);
                if (r.status == CoverStatus::Ok) r.image = img;
                else
                    std::fprintf(stderr, "[vinyl] cover of %s not shown: %s (%dx%d)\n", cover.c_str(),
                                 cover_status_name(r.status), r.pic_w, r.pic_h);
            }
            std::lock_guard<std::mutex> g(mu_);
            if (!cover_wanted_) { /* else a newer request came meanwhile: this one is stale */
                cover_ = std::move(r);
                cover_ready_ = true;
            }
            if (!cover.empty() && t.ok && !cache_.count(cover)) {
                t.has_picture = t.has_picture || !pic.data.empty();
                cache_[cover] = t;
                gen_.fetch_add(1);
            }
            continue;
        }
        TrackTags t;
        read_tags(path, t, nullptr);
        std::lock_guard<std::mutex> g(mu_);
        queued_.erase(path);
        if (cache_.size() >= kMaxCached) cache_.clear();
        cache_[path] = t;
        gen_.fetch_add(1);
    }
}

}  // namespace vinyl
