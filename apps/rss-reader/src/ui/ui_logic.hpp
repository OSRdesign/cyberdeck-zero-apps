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
/* The group shown on a favorite's second line, one rule: a catalog feed shows its topic ("Technology", "General"; never
 * its country), a feed added by its address shows its site's host ("korben.info", without "www."). A feed the catalog no
 * longer knows shows the topic it was pinned with, else its host. */
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

/* The options of an article of the list: Mark as read or Mark as unread, Save for offline or Remove from saved, Mark all
 * read, Refresh this feed, Article info. */
enum class ArticleOption { MarkRead, MarkUnread, Save, Unsave, MarkAllRead, Refresh, Info };
std::vector<ArticleOption> article_options(bool read, bool saved = false);
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

/* ------------------------------------------------------------------ images (phase 3) */

/* At most this many images of an article are downloaded (the others show "Not loaded"). */
constexpr std::size_t kMaxReaderImages = 6;

/* The pictures that are not content: WordPress emoji, share and comment counters, feed flare, statistics pixels. */
bool junk_image_url(const std::string &url);

/* An image the reader shows: the image blocks of the text in their order (junk left out), or, when the text has none,
 * the first image of the feed's own media (an enclosure, media:content or media:thumbnail) shown under the title: the
 * "lead" (block -1). `fetch`: one of the first kMaxReaderImages different addresses. */
struct ArticleImage {
    std::string url, alt;
    int block = -1;              /* index in the blocks, -1 for the lead image */
    bool fetch = true;
};
std::vector<ArticleImage> article_images(const std::vector<rssreader::Block> &blocks, const rssreader::Article &a,
                                         std::size_t max_fetch = kMaxReaderImages);

/* Where an image of the reader is. */
enum class ImageState { Loading, Downloading, Ready, Unavailable, TooLarge, Unsupported, Off, NotLoaded, Hidden };
/* The grey line of a placeholder: "Loading...", "Downloading...", "Image unavailable", "Too large to show", "Format not
 * supported", "Images are off", "Not loaded"; "" for Ready and Hidden. */
std::string image_caption(ImageState s);
/* One letter per state for the scripted tests: L D R U T S O N H. */
char image_state_letter(ImageState s);
/* The image nearest to the middle of the view (top .. top + height), among those at least half in view: its index in
 * `tiles` (top and bottom of each, in the same coordinates; a tile with bottom <= top is not shown), or -1. */
int nearest_image(const std::vector<std::pair<int, int>> &tiles, int view_top, int view_h);
/* The full-screen view's line: "Esc or tap to close  ·  1 / 2". */
std::string viewer_caption(int index, int count);

/* ------------------------------------------------------------------ saved articles (phase 3) */

/* The refusal when the cap would be passed: "Saved articles use 198 of 200 MB. Delete some or raise the limit in Settings." */
std::string save_refusal_text(std::uint64_t used, int cap_mb);
/* What a save did: "Saved for offline  ·  1.2 MB", "Saved without images: they are off in Settings", "Saved for offline,
 * 1 image could not be downloaded". */
std::string save_result_text(std::uint64_t bytes, int images_saved, int images_failed, bool images_off);
/* A saved article's second line: "Ars Technica  ·  saved 2 h  ·  1.2 MB". */
std::string saved_row_line2(const rssreader::SavedSummary &s, std::int64_t now);
/* The line at the bottom of Saved articles: "3 articles, 4.2 MB of 200 MB". */
std::string saved_footer(std::size_t count, std::uint64_t bytes, int cap_mb);
/* The options of a saved article (Saved articles): Remove from saved, Article info. */
enum class SavedOption { Remove, Info };
std::vector<SavedOption> saved_options();
const char *saved_option_label(SavedOption o);
/* The first-run hint under the titles until the options were opened once. */
std::string options_hint();
/* Settings > Clear image cache: its value, "3.4 MB" ("empty" when nothing is cached). */
std::string image_cache_text(std::uint64_t bytes);

} // namespace rssui
