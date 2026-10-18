/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: persistence under one root folder (default ~/.local/share/rss-reader, or $RSSREADER_DATA, or
 * $XDG_DATA_HOME/rss-reader; injectable for the tests):
 *
 *   subscriptions.json   {"version":1,"feeds":[{id,name,url,site,group,lang,custom,etag,last_modified,last_fetch,
 *                        last_attempt,last_error,added,unread,total,newest}]}  the favourites, in order (the catalog ones and the
 *                        user's own), with each feed's conditional-GET validators, fetch state and unread / total counts
 *   settings.json        {"version":1,"text_size":"small|medium|large","image_download":"always|off","storage_cap_mb":200,
 *                        "refresh_on_open":true}
 *   cache/<feed id>.json {"version":1,"feed":id,"articles":[{id,guid,title,link,published,dated,author,summary,content,
 *                        images,full}]}  at most 200 articles per feed (oldest dropped, saved ones kept)
 *   read/<feed id>.json  {"version":1,"read":[article id, ...]}  the READ STATE, a small file apart from the cache so that
 *                        marking one article read rewrites 4 KB, not the whole cache; pruned with the articles on every
 *                        store_articles() (so it is bounded by 250 ids per feed) and deleted with the subscription
 *   saved/<article id>/  article.json (the article, its feed, when it was saved, and its images: url, file, type, bytes)
 *                        and images/001.jpg ... : the bytes exactly as fetched. Nothing is evicted automatically: saving
 *                        is refused (CapExceeded) when it would take the saved articles over the storage cap
 *
 * Rules for every file: written atomically (temp file + fsync + rename); carries a schema version; a file that cannot be
 * parsed, or whose version is newer than this code knows, is renamed to "<name>.bad" (replacing an older .bad) and the
 * store starts that part empty. The class is thread safe (one mutex): a refresh on a worker thread and the UI can share it.
 */
#pragma once

#include "feed.hpp"
#include "json.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace rssreader {

constexpr int kSchemaVersion = 1;
constexpr std::size_t kMaxSavedKeepBeyondCap = 50;                 /* saved articles that stay in a feed cache past the 200 */
constexpr std::uint64_t kMaxSavedArticleBytes = 20u * 1024 * 1024; /* one saved article with its images */

enum class TextSize { Small, Medium, Large };
enum class ImageDownload { Always, Off };

struct Settings {
    TextSize text_size = TextSize::Medium;
    ImageDownload image_download = ImageDownload::Always;
    int storage_cap_mb = 200;          /* clamped to 10..4096 */
    bool refresh_on_open = true;
};

struct Subscription {
    std::string id;                    /* catalog id, or "c-" + hash16(url) for the user's own feeds */
    std::string name;
    std::string url;
    std::string site;
    std::string group;                 /* catalog topic, or an OPML folder name */
    std::string lang;
    bool custom = false;
    std::string etag;
    std::string last_modified;
    std::int64_t added = 0;
    std::int64_t last_fetch = 0;       /* last successful fetch (200 or 304) */
    std::int64_t last_attempt = 0;
    std::string last_error;            /* "" after a success */
    int unread = 0;
    int total = 0;
    std::int64_t newest = 0;           /* `published` of the newest cached article (0: none), kept by store_articles() */
};

struct StoredArticle {
    Article article;
    bool read = false;
};

enum class StoreError { Ok, Duplicate, NotFound, BadArgument, CapExceeded, ArticleTooLarge, IoError };

struct SavedImage {                    /* input of save_article */
    std::string url;
    std::string content_type;
    std::string bytes;
};

struct SavedImageInfo {
    std::string url;
    std::string file;                  /* "images/001.jpg" relative to the article's folder */
    std::string content_type;
    std::uint64_t bytes = 0;
};

struct SavedArticle {
    Article article;
    std::string feed_id, feed_name;
    std::int64_t saved_at = 0;
    std::vector<SavedImageInfo> images;
    std::uint64_t bytes = 0;           /* on disk: article.json + images */
};

struct SavedSummary {
    std::string id, title, feed_id, feed_name;
    std::int64_t published = 0, saved_at = 0;
    std::uint64_t bytes = 0;
    int image_count = 0;
};

std::string text_size_name(TextSize s);
std::string image_download_name(ImageDownload d);

Json article_to_json(const Article &a);
bool article_from_json(const Json &j, Article &a);

class Store {
public:
    explicit Store(std::string root);
    static std::string default_root();
    const std::string &root() const { return root_; }
    /* Creates the folders and loads subscriptions, settings and the saved index. Called by every method when needed;
     * false when the root cannot be created or written. */
    bool open();

    /* ---- settings */
    Settings settings();
    bool set_settings(const Settings &s);

    /* ---- subscriptions */
    std::vector<Subscription> subscriptions();
    bool find_subscription(const std::string &id, Subscription &out);
    /* Duplicate when the id or the URL is already there. */
    StoreError add_subscription(const Subscription &s);
    bool remove_subscription(const std::string &id);          /* also deletes its article cache; saved articles stay */
    bool move_subscription(const std::string &id, std::size_t new_index);
    /* Records the outcome of a fetch attempt: on success (error "") last_fetch = now and the validators are replaced;
     * on failure only last_attempt / last_error change. */
    bool record_fetch(const std::string &id, std::int64_t now, bool success, const std::string &etag, const std::string &last_modified,
                      const std::string &error);

    /* ---- articles of a feed */
    std::vector<StoredArticle> load_articles(const std::string &feed_id);
    /* Replaces the cache (newest first is expected): keeps the first 200 plus the saved ones (up to 50 more); updates the
     * subscription's unread / total. */
    bool store_articles(const std::string &feed_id, std::vector<StoredArticle> articles);
    bool set_read(const std::string &feed_id, const std::string &article_id, bool read);
    bool mark_all_read(const std::string &feed_id);
    /* Forgets every read mark (every feed's read/ file goes): all cached articles are unread again. */
    bool clear_read_history();
    int unread_count(const std::string &feed_id);
    int total_unread();

    /* ---- saved articles */
    StoreError save_article(const Article &a, const std::string &feed_id, const std::string &feed_name, const std::vector<SavedImage> &images,
                            std::int64_t now);
    bool is_saved(const std::string &article_id);
    std::vector<SavedSummary> list_saved();                   /* newest saved first */
    bool load_saved(const std::string &article_id, SavedArticle &out);
    bool read_saved_image(const std::string &article_id, const std::string &file, std::string &bytes);
    bool delete_saved(const std::string &article_id);
    std::uint64_t saved_usage_bytes();
    std::uint64_t storage_cap_bytes();
    std::uint64_t cache_usage_bytes();                        /* the article caches (not counted against the cap) */

    /* ---- paths (for the tests) */
    std::string subscriptions_path() const { return root_ + "/subscriptions.json"; }
    std::string settings_path() const { return root_ + "/settings.json"; }
    std::string cache_path(const std::string &feed_id) const;
    std::string saved_dir(const std::string &article_id) const { return root_ + "/saved/" + article_id; }

private:
    bool ensure_open();
    bool save_subscriptions();
    bool save_settings();
    std::vector<StoredArticle> load_articles_locked(const std::string &feed_id);
    bool store_articles_locked(const std::string &feed_id, std::vector<StoredArticle> articles);
    bool read_json_or_quarantine(const std::string &path, Json &out, bool &existed);
    void scan_saved();

    std::string root_;
    std::mutex mu_;
    bool opened_ = false, open_ok_ = false;
    Settings settings_;
    std::vector<Subscription> subs_;
    std::map<std::string, SavedSummary> saved_;
};

} // namespace rssreader
