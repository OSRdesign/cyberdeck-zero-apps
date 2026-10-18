/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: orchestration of fetch + parse + store, with the HTTP client and the clock injected (the tests use a
 * fake client; the app uses CurlClient). Everything is synchronous: run it on a worker thread (apps/_common/async_run
 * is for commands whose lines go to the UI; a refresh is a library call and reports through its return values and the
 * optional progress callback, which is invoked on the calling thread).
 */
#pragma once

#include "catalog.hpp"
#include "feed.hpp"
#include "fetch.hpp"
#include "store.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace rssreader {

using NowFn = std::function<std::int64_t()>;     /* epoch seconds */

std::string custom_feed_id(const std::string &url);                                  /* "c-" + hash16(url) */
Subscription make_subscription(const CatalogFeed &f, std::int64_t now);              /* a pinned catalog feed */

struct RefreshResult {
    enum class Status { Updated, NotModified, Failed };
    std::string feed_id;
    Status status = Status::Failed;
    int new_articles = 0;          /* articles that were not in the cache before */
    int total = 0;                 /* articles in the cache after */
    FetchError error = FetchError::None;
    std::string message;           /* for a user: "network error: Could not resolve host", "HTTP 503", "not a feed" ... */
};

struct FoundFeed {
    std::string title;
    std::string url;               /* the feed's own URL (after redirects) */
    std::string site_url;
    std::string description;
    std::string lang;
    std::string format;
    int article_count = 0;
    Feed feed;                     /* what was parsed, so pinning needs no second fetch */
    std::string etag, last_modified;
};

struct AddResult {
    enum class Status { Ok, BadInput, FetchFailed, NoFeedFound };
    Status status = Status::BadInput;
    FoundFeed found;               /* Ok only */
    FetchError error = FetchError::None;
    std::string message;
};

class Refresher {
public:
    Refresher(Store &store, HttpClient &client, NowFn now) : store_(store), client_(client), now_(std::move(now)) {}

    /* Conditional GET (ETag / Last-Modified, only when a cache exists), parse, merge into the cache, record the outcome.
     * A failure keeps the cached articles and sets the subscription's last_error. */
    RefreshResult refresh_feed(const Subscription &sub, const std::atomic<bool> *cancel = nullptr);

    /* One after the other, in the given order; `progress(done, total, result)` after each; stops early when cancelled. */
    std::vector<RefreshResult> refresh_all(const std::vector<Subscription> &subs, const std::atomic<bool> *cancel = nullptr,
                                           const std::function<void(std::size_t, std::size_t, const RefreshResult &)> &progress = nullptr);

    /* Accepts a feed URL or a site address ("example.com", "https://example.com/blog"): fetches it; if it is a feed,
     * done; if it is HTML, discovers the candidates and tries them in order. Does NOT pin: show the result to the user,
     * then call pin_custom_feed(). */
    AddResult add_custom_feed(const std::string &url_or_site, const std::atomic<bool> *cancel = nullptr);
    /* Subscribes to a found feed and stores its articles (no second fetch). Duplicate if the URL is already pinned. */
    StoreError pin_custom_feed(const FoundFeed &found, const std::string &group = "");

private:
    bool try_feed(const std::string &url, const std::atomic<bool> *cancel, bool need_articles, FoundFeed &out, AddResult &failure, std::string *html_body,
                  std::string *html_url);
    Store &store_;
    HttpClient &client_;
    NowFn now_;
};

/* Merge of a freshly parsed feed into the cached articles of the same feed (public for the tests): the new articles
 * replace their cached copies but keep the read flag (and the date when the feed has none); cached articles the feed no
 * longer lists stay; the result is newest first. new_count = articles not in `cached`. */
std::vector<StoredArticle> merge_articles(const std::vector<StoredArticle> &cached, const std::vector<Article> &fresh, int &new_count);

} // namespace rssreader
