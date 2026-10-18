/*
 * SPDX-License-Identifier: MIT
 */
#include "refresh.hpp"

#include "discover.hpp"
#include "url.hpp"
#include "util.hpp"

#include <algorithm>
#include <map>

namespace rssreader {

std::string custom_feed_id(const std::string &url) { return "c-" + hash16(url); }

Subscription make_subscription(const CatalogFeed &f, std::int64_t now)
{
    Subscription s;
    s.id = f.id;
    s.name = f.name;
    s.url = f.url;
    s.site = f.site;
    s.group = f.topic;
    s.lang = f.lang;
    s.custom = false;
    s.added = now;
    return s;
}

namespace {

std::string error_message(const HttpResponse &r)
{
    if (r.error == FetchError::Http) return "HTTP " + std::to_string(r.status);
    std::string m = fetch_error_text(r.error);
    if (!r.error_detail.empty() && (r.error == FetchError::Network || r.error == FetchError::Tls || r.error == FetchError::Internal))
        m += ": " + r.error_detail;
    return m;
}

} // namespace

std::vector<StoredArticle> merge_articles(const std::vector<StoredArticle> &cached, const std::vector<Article> &fresh, int &new_count)
{
    new_count = 0;
    std::map<std::string, const StoredArticle *> old;
    for (const StoredArticle &c : cached) old[c.article.id] = &c;
    std::vector<StoredArticle> out;
    std::map<std::string, bool> in_fresh;
    for (const Article &n : fresh) {
        if (in_fresh.count(n.id)) continue;
        in_fresh[n.id] = true;
        StoredArticle sa;
        sa.article = n;
        auto it = old.find(n.id);
        if (it == old.end()) {
            ++new_count;
        } else {
            sa.read = it->second->read;
            if (!n.dated && it->second->article.dated) {
                sa.article.published = it->second->article.published;
                sa.article.dated = true;
            } else if (!n.dated) {
                sa.article.published = it->second->article.published;
            }
        }
        out.push_back(std::move(sa));
    }
    for (const StoredArticle &c : cached)
        if (!in_fresh.count(c.article.id)) out.push_back(c);
    std::stable_sort(out.begin(), out.end(), [](const StoredArticle &a, const StoredArticle &b) { return a.article.published > b.article.published; });
    return out;
}

RefreshResult Refresher::refresh_feed(const Subscription &sub, const std::atomic<bool> *cancel)
{
    RefreshResult res;
    res.feed_id = sub.id;
    const std::int64_t now = now_();
    Subscription cur = sub;
    store_.find_subscription(sub.id, cur);        /* the stored validators win over a stale copy */

    std::vector<StoredArticle> cached = store_.load_articles(sub.id);
    HttpRequest req;
    req.url = cur.url;
    req.cancel = cancel;
    if (!cached.empty()) {                          /* a 304 is only useful when there is something to show */
        req.etag = cur.etag;
        req.last_modified = cur.last_modified;
    }
    HttpResponse resp = client_.get(req);

    if (resp.not_modified()) {
        store_.record_fetch(sub.id, now, true, cur.etag, cur.last_modified, "");
        res.status = RefreshResult::Status::NotModified;
        res.total = static_cast<int>(cached.size());
        return res;
    }
    if (!resp.ok()) {
        res.error = resp.error;
        res.message = error_message(resp);
        if (resp.error != FetchError::Cancelled) store_.record_fetch(sub.id, now, false, "", "", res.message);
        res.total = static_cast<int>(cached.size());
        return res;
    }
    Feed feed;
    FeedStatus fs = parse_feed(resp.body, resp.final_url.empty() ? cur.url : resp.final_url, sub.id, now, feed);
    if (fs != FeedStatus::Ok) {
        res.message = feed_status_text(fs);
        store_.record_fetch(sub.id, now, false, "", "", res.message);
        res.total = static_cast<int>(cached.size());
        return res;
    }
    int fresh_count = 0;
    std::vector<StoredArticle> merged = merge_articles(cached, feed.articles, fresh_count);
    if (!store_.store_fetched_articles(sub.id, std::move(merged))) {     // the read state as it is now, not as it was loaded
        res.message = "could not save the articles";
        store_.record_fetch(sub.id, now, false, "", "", res.message);
        return res;
    }
    store_.record_fetch(sub.id, now, true, resp.etag, resp.last_modified, "");
    Subscription after;
    res.total = store_.find_subscription(sub.id, after) ? after.total : static_cast<int>(feed.articles.size());
    res.new_articles = fresh_count;
    res.status = RefreshResult::Status::Updated;
    return res;
}

std::vector<RefreshResult> Refresher::refresh_all(const std::vector<Subscription> &subs, const std::atomic<bool> *cancel,
                                                  const std::function<void(std::size_t, std::size_t, const RefreshResult &)> &progress)
{
    std::vector<RefreshResult> out;
    for (std::size_t i = 0; i < subs.size(); ++i) {
        if (cancel && cancel->load()) break;
        out.push_back(refresh_feed(subs[i], cancel));
        if (progress) progress(i + 1, subs.size(), out.back());
    }
    return out;
}

/* Fetches `url`. If it parses as a feed, fills `out` and returns true. Otherwise the failure is described in `failure`
 * and, when the body looks like a page, it is handed back in html_body / html_url. */
bool Refresher::try_feed(const std::string &url, const std::atomic<bool> *cancel, bool need_articles, FoundFeed &out, AddResult &failure,
                         std::string *html_body, std::string *html_url)
{
    HttpRequest req;
    req.url = url;
    req.cancel = cancel;
    HttpResponse resp = client_.get(req);
    if (!resp.ok()) {
        failure.status = AddResult::Status::FetchFailed;
        failure.error = resp.error;
        failure.message = error_message(resp);
        return false;
    }
    const std::string final_url = resp.final_url.empty() ? url : resp.final_url;
    const std::string id = custom_feed_id(final_url);
    Feed feed;
    FeedStatus fs = parse_feed(resp.body, final_url, id, now_(), feed);
    if (fs == FeedStatus::Ok && (!need_articles || !feed.articles.empty())) {
        out.title = feed.title.empty() ? url_host(final_url) : feed.title;
        out.url = final_url;
        out.site_url = feed.site_url;
        out.description = feed.description;
        out.lang = feed.lang;
        out.format = feed.format;
        out.article_count = static_cast<int>(feed.articles.size());
        out.feed = std::move(feed);
        out.etag = resp.etag;
        out.last_modified = resp.last_modified;
        return true;
    }
    failure.status = AddResult::Status::NoFeedFound;
    failure.error = FetchError::None;
    failure.message = fs == FeedStatus::Ok ? "the feed has no articles" : feed_status_text(fs);
    if (html_body) *html_body = std::move(resp.body);
    if (html_url) *html_url = final_url;
    return false;
}

AddResult Refresher::add_custom_feed(const std::string &input, const std::atomic<bool> *cancel)
{
    AddResult res;
    const std::string url = normalize_user_url(input);
    if (url.empty()) {
        res.message = "that is not a web address";
        return res;
    }
    std::string html, html_url;
    AddResult failure;
    FoundFeed found;
    bool ok = try_feed(url, cancel, false, found, failure, &html, &html_url);
    const bool had_scheme = input.find("://") != std::string::npos;
    if (!ok && !had_scheme && (failure.error == FetchError::Network || failure.error == FetchError::Tls || failure.error == FetchError::Timeout)) {
        /* "example.com" typed without a scheme: https first, plain http if the site has no TLS */
        ok = try_feed("http://" + url.substr(8), cancel, false, found, failure, &html, &html_url);
    }
    if (ok) {
        res.status = AddResult::Status::Ok;
        res.found = std::move(found);
        return res;
    }
    if (failure.status == AddResult::Status::FetchFailed || html.empty()) {
        res.status = failure.status;
        res.error = failure.error;
        res.message = failure.message;
        return res;
    }
    /* a page: try what it advertises, then the usual places */
    std::vector<FeedCandidate> cands = discover_feeds(html, html_url);
    int tried = 0;
    AddResult last = failure;
    for (const FeedCandidate &c : cands) {
        if (cancel && cancel->load()) {
            res.status = AddResult::Status::FetchFailed;
            res.error = FetchError::Cancelled;
            res.message = "cancelled";
            return res;
        }
        if (c.url == html_url || c.url == url) continue;
        if (++tried > 8) break;
        FoundFeed f;
        AddResult fail;
        if (try_feed(c.url, cancel, !c.declared, f, fail, nullptr, nullptr)) {
            res.status = AddResult::Status::Ok;
            res.found = std::move(f);
            return res;
        }
        if (fail.error == FetchError::Cancelled) {
            res.status = AddResult::Status::FetchFailed;
            res.error = FetchError::Cancelled;
            res.message = "cancelled";
            return res;
        }
    }
    res.status = AddResult::Status::NoFeedFound;
    res.message = "no feed found at that address";
    return res;
}

StoreError Refresher::pin_custom_feed(const FoundFeed &found, const std::string &group)
{
    if (!is_http_url(found.url)) return StoreError::BadArgument;
    const std::int64_t now = now_();
    Subscription s;
    s.id = custom_feed_id(found.url);
    s.name = found.title.empty() ? url_host(found.url) : found.title;
    s.url = found.url;
    s.site = found.site_url;
    s.group = group;
    s.lang = found.lang;
    s.custom = true;
    s.added = now;
    StoreError e = store_.add_subscription(s);
    if (e != StoreError::Ok) return e;
    std::vector<StoredArticle> arts;
    for (const Article &a : found.feed.articles) {
        StoredArticle sa;
        sa.article = a;
        arts.push_back(std::move(sa));
    }
    if (!arts.empty() && !store_.store_articles(s.id, std::move(arts))) return StoreError::IoError;
    store_.record_fetch(s.id, now, true, found.etag, found.last_modified, "");
    return StoreError::Ok;
}

} // namespace rssreader
