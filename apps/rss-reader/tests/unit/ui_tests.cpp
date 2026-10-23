// Host tests of RSS Reader's UI-independent logic (src/ui: ui_logic, keys, key_bridge, jobs) and of the package files
// (.desktop, app.json, the icon, the catalog copy). No LVGL. Built and run by run_tests.sh after the core tests.
#include "test_common.hpp"

#include "catalog.hpp"
#include "fsutil.hpp"
#include "jobs.hpp"
#include "json.hpp"
#include "key_bridge.hpp"
#include "keys.hpp"
#include "ui_logic.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <thread>

using namespace rssreader;
using namespace rssui;
using namespace rssreader_test;

int g_fail = 0, g_checks = 0;

#ifndef APP_DIR
#define APP_DIR "../.."
#endif

namespace {

constexpr std::int64_t kNow = 1792339200;   // 2026-10-18 16:00 UTC

KeyEvent key(Key k) { KeyEvent e; e.key = k; return e; }
KeyEvent ch(const std::string &t, bool ctrl = false) { KeyEvent e; e.key = Key::Char; e.text = t; e.ctrl = ctrl; return e; }

void test_keys()
{
    KeyTranslator t(Layout::US);
    KeyEvent e;
    CHECK(t.feed(30, 1, e) && e.key == Key::Char && e.text == "a");
    CHECK(!t.feed(42, 1, e));                       // left shift down
    CHECK(t.feed(30, 1, e) && e.text == "A");
    CHECK(!t.feed(42, 0, e));
    CHECK(!t.feed(29, 1, e));                       // ctrl down
    CHECK(t.feed(24, 1, e) && e.ctrl && e.text == "o");
    CHECK(is_ctrl_letter(e, 'o') && is_ctrl_letter(e, 'O') && !is_ctrl_letter(e, 'r'));
    CHECK(!is_typing(e));
    CHECK(!t.feed(29, 0, e));
    CHECK(t.feed(1, 1, e) && e.key == Key::Esc);
    CHECK(t.feed(105, 2, e) && e.key == Key::Left && e.repeat);
    CHECK(t.feed(63, 1, e) && e.key == Key::Function && e.fn == 5);
    KeyTranslator fr(Layout::FR);
    CHECK(fr.feed(16, 1, e) && e.text == "a");      // AZERTY: the Q key types a
    CHECK(layout_from_name("fr(azerty)") == Layout::FR && layout_from_name("de") == Layout::US);
    CHECK_EQ(xkb_layout_from_text("XKBMODEL=x\nXKBLAYOUT=\"fr\"\n"), std::string("fr"));
    // the bridge: a queued key (no keysym) brings its own text and modifiers
    KeyBridge b;
    CHECK(b.feed(0, KeyBridge::kPressed, 0, 0, "\xC3\xA9", e) && e.key == Key::Char && e.text == "\xC3\xA9");
    CHECK(!b.feed(0, KeyBridge::kReleased, 0, 0, "\xC3\xA9", e));
    CHECK(b.feed(24, KeyBridge::kPressed, KeyBridge::kModCtrl, 0, "o", e) && is_ctrl_letter(e, 'o'));
    CHECK(b.feed(15, KeyBridge::kPressed, KeyBridge::kModShift, 0, "", e) && e.key == Key::BackTab);
    CHECK(is_typing(ch("x")) && !is_typing(ch("x", true)));
}

void test_dialog_keys()
{
    Choice c = text_size_choice(TextSize::Medium);
    CHECK(c.index == 1 && c.label() == "Medium" && c.position() == "2 / 3");
    CHECK(choice_key(key(Key::Right), c) == ChoiceResult::Moved && c.label() == "Large");
    CHECK(choice_key(key(Key::Right), c) == ChoiceResult::None);
    CHECK(choice_key(key(Key::Home), c) == ChoiceResult::Moved && c.index == 0 && !c.can_prev() && c.can_next());
    CHECK(choice_key(key(Key::Enter), c) == ChoiceResult::Accept);
    CHECK(choice_key(key(Key::Esc), c) == ChoiceResult::Cancel);
    KeyEvent rep = key(Key::Enter);
    rep.repeat = true;
    CHECK(choice_key(rep, c) == ChoiceResult::None);
    int focus = 0;
    CHECK(confirm_key(key(Key::Right), focus) == ConfirmResult::Moved && focus == 1);
    CHECK(confirm_key(key(Key::Enter), focus) == ConfirmResult::Yes);
    CHECK(confirm_key(key(Key::Tab), focus) == ConfirmResult::Moved && focus == 0);
    CHECK(confirm_key(key(Key::Enter), focus) == ConfirmResult::No);
    CHECK(confirm_key(ch("y"), focus) == ConfirmResult::Yes && confirm_key(ch("N"), focus) == ConfirmResult::No);
    CHECK(confirm_key(key(Key::Esc), focus) == ConfirmResult::No);
    int sel = 0;
    CHECK(menu_key(key(Key::Up), sel, 3) == MenuResult::None && sel == 0);
    CHECK(menu_key(key(Key::Down), sel, 3) == MenuResult::Moved && sel == 1);
    CHECK(menu_key(key(Key::End), sel, 3) == MenuResult::Moved && sel == 2);
    CHECK(menu_key(key(Key::Down), sel, 3) == MenuResult::None && sel == 2);
    CHECK(menu_key(key(Key::Enter), sel, 3) == MenuResult::Accept);
    CHECK(menu_key(key(Key::Esc), sel, 0) == MenuResult::Cancel);
}

void test_hold()
{
    HoldTracker h;
    h.begin("row", 100, 100, 1000);
    CHECK(h.active() && !h.visible(1200) && h.visible(1600));
    CHECK(!h.poll(3999) && h.poll(4000) && !h.active());
    CHECK(h.take_swallow() && !h.take_swallow());
    h.begin("row", 100, 100, 0);
    h.move(110, 112);
    CHECK(h.active());
    h.move(100, 130);                                // a drag: no hold
    CHECK(!h.active() && !h.poll(5000));
    h.begin("x", 0, 0, 0);
    h.cancel();
    CHECK(!h.poll(5000) && !h.take_swallow());
    h.begin("x", 0, 0, 0);
    CHECK(h.progress(1500) > 0.49 && h.progress(1500) < 0.51);
}

void test_layout_rules()
{
    PageColumn c = page_column(640, 20, 640, 960);
    CHECK(c.x == 20 && c.w == 600);
    c = page_column(800, 20, 640, 960);
    CHECK(c.x == 40 && c.w == 720);
    c = page_column(1280, 20, 640, 960);
    CHECK(c.w == 960 && c.x == 160);
    auto w = [](const std::string &s) { return static_cast<int>(s.size()) * 10; };     // 10 px a byte
    CHECK_EQ(fit_title({"Browse"}, w, 50), std::string("Browse"));
    CHECK_EQ(fit_title({"Browse", "France", "Tech"}, w, 400), std::string("Browse > France > Tech"));
    CHECK_EQ(fit_title({"Browse", "International", "Technology"}, w, 260), std::string("Browse > \xE2\x80\xA6 > Technology"));
    CHECK_EQ(fit_title({"Browse", "International", "Technology"}, w, 180), std::string("\xE2\x80\xA6 > Technology"));
    CHECK_EQ(fit_title({"Browse", "International", "Technology"}, w, 90), std::string("Technology"));
    CHECK_EQ(fit_title({"Browse", "France"}, w, 130), std::string("\xE2\x80\xA6 > France"));
    CHECK_EQ(fit_title({"Browse", "France"}, w, 100), std::string("France"));
    CHECK_EQ(fit_title({}, w, 100), std::string(""));
}

void test_formatting()
{
    CHECK_EQ(fmt_age(kNow - 30, kNow), std::string("now"));
    CHECK_EQ(fmt_age(kNow + 300, kNow), std::string("now"));
    CHECK_EQ(fmt_age(kNow - 14 * 60, kNow), std::string("14 min"));
    CHECK_EQ(fmt_age(kNow - 3600, kNow), std::string("1 h"));
    CHECK_EQ(fmt_age(kNow - 2 * 86400, kNow), std::string("2 d"));
    CHECK_EQ(fmt_age(kNow - 20 * 86400, kNow), std::string("Sep 28"));
    CHECK_EQ(fmt_age(kNow - 400 * 86400, kNow), std::string("Sep 13 2025"));
    CHECK_EQ(fmt_age(0, kNow), std::string(""));
    CHECK_EQ(fmt_updated(0, kNow), std::string("Not updated yet"));
    CHECK_EQ(fmt_updated(kNow - 10, kNow), std::string("Updated just now"));
    CHECK_EQ(fmt_updated(kNow - 120, kNow), std::string("Updated 2 min ago"));
    CHECK_EQ(fmt_updated(kNow - 3 * 3600, kNow), std::string("Updated 3 h ago"));
    CHECK_EQ(fmt_updated(kNow - 3 * 86400, kNow), std::string("Updated 3 d ago"));
    CHECK_EQ(fmt_datetime(kNow - 7 * 3600 - 48 * 60, kNow), std::string("Oct 18, 08:12"));
    CHECK_EQ(fmt_datetime(kNow - 400 * 86400, kNow), std::string("Sep 13 2025, 16:00"));
    CHECK_EQ(fmt_bytes(0), std::string("0 KB"));
    CHECK_EQ(fmt_bytes(340 * 1024), std::string("340 KB"));
    CHECK_EQ(fmt_bytes(1258291), std::string("1.2 MB"));
    CHECK_EQ(fmt_bytes(12ull * 1024 * 1024), std::string("12 MB"));
    CHECK_EQ(fmt_bytes(1610612736ull), std::string("1.5 GB"));
    CHECK_EQ(plural(1, "feed", "feeds"), std::string("1 feed"));
    CHECK_EQ(plural(6, "feed", "feeds"), std::string("6 feeds"));
    CHECK_EQ(new_text(5), std::string("5 new"));
    CHECK_EQ(new_text(0), std::string(""));
    CHECK_EQ(truncate_text("caf\xC3\xA9 au lait", 4), std::string("caf\xC3\xA9..."));
    CHECK_EQ(truncate_text("short", 10), std::string("short"));
    CHECK_EQ(storage_text(1258291, 200), std::string("1.2 MB of 200 MB"));
    CHECK_EQ(launcher_text("f35c2d6fd2f9fba9864bd154378c105d74c6d510", "pinned"), std::string("f35c2d6 (pinned)"));
    CHECK_EQ(launcher_text("unknown", "unchecked"), std::string("unknown (unchecked)"));
}

void test_failures()
{
    CHECK_EQ(fetch_problem(""), std::string(""));
    CHECK_EQ(fetch_problem("network error: Could not resolve host"), std::string("No network"));
    CHECK_EQ(fetch_problem("timed out"), std::string("Timed out"));
    CHECK_EQ(fetch_problem("HTTP 503"), std::string("Server error (HTTP 503)"));
    CHECK_EQ(fetch_problem("not a feed"), std::string("Not a feed any more"));
    CHECK_EQ(fetch_problem("secure connection failed: certificate expired"), std::string("Secure connection failed"));
    CHECK_EQ(fetch_problem("feed too big"), std::string("Feed too large"));
    CHECK_EQ(fetch_problem("could not save the articles"), std::string("Could not update"));
    AddResult r;
    r.status = AddResult::Status::BadInput;
    CHECK(add_failure_text(r).rfind("That is not a web address", 0) == 0);
    r.status = AddResult::Status::FetchFailed;
    r.error = FetchError::Network;
    CHECK(add_failure_text(r).rfind("No network", 0) == 0);
    r.error = FetchError::Timeout;
    CHECK(add_failure_text(r).rfind("Timed out", 0) == 0);
    r.error = FetchError::Http;
    r.message = "HTTP 404";
    CHECK_EQ(add_failure_text(r), std::string("The site answered with an error (HTTP 404)."));
    r.error = FetchError::Cancelled;
    CHECK_EQ(add_failure_text(r), std::string("Stopped."));
    r.status = AddResult::Status::NoFeedFound;
    r.message = "no feed found at that address";
    CHECK(add_failure_text(r).rfind("No feed found", 0) == 0);
    r.message = "the feed has no articles";
    CHECK_EQ(add_failure_text(r), std::string("That feed has no articles yet."));
    RefreshResult rr;
    rr.status = RefreshResult::Status::Updated;
    rr.new_articles = 3;
    CHECK_EQ(refresh_result_text(rr), std::string("Updated: 3 new"));
    rr.new_articles = 0;
    CHECK_EQ(refresh_result_text(rr), std::string("Up to date"));
    rr.status = RefreshResult::Status::NotModified;
    CHECK_EQ(refresh_result_text(rr), std::string("Up to date"));
    rr.status = RefreshResult::Status::Failed;
    rr.message = "network error: x";
    CHECK_EQ(refresh_result_text(rr), std::string("Not updated: No network"));
    rr.error = FetchError::Cancelled;
    CHECK_EQ(refresh_result_text(rr), std::string("Stopped"));
}

Catalog load_catalog()
{
    Catalog c;
    CHECK(c.load_file(std::string(APP_DIR) + "/data/catalog.json"));
    return c;
}

void test_favorites()
{
    const Catalog cat = load_catalog();
    Subscription s;
    s.id = "us-ars-technica";
    s.name = "Ars Technica";
    s.url = "https://feeds.arstechnica.com/arstechnica/index";
    s.unread = 3;
    s.total = 12;
    s.newest = kNow - 3600;
    s.last_fetch = kNow - 120;
    FavRow r = favorite_row(s, &cat, kNow);
    CHECK(r.unread && r.right == "3 new" && !r.warn);
    CHECK_EQ(r.line2, std::string("Technology  \xC2\xB7  1 h"));
    s.unread = 0;
    r = favorite_row(s, &cat, kNow);
    CHECK(!r.unread && r.right.empty());
    s.last_error = "network error: Could not resolve host";
    r = favorite_row(s, &cat, kNow);
    CHECK(r.warn && r.line2 == "Not updated: No network  \xC2\xB7  1 h");
    // one rule (phase 3): a catalog feed shows its topic, never its country ("General" for a general one); a feed added by
    // its address shows its site's host, whatever its OPML folder
    Subscription g;
    g.id = "gb-bbc-news-top-stories";
    g.name = "BBC News";
    CHECK_EQ(favorite_group(g, &cat), std::string("General"));
    g.newest = kNow - 14 * 60;
    CHECK_EQ(favorite_row(g, &cat, kNow).line2, std::string("General  \xC2\xB7  14 min"));
    Subscription c;
    c.id = "c-0123";
    c.custom = true;
    c.url = "https://www.example.org/feed.xml";
    CHECK_EQ(favorite_group(c, &cat), std::string("example.org"));
    c.group = "Tech / Linux";
    CHECK_EQ(favorite_group(c, &cat), std::string("example.org"));
    c.site = "https://korben.info/";
    CHECK_EQ(favorite_group(c, &cat), std::string("korben.info"));
    Subscription gone;                                  // pinned from the catalog, no longer in it: the topic it was pinned with
    gone.id = "xx-removed-feed";
    gone.url = "https://removed.example.net/rss";
    gone.group = "science";
    CHECK_EQ(favorite_group(gone, &cat), std::string("Science"));
    gone.group.clear();
    CHECK_EQ(favorite_group(gone, &cat), std::string("removed.example.net"));
    g.newest = 0;
    r = favorite_row(c, &cat, kNow);
    CHECK(r.line2.find("not updated yet") != std::string::npos);
    std::vector<Subscription> subs = {s, g};
    subs[0].unread = 4;
    CHECK_EQ(favorites_caption(subs), std::string("2 feeds  \xC2\xB7  4 unread"));
    subs[0].unread = 0;
    CHECK_EQ(favorites_caption(subs), std::string("2 feeds  \xC2\xB7  all read"));
    CHECK_EQ(favorites_caption({}), std::string("No favorites yet"));
    CHECK_EQ(home_caption(12, 6), std::string("12 unread in your favorites"));
    CHECK_EQ(home_caption(0, 6), std::string("All read in your 6 favorites"));
    CHECK_EQ(home_caption(0, 0), std::string("No favorites yet"));
    CHECK_EQ(refresh_status(true, 2, 6, subs, kNow), std::string("Updating 3/6\xE2\x80\xA6"));
    CHECK_EQ(refresh_status(true, 6, 6, subs, kNow), std::string("Updating 6/6\xE2\x80\xA6"));
    CHECK_EQ(refresh_status(false, 0, 0, subs, kNow), std::string("Updated 2 min ago"));
    std::vector<FavOption> o = favorite_options(0, 3);
    CHECK(o.size() == 5 && o[0] == FavOption::MoveDown && o.back() == FavOption::Details);
    o = favorite_options(2, 3);
    CHECK(o.size() == 5 && o[0] == FavOption::MoveUp && o[1] == FavOption::Unpin);
    o = favorite_options(1, 3);
    CHECK(o.size() == 6 && o[0] == FavOption::MoveUp && o[1] == FavOption::MoveDown);
    o = favorite_options(0, 1);
    CHECK(o.size() == 4 && o[0] == FavOption::Unpin);
    CHECK(std::string(fav_option_label(FavOption::Refresh)) == "Refresh this feed");
    const std::string d = favorite_details(s, &cat, kNow);
    CHECK(d.find("United States") != std::string::npos && d.find("Feed: https://feeds.arstechnica.com") != std::string::npos);
    CHECK(d.find("Last try failed: No network") != std::string::npos);
}

void test_articles()
{
    StoredArticle a;
    a.article.id = "x1";
    a.article.title = "A title";
    a.article.published = kNow - 4 * 3600;
    ArticleRow r = article_row(a, true, kNow);
    CHECK(r.unread && r.saved && r.age == "4 h" && r.title == "A title");
    a.read = true;
    a.article.title.clear();
    r = article_row(a, false, kNow);
    CHECK(!r.unread && !r.saved && r.title == "(no title)");
    CHECK_EQ(articles_caption(3, 25, false, false), std::string("3 unread  \xC2\xB7  25 articles"));
    CHECK_EQ(articles_caption(0, 1, false, true), std::string("0 unread  \xC2\xB7  1 article  \xC2\xB7  updating\xE2\x80\xA6"));
    CHECK_EQ(articles_caption(0, 0, false, false), std::string("No articles yet"));
    CHECK_EQ(articles_caption(0, 0, true, false), std::string("Loading\xE2\x80\xA6"));
    std::vector<ArticleOption> o = article_options(false);
    CHECK(o.size() == 5 && o[0] == ArticleOption::MarkRead && o[1] == ArticleOption::Save && o[4] == ArticleOption::Info);
    o = article_options(true, true);
    CHECK(o[0] == ArticleOption::MarkUnread && o[1] == ArticleOption::Unsave);
    CHECK_EQ(std::string(article_option_label(ArticleOption::Save)), std::string("Save for offline"));
    CHECK_EQ(std::string(article_option_label(ArticleOption::Unsave)), std::string("Remove from saved"));
    Article art;
    art.title = "Kernel fix";
    art.author = "Jon";
    art.published = kNow - 7 * 3600 - 48 * 60;
    art.link = "https://example.com/a";
    art.content_html = "<p>Hello world</p>";
    art.has_full_text = true;
    art.images = {"https://example.com/i.png"};
    const std::string info = article_info(art, "Ars Technica", kNow);
    CHECK(info.find("Kernel fix\nArs Technica  \xC2\xB7  by Jon\nOct 18, 08:12\nhttps://example.com/a\nText: 1 KB (full text)  \xC2\xB7  1 image") == 0);
    art.dated = false;
    CHECK(article_info(art, "X", kNow).find("No date in the feed") != std::string::npos);
    art.dated = true;
    CHECK_EQ(reader_caption("Ars Technica", art, kNow), std::string("Ars Technica  \xC2\xB7  Oct 18, 08:12"));
    art.dated = false;
    CHECK_EQ(reader_caption("Ars Technica", art, kNow), std::string("Ars Technica"));
}

void test_browse()
{
    const Catalog cat = load_catalog();
    const std::vector<CatalogGroup> countries = browse_countries(cat);
    CHECK(!countries.empty() && countries[0].key == "INT" && countries[0].label == "International");
    std::size_t total = 0, podcasts = 0;
    for (const CatalogFeed &f : cat.all()) podcasts += f.topic == "podcasts";
    for (const CatalogGroup &g : countries) total += g.feeds;
    CHECK(podcasts > 0 && total == cat.size() - podcasts);
    CHECK_EQ(country_languages(cat, "CA"), std::string("English, French"));
    CHECK_EQ(country_languages(cat, "FR"), std::string("French"));
    for (const CatalogGroup &c : countries)
        for (const CatalogGroup &t : browse_topics(cat, c.key)) CHECK(t.key != "podcasts");
    for (const CatalogGroup &c : countries) {
        for (const CatalogGroup &t : browse_topics(cat, c.key)) {
            const std::vector<const CatalogFeed *> feeds = browse_feeds(cat, c.key, t.key);
            CHECK(feeds.size() == t.feeds);
        }
    }
    const std::vector<const CatalogFeed *> s = browse_search(cat, "linux");
    CHECK(!s.empty());
    for (const CatalogFeed *f : s) CHECK(f->topic != "podcasts");
    CHECK(browse_search(cat, "   ").empty());
    const CatalogFeed *korben = cat.find("fr-korben");
    CHECK(korben && feed_content_text(*korben) == "full text" && search_line2(*korben) == "France  \xC2\xB7  Technology  \xC2\xB7  full text");
    CHECK(hidden_topic("podcasts") && !hidden_topic("linux"));
    CHECK_EQ(country_name("INT"), std::string("International"));
}

void test_settings_rules()
{
    CHECK(text_size_from_index(0) == TextSize::Small && text_size_from_index(1) == TextSize::Medium && text_size_from_index(5) == TextSize::Large);
    CHECK_EQ(text_size_label(TextSize::Large), std::string("Large"));
    auto snap = [](int px) { return px % 2 ? px + 1 : px; };
    CHECK(reader_text_px(TextSize::Small, 18, 24, 28, 32, snap) == 18);
    CHECK(reader_text_px(TextSize::Medium, 18, 24, 28, 32, snap) == 24);
    CHECK(reader_text_px(TextSize::Large, 18, 24, 28, 32, snap) == 30);
    Choice c = storage_cap_choice(200);
    CHECK(c.labels.size() == 4 && c.label() == "200 MB");
    CHECK(storage_cap_choice(120).label() == "100 MB");
    CHECK(storage_cap_choice(4000).label() == "500 MB");
    CHECK(storage_cap_from_index(0) == 50 && storage_cap_from_index(3) == 500 && storage_cap_from_index(9) == 500);
    const std::vector<std::string> files = opml_candidates({"b.OPML", ".hidden.opml", "a.opml", "notes.txt", "x.opml.bak", "opml"});
    CHECK(files.size() == 2 && files[0] == "a.opml" && files[1] == "b.OPML");
    OpmlImportResult r;
    r.added = 2;
    r.duplicates = 1;
    CHECK_EQ(import_summary(r, 3), std::string("Imported 2 feeds, 1 already in your favorites."));
    CHECK_EQ(import_summary(OpmlImportResult(), 0), std::string("Nothing to import: no feed in that file."));
}

void test_reader_blocks()
{
    const std::vector<Block> b = html_to_blocks("<h2>T</h2><ul><li>one</li><li>two</li></ul><ol><li>a</li><li>b</li></ol>"
                                                "<img src='https://x.org/p.png' alt='A photo'><img src='https://x.org/q.png'><hr><p>end</p>",
                                                "https://x.org/");
    std::vector<std::string> t;
    for (const Block &x : b) t.push_back(block_text(x));
    CHECK(t.size() >= 9);
    CHECK_EQ(t[0], std::string("T"));
    CHECK_EQ(t[1], std::string("\xE2\x80\xA2  one"));
    CHECK_EQ(t[3], std::string("1.  a"));
    CHECK_EQ(t[4], std::string("2.  b"));
    CHECK_EQ(t[5], std::string("[image: A photo]"));
    CHECK_EQ(t[6], std::string("[image]"));
    CHECK_EQ(t[7], std::string(""));
    CHECK_EQ(t[8], std::string("end"));
}

/* Phase 3: which images an article shows, their states, the keys' image, the full-screen line. */
void test_images()
{
    CHECK(junk_image_url("https://s.w.org/images/core/emoji/15.0.3/72x72/1f600.png"));
    CHECK(junk_image_url("https://feeds.feedburner.com/~ff/Example?d=yIl2AUoC8zA"));
    CHECK(junk_image_url("https://pixel.wp.com/b.gif?x=1"));
    CHECK(!junk_image_url("https://cdn.arstechnica.net/2026/10/scheduler.jpg"));
    const std::string html = "<p>a</p><img src='https://x.org/1.jpg' alt='One'><p>b</p><img src='https://s.w.org/images/core/emoji/x.png'>"
                             "<img src='https://x.org/2.png'><img src='https://x.org/1.jpg' alt='One again'><img src='https://x.org/3.jpg'>"
                             "<img src='https://x.org/4.jpg'><img src='https://x.org/5.jpg'><img src='https://x.org/6.jpg'><img src='https://x.org/7.jpg'>";
    Article a;
    a.images = {"https://x.org/lead.jpg"};
    const std::vector<Block> blocks = html_to_blocks(html, "https://x.org/");
    std::vector<ArticleImage> im = article_images(blocks, a);
    CHECK_EQ(im.size(), std::size_t(8));               // the emoji left out; the same address twice is two tiles
    if (im.size() == 8) {
        CHECK(im[0].url == "https://x.org/1.jpg" && im[0].alt == "One" && im[0].fetch && blocks[static_cast<std::size_t>(im[0].block)].type == BlockType::Image);
        CHECK(im[2].url == "https://x.org/1.jpg" && im[2].fetch);                     // a repeat: no new download slot
        CHECK(im[6].url == "https://x.org/6.jpg" && im[6].fetch);                     // the sixth different address
        CHECK(im[7].url == "https://x.org/7.jpg" && !im[7].fetch);                    // the seventh: not loaded
    }
    // no picture in the text: the feed's own (enclosure, media), under the title
    im = article_images(html_to_blocks("<p>text only</p>"), a);
    CHECK(im.size() == 1 && im[0].block == -1 && im[0].url == "https://x.org/lead.jpg" && im[0].fetch);
    a.images = {"https://s.w.org/images/core/emoji/x.png", "https://x.org/second.jpg"};
    im = article_images(html_to_blocks("<p>text only</p>"), a);
    CHECK(im.size() == 1 && im[0].url == "https://x.org/second.jpg");
    a.images.clear();
    CHECK(article_images(html_to_blocks("<p>none</p>"), a).empty());
    CHECK(article_images(blocks, a, 2)[2].fetch && !article_images(blocks, a, 2)[3].fetch);
    // the captions and the letters of the tests
    CHECK_EQ(image_caption(ImageState::Downloading), std::string("Downloading\xE2\x80\xA6"));
    CHECK_EQ(image_caption(ImageState::Unavailable), std::string("Image unavailable"));
    CHECK_EQ(image_caption(ImageState::TooLarge), std::string("Too large to show"));
    CHECK_EQ(image_caption(ImageState::Unsupported), std::string("Format not supported"));
    CHECK_EQ(image_caption(ImageState::Off), std::string("Images are off"));
    CHECK_EQ(image_caption(ImageState::NotLoaded), std::string("Not loaded"));
    CHECK(image_caption(ImageState::Ready).empty());
    std::string letters;
    for (ImageState st : {ImageState::Loading, ImageState::Downloading, ImageState::Ready, ImageState::Unavailable, ImageState::TooLarge,
                          ImageState::Unsupported, ImageState::Off, ImageState::NotLoaded, ImageState::Hidden})
        letters += image_state_letter(st);
    CHECK_EQ(letters, std::string("LDRUTSONH"));
    // the image nearest the middle of the view (0..400: the middle is 200), at least half in view
    CHECK_EQ(nearest_image({{0, 100}, {150, 250}, {300, 500}}, 0, 400), 1);
    CHECK_EQ(nearest_image({{0, 100}, {150, 250}, {300, 500}}, 200, 400), 2);
    CHECK_EQ(nearest_image({{-300, -100}, {0, 0}}, 0, 400), -1);             // above the view; a hidden tile
    CHECK_EQ(nearest_image({{350, 700}}, 0, 400), -1);                        // less than half in view
    CHECK_EQ(nearest_image({{-100, 900}}, 0, 400), 0);                        // taller than the view: half of the view counts
    CHECK_EQ(nearest_image({}, 0, 400), -1);
    CHECK_EQ(viewer_caption(0, 2), std::string("Esc or tap to close  \xC2\xB7  1 / 2"));
    CHECK_EQ(viewer_caption(0, 1), std::string("Esc or tap to close"));
}

/* Phase 3: the texts of Save for offline and of Saved articles. */
void test_saved_texts()
{
    CHECK_EQ(save_refusal_text(198ull * 1024 * 1024 + 300 * 1024, 200),
             std::string("Saved articles use 198 of 200 MB. Delete some or raise the limit in Settings."));
    CHECK_EQ(save_refusal_text(9ull * 1024 * 1024 + 900 * 1024, 10), std::string("Saved articles use 10 of 10 MB. Delete some or raise the limit in Settings."));
    CHECK_EQ(save_result_text(1258291, 2, 0, false), std::string("Saved for offline  \xC2\xB7  1.2 MB"));
    CHECK_EQ(save_result_text(2048, 0, 0, true), std::string("Saved without images: they are off in Settings"));
    CHECK_EQ(save_result_text(2048, 1, 1, false), std::string("Saved for offline, 1 image could not be downloaded"));
    CHECK_EQ(save_result_text(2048, 0, 3, false), std::string("Saved for offline, 3 images could not be downloaded"));
    SavedSummary s;
    s.feed_name = "Ars Technica";
    s.saved_at = kNow - 2 * 3600;
    s.bytes = 1258291;
    CHECK_EQ(saved_row_line2(s, kNow), std::string("Ars Technica  \xC2\xB7  saved 2 h  \xC2\xB7  1.2 MB"));
    s.feed_name.clear();
    s.saved_at = 0;
    CHECK_EQ(saved_row_line2(s, kNow), std::string("Saved  \xC2\xB7  1.2 MB"));
    CHECK_EQ(saved_footer(3, 4404019, 200), std::string("3 articles, 4.2 MB of 200 MB"));
    CHECK_EQ(saved_footer(1, 2048, 50), std::string("1 article, 2 KB of 50 MB"));
    const std::vector<SavedOption> o = saved_options();
    CHECK(o.size() == 2 && o[0] == SavedOption::Remove && o[1] == SavedOption::Info);
    CHECK_EQ(std::string(saved_option_label(SavedOption::Remove)), std::string("Remove from saved"));
    CHECK_EQ(options_hint(), std::string("Hold or press Ctrl+O for options"));
    CHECK_EQ(image_cache_text(0), std::string("empty"));
    CHECK_EQ(image_cache_text(3565158), std::string("3.4 MB"));
}

void test_jobs()
{
    Jobs jobs;
    std::atomic<int> runs{0};
    int seen = 0;
    jobs.start([&](Jobs::Token &) {
        runs++;
        jobs.post([&] { seen += 1; });
    });
    for (int i = 0; i < 200 && seen == 0; ++i) {
        jobs.run_posted();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(seen == 1 && runs == 1);
    // a job that waits for its cancel flag ends when asked
    Jobs::Ptr t = jobs.start([](Jobs::Token &tok) {
        while (!tok.cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });
    CHECK(jobs.running() >= 1);
    t->cancel.store(true);
    for (int i = 0; i < 200 && !t->finished.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(t->finished.load());
    jobs.run_posted();
    CHECK(jobs.running() == 0);
    // the destructor cancels and joins what is still running
    {
        Jobs j2;
        j2.start([](Jobs::Token &tok) {
            while (!tok.cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        });
    }
    CHECK(true);
}

void test_package()
{
    const std::string app = APP_DIR;
    std::string desktop;
    CHECK(read_file(app + "/root/usr/share/APPLaunch/applications/rss-reader.desktop", desktop));
    CHECK(desktop.find("\nX-Fullscreen=true") != std::string::npos);
    CHECK(desktop.find("\nX-MinSize=640x480") != std::string::npos);        // = AppSpec min_w x min_h (stage.hpp kScreenW/H)
    CHECK(desktop.find("\nExec=/usr/share/APPLaunch/bin/rss-reader\n") != std::string::npos);
    CHECK(desktop.find("\nIcon=share/images/rss-reader.png\n") != std::string::npos);
    CHECK(desktop.find("\nX-Background=freeze\n") != std::string::npos);  // multitasking: stopped while hidden
    CHECK(path_exists(app + "/root/usr/share/APPLaunch/share/images/rss-reader.png"));
    std::string a, b;
    CHECK(read_file(app + "/root/usr/share/APPLaunch/share/rss-reader/catalog.json", a) && read_file(app + "/data/catalog.json", b) && a == b);
    std::string icon, icon2;
    CHECK(read_file(app + "/icon.png", icon) && icon.size() > 24 && icon.compare(1, 3, "PNG") == 0);
    const auto be32 = [&](std::size_t at) {
        return (static_cast<unsigned>(static_cast<unsigned char>(icon[at])) << 24) | (static_cast<unsigned>(static_cast<unsigned char>(icon[at + 1])) << 16) |
               (static_cast<unsigned>(static_cast<unsigned char>(icon[at + 2])) << 8) | static_cast<unsigned>(static_cast<unsigned char>(icon[at + 3]));
    };
    CHECK(be32(16) == 100 && be32(20) == 100);                                // 100x100 (ICON-PALETTE.md)
    CHECK(read_file(app + "/root/usr/share/APPLaunch/share/images/rss-reader.png", icon2) && icon == icon2);
    std::string manifest;
    CHECK(read_file(app + "/app.json", manifest));
    Json j;
    CHECK(json_parse(manifest, j));
    CHECK(j.get_string("share_code") == "rss-reader" && j.get_string("package") == "rss-reader" && j.get_string("title") == "RSS Reader");
    CHECK(j.get_string("version") == "0.1.3" && !j.get_string("summary").empty() && !j.get_string("description").empty());
    CHECK(j.get_string("depends") == "libfreetype6, libinput10, libxkbcommon0, curl, ca-certificates");
    CHECK(j.get_string("depends").find("libc6") == std::string::npos);
    std::string copyright;
    CHECK(read_file(app + "/root/usr/share/doc/rss-reader/copyright", copyright) && copyright.find("Files: usr/share/APPLaunch/bin/rss-reader") != std::string::npos);
}

} // namespace

#define RUN(fn)                                                         \
    do {                                                                \
        int before_ = g_fail;                                           \
        fn();                                                           \
        std::printf("%-28s %s\n", #fn, g_fail == before_ ? "ok" : "FAILED"); \
    } while (0)

int main()
{
    setenv("TZ", "UTC", 1);
    tzset();
    RUN(test_keys);
    RUN(test_dialog_keys);
    RUN(test_hold);
    RUN(test_layout_rules);
    RUN(test_formatting);
    RUN(test_failures);
    RUN(test_favorites);
    RUN(test_articles);
    RUN(test_browse);
    RUN(test_settings_rules);
    RUN(test_reader_blocks);
    RUN(test_images);
    RUN(test_saved_texts);
    RUN(test_jobs);
    RUN(test_package);
    std::printf("ui: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
