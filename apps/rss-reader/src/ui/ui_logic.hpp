/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: the UI-independent logic of the pages. The texts of the rows and of the lines under the titles, the plain
 * words for a failure, the choices of Settings, the key rules of the dialogs (choice with arrows, Yes / No, menu), the
 * 3 s hold, the page column and the title that must fit beside the header's Esc. No LVGL, no I/O: everything here is
 * unit tested on the PC (tests/unit/ui_tests.cpp). The LVGL side is rss_app*.cpp.
 */
#pragma once

#include "catalog.hpp"
#include "html_text.hpp"
#include "keys.hpp"
#include "opml.hpp"
#include "refresh.hpp"
#include "store.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rssui {

/* ------------------------------------------------------------------ the pages */

enum class PageId { Home, Favorites, Articles, Reader, Browse, Saved, Add, Settings };
const char *page_name(PageId p);              /* "Favorites", "Browse feeds", ... (Home: "RSS Reader") */

/* ------------------------------------------------------------------ dialogs */

/* The choice dialog: the value in the middle, an arrow on each side (text size, storage cap). */
struct Choice {
    std::string title;
    std::vector<std::string> labels;
    std::vector<std::string> details;       /* optional second line per label */
    std::string note;
    int index = 0;
    bool step(int delta);                   /* clamps at both ends; true when the index moved */
    bool can_prev() const { return index > 0; }
    bool can_next() const { return index + 1 < static_cast<int>(labels.size()); }
    const std::string &label() const;
    std::string detail() const;
    std::string position() const;           /* "2 / 3" */
};
enum class ChoiceResult { None, Moved, Accept, Cancel };
/* Left / Right (also Up / Down) step, Enter accepts, Esc cancels. A repeat of Enter or Esc is ignored. */
ChoiceResult choice_key(const KeyEvent &e, Choice &c);

/* The Yes / No box. focus: 0 = No, 1 = Yes. Y and N answer at once, Left / Right / Tab move, Enter takes the focus, Esc is No. */
enum class ConfirmResult { None, Moved, Yes, No };
ConfirmResult confirm_key(const KeyEvent &e, int &focus);

/* A menu or a pick list: Up / Down (Left / Right) move, Enter picks, Esc cancels. */
enum class MenuResult { None, Moved, Accept, Cancel };
MenuResult menu_key(const KeyEvent &e, int &selected, int count);

/* A touch held on a row (or on the text of an article) for 3 s opens the options. The cue appears after kShowMs so a tap
 * or a scroll never flashes it; a move of more than kSlop pixels, a release or a lost press cancels (Mesh Hop's rule). */
class HoldTracker {
public:
    static constexpr std::uint64_t kShowMs = 500, kFireMs = 3000;
    static constexpr int kSlop = 16;
    void begin(const std::string &target, int x, int y, std::uint64_t now);
    void move(int x, int y);
    void cancel();
    bool active() const { return active_; }
    const std::string &target() const { return target_; }
    bool visible(std::uint64_t now) const { return active_ && now - start_ >= kShowMs; }
    double progress(std::uint64_t now) const;       /* 0..1 over kFireMs */
    /* True exactly once when the hold has lasted kFireMs; the click of the release is then to be swallowed. */
    bool poll(std::uint64_t now);
    bool take_swallow();

private:
    bool active_ = false, swallow_ = false;
    std::string target_;
    int x_ = 0, y_ = 0;
    std::uint64_t start_ = 0;
};

/* ------------------------------------------------------------------ layout */

/* The column the list pages are laid out in (Mesh Hop's page_column): one SPACE_L margin each side on the smallest
 * screen, two once the screen is that much wider, never wider than cap_w. */
struct PageColumn {
    int x = 0, w = 0;
};
PageColumn page_column(int screen_w, int inset, int min_w, int cap_w);

/* The header title for a path ("Browse", "France", "Technology") that fits in max_w px (width() measures a text): the
 * whole path joined with " > "; else the middle parts become one ellipsis ("Browse > ... > Technology"); else
 * "... > Technology"; else the last part alone (the toolkit's Page then ends it with dots). One element: give it to
 * Page::set_title as it is. */
std::string fit_title(const std::vector<std::string> &path, const std::function<int(const std::string &)> &width, int max_w);

/* ------------------------------------------------------------------ formatting */

/* How old: "now" (under a minute), "5 min", "3 h", "2 d" (under 2 weeks), else the date "Oct 3" (this year) or "Oct 3 2025".
 * 0 (unknown): "". A date in the future counts as "now". */
std::string fmt_age(std::int64_t when, std::int64_t now);
/* "Updated just now", "Updated 2 min ago", "Updated 3 h ago", "Updated 2 d ago"; 0: "Not updated yet". */
std::string fmt_updated(std::int64_t when, std::int64_t now);
/* Local time: "Oct 18, 08:12" this year, "Oct 18 2025, 08:12" otherwise; 0: "". */
std::string fmt_datetime(std::int64_t when, std::int64_t now);
/* "0 KB", "340 KB", "1.2 MB", "12 MB", "1.5 GB". */
std::string fmt_bytes(std::uint64_t bytes);
/* "1 feed", "6 feeds". */
std::string plural(long n, const char *one, const char *many);
/* "5 new", or "" for 0. */
std::string new_text(int unread);
/* By characters (UTF-8), with "..." added when cut. */
std::string truncate_text(const std::string &s, std::size_t max_chars);

/* ------------------------------------------------------------------ failures in plain words */

/* A subscription's last_error (Refresher's message: "network error: Could not resolve host", "HTTP 503", "timed out",
 * "not a feed" ...) in a few plain words for its row: "No network", "Timed out", "Server error (HTTP 503)", "Not a feed",
 * "Secure connection failed", "Feed too large", "Could not update". */
std::string fetch_problem(const std::string &last_error);
/* Why "Find feed" failed, for the Add a feed page: "No network: check the Wi-Fi and try again.", "Timed out: the site did
 * not answer.", "That is not a web address.", "No feed found: the page has no feed.", "Not a feed: ...". */
std::string add_failure_text(const rssreader::AddResult &r);
/* The outcome of one feed's refresh for a notice: "Updated: 3 new", "Up to date", "Not updated: No network". */
std::string refresh_result_text(const rssreader::RefreshResult &r);

/* ------------------------------------------------------------------ Home, Favorites */

/* The line under Home's title: "12 unread in your favorites", "All read in your 6 favorites", "No favorites yet". */
std::string home_caption(int unread, std::size_t feeds);
/* The line under the Favorites title: "6 feeds  ·  12 unread", "6 feeds  ·  all read", "No favorites yet". */
std::string favorites_caption(const std::vector<rssreader::Subscription> &subs);
/* The status on the Refresh bar: "Updating 3/6...", "Updated 2 min ago" (newest last_fetch), "Not updated yet". */
std::string refresh_status(bool running, std::size_t done, std::size_t total, const std::vector<rssreader::Subscription> &subs,
                           std::int64_t now);

struct FavRow {
    std::string id;
    std::string name;
    std::string line2;        /* "<topic or country>  ·  <age of newest>", or the problem of the last update */
    bool warn = false;        /* line2 is a problem (the warn tone) */
    std::string right;        /* "5 new" or "" */
    bool unread = false;      /* the blue dot at the left, the name bold and white; else the name grey */
};
/* The group shown on a favorite's second line: the catalog topic ("Technology"), else the country of its catalog entry,
 * else the OPML folder, else the site's host ("korben.info"). */
std::string favorite_group(const rssreader::Subscription &s, const rssreader::Catalog *catalog);
FavRow favorite_row(const rssreader::Subscription &s, const rssreader::Catalog *catalog, std::int64_t now);

/* The options of a favorite (a 3 s hold on its row, Ctrl+O), in this order: Move up (not on the first), Move down (not on
 * the last), Unpin, Mark all read, Refresh this feed, Details. */
enum class FavOption { MoveUp, MoveDown, Unpin, MarkAllRead, Refresh, Details };
std::vector<FavOption> favorite_options(std::size_t index, std::size_t count);
const char *fav_option_label(FavOption o);
/* The text of the Details box of a favorite. */
std::string favorite_details(const rssreader::Subscription &s, const rssreader::Catalog *catalog, std::int64_t now);

/* ------------------------------------------------------------------ the article list */

std::string articles_caption(int unread, std::size_t total, bool loading, bool updating);
struct ArticleRow {
    std::string id;
    std::string title;
    std::string age;          /* "4 h" */
    bool unread = false;      /* blue dot, bright title; else grey title, no dot */
    bool saved = false;       /* "  ·  saved" after the age, in the accent */
};
ArticleRow article_row(const rssreader::StoredArticle &a, bool saved, std::int64_t now);

/* The options of an article of the list: Mark as read or Mark as unread, Mark all read, Refresh this feed, Article info. */
enum class ArticleOption { MarkRead, MarkUnread, MarkAllRead, Refresh, Info };
std::vector<ArticleOption> article_options(bool read);
const char *article_option_label(ArticleOption o);
/* The Article info box: title, author, date, link, size of the text, images. */
std::string article_info(const rssreader::Article &a, const std::string &feed_name, std::int64_t now);

/* ------------------------------------------------------------------ Browse */

/* The countries with at least one text feed (podcasts are not shown: the app reads text feeds), International first,
 * then by name; their counts without the podcasts. */
std::vector<rssreader::CatalogGroup> browse_countries(const rssreader::Catalog &c);
/* The languages of a country's text feeds, by feed count then name: "French", "English, French". */
std::string country_languages(const rssreader::Catalog &c, const std::string &country);
std::vector<rssreader::CatalogGroup> browse_topics(const rssreader::Catalog &c, const std::string &country);
std::vector<const rssreader::CatalogFeed *> browse_feeds(const rssreader::Catalog &c, const std::string &country, const std::string &topic);
/* Search in the whole catalog (names, sites, topics), podcasts left out. */
std::vector<const rssreader::CatalogFeed *> browse_search(const rssreader::Catalog &c, const std::string &query, std::size_t limit = 60);
bool hidden_topic(const std::string &topic);              /* "podcasts" */
/* "full text" or "summary" (what the feed carries). */
std::string feed_content_text(const rssreader::CatalogFeed &f);
/* A search result's second line: "France  ·  Technology  ·  summary". */
std::string search_line2(const rssreader::CatalogFeed &f);
/* The country's name for the title: "International" for INT. */
std::string country_name(const std::string &code);

/* ------------------------------------------------------------------ Settings */

Choice text_size_choice(rssreader::TextSize current);
rssreader::TextSize text_size_from_index(int index);
std::string text_size_label(rssreader::TextSize s);       /* "Small", "Medium", "Large" */
/* The reading text size for a setting, from the layout service's text sizes: Small the CAPTION size, Medium BODY, Large
 * halfway between HEADING and TITLE (snap() rounds a size to one the font set has). */
int reader_text_px(rssreader::TextSize s, int caption_px, int body_px, int heading_px, int title_px, const std::function<int(int)> &snap);

const std::vector<int> &storage_cap_options();            /* 50, 100, 200, 500 MB */
Choice storage_cap_choice(int current_mb);
int storage_cap_from_index(int index);
/* "1.2 MB of 200 MB". */
std::string storage_text(std::uint64_t used, int cap_mb);
/* The OPML files of a folder listing (*.opml, any case), sorted. */
std::vector<std::string> opml_candidates(const std::vector<std::string> &names);
/* "Imported 4 feeds" / "Imported 4 feeds, 2 already there" / "Nothing to import: no feed in that file". */
std::string import_summary(const rssreader::OpmlImportResult &r, std::size_t in_file);
/* The launcher line of About: "f35c2d6 (pinned)". */
std::string launcher_text(const std::string &ref, const std::string &pin);

/* ------------------------------------------------------------------ the reader */

/* A block as one text: list items with their marker ("•  ", "3.  "), an image as "[image: alt]" (or "[image]"), a rule
 * as "". */
std::string block_text(const rssreader::Block &b);
/* The line under the reader's title: "Ars Technica  ·  Oct 18, 08:12" (the date left out when the feed gave none). */
std::string reader_caption(const std::string &feed_name, const rssreader::Article &a, std::int64_t now);
/* Up to max_blocks blocks are shown; the rest is cut ("The rest of this article is not shown."). */
constexpr std::size_t kReaderMaxBlocks = 400;

} // namespace rssui
