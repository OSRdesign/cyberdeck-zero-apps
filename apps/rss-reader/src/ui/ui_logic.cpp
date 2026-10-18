/*
 * SPDX-License-Identifier: MIT
 */
#include "ui_logic.hpp"

#include "html_text.hpp"
#include "url.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>

using namespace rssreader;

namespace rssui {

namespace {

const std::string kDot = "  \xC2\xB7  ";      // "  ·  ": the separator of the grey lines

std::string lower_ascii(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool local_tm(std::int64_t t, std::tm &out)
{
    const std::time_t tt = static_cast<std::time_t>(t);
    return localtime_r(&tt, &out) != nullptr;
}

const char *month_name(int m)
{
    static const char *const names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return m >= 0 && m < 12 ? names[m] : "?";
}

} // namespace

const char *page_name(PageId p)
{
    switch (p) {
    case PageId::Home: return "RSS Reader";
    case PageId::Favorites: return "Favorites";
    case PageId::Articles: return "Articles";
    case PageId::Reader: return "Reading";
    case PageId::Browse: return "Browse";
    case PageId::Saved: return "Saved articles";
    case PageId::Add: return "Add a feed";
    case PageId::Settings: return "Settings";
    }
    return "";
}

/* ------------------------------------------------------------------ dialogs */

bool Choice::step(int delta)
{
    if (labels.empty()) return false;
    const int next = std::clamp(index + delta, 0, static_cast<int>(labels.size()) - 1);
    if (next == index) return false;
    index = next;
    return true;
}

const std::string &Choice::label() const
{
    static const std::string empty;
    if (index < 0 || index >= static_cast<int>(labels.size())) return empty;
    return labels[static_cast<std::size_t>(index)];
}

std::string Choice::detail() const
{
    if (index < 0 || index >= static_cast<int>(details.size())) return "";
    return details[static_cast<std::size_t>(index)];
}

std::string Choice::position() const
{
    return std::to_string(index + 1) + " / " + std::to_string(labels.size());
}

ChoiceResult choice_key(const KeyEvent &e, Choice &c)
{
    switch (e.key) {
    case Key::Left:
    case Key::Up: return c.step(-1) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::Right:
    case Key::Down: return c.step(1) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::Home: return c.step(-1000) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::End: return c.step(1000) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::Enter: return e.repeat ? ChoiceResult::None : ChoiceResult::Accept;
    case Key::Esc: return e.repeat ? ChoiceResult::None : ChoiceResult::Cancel;
    default: return ChoiceResult::None;
    }
}

ConfirmResult confirm_key(const KeyEvent &e, int &focus)
{
    switch (e.key) {
    case Key::Left:
    case Key::Up:
        focus = 0;
        return ConfirmResult::Moved;
    case Key::Right:
    case Key::Down:
        focus = 1;
        return ConfirmResult::Moved;
    case Key::Tab:
    case Key::BackTab:
        focus = 1 - focus;
        return ConfirmResult::Moved;
    case Key::Enter: return e.repeat ? ConfirmResult::None : (focus == 1 ? ConfirmResult::Yes : ConfirmResult::No);
    case Key::Esc: return e.repeat ? ConfirmResult::None : ConfirmResult::No;
    case Key::Char: {
        if (e.ctrl || e.alt || e.repeat || e.text.size() != 1) return ConfirmResult::None;
        const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0])));
        if (ch == 'y') return ConfirmResult::Yes;
        if (ch == 'n') return ConfirmResult::No;
        return ConfirmResult::None;
    }
    default: return ConfirmResult::None;
    }
}

MenuResult menu_key(const KeyEvent &e, int &selected, int count)
{
    if (e.key == Key::Esc) return e.repeat ? MenuResult::None : MenuResult::Cancel;
    if (count <= 0) return MenuResult::None;
    int n = selected;
    switch (e.key) {
    case Key::Up:
    case Key::Left: n = std::clamp(selected - 1, 0, count - 1); break;
    case Key::Down:
    case Key::Right: n = std::clamp(selected + 1, 0, count - 1); break;
    case Key::Home: n = 0; break;
    case Key::End: n = count - 1; break;
    case Key::Enter: return e.repeat ? MenuResult::None : MenuResult::Accept;
    default: return MenuResult::None;
    }
    const bool moved = n != selected;
    selected = n;
    return moved ? MenuResult::Moved : MenuResult::None;
}

void HoldTracker::begin(const std::string &target, int x, int y, std::uint64_t now)
{
    active_ = true;
    swallow_ = false;
    target_ = target;
    x_ = x;
    y_ = y;
    start_ = now;
}

void HoldTracker::move(int x, int y)
{
    if (!active_) return;
    if (std::abs(x - x_) > kSlop || std::abs(y - y_) > kSlop) active_ = false;
}

void HoldTracker::cancel() { active_ = false; }

double HoldTracker::progress(std::uint64_t now) const
{
    if (!active_) return 0;
    const double p = static_cast<double>(now - start_) / static_cast<double>(kFireMs);
    return p < 0 ? 0 : p > 1 ? 1 : p;
}

bool HoldTracker::poll(std::uint64_t now)
{
    if (!active_ || now - start_ < kFireMs) return false;
    active_ = false;
    swallow_ = true;
    return true;
}

bool HoldTracker::take_swallow()
{
    const bool s = swallow_;
    swallow_ = false;
    return s;
}

/* ------------------------------------------------------------------ layout */

PageColumn page_column(int screen_w, int inset, int min_w, int cap_w)
{
    PageColumn c;
    const int margin = screen_w >= min_w + 2 * inset ? 2 * inset : inset;
    c.w = std::max(1, std::min(cap_w, screen_w - 2 * margin));
    c.x = (screen_w - c.w) / 2;
    return c;
}

std::string fit_title(const std::vector<std::string> &path, const std::function<int(const std::string &)> &width, int max_w)
{
    if (path.empty()) return "";
    std::string full;
    for (std::size_t i = 0; i < path.size(); ++i) full += (i ? " > " : "") + path[i];
    if (path.size() == 1 || width(full) <= max_w) return full;
    const std::string ell = "\xE2\x80\xA6";      // "…"
    if (path.size() > 2) {
        const std::string mid = path.front() + " > " + ell + " > " + path.back();
        if (width(mid) <= max_w) return mid;
    }
    const std::string tail = ell + " > " + path.back();
    if (width(tail) <= max_w) return tail;
    return path.back();
}

/* ------------------------------------------------------------------ formatting */

std::string fmt_age(std::int64_t when, std::int64_t now)
{
    if (when <= 0) return "";
    const std::int64_t d = now - when;
    if (d < 60) return "now";
    if (d < 3600) return std::to_string(d / 60) + " min";
    if (d < 86400) return std::to_string(d / 3600) + " h";
    if (d < 14 * 86400) return std::to_string(d / 86400) + " d";
    std::tm t{}, n{};
    if (!local_tm(when, t)) return "";
    local_tm(now, n);
    std::string s = std::string(month_name(t.tm_mon)) + " " + std::to_string(t.tm_mday);
    if (t.tm_year != n.tm_year) s += " " + std::to_string(t.tm_year + 1900);
    return s;
}

std::string fmt_updated(std::int64_t when, std::int64_t now)
{
    if (when <= 0) return "Not updated yet";
    const std::int64_t d = now - when;
    if (d < 60) return "Updated just now";
    if (d < 3600) return "Updated " + std::to_string(d / 60) + " min ago";
    if (d < 86400) return "Updated " + std::to_string(d / 3600) + " h ago";
    return "Updated " + std::to_string(d / 86400) + " d ago";
}

std::string fmt_datetime(std::int64_t when, std::int64_t now)
{
    if (when <= 0) return "";
    std::tm t{}, n{};
    if (!local_tm(when, t)) return "";
    local_tm(now, n);
    char hm[8];
    std::snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
    std::string s = std::string(month_name(t.tm_mon)) + " " + std::to_string(t.tm_mday);
    if (t.tm_year != n.tm_year) s += " " + std::to_string(t.tm_year + 1900);
    return s + ", " + hm;
}

std::string fmt_bytes(std::uint64_t n)
{
    char b[32];
    if (n < 1000ull * 1024) {
        std::snprintf(b, sizeof(b), "%llu KB", static_cast<unsigned long long>((n + 1023) / 1024));
    } else if (n < 10ull * 1024 * 1024) {
        std::snprintf(b, sizeof(b), "%.1f MB", static_cast<double>(n) / (1024.0 * 1024.0));
    } else if (n < 1000ull * 1024 * 1024) {
        std::snprintf(b, sizeof(b), "%llu MB", static_cast<unsigned long long>((n + 512 * 1024) / (1024 * 1024)));
    } else {
        std::snprintf(b, sizeof(b), "%.1f GB", static_cast<double>(n) / (1024.0 * 1024.0 * 1024.0));
    }
    return b;
}

std::string plural(long n, const char *one, const char *many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string new_text(int unread)
{
    return unread > 0 ? std::to_string(unread) + " new" : std::string();
}

std::string truncate_text(const std::string &s, std::size_t max_chars)
{
    std::size_t chars = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;
        if (chars == max_chars) return s.substr(0, i) + "...";
        ++chars;
    }
    return s;
}

/* ------------------------------------------------------------------ failures */

std::string fetch_problem(const std::string &err)
{
    const std::string e = lower_ascii(err);
    if (e.empty()) return "";
    if (e.rfind("network error", 0) == 0) return "No network";
    if (e.rfind("timed out", 0) == 0) return "Timed out";
    if (e.rfind("secure connection", 0) == 0) return "Secure connection failed";
    if (e.rfind("http ", 0) == 0) return "Server error (" + err + ")";
    if (e.rfind("not a feed", 0) == 0) return "Not a feed any more";
    if (e.find("too big") != std::string::npos || e.find("too large") != std::string::npos) return "Feed too large";
    if (e.rfind("too many redirects", 0) == 0) return "Too many redirects";
    if (e.rfind("bad address", 0) == 0) return "Bad feed address";
    if (e.rfind("internal", 0) == 0) return "curl is missing";
    return "Could not update";
}

std::string add_failure_text(const AddResult &r)
{
    switch (r.status) {
    case AddResult::Status::Ok: return "";
    case AddResult::Status::BadInput: return "That is not a web address. Type a site (example.com) or the address of its feed.";
    case AddResult::Status::NoFeedFound:
        if (r.message == "not a feed" || r.message == "no feed found at that address")
            return "No feed found: that page has no feed, and none was found at the usual places.";
        if (r.message == "the feed has no articles") return "That feed has no articles yet.";
        return "No feed found: " + r.message + ".";
    case AddResult::Status::FetchFailed:
        switch (r.error) {
        case FetchError::Network: return "No network: check the Wi-Fi and try again.";
        case FetchError::Timeout: return "Timed out: the site did not answer.";
        case FetchError::Tls: return "Secure connection failed: the site's certificate was refused.";
        case FetchError::Http: return "The site answered with an error (" + r.message + ").";
        case FetchError::TooLarge: return "The page is too large to be a feed.";
        case FetchError::TooManyRedirects: return "The site redirects too many times.";
        case FetchError::BadUrl: return "That address cannot be used.";
        case FetchError::Cancelled: return "Stopped.";
        case FetchError::Internal: return "Cannot fetch: the curl program is missing.";
        default: return "Could not reach the site (" + r.message + ").";
        }
    }
    return r.message;
}

std::string refresh_result_text(const RefreshResult &r)
{
    switch (r.status) {
    case RefreshResult::Status::Updated: return r.new_articles > 0 ? "Updated: " + new_text(r.new_articles) : "Up to date";
    case RefreshResult::Status::NotModified: return "Up to date";
    case RefreshResult::Status::Failed: break;
    }
    if (r.error == FetchError::Cancelled) return "Stopped";
    return "Not updated: " + fetch_problem(r.message);
}

/* ------------------------------------------------------------------ Home, Favorites */

std::string home_caption(int unread, std::size_t feeds)
{
    if (feeds == 0) return "No favorites yet";
    if (unread > 0) return std::to_string(unread) + " unread in your favorites";
    return "All read in your " + plural(static_cast<long>(feeds), "favorite", "favorites");
}

std::string favorites_caption(const std::vector<Subscription> &subs)
{
    if (subs.empty()) return "No favorites yet";
    int unread = 0;
    for (const Subscription &s : subs) unread += s.unread;
    return plural(static_cast<long>(subs.size()), "feed", "feeds") + kDot + (unread > 0 ? std::to_string(unread) + " unread" : std::string("all read"));
}

std::string refresh_status(bool running, std::size_t done, std::size_t total, const std::vector<Subscription> &subs, std::int64_t now)
{
    if (running) return "Updating " + std::to_string(std::min(done + 1, total)) + "/" + std::to_string(total) + "\xE2\x80\xA6";
    std::int64_t last = 0;
    for (const Subscription &s : subs) last = std::max(last, s.last_fetch);
    return fmt_updated(last, now);
}

std::string favorite_group(const Subscription &s, const Catalog *catalog)
{
    if (!s.custom) {
        if (catalog)
            if (const CatalogFeed *f = catalog->find(s.id)) return topic_label(f->topic);
        if (!s.group.empty()) return topic_label(s.group);
    }
    const std::string host = url_host(s.site.empty() ? s.url : s.site);
    return host.rfind("www.", 0) == 0 ? host.substr(4) : host;
}

FavRow favorite_row(const Subscription &s, const Catalog *catalog, std::int64_t now)
{
    FavRow r;
    r.id = s.id;
    r.name = s.name.empty() ? s.id : s.name;
    r.unread = s.unread > 0;
    r.right = new_text(s.unread);
    const std::string group = favorite_group(s, catalog);
    if (!s.last_error.empty()) {
        r.warn = true;
        r.line2 = "Not updated: " + fetch_problem(s.last_error);
        if (s.newest > 0) r.line2 += kDot + fmt_age(s.newest, now);
    } else if (s.newest > 0) {
        r.line2 = group.empty() ? fmt_age(s.newest, now) : group + kDot + fmt_age(s.newest, now);
    } else if (s.last_fetch > 0) {
        r.line2 = group.empty() ? std::string("no articles") : group + kDot + "no articles";
    } else {
        r.line2 = group.empty() ? std::string("not updated yet") : group + kDot + "not updated yet";
    }
    return r;
}

std::vector<FavOption> favorite_options(std::size_t index, std::size_t count)
{
    std::vector<FavOption> o;
    if (index > 0) o.push_back(FavOption::MoveUp);
    if (index + 1 < count) o.push_back(FavOption::MoveDown);
    o.push_back(FavOption::Unpin);
    o.push_back(FavOption::MarkAllRead);
    o.push_back(FavOption::Refresh);
    o.push_back(FavOption::Details);
    return o;
}

const char *fav_option_label(FavOption o)
{
    switch (o) {
    case FavOption::MoveUp: return "Move up";
    case FavOption::MoveDown: return "Move down";
    case FavOption::Unpin: return "Unpin";
    case FavOption::MarkAllRead: return "Mark all read";
    case FavOption::Refresh: return "Refresh this feed";
    case FavOption::Details: return "Details";
    }
    return "";
}

std::string favorite_details(const Subscription &s, const Catalog *catalog, std::int64_t now)
{
    std::string t = s.name + "\n";
    const CatalogFeed *f = catalog && !s.custom ? catalog->find(s.id) : nullptr;
    if (f) t += country_name(f->country) + kDot + topic_label(f->topic) + kDot + feed_content_text(*f) + "\n";
    else t += std::string("Added by address") + (s.group.empty() ? "" : kDot + s.group) + "\n";
    t += "Feed: " + s.url + "\n";
    if (!s.site.empty()) t += "Site: " + s.site + "\n";
    t += plural(s.total, "article", "articles") + ", " + std::to_string(s.unread) + " unread\n";
    t += fmt_updated(s.last_fetch, now);
    if (!s.last_error.empty()) t += "\nLast try failed: " + fetch_problem(s.last_error) + " (" + s.last_error + ")";
    return t;
}

/* ------------------------------------------------------------------ the article list */

std::string articles_caption(int unread, std::size_t total, bool loading, bool updating)
{
    if (loading) return "Loading\xE2\x80\xA6";
    std::string s = total == 0 ? std::string("No articles yet") : std::to_string(unread) + " unread" + kDot + plural(static_cast<long>(total), "article", "articles");
    if (updating) s += kDot + "updating\xE2\x80\xA6";
    return s;
}

ArticleRow article_row(const StoredArticle &a, bool saved, std::int64_t now)
{
    ArticleRow r;
    r.id = a.article.id;
    r.title = a.article.title.empty() ? std::string("(no title)") : a.article.title;
    r.age = fmt_age(a.article.published, now);
    r.unread = !a.read;
    r.saved = saved;
    return r;
}

std::vector<ArticleOption> article_options(bool read, bool saved)
{
    return {read ? ArticleOption::MarkUnread : ArticleOption::MarkRead, saved ? ArticleOption::Unsave : ArticleOption::Save, ArticleOption::MarkAllRead,
            ArticleOption::Refresh, ArticleOption::Info};
}

const char *article_option_label(ArticleOption o)
{
    switch (o) {
    case ArticleOption::MarkRead: return "Mark as read";
    case ArticleOption::MarkUnread: return "Mark as unread";
    case ArticleOption::Save: return "Save for offline";
    case ArticleOption::Unsave: return "Remove from saved";
    case ArticleOption::MarkAllRead: return "Mark all read";
    case ArticleOption::Refresh: return "Refresh this feed";
    case ArticleOption::Info: return "Article info";
    }
    return "";
}

std::string article_info(const Article &a, const std::string &feed_name, std::int64_t now)
{
    std::string t = (a.title.empty() ? std::string("(no title)") : a.title) + "\n";
    t += feed_name;
    if (!a.author.empty()) t += kDot + "by " + a.author;
    t += "\n";
    if (a.dated && a.published > 0) t += fmt_datetime(a.published, now) + "\n";
    else t += "No date in the feed\n";
    if (!a.link.empty()) t += a.link + "\n";
    const std::string text = html_to_text(a.body_html());
    t += "Text: " + fmt_bytes(text.size()) + (a.has_full_text ? " (full text)" : " (summary)");
    if (!a.images.empty()) t += kDot + plural(static_cast<long>(a.images.size()), "image", "images");
    return t;
}

/* ------------------------------------------------------------------ Browse */

bool hidden_topic(const std::string &topic) { return topic == "podcasts"; }

std::string country_name(const std::string &code) { return country_label(code); }

std::vector<CatalogGroup> browse_countries(const Catalog &c)
{
    std::vector<CatalogGroup> out;
    for (CatalogGroup g : c.countries()) {
        std::size_t n = 0;
        for (const CatalogFeed *f : c.feeds_in(g.key, ""))
            if (!hidden_topic(f->topic)) ++n;
        if (n == 0) continue;
        g.feeds = n;
        out.push_back(g);
    }
    return out;
}

std::string country_languages(const Catalog &c, const std::string &country)
{
    std::map<std::string, int> n;
    for (const CatalogFeed *f : c.feeds_in(country, ""))
        if (!hidden_topic(f->topic) && !f->lang.empty()) ++n[f->lang];
    std::vector<std::pair<std::string, int>> v(n.begin(), n.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second) return a.second > b.second;
        return language_label(a.first) < language_label(b.first);
    });
    std::string s;
    for (const auto &kv : v) s += (s.empty() ? "" : ", ") + language_label(kv.first);
    return s;
}

std::vector<CatalogGroup> browse_topics(const Catalog &c, const std::string &country)
{
    std::vector<CatalogGroup> out;
    for (const CatalogGroup &g : c.topics_in_country(country))
        if (!hidden_topic(g.key)) out.push_back(g);
    return out;
}

std::vector<const CatalogFeed *> browse_feeds(const Catalog &c, const std::string &country, const std::string &topic)
{
    std::vector<const CatalogFeed *> out;
    for (const CatalogFeed *f : c.feeds_in(country, topic))
        if (!hidden_topic(f->topic)) out.push_back(f);
    return out;
}

std::vector<const CatalogFeed *> browse_search(const Catalog &c, const std::string &query, std::size_t limit)
{
    std::vector<const CatalogFeed *> out;
    if (trim(query).empty()) return out;
    for (const CatalogFeed *f : c.search(query, limit + 16)) {
        if (hidden_topic(f->topic)) continue;
        out.push_back(f);
        if (out.size() >= limit) break;
    }
    return out;
}

std::string feed_content_text(const CatalogFeed &f) { return f.content == "full" ? "full text" : "summary"; }

std::string search_line2(const CatalogFeed &f)
{
    return country_name(f.country) + kDot + topic_label(f.topic) + kDot + feed_content_text(f);
}

/* ------------------------------------------------------------------ Settings */

Choice text_size_choice(TextSize current)
{
    Choice c;
    c.title = "Text size";
    c.labels = {"Small", "Medium", "Large"};
    c.details = {"More text on the screen", "The usual size", "Easier to read at arm's length"};
    c.index = current == TextSize::Small ? 0 : current == TextSize::Large ? 2 : 1;
    return c;
}

TextSize text_size_from_index(int index) { return index <= 0 ? TextSize::Small : index >= 2 ? TextSize::Large : TextSize::Medium; }

std::string text_size_label(TextSize s) { return s == TextSize::Small ? "Small" : s == TextSize::Large ? "Large" : "Medium"; }

int reader_text_px(TextSize s, int caption_px, int body_px, int heading_px, int title_px, const std::function<int(int)> &snap)
{
    switch (s) {
    case TextSize::Small: return snap(caption_px);
    case TextSize::Large: return snap((heading_px + title_px + 1) / 2);
    default: return snap(body_px);
    }
}

const std::vector<int> &storage_cap_options()
{
    static const std::vector<int> v = {50, 100, 200, 500};
    return v;
}

Choice storage_cap_choice(int current_mb)
{
    Choice c;
    c.title = "Storage for saved articles";
    const std::vector<int> &o = storage_cap_options();
    int best = 0;
    for (std::size_t i = 0; i < o.size(); ++i) {
        c.labels.push_back(std::to_string(o[i]) + " MB");
        if (std::abs(o[i] - current_mb) < std::abs(o[static_cast<std::size_t>(best)] - current_mb)) best = static_cast<int>(i);
    }
    c.index = best;
    c.note = "Saving an article is refused once the saved articles reach this size.";
    return c;
}

int storage_cap_from_index(int index)
{
    const std::vector<int> &o = storage_cap_options();
    return o[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(o.size()) - 1))];
}

std::string storage_text(std::uint64_t used, int cap_mb)
{
    return fmt_bytes(used) + " of " + std::to_string(cap_mb) + " MB";
}

std::vector<std::string> opml_candidates(const std::vector<std::string> &names)
{
    std::vector<std::string> out;
    for (const std::string &n : names) {
        if (n.empty() || n[0] == '.') continue;
        if (n.size() > 5 && lower_ascii(n.substr(n.size() - 5)) == ".opml") out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string import_summary(const OpmlImportResult &r, std::size_t in_file)
{
    if (in_file == 0) return "Nothing to import: no feed in that file.";
    std::string s = "Imported " + plural(r.added, "feed", "feeds");
    if (r.duplicates) s += ", " + std::to_string(r.duplicates) + " already in your favorites";
    if (r.failed) s += ", " + std::to_string(r.failed) + " could not be added";
    return s + ".";
}

std::string launcher_text(const std::string &ref, const std::string &pin)
{
    const std::string shortref = ref.size() > 7 && ref != "unknown" ? ref.substr(0, 7) : ref;
    return shortref + " (" + pin + ")";
}

/* ------------------------------------------------------------------ the reader */

std::string block_text(const Block &b)
{
    switch (b.type) {
    case BlockType::ListItem: {
        if (b.number == 0 && b.ordered) return b.text;      // a continuation of the item above
        const std::string mark = b.ordered ? std::to_string(std::max(1, b.number)) + "." : std::string("\xE2\x80\xA2");
        return mark + "  " + b.text;
    }
    case BlockType::Image: return b.alt.empty() ? std::string("[image]") : "[image: " + b.alt + "]";
    case BlockType::Rule: return "";
    default: return b.text;
    }
}

std::string reader_caption(const std::string &feed_name, const Article &a, std::int64_t now)
{
    std::string s = feed_name;
    if (a.dated && a.published > 0) s += (s.empty() ? "" : kDot) + fmt_datetime(a.published, now);
    return s;
}

/* ------------------------------------------------------------------ images */

bool junk_image_url(const std::string &url)
{
    const std::string u = lower_ascii(url);
    static const char *const parts[] = {"/images/core/emoji/", "s.w.org/images/", "feeds.feedburner.com/~", "feeds.wordpress.com/1.0/",
                                        "pixel.wp.com/", "stats.wordpress.com/", "/wp-includes/images/smilies/", "gravatar.com/avatar/",
                                        "feedblitz.com/", "/share-button", "addtoany.com/"};
    for (const char *p : parts)
        if (u.find(p) != std::string::npos) return true;
    return false;
}

std::vector<ArticleImage> article_images(const std::vector<Block> &blocks, const Article &a, std::size_t max_fetch)
{
    std::vector<ArticleImage> out;
    std::vector<std::string> fetched;
    auto fetch_slot = [&](const std::string &url) {
        if (std::find(fetched.begin(), fetched.end(), url) != fetched.end()) return true;
        if (fetched.size() >= max_fetch) return false;
        fetched.push_back(url);
        return true;
    };
    const std::size_t n = std::min(blocks.size(), kReaderMaxBlocks);
    for (std::size_t i = 0; i < n; ++i) {
        const Block &b = blocks[i];
        if (b.type != BlockType::Image || b.url.empty() || junk_image_url(b.url)) continue;
        ArticleImage im;
        im.url = b.url;
        im.alt = b.alt;
        im.block = static_cast<int>(i);
        im.fetch = fetch_slot(b.url);
        out.push_back(std::move(im));
    }
    if (out.empty()) {
        for (const std::string &u : a.images) {
            if (u.empty() || junk_image_url(u)) continue;
            ArticleImage im;
            im.url = u;
            im.block = -1;
            im.fetch = fetch_slot(u);
            out.push_back(std::move(im));
            break;
        }
    }
    return out;
}

std::string image_caption(ImageState s)
{
    switch (s) {
    case ImageState::Loading: return "Loading\xE2\x80\xA6";
    case ImageState::Downloading: return "Downloading\xE2\x80\xA6";
    case ImageState::Unavailable: return "Image unavailable";
    case ImageState::TooLarge: return "Too large to show";
    case ImageState::Unsupported: return "Format not supported";
    case ImageState::Off: return "Images are off";
    case ImageState::NotLoaded: return "Not loaded";
    case ImageState::Ready:
    case ImageState::Hidden: break;
    }
    return "";
}

char image_state_letter(ImageState s)
{
    switch (s) {
    case ImageState::Loading: return 'L';
    case ImageState::Downloading: return 'D';
    case ImageState::Ready: return 'R';
    case ImageState::Unavailable: return 'U';
    case ImageState::TooLarge: return 'T';
    case ImageState::Unsupported: return 'S';
    case ImageState::Off: return 'O';
    case ImageState::NotLoaded: return 'N';
    case ImageState::Hidden: return 'H';
    }
    return '?';
}

int nearest_image(const std::vector<std::pair<int, int>> &tiles, int view_top, int view_h)
{
    const long centre = view_top + view_h / 2;
    int best = -1;
    long best_d = 0;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const int top = tiles[i].first, bottom = tiles[i].second;
        if (bottom <= top) continue;
        const int vis = std::min(bottom, view_top + view_h) - std::max(top, view_top);
        if (vis * 2 < std::min(bottom - top, view_h)) continue;          // less than half of it in view
        const long d = std::labs((top + bottom) / 2 - centre);
        if (best < 0 || d < best_d) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    return best;
}

std::string viewer_caption(int index, int count)
{
    std::string s = "Esc or tap to close";
    if (count > 1) s += kDot + std::to_string(index + 1) + " / " + std::to_string(count);
    return s;
}

/* ------------------------------------------------------------------ saved articles */

namespace {
std::string whole_mb(std::uint64_t bytes)
{
    return std::to_string((bytes + 512 * 1024) / (1024 * 1024));
}
} // namespace

std::string save_refusal_text(std::uint64_t used, int cap_mb)
{
    return "Saved articles use " + whole_mb(used) + " of " + std::to_string(cap_mb) + " MB. Delete some or raise the limit in Settings.";
}

std::string save_result_text(std::uint64_t bytes, int images_saved, int images_failed, bool images_off)
{
    if (images_off) return "Saved without images: they are off in Settings";
    if (images_failed > 0)
        return "Saved for offline, " + plural(images_failed, "image", "images") + " could not be downloaded";
    (void)images_saved;
    return "Saved for offline" + kDot + fmt_bytes(bytes);
}

std::string saved_row_line2(const SavedSummary &s, std::int64_t now)
{
    std::string t = s.feed_name.empty() ? std::string("Saved") : s.feed_name + kDot + "saved";
    const std::string age = fmt_age(s.saved_at, now);
    if (!age.empty()) t += " " + age;
    return t + kDot + fmt_bytes(s.bytes);
}

std::string saved_footer(std::size_t count, std::uint64_t bytes, int cap_mb)
{
    return plural(static_cast<long>(count), "article", "articles") + ", " + fmt_bytes(bytes) + " of " + std::to_string(cap_mb) + " MB";
}

std::vector<SavedOption> saved_options() { return {SavedOption::Remove, SavedOption::Info}; }

const char *saved_option_label(SavedOption o)
{
    switch (o) {
    case SavedOption::Remove: return "Remove from saved";
    case SavedOption::Info: return "Article info";
    }
    return "";
}

std::string options_hint() { return "Hold or press Ctrl+O for options"; }

std::string image_cache_text(std::uint64_t bytes) { return bytes == 0 ? std::string("empty") : fmt_bytes(bytes); }

} // namespace rssui
