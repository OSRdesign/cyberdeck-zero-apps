/*
 * SPDX-License-Identifier: MIT
 */
#include "store.hpp"

#include "fetch.hpp"
#include "fsutil.hpp"
#include "url.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace rssreader {

std::string text_size_name(TextSize s)
{
    switch (s) {
    case TextSize::Small: return "small";
    case TextSize::Large: return "large";
    default: return "medium";
    }
}

std::string image_download_name(ImageDownload d) { return d == ImageDownload::Off ? "off" : "always"; }

/* ---------------------------------------------------------------- JSON <-> structs */

Json article_to_json(const Article &a)
{
    Json j = Json::object();
    j.set("id", Json::string(a.id));
    j.set("guid", Json::string(a.guid));
    j.set("title", Json::string(a.title));
    j.set("link", Json::string(a.link));
    j.set("published", Json::number(static_cast<double>(a.published)));
    j.set("dated", Json::boolean(a.dated));
    j.set("author", Json::string(a.author));
    j.set("summary", Json::string(a.summary_html));
    j.set("content", Json::string(a.content_html));
    Json imgs = Json::array();
    for (const std::string &u : a.images) imgs.push(Json::string(u));
    j.set("images", std::move(imgs));
    j.set("full", Json::boolean(a.has_full_text));
    return j;
}

bool article_from_json(const Json &j, Article &a)
{
    if (!j.is_object()) return false;
    a = Article();
    a.id = j.get_string("id");
    if (a.id.empty()) return false;
    a.guid = j.get_string("guid");
    a.title = j.get_string("title");
    a.link = j.get_string("link");
    a.published = j.get_int("published");
    a.dated = j.get_bool("dated", true);
    a.author = j.get_string("author");
    a.summary_html = j.get_string("summary");
    a.content_html = j.get_string("content");
    if (const Json *im = j.get("images"); im && im->is_array())
        for (const Json &u : im->arr)
            if (u.is_string() && !u.str.empty()) a.images.push_back(u.str);
    a.has_full_text = j.get_bool("full");
    return true;
}

namespace {

Json subscription_to_json(const Subscription &s)
{
    Json j = Json::object();
    j.set("id", Json::string(s.id));
    j.set("name", Json::string(s.name));
    j.set("url", Json::string(s.url));
    j.set("site", Json::string(s.site));
    j.set("group", Json::string(s.group));
    j.set("lang", Json::string(s.lang));
    j.set("custom", Json::boolean(s.custom));
    j.set("etag", Json::string(s.etag));
    j.set("last_modified", Json::string(s.last_modified));
    j.set("last_fetch", Json::number(static_cast<double>(s.last_fetch)));
    j.set("last_attempt", Json::number(static_cast<double>(s.last_attempt)));
    j.set("last_error", Json::string(s.last_error));
    j.set("added", Json::number(static_cast<double>(s.added)));
    j.set("unread", Json::number(s.unread));
    j.set("total", Json::number(s.total));
    j.set("newest", Json::number(static_cast<double>(s.newest)));
    return j;
}

bool subscription_from_json(const Json &j, Subscription &s)
{
    if (!j.is_object()) return false;
    s = Subscription();
    s.id = j.get_string("id");
    s.url = j.get_string("url");
    if (s.id.empty() || s.id != safe_name(s.id) || s.url.empty()) return false;
    s.name = j.get_string("name", s.id);
    s.site = j.get_string("site");
    s.group = j.get_string("group");
    s.lang = j.get_string("lang");
    s.custom = j.get_bool("custom");
    s.etag = j.get_string("etag");
    s.last_modified = j.get_string("last_modified");
    s.last_fetch = j.get_int("last_fetch");
    s.last_attempt = j.get_int("last_attempt");
    s.last_error = j.get_string("last_error");
    s.added = j.get_int("added");
    s.unread = static_cast<int>(std::max<std::int64_t>(0, j.get_int("unread")));
    s.total = static_cast<int>(std::max<std::int64_t>(0, j.get_int("total")));
    s.newest = std::max<std::int64_t>(0, j.get_int("newest"));
    return true;
}

bool valid_file_id(const std::string &id) { return !id.empty() && id == safe_name(id) && id[0] != '_'; }

} // namespace

/* ---------------------------------------------------------------- Store basics */

Store::Store(std::string root) : root_(std::move(root))
{
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
}

std::string Store::default_root()
{
    if (const char *d = std::getenv("RSSREADER_DATA"); d && *d) return d;
    if (const char *x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/rss-reader";
    const char *h = std::getenv("HOME");
    return std::string(h && *h ? h : "/tmp") + "/.local/share/rss-reader";
}

std::string Store::cache_path(const std::string &feed_id) const { return root_ + "/cache/" + safe_name(feed_id) + ".json"; }

bool Store::read_json_or_quarantine(const std::string &path, Json &out, bool &existed)
{
    existed = false;
    std::string text;
    if (!path_exists(path)) return false;
    existed = true;
    bool ok = read_file(path, text) && json_parse(text, out) && out.is_object() && out.get_int("version", 0) >= 1 &&
              out.get_int("version", 0) <= kSchemaVersion;
    if (ok) return true;
    rename_path(path, path + ".bad");     /* replaces an older .bad */
    out = Json();
    return false;
}

bool Store::open()
{
    std::lock_guard<std::mutex> g(mu_);
    return ensure_open();
}

bool Store::ensure_open()
{
    if (opened_) return open_ok_;
    opened_ = true;
    open_ok_ = make_dirs(root_) && make_dirs(root_ + "/cache") && make_dirs(root_ + "/read") && make_dirs(root_ + "/saved");
    if (!open_ok_) return false;

    Json j;
    bool existed;
    if (read_json_or_quarantine(settings_path(), j, existed)) {
        std::string ts = j.get_string("text_size"), im = j.get_string("image_download");
        settings_.text_size = ts == "small" ? TextSize::Small : ts == "large" ? TextSize::Large : TextSize::Medium;
        settings_.image_download = im == "off" ? ImageDownload::Off : ImageDownload::Always;
        settings_.storage_cap_mb = static_cast<int>(std::min<std::int64_t>(4096, std::max<std::int64_t>(10, j.get_int("storage_cap_mb", 200))));
        settings_.refresh_on_open = j.get_bool("refresh_on_open", true);
    }
    subs_.clear();
    if (read_json_or_quarantine(subscriptions_path(), j, existed)) {
        if (const Json *f = j.get("feeds"); f && f->is_array()) {
            std::set<std::string> ids;
            for (const Json &e : f->arr) {
                Subscription s;
                if (subscription_from_json(e, s) && ids.insert(s.id).second) subs_.push_back(std::move(s));
            }
        }
    }
    scan_saved();
    return true;
}

bool Store::save_settings()
{
    Json j = Json::object();
    j.set("version", Json::number(kSchemaVersion));
    j.set("text_size", Json::string(text_size_name(settings_.text_size)));
    j.set("image_download", Json::string(image_download_name(settings_.image_download)));
    j.set("storage_cap_mb", Json::number(settings_.storage_cap_mb));
    j.set("refresh_on_open", Json::boolean(settings_.refresh_on_open));
    return write_file_atomic(settings_path(), json_dump(j, true));
}

bool Store::save_subscriptions()
{
    Json j = Json::object();
    j.set("version", Json::number(kSchemaVersion));
    Json arr = Json::array();
    for (const Subscription &s : subs_) arr.push(subscription_to_json(s));
    j.set("feeds", std::move(arr));
    return write_file_atomic(subscriptions_path(), json_dump(j, true));
}

/* ---------------------------------------------------------------- settings */

Settings Store::settings()
{
    std::lock_guard<std::mutex> g(mu_);
    ensure_open();
    return settings_;
}

bool Store::set_settings(const Settings &s)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open()) return false;
    Settings old = settings_;
    settings_ = s;
    settings_.storage_cap_mb = std::min(4096, std::max(10, settings_.storage_cap_mb));
    if (!save_settings()) {
        settings_ = old;
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- subscriptions */

std::vector<Subscription> Store::subscriptions()
{
    std::lock_guard<std::mutex> g(mu_);
    ensure_open();
    return subs_;
}

bool Store::find_subscription(const std::string &id, Subscription &out)
{
    std::lock_guard<std::mutex> g(mu_);
    ensure_open();
    for (const Subscription &s : subs_)
        if (s.id == id) { out = s; return true; }
    return false;
}

StoreError Store::add_subscription(const Subscription &in)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open()) return StoreError::IoError;
    if (!valid_file_id(in.id) || !is_http_url(in.url)) return StoreError::BadArgument;
    for (const Subscription &s : subs_)
        if (s.id == in.id || s.url == in.url) return StoreError::Duplicate;
    subs_.push_back(in);
    subs_.back().unread = 0;
    subs_.back().total = 0;
    if (!save_subscriptions()) {
        subs_.pop_back();
        return StoreError::IoError;
    }
    return StoreError::Ok;
}

bool Store::remove_subscription(const std::string &id)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open()) return false;
    auto it = std::find_if(subs_.begin(), subs_.end(), [&](const Subscription &s) { return s.id == id; });
    if (it == subs_.end()) return false;
    Subscription keep = *it;
    subs_.erase(it);
    if (!save_subscriptions()) {
        subs_.push_back(keep);
        return false;
    }
    remove_tree(cache_path(id));
    remove_tree(root_ + "/read/" + safe_name(id) + ".json");
    return true;
}

bool Store::move_subscription(const std::string &id, std::size_t new_index)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open()) return false;
    auto it = std::find_if(subs_.begin(), subs_.end(), [&](const Subscription &s) { return s.id == id; });
    if (it == subs_.end()) return false;
    std::vector<Subscription> backup = subs_;
    Subscription s = *it;
    subs_.erase(it);
    if (new_index > subs_.size()) new_index = subs_.size();
    subs_.insert(subs_.begin() + static_cast<std::ptrdiff_t>(new_index), s);
    if (!save_subscriptions()) {
        subs_ = backup;
        return false;
    }
    return true;
}

bool Store::record_fetch(const std::string &id, std::int64_t now, bool success, const std::string &etag, const std::string &last_modified,
                         const std::string &error)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open()) return false;
    for (Subscription &s : subs_) {
        if (s.id != id) continue;
        s.last_attempt = now;
        if (success) {
            s.last_fetch = now;
            s.last_error.clear();
            s.etag = etag;
            s.last_modified = last_modified;
        } else {
            s.last_error = error.empty() ? "failed" : error;
        }
        return save_subscriptions();
    }
    return false;
}

/* ---------------------------------------------------------------- articles */

static std::set<std::string> read_ids(const Json &j)
{
    std::set<std::string> s;
    if (const Json *r = j.get("read"); r && r->is_array())
        for (const Json &e : r->arr)
            if (e.is_string() && !e.str.empty()) s.insert(e.str);
    return s;
}

std::vector<StoredArticle> Store::load_articles_locked(const std::string &feed_id)
{
    std::vector<StoredArticle> out;
    Json j;
    bool existed;
    if (!read_json_or_quarantine(cache_path(feed_id), j, existed)) return out;
    std::set<std::string> read;
    Json rj;
    if (read_json_or_quarantine(root_ + "/read/" + safe_name(feed_id) + ".json", rj, existed)) read = read_ids(rj);
    if (const Json *a = j.get("articles"); a && a->is_array()) {
        std::set<std::string> seen;
        for (const Json &e : a->arr) {
            StoredArticle sa;
            if (!article_from_json(e, sa.article) || !seen.insert(sa.article.id).second) continue;
            sa.read = read.count(sa.article.id) != 0;
            out.push_back(std::move(sa));
        }
    }
    return out;
}

std::vector<StoredArticle> Store::load_articles(const std::string &feed_id)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open()) return {};
    return load_articles_locked(feed_id);
}

bool Store::store_articles_locked(const std::string &feed_id, std::vector<StoredArticle> articles)
{
    std::vector<StoredArticle> kept;
    std::size_t extra = 0;
    std::set<std::string> ids;
    for (StoredArticle &sa : articles) {
        if (!ids.insert(sa.article.id).second) continue;
        bool saved = saved_.count(sa.article.id) != 0;
        if (kept.size() < kMaxArticlesPerFeed || (saved && extra < kMaxSavedKeepBeyondCap)) {
            if (kept.size() >= kMaxArticlesPerFeed) ++extra;
            kept.push_back(std::move(sa));
        }
    }
    Json arts = Json::array();
    Json read = Json::array();
    int unread = 0;
    std::int64_t newest = 0;
    for (const StoredArticle &sa : kept) {
        arts.push(article_to_json(sa.article));
        if (sa.read) read.push(Json::string(sa.article.id));
        else ++unread;
        newest = std::max(newest, sa.article.published);
    }
    Json c = Json::object();
    c.set("version", Json::number(kSchemaVersion));
    c.set("feed", Json::string(feed_id));
    c.set("articles", std::move(arts));
    Json r = Json::object();
    r.set("version", Json::number(kSchemaVersion));
    r.set("read", std::move(read));
    if (!write_file_atomic(cache_path(feed_id), json_dump(c))) return false;
    if (!write_file_atomic(root_ + "/read/" + safe_name(feed_id) + ".json", json_dump(r))) return false;
    for (Subscription &s : subs_)
        if (s.id == feed_id) {
            s.total = static_cast<int>(kept.size());
            s.unread = unread;
            s.newest = newest;
            return save_subscriptions();
        }
    return true;
}

bool Store::store_articles(const std::string &feed_id, std::vector<StoredArticle> articles)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open() || !valid_file_id(feed_id)) return false;
    return store_articles_locked(feed_id, std::move(articles));
}

bool Store::set_read(const std::string &feed_id, const std::string &article_id, bool read)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open() || !valid_file_id(feed_id) || article_id.empty()) return false;
    const std::string path = root_ + "/read/" + safe_name(feed_id) + ".json";
    Json rj;
    bool existed;
    std::set<std::string> ids;
    if (read_json_or_quarantine(path, rj, existed)) ids = read_ids(rj);
    bool changed = read ? ids.insert(article_id).second : ids.erase(article_id) > 0;
    if (!changed) return true;
    Json arr = Json::array();
    for (const std::string &i : ids) arr.push(Json::string(i));
    Json r = Json::object();
    r.set("version", Json::number(kSchemaVersion));
    r.set("read", std::move(arr));
    if (!write_file_atomic(path, json_dump(r))) return false;
    for (Subscription &s : subs_)
        if (s.id == feed_id) {
            s.unread = std::max(0, s.total - static_cast<int>(ids.size()));
            return save_subscriptions();
        }
    return true;
}

bool Store::mark_all_read(const std::string &feed_id)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open() || !valid_file_id(feed_id)) return false;
    std::vector<StoredArticle> arts = load_articles_locked(feed_id);
    for (StoredArticle &a : arts) a.read = true;
    return store_articles_locked(feed_id, std::move(arts));
}

bool Store::clear_read_history()
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open()) return false;
    bool ok = true;
    for (const std::string &name : list_dir(root_ + "/read"))
        if (!remove_tree(root_ + "/read/" + name)) ok = false;
    for (Subscription &s : subs_) s.unread = s.total;
    return save_subscriptions() && ok;
}

int Store::unread_count(const std::string &feed_id)
{
    std::lock_guard<std::mutex> g(mu_);
    ensure_open();
    for (const Subscription &s : subs_)
        if (s.id == feed_id) return s.unread;
    return 0;
}

int Store::total_unread()
{
    std::lock_guard<std::mutex> g(mu_);
    ensure_open();
    int n = 0;
    for (const Subscription &s : subs_) n += s.unread;
    return n;
}

/* ---------------------------------------------------------------- saved articles */

void Store::scan_saved()
{
    saved_.clear();
    const std::string dir = root_ + "/saved";
    for (const std::string &n : list_dir(dir)) {
        if (starts_with(n, ".tmp-")) { remove_tree(dir + "/" + n); continue; }
        if (starts_with(n, ".old-")) {
            std::string id = n.substr(5);
            if (path_exists(dir + "/" + id)) remove_tree(dir + "/" + n);
            else rename_path(dir + "/" + n, dir + "/" + id);       /* a save was interrupted between its two renames */
        }
    }
    for (const std::string &n : list_dir(dir)) {
        if (n[0] == '.' || ends_with(n, ".bad") || !is_directory(dir + "/" + n)) continue;
        std::string text;
        Json j;
        bool ok = read_file(dir + "/" + n + "/article.json", text) && json_parse(text, j) && j.is_object() &&
                  j.get_int("version", 0) >= 1 && j.get_int("version", 0) <= kSchemaVersion;
        SavedSummary s;
        const Json *aj = ok ? j.get("article") : nullptr;
        Article a;
        if (!aj || !article_from_json(*aj, a) || a.id != n) {
            rename_path(dir + "/" + n, dir + "/" + n + ".bad");
            continue;
        }
        s.id = a.id;
        s.title = a.title;
        s.published = a.published;
        s.feed_id = j.get_string("feed_id");
        s.feed_name = j.get_string("feed_name");
        s.saved_at = j.get_int("saved_at");
        if (const Json *im = j.get("images"); im && im->is_array()) s.image_count = static_cast<int>(im->arr.size());
        s.bytes = tree_size(dir + "/" + n);
        saved_[s.id] = s;
    }
}

StoreError Store::save_article(const Article &a, const std::string &feed_id, const std::string &feed_name, const std::vector<SavedImage> &images,
                               std::int64_t now)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open()) return StoreError::IoError;
    if (!valid_file_id(a.id)) return StoreError::BadArgument;

    Json imgs = Json::array();
    std::uint64_t bytes = 0;
    int index = 0;
    std::vector<std::pair<std::string, const SavedImage *>> files;
    for (const SavedImage &im : images) {
        if (im.bytes.empty()) continue;
        char name[32];
        std::snprintf(name, sizeof name, "images/%03d%s", ++index, image_extension(im.content_type).c_str());
        Json e = Json::object();
        e.set("url", Json::string(im.url));
        e.set("file", Json::string(name));
        e.set("type", Json::string(im.content_type));
        e.set("bytes", Json::number(static_cast<double>(im.bytes.size())));
        imgs.push(std::move(e));
        files.emplace_back(name, &im);
        bytes += im.bytes.size();
    }
    Json meta = Json::object();
    meta.set("version", Json::number(kSchemaVersion));
    meta.set("article", article_to_json(a));
    meta.set("feed_id", Json::string(feed_id));
    meta.set("feed_name", Json::string(feed_name));
    meta.set("saved_at", Json::number(static_cast<double>(now)));
    meta.set("images", std::move(imgs));
    const std::string meta_text = json_dump(meta, true);
    bytes += meta_text.size();

    if (bytes > kMaxSavedArticleBytes) return StoreError::ArticleTooLarge;
    const std::string dir = root_ + "/saved";
    const std::string final_dir = dir + "/" + a.id;
    std::uint64_t existing = path_exists(final_dir) ? tree_size(final_dir) : 0;
    std::uint64_t usage = tree_size(dir);
    usage = usage > existing ? usage - existing : 0;
    if (usage + bytes > static_cast<std::uint64_t>(settings_.storage_cap_mb) * 1024 * 1024) return StoreError::CapExceeded;

    const std::string stage = dir + "/.tmp-" + a.id;
    remove_tree(stage);
    bool ok = make_dirs(stage + "/images");
    for (std::size_t k = 0; ok && k < files.size(); ++k) ok = write_file_atomic(stage + "/" + files[k].first, files[k].second->bytes);
    ok = ok && write_file_atomic(stage + "/article.json", meta_text);
    if (ok) {
        const std::string old = dir + "/.old-" + a.id;
        bool had = path_exists(final_dir);
        remove_tree(old);
        if (had) ok = rename_path(final_dir, old);
        ok = ok && rename_path(stage, final_dir);
        if (!ok && had && !path_exists(final_dir)) rename_path(old, final_dir);
        else if (ok && had) remove_tree(old);
    }
    if (!ok) {
        remove_tree(stage);
        return StoreError::IoError;
    }
    SavedSummary s;
    s.id = a.id;
    s.title = a.title;
    s.published = a.published;
    s.feed_id = feed_id;
    s.feed_name = feed_name;
    s.saved_at = now;
    s.image_count = index;
    s.bytes = tree_size(final_dir);
    saved_[s.id] = s;
    return StoreError::Ok;
}

bool Store::is_saved(const std::string &id)
{
    std::lock_guard<std::mutex> g(mu_);
    ensure_open();
    return saved_.count(id) != 0;
}

std::vector<SavedSummary> Store::list_saved()
{
    std::lock_guard<std::mutex> g(mu_);
    ensure_open();
    std::vector<SavedSummary> v;
    for (const auto &kv : saved_) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), [](const SavedSummary &a, const SavedSummary &b) {
        return a.saved_at != b.saved_at ? a.saved_at > b.saved_at : a.id < b.id;
    });
    return v;
}

bool Store::load_saved(const std::string &id, SavedArticle &out)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open() || !valid_file_id(id) || !saved_.count(id)) return false;
    std::string text;
    Json j;
    if (!read_file(saved_dir(id) + "/article.json", text) || !json_parse(text, j)) return false;
    const Json *aj = j.get("article");
    out = SavedArticle();
    if (!aj || !article_from_json(*aj, out.article)) return false;
    out.feed_id = j.get_string("feed_id");
    out.feed_name = j.get_string("feed_name");
    out.saved_at = j.get_int("saved_at");
    if (const Json *im = j.get("images"); im && im->is_array())
        for (const Json &e : im->arr) {
            SavedImageInfo i;
            i.url = e.get_string("url");
            i.file = e.get_string("file");
            i.content_type = e.get_string("type");
            i.bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, e.get_int("bytes")));
            if (!i.file.empty()) out.images.push_back(std::move(i));
        }
    out.bytes = saved_[id].bytes;
    return true;
}

bool Store::read_saved_image(const std::string &id, const std::string &file, std::string &bytes)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open() || !valid_file_id(id) || !saved_.count(id)) return false;
    /* only files this store wrote: "images/<name>" with a plain name */
    if (!starts_with(file, "images/") || file.find("..") != std::string::npos || file.find('/', 7) != std::string::npos) return false;
    return read_file(saved_dir(id) + "/" + file, bytes, 8u * 1024 * 1024);
}

bool Store::delete_saved(const std::string &id)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open() || !valid_file_id(id) || !saved_.count(id)) return false;
    if (!remove_tree(saved_dir(id))) return false;
    saved_.erase(id);
    return true;
}

std::uint64_t Store::saved_usage_bytes()
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open()) return 0;
    return tree_size(root_ + "/saved");
}

std::uint64_t Store::storage_cap_bytes()
{
    std::lock_guard<std::mutex> g(mu_);
    ensure_open();
    return static_cast<std::uint64_t>(settings_.storage_cap_mb) * 1024 * 1024;
}

std::uint64_t Store::cache_usage_bytes()
{
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure_open()) return 0;
    return tree_size(root_ + "/cache") + tree_size(root_ + "/read");
}

} // namespace rssreader
