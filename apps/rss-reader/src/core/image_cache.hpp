/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: the downloaded images of the articles, on disk, apart from the saved articles (which keep their own
 * copies: store.hpp). One file per image URL, "<hash16(url)><ext>" (ext from the content type: .jpg .png .gif .webp
 * .bmp .img), the bytes exactly as fetched, in ~/.cache/rss-reader/images (or $RSSREADER_CACHE, or
 * $XDG_CACHE_HOME/rss-reader/images; injectable for the tests).
 *
 * Bounded: after each put the least recently used files go (a get sets a file's time to now) until the cache is under
 * 90 % of its cap (50 MB by default, independent of the saved articles' cap). Thread safe (one mutex): the reader's
 * workers put while the UI asks for the usage. A missing or unwritable folder only means nothing is cached.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace rssreader {

constexpr std::uint64_t kImageCacheCapBytes = 50u * 1024 * 1024;

class ImageCache {
public:
    explicit ImageCache(std::string dir, std::uint64_t cap = kImageCacheCapBytes);
    static std::string default_dir();
    const std::string &dir() const { return dir_; }

    /* The bytes stored for url (true), and their content type from the file's extension. Marks the file as used now. */
    bool get(const std::string &url, std::string &bytes, std::string *content_type = nullptr);
    bool contains(const std::string &url);
    /* Stores the bytes (empty or over kMaxImageBytes: refused), then evicts the least recently used files over the cap. */
    bool put(const std::string &url, const std::string &bytes, const std::string &content_type);
    bool remove(const std::string &url);
    std::uint64_t usage();
    std::size_t count();
    /* Removes every file of the cache. */
    bool clear();
    void set_cap(std::uint64_t bytes);
    std::uint64_t cap() const { return cap_; }

    /* The file name for url (no extension) and the extension of a content type (the tests). */
    static std::string key(const std::string &url);

private:
    bool find_locked(const std::string &url, std::string &path);
    void evict_locked(const std::string &keep);

    std::string dir_;
    std::uint64_t cap_;
    std::mutex mu_;
};

} // namespace rssreader
