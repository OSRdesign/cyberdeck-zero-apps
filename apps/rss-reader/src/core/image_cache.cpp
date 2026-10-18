/*
 * SPDX-License-Identifier: MIT
 */
#include "image_cache.hpp"

#include "fetch.hpp"
#include "fsutil.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <tuple>
#include <vector>

namespace rssreader {

namespace {

const char *const kExts[] = {".jpg", ".png", ".gif", ".webp", ".bmp", ".img"};

std::string type_of_ext(const std::string &path)
{
    if (ends_with(path, ".jpg")) return "image/jpeg";
    if (ends_with(path, ".png")) return "image/png";
    if (ends_with(path, ".gif")) return "image/gif";
    if (ends_with(path, ".webp")) return "image/webp";
    if (ends_with(path, ".bmp")) return "image/bmp";
    return "application/octet-stream";
}

void touch(const std::string &path)
{
    struct timespec ts[2];
    ts[0].tv_sec = 0;
    ts[0].tv_nsec = UTIME_NOW;
    ts[1].tv_sec = 0;
    ts[1].tv_nsec = UTIME_NOW;
    ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
}

} // namespace

ImageCache::ImageCache(std::string dir, std::uint64_t cap) : dir_(std::move(dir)), cap_(cap)
{
    while (dir_.size() > 1 && dir_.back() == '/') dir_.pop_back();
}

std::string ImageCache::default_dir()
{
    if (const char *d = std::getenv("RSSREADER_CACHE"); d && *d) return d;
    if (const char *x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/rss-reader/images";
    const char *h = std::getenv("HOME");
    return std::string(h && *h ? h : "/tmp") + "/.cache/rss-reader/images";
}

std::string ImageCache::key(const std::string &url) { return hash16(url); }

bool ImageCache::find_locked(const std::string &url, std::string &path)
{
    const std::string base = dir_ + "/" + key(url);
    for (const char *e : kExts) {
        if (file_size(base + e) > 0) {
            path = base + e;
            return true;
        }
    }
    return false;
}

bool ImageCache::get(const std::string &url, std::string &bytes, std::string *content_type)
{
    std::lock_guard<std::mutex> g(mu_);
    std::string path;
    if (!find_locked(url, path) || !read_file(path, bytes, kMaxImageBytes + 1) || bytes.empty() || bytes.size() > kMaxImageBytes) {
        bytes.clear();
        return false;
    }
    touch(path);
    if (content_type) *content_type = type_of_ext(path);
    return true;
}

bool ImageCache::contains(const std::string &url)
{
    std::lock_guard<std::mutex> g(mu_);
    std::string path;
    return find_locked(url, path);
}

bool ImageCache::put(const std::string &url, const std::string &bytes, const std::string &content_type)
{
    if (bytes.empty() || bytes.size() > kMaxImageBytes || url.empty()) return false;
    std::lock_guard<std::mutex> g(mu_);
    if (!make_dirs(dir_)) return false;
    std::string old;
    if (find_locked(url, old)) remove_tree(old);          // another type than before: one file per URL
    const std::string path = dir_ + "/" + key(url) + image_extension(content_type);
    if (!write_file_atomic(path, bytes)) return false;
    evict_locked(path);
    return true;
}

bool ImageCache::remove(const std::string &url)
{
    std::lock_guard<std::mutex> g(mu_);
    std::string path;
    return find_locked(url, path) && remove_tree(path);
}

/* The least recently used files go until the cache is under 90 % of the cap; the file just written stays. */
void ImageCache::evict_locked(const std::string &keep)
{
    struct Entry {
        std::int64_t sec, nsec;
        std::string path;
        std::uint64_t size;
    };
    std::vector<Entry> files;
    std::uint64_t total = 0;
    for (const std::string &n : list_dir(dir_)) {
        const std::string p = dir_ + "/" + n;
        struct stat st;
        if (::lstat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (ends_with(n, ".tmp")) {                     // a write that never finished
            remove_tree(p);
            continue;
        }
        files.push_back({static_cast<std::int64_t>(st.st_mtim.tv_sec), static_cast<std::int64_t>(st.st_mtim.tv_nsec), p,
                         static_cast<std::uint64_t>(st.st_size)});
        total += static_cast<std::uint64_t>(st.st_size);
    }
    if (total <= cap_) return;
    const std::uint64_t target = cap_ / 10 * 9;
    std::sort(files.begin(), files.end(), [](const Entry &a, const Entry &b) { return std::tie(a.sec, a.nsec, a.path) < std::tie(b.sec, b.nsec, b.path); });
    for (const Entry &e : files) {
        if (total <= target) break;
        if (e.path == keep) continue;
        if (remove_tree(e.path)) total -= e.size;
    }
}

std::uint64_t ImageCache::usage()
{
    std::lock_guard<std::mutex> g(mu_);
    return tree_size(dir_);
}

std::size_t ImageCache::count()
{
    std::lock_guard<std::mutex> g(mu_);
    std::size_t n = 0;
    for (const std::string &f : list_dir(dir_))
        if (!ends_with(f, ".tmp")) ++n;
    return n;
}

bool ImageCache::clear()
{
    std::lock_guard<std::mutex> g(mu_);
    bool ok = true;
    for (const std::string &n : list_dir(dir_))
        if (!remove_tree(dir_ + "/" + n)) ok = false;
    return ok;
}

void ImageCache::set_cap(std::uint64_t bytes)
{
    std::lock_guard<std::mutex> g(mu_);
    cap_ = bytes;
    evict_locked("");
}

} // namespace rssreader
