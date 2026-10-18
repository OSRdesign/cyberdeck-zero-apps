// refresh orchestration with a fake HTTP client, custom feed discovery, OPML.
#include "test_common.hpp"

#include "catalog.hpp"
#include "opml.hpp"
#include "refresh.hpp"
#include "util.hpp"

#include <atomic>

using namespace rssreader;
using namespace rssreader_test;

static std::int64_t g_now = 1792336205;
static NowFn clock_fn() { return [] { return g_now; }; }

static std::string rss(std::initializer_list<std::pair<std::string, std::string>> items, const std::string &title = "Test Feed")
{
    // items: {guid, pubDate or ""}
    std::string x = "<?xml version=\"1.0\"?><rss version=\"2.0\"><channel><title>" + title + "</title><link>https://t.example.com/</link>";
    for (const auto &it : items) {
        x += "<item><title>Title " + it.first + "</title><guid>" + it.first + "</guid><link>https://t.example.com/" + it.first + "</link>";
        if (!it.second.empty()) x += "<pubDate>" + it.second + "</pubDate>";
        x += "</item>";
    }
    return x + "</channel></rss>";
}

static const char *kD1 = "Sat, 17 Oct 2026 10:00:00 GMT";
static const char *kD2 = "Sat, 17 Oct 2026 11:00:00 GMT";
static const char *kD3 = "Sat, 17 Oct 2026 12:00:00 GMT";

static Subscription pin(Store &st, const std::string &id, const std::string &url)
{
    Subscription s;
    s.id = id;
    s.name = id;
    s.url = url;
    CHECK(st.add_subscription(s) == StoreError::Ok);
    return s;
}

void test_merge()
{
    auto mk = [](const std::string &id, std::int64_t pub, bool dated = true) {
        Article a;
        a.id = id;
        a.title = "T" + id;
        a.published = pub;
        a.dated = dated;
        return a;
    };
    std::vector<StoredArticle> cached;
    for (const char *id : {"b", "a"}) {
        StoredArticle s;
        s.article = mk(id, id[0] == 'b' ? 200 : 100);
        s.read = (id[0] == 'a');
        cached.push_back(s);
    }
    int n = -1;
    std::vector<StoredArticle> m = merge_articles(cached, {mk("c", 300), mk("b", 200), mk("a", 100)}, n);
    CHECK(n == 1 && m.size() == 3 && m[0].article.id == "c" && m[1].article.id == "b" && m[2].article.id == "a");
    CHECK(!m[0].read && !m[1].read && m[2].read);                         // read flags survive
    // articles the feed dropped stay
    m = merge_articles(cached, {mk("c", 300)}, n);
    CHECK(n == 1 && m.size() == 3 && m[1].article.id == "b");
    // an empty fetch changes nothing
    m = merge_articles(cached, {}, n);
    CHECK(n == 0 && m.size() == 2 && m[1].read);
    // nothing cached: everything is new
    m = merge_articles({}, {mk("x", 1), mk("y", 2)}, n);
    CHECK(n == 2 && m.size() == 2 && m[0].article.id == "y");
    // duplicates in the fresh list count once
    m = merge_articles({}, {mk("x", 1), mk("x", 1)}, n);
    CHECK(n == 1 && m.size() == 1);
    // an undated item keeps the date it was first given
    m = merge_articles(cached, {mk("b", 999, false)}, n);
    CHECK(n == 0 && m.size() == 2);
    for (const StoredArticle &s : m)
        if (s.article.id == "b") CHECK(s.article.published == 200 && s.article.dated);
    // a changed article is replaced (edited title) but stays read
    Article edited = mk("a", 100);
    edited.title = "edited";
    m = merge_articles(cached, {edited}, n);
    for (const StoredArticle &s : m)
        if (s.article.id == "a") CHECK(s.article.title == "edited" && s.read);
}

void test_refresh_feed()
{
    TempDir t;
    Store st(t.path + "/d");
    FakeHttp http;
    Refresher rf(st, http, clock_fn());
    const std::string url = "https://t.example.com/rss";
    Subscription sub = pin(st, "feed-a", url);

    // first fetch
    http.add_ok(url, rss({{"g1", kD1}, {"g2", kD2}}), "application/rss+xml", "\"e1\"");
    g_now = 1792336205;
    RefreshResult r = rf.refresh_feed(sub);
    CHECK(r.status == RefreshResult::Status::Updated && r.new_articles == 2 && r.total == 2 && r.error == FetchError::None);
    CHECK_EQ(st.unread_count("feed-a"), 2);
    Subscription s2;
    CHECK(st.find_subscription("feed-a", s2) && s2.etag == "\"e1\"" && s2.last_fetch == g_now && s2.last_error.empty());
    CHECK(http.requests.size() == 1 && http.requests[0].etag.empty());     // nothing cached yet: unconditional
    CHECK(http.requests[0].url == url && http.requests[0].max_bytes == kMaxFeedBytes);

    // user reads the newest
    std::vector<StoredArticle> cur = st.load_articles("feed-a");
    CHECK(cur.size() == 2 && cur[0].article.guid == "g2");
    CHECK(st.set_read("feed-a", cur[0].article.id, true));
    CHECK_EQ(st.unread_count("feed-a"), 1);

    // 304: conditional headers sent from the stored validators, cache untouched
    g_now += 600;
    HttpResponse nm;
    nm.error = FetchError::NotModified;
    nm.status = 304;
    http.add(url, nm);
    r = rf.refresh_feed(sub);                     // `sub` is a stale copy: the stored validators win
    CHECK(r.status == RefreshResult::Status::NotModified && r.new_articles == 0 && r.total == 2);
    CHECK_EQ(http.requests.back().etag, "\"e1\"");
    CHECK(st.find_subscription("feed-a", s2) && s2.last_fetch == g_now && s2.etag == "\"e1\"" && s2.last_error.empty());
    CHECK_EQ(st.unread_count("feed-a"), 1);

    // 200 with a new article and one dropped from the feed: read state kept, new one counted, nothing lost
    g_now += 600;
    http.add_ok(url, rss({{"g3", kD3}, {"g2", kD2}}), "application/rss+xml", "\"e2\"");
    r = rf.refresh_feed(sub);
    CHECK(r.status == RefreshResult::Status::Updated && r.new_articles == 1 && r.total == 3);
    cur = st.load_articles("feed-a");
    CHECK(cur.size() == 3 && cur[0].article.guid == "g3" && !cur[0].read && cur[1].article.guid == "g2" && cur[1].read && cur[2].article.guid == "g1" && !cur[2].read);
    CHECK_EQ(st.unread_count("feed-a"), 2);
    CHECK(st.find_subscription("feed-a", s2) && s2.etag == "\"e2\"");

    // the same content again (a server without validators): no new articles, read state intact
    g_now += 600;
    http.add_ok(url, rss({{"g3", kD3}, {"g2", kD2}}));
    r = rf.refresh_feed(sub);
    CHECK(r.status == RefreshResult::Status::Updated && r.new_articles == 0 && r.total == 3);
    CHECK_EQ(st.unread_count("feed-a"), 2);
    CHECK(st.find_subscription("feed-a", s2) && s2.etag.empty());

    // failures keep the cache and say why
    const std::int64_t good_fetch = g_now;
    g_now += 600;
    http.add_error(url, FetchError::Network, 0, "Could not resolve host: t.example.com");
    r = rf.refresh_feed(sub);
    CHECK(r.status == RefreshResult::Status::Failed && r.error == FetchError::Network && r.total == 3);
    CHECK(r.message.find("network error") == 0 && r.message.find("Could not resolve host") != std::string::npos);
    CHECK(st.find_subscription("feed-a", s2) && s2.last_fetch == good_fetch && s2.last_attempt == g_now && !s2.last_error.empty());
    CHECK_EQ(st.load_articles("feed-a").size(), std::size_t(3));
    http.add_error(url, FetchError::Http, 503);
    r = rf.refresh_feed(sub);
    CHECK(r.status == RefreshResult::Status::Failed && r.message == "HTTP 503");
    http.add_error(url, FetchError::Timeout);
    r = rf.refresh_feed(sub);
    CHECK(r.error == FetchError::Timeout && r.message == "timed out");
    http.add_error(url, FetchError::TooLarge);
    r = rf.refresh_feed(sub);
    CHECK(r.error == FetchError::TooLarge);
    http.add_error(url, FetchError::Tls, 0, "SSL certificate problem");
    r = rf.refresh_feed(sub);
    CHECK(r.error == FetchError::Tls && r.message.find("SSL certificate problem") != std::string::npos);
    http.add_ok(url, "<html><body>Please log in to the wifi</body></html>", "text/html");
    r = rf.refresh_feed(sub);
    CHECK(r.status == RefreshResult::Status::Failed && r.message == "not a feed" && r.error == FetchError::None);
    CHECK(st.find_subscription("feed-a", s2) && s2.last_error == "not a feed");
    CHECK_EQ(st.load_articles("feed-a").size(), std::size_t(3));
    http.add_ok(url, std::string("<?xml version=\"1.0\" encoding=\"UTF-16\"?><rss/>"));
    r = rf.refresh_feed(sub);
    CHECK(r.message == "unsupported text encoding");

    // recovery: the error clears, the validators come back
    http.add_ok(url, rss({{"g4", kD3}, {"g3", kD3}}), "application/rss+xml", "\"e9\"");
    r = rf.refresh_feed(sub);
    CHECK(r.status == RefreshResult::Status::Updated && r.new_articles == 1);
    CHECK(st.find_subscription("feed-a", s2) && s2.last_error.empty() && s2.etag == "\"e9\"");

    // a cancelled transfer is not recorded as a failure
    std::string before_err = s2.last_error;
    http.add_error(url, FetchError::Cancelled);
    std::atomic<bool> cancel{false};
    r = rf.refresh_feed(sub, &cancel);
    CHECK(r.status == RefreshResult::Status::Failed && r.error == FetchError::Cancelled);
    CHECK(st.find_subscription("feed-a", s2) && s2.last_error == before_err);
    CHECK(http.requests.back().cancel == &cancel);
}

void test_refresh_edge()
{
    TempDir t;
    Store st(t.path + "/d");
    FakeHttp http;
    Refresher rf(st, http, clock_fn());

    // 304 with an empty cache must not be requested conditionally even if validators exist
    Subscription s = pin(st, "lost-cache", "https://lc.example.com/rss");
    st.record_fetch("lost-cache", 1, true, "\"stale\"", "lm", "");
    http.add_ok(s.url, rss({{"a", kD1}}), "application/xml", "\"n\"");
    RefreshResult r = rf.refresh_feed(s);
    CHECK(r.status == RefreshResult::Status::Updated && r.new_articles == 1);
    CHECK(http.requests.back().etag.empty() && http.requests.back().last_modified.empty());

    // undated items: ordered as in the feed, dated from the first fetch, and not moved by later fetches
    Subscription u = pin(st, "undated", "https://un.example.com/rss");
    http.add_ok(u.url, rss({{"n1", ""}, {"n2", ""}, {"n3", ""}}));
    g_now = 1792336205;
    r = rf.refresh_feed(u);
    CHECK(r.new_articles == 3);
    std::vector<StoredArticle> a = st.load_articles("undated");
    CHECK(a.size() == 3 && a[0].article.guid == "n1" && a[1].article.guid == "n2" && a[2].article.guid == "n3");
    std::int64_t p1 = a.empty() ? 0 : a[0].article.published;
    g_now += 5000;
    http.add_ok(u.url, rss({{"n0", ""}, {"n1", ""}, {"n2", ""}, {"n3", ""}}));
    r = rf.refresh_feed(u);
    a = st.load_articles("undated");
    CHECK(r.new_articles == 1 && a.size() == 4 && a[0].article.guid == "n0");
    bool kept = false;
    for (const StoredArticle &x : a)
        if (x.article.guid == "n1") kept = x.article.published == p1;
    CHECK(kept);

    // relative links resolve against the final URL after a redirect
    Subscription m = pin(st, "moved", "https://old.example.com/rss");
    HttpResponse moved;
    moved.status = 200;
    moved.body = "<rss><channel><title>M</title><item><title>x</title><guid>1</guid><link>/post/1</link></item></channel></rss>";
    moved.final_url = "https://new.example.com/feed";
    http.add("https://old.example.com/rss", moved);
    r = rf.refresh_feed(m);
    a = st.load_articles("moved");
    CHECK(r.new_articles == 1 && a.size() == 1 && a[0].article.link == "https://new.example.com/post/1");

    // a feed that is gone from the subscriptions mid-flight does not crash anything
    Subscription ghost;
    ghost.id = "ghost";
    ghost.url = "https://ghost.example.com/rss";
    http.add_ok(ghost.url, rss({{"z", kD1}}));
    r = rf.refresh_feed(ghost);
    CHECK(r.status == RefreshResult::Status::Updated || r.status == RefreshResult::Status::Failed);
}

void test_refresh_all()
{
    TempDir t;
    Store st(t.path + "/d");
    FakeHttp http;
    Refresher rf(st, http, clock_fn());
    std::vector<Subscription> subs;
    for (const char *id : {"a", "b", "c", "d"}) subs.push_back(pin(st, id, std::string("https://") + id + ".example.com/rss"));
    http.add_ok(subs[0].url, rss({{"a1", kD1}}));
    http.add_ok(subs[1].url, rss({{"b1", kD1}, {"b2", kD2}}));
    http.add_error(subs[2].url, FetchError::Http, 500);
    http.add_ok(subs[3].url, rss({{"d1", kD1}}));

    std::vector<std::pair<std::size_t, std::string>> seen;
    std::vector<RefreshResult> rs = rf.refresh_all(subs, nullptr, [&](std::size_t done, std::size_t total, const RefreshResult &res) {
        CHECK_EQ(total, std::size_t(4));
        seen.emplace_back(done, res.feed_id);
    });
    CHECK_EQ(rs.size(), std::size_t(4));
    CHECK(seen.size() == 4 && seen[0].second == "a" && seen[3].second == "d" && seen[3].first == 4);
    CHECK(rs.size() == 4 && rs[1].new_articles == 2 && rs[2].status == RefreshResult::Status::Failed && rs[3].status == RefreshResult::Status::Updated);
    CHECK(http.requests.size() == 4 && http.requests[0].url == subs[0].url && http.requests[3].url == subs[3].url);   // sequential, in order
    CHECK_EQ(st.total_unread(), 4);

    // cancel after the second feed: the rest is not attempted
    std::atomic<bool> cancel{false};
    http.requests.clear();
    rs = rf.refresh_all(subs, &cancel, [&](std::size_t done, std::size_t, const RefreshResult &) {
        if (done == 2) cancel = true;
    });
    CHECK(rs.size() == 2 && http.requests.size() == 2);
    CHECK(rf.refresh_all({}, nullptr).empty());
    cancel = true;
    CHECK(rf.refresh_all(subs, &cancel).empty());
}

void test_add_custom_feed()
{
    TempDir t;
    Store st(t.path + "/d");
    FakeHttp http;
    Refresher rf(st, http, clock_fn());

    // a feed URL
    http.add_ok("https://direct.example.com/rss", rss({{"a", kD1}, {"b", kD2}}, "Direct Feed"), "application/rss+xml", "\"d1\"");
    AddResult r = rf.add_custom_feed("https://direct.example.com/rss");
    CHECK(r.status == AddResult::Status::Ok && r.found.title == "Direct Feed" && r.found.url == "https://direct.example.com/rss" && r.found.article_count == 2);
    CHECK(r.found.format == "rss" && r.found.site_url == "https://t.example.com/" && r.found.etag == "\"d1\"");
    CHECK_EQ(http.requests.size(), std::size_t(1));
    // pinning needs no second fetch and stores the articles
    CHECK(rf.pin_custom_feed(r.found, "My group") == StoreError::Ok);
    CHECK_EQ(http.requests.size(), std::size_t(1));
    std::vector<Subscription> subs = st.subscriptions();
    CHECK(subs.size() == 1 && subs[0].custom && subs[0].id == custom_feed_id("https://direct.example.com/rss") && subs[0].name == "Direct Feed" && subs[0].group == "My group");
    CHECK(subs.size() == 1 && subs[0].etag == "\"d1\"" && subs[0].unread == 2 && subs[0].total == 2 && subs[0].last_fetch == g_now);
    CHECK_EQ(st.load_articles(subs[0].id).size(), std::size_t(2));
    // ... and the article ids are the ones a later refresh produces: no duplicates
    HttpResponse nm;
    nm.error = FetchError::NotModified;
    nm.status = 304;
    http.add("https://direct.example.com/rss", nm);
    RefreshResult rr = rf.refresh_feed(subs[0]);
    CHECK(rr.status == RefreshResult::Status::NotModified);
    http.add_ok("https://direct.example.com/rss", rss({{"a", kD1}, {"b", kD2}}, "Direct Feed"));
    rr = rf.refresh_feed(subs[0]);
    CHECK(rr.status == RefreshResult::Status::Updated && rr.new_articles == 0 && rr.total == 2);
    CHECK(rf.pin_custom_feed(r.found) == StoreError::Duplicate);
    FoundFeed bogus;
    bogus.url = "ftp://x";
    CHECK(rf.pin_custom_feed(bogus) == StoreError::BadArgument);

    // a site address: the page advertises its feed
    http.add_ok("https://site.example.com", "<html><head><link rel=\"alternate\" type=\"application/atom+xml\" href=\"/atom\"></head></html>", "text/html");
    http.add_ok("https://site.example.com/atom", fixture("atom.xml"), "application/atom+xml");
    r = rf.add_custom_feed("site.example.com");
    CHECK(r.status == AddResult::Status::Ok && r.found.url == "https://site.example.com/atom" && r.found.format == "atom" && r.found.article_count == 2);
    CHECK_EQ(http.count("https://site.example.com"), 1);

    // no advertised feed: the usual places are tried in order, an HTML answer is skipped
    http.add_ok("https://guess.example.com/", "<html><body>nothing</body></html>", "text/html");
    http.add_ok("https://guess.example.com/feed", "<html><body>pretty 404 page</body></html>", "text/html");
    http.add_ok("https://guess.example.com/rss", rss({{"q", kD1}}, "Guessed"));
    r = rf.add_custom_feed("https://guess.example.com/");
    CHECK(r.status == AddResult::Status::Ok && r.found.url == "https://guess.example.com/rss" && r.found.title == "Guessed");
    CHECK(http.count("https://guess.example.com/feed") == 1 && http.count("https://guess.example.com/atom.xml") == 0);

    // a guess that is an empty feed is not accepted; a declared empty feed is
    http.add_ok("https://empty.example.com/", "<html></html>", "text/html");
    http.add_ok("https://empty.example.com/feed", "<rss><channel><title>E</title></channel></rss>");
    r = rf.add_custom_feed("https://empty.example.com/");
    CHECK(r.status == AddResult::Status::NoFeedFound);
    http.add_ok("https://decl.example.com/", "<link rel=alternate type=application/rss+xml href=/e.xml>", "text/html");
    http.add_ok("https://decl.example.com/e.xml", "<rss><channel><title>E</title></channel></rss>");
    r = rf.add_custom_feed("https://decl.example.com/");
    CHECK(r.status == AddResult::Status::Ok && r.found.article_count == 0 && r.found.title == "E");

    // nothing at all
    http.add_ok("https://none.example.com/", "<html><body>hi</body></html>", "text/html");
    r = rf.add_custom_feed("https://none.example.com/");
    CHECK(r.status == AddResult::Status::NoFeedFound && !r.message.empty());
    CHECK(http.count("https://none.example.com/feed") == 1);
    // at most 8 candidates are tried
    std::string many = "<html>";
    for (int i = 0; i < 12; ++i) many += "<link rel=alternate type=application/rss+xml href=/c" + std::to_string(i) + ">";
    http.add_ok("https://many.example.com/", many, "text/html");
    http.requests.clear();
    r = rf.add_custom_feed("https://many.example.com/");
    CHECK(r.status == AddResult::Status::NoFeedFound && http.requests.size() == 9);

    // the site does not answer
    http.add_error("https://down.example.com/", FetchError::Network, 0, "Connection refused");
    r = rf.add_custom_feed("https://down.example.com/");
    CHECK(r.status == AddResult::Status::FetchFailed && r.error == FetchError::Network && r.message.find("Connection refused") != std::string::npos);
    http.add_error("https://forbidden.example.com/", FetchError::Http, 403);
    r = rf.add_custom_feed("https://forbidden.example.com/");
    CHECK(r.status == AddResult::Status::FetchFailed && r.message == "HTTP 403");

    // scheme-less input: https first, then plain http for a site without TLS
    http.add_error("https://plain.example.com", FetchError::Tls, 0, "handshake");
    http.add_ok("http://plain.example.com", rss({{"p", kD1}}, "Plain HTTP"));
    r = rf.add_custom_feed("plain.example.com");
    CHECK(r.status == AddResult::Status::Ok && r.found.title == "Plain HTTP");
    // ... but an explicit https:// is not downgraded
    http.add_error("https://strict.example.com/", FetchError::Tls);
    http.add_ok("http://strict.example.com/", rss({{"p", kD1}}));
    r = rf.add_custom_feed("https://strict.example.com/");
    CHECK(r.status == AddResult::Status::FetchFailed && http.count("http://strict.example.com/") == 0);

    // redirects: the final URL is the feed's address
    HttpResponse redirected;
    redirected.status = 200;
    redirected.body = rss({{"r", kD1}}, "Redirected");
    redirected.final_url = "https://final.example.com/feed.xml";
    http.add("https://short.example.com/f", redirected);
    r = rf.add_custom_feed("https://short.example.com/f");
    CHECK(r.status == AddResult::Status::Ok && r.found.url == "https://final.example.com/feed.xml");

    // bad input never reaches the network
    http.requests.clear();
    for (const char *bad : {"", "   ", "nodots", "ftp://x.org/f", "file:///etc/passwd", "javascript:alert(1)", "a b c"}) {
        r = rf.add_custom_feed(bad);
        CHECK(r.status == AddResult::Status::BadInput && !r.message.empty());
    }
    CHECK(http.requests.empty());

    // cancel: between candidates
    std::atomic<bool> cancel{true};
    r = rf.add_custom_feed("https://none.example.com/", &cancel);
    CHECK(r.status == AddResult::Status::FetchFailed && r.error == FetchError::Cancelled);
    // a feed with a title missing falls back to the host
    http.add_ok("https://notitle.example.com/rss", "<rss><channel><item><title>x</title><guid>1</guid></item></channel></rss>");
    r = rf.add_custom_feed("https://notitle.example.com/rss");
    CHECK(r.status == AddResult::Status::Ok && r.found.title == "notitle.example.com");
}

void test_opml()
{
    const std::string doc =
        "<?xml version=\"1.0\"?><opml version=\"2.0\"><head><title>x</title></head><body>"
        "<outline text=\"Top level\" type=\"rss\" xmlUrl=\"https://top.example.com/rss\" htmlUrl=\"https://top.example.com/\"/>"
        "<outline text=\"Tech\" title=\"Tech\">"
        "<outline text=\"Blog &amp; Co\" xmlUrl=\"https://blog.example.com/feed?a=1&amp;b=2\"/>"
        "<outline text=\"Linux\"><outline title=\"Kernel\" text=\"ignored\" xmlUrl=\"http://kernel.example.org/rss\"/></outline>"
        "</outline>"
        "<outline text=\"Dup\" xmlUrl=\"https://top.example.com/rss\"/>"
        "<outline text=\"Bad\" xmlUrl=\"javascript:x\"/><outline text=\"Bad2\" xmlUrl=\"ftp://x.org/f\"/><outline text=\"NoUrl\"/>"
        "<outline xmlUrl=\"https://notitle.example.com/rss\"/>"
        "<outline text=\"Sloppy\" xmlUrl=\"https://sloppy.example.com/rss\"><outline text=\"Child\" xmlUrl=\"https://child.example.com/rss\"/></outline>"
        "</body></opml>";
    std::vector<OpmlFeed> f = parse_opml(doc);
    CHECK_EQ(f.size(), std::size_t(6));
    if (f.size() == 6) {
        CHECK(f[0].title == "Top level" && f[0].url == "https://top.example.com/rss" && f[0].site == "https://top.example.com/" && f[0].group.empty());
        CHECK(f[1].title == "Blog & Co" && f[1].url == "https://blog.example.com/feed?a=1&b=2" && f[1].group == "Tech");
        CHECK(f[2].title == "Kernel" && f[2].group == "Tech / Linux");           // nested folders are flattened into a group name
        CHECK(f[3].title == "notitle.example.com");
        CHECK(f[4].url == "https://sloppy.example.com/rss" && f[5].url == "https://child.example.com/rss");
    }
    // not OPML, damaged, hostile
    CHECK(parse_opml("").empty());
    CHECK(parse_opml("<rss><channel/></rss>").empty());
    CHECK(parse_opml("<opml><head/></opml>").empty());
    CHECK(parse_opml("not xml at all").empty());
    CHECK(parse_opml("<opml><body><outline xmlUrl='https://a.org/r'").size() <= 1);      // truncated: must not crash
    std::string deep = "<opml><body>";
    for (int i = 0; i < 100; ++i) deep += "<outline text='g'>";
    CHECK(parse_opml(deep).empty());                                                      // too deep: refused, no crash
    std::string nest = "<opml><body>";
    for (int i = 0; i < 50; ++i) nest += "<outline text='g" + std::to_string(i) + "'>";
    nest += "<outline xmlUrl='https://deep.example.com/r'/>";
    CHECK_EQ(parse_opml(nest).size(), std::size_t(1));

    // write + round trip, special characters escaped
    std::vector<OpmlFeed> in = {{"Plain", "https://p.example.com/rss", "https://p.example.com/", ""},
                                {"A & B <c> \"q\"", "https://q.example.com/feed?x=1&y=2", "", "News"},
                                {"Caf\xC3\xA9", "https://c.example.com/rss", "", "News"},
                                {"Other", "https://o.example.com/rss", "", "Other group"}};
    std::string text = write_opml(in, "My <feeds>");
    CHECK(text.find("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") == 0 && text.find("<title>My &lt;feeds&gt;</title>") != std::string::npos);
    std::vector<OpmlFeed> out = parse_opml(text);
    CHECK_EQ(out.size(), in.size());
    for (std::size_t i = 0; i < out.size() && i < in.size(); ++i) {
        // order: ungrouped first, then by group; compare by URL
        bool found = false;
        for (const OpmlFeed &o : out)
            if (o.url == in[i].url) { found = o.title == in[i].title && o.group == in[i].group && o.site == in[i].site; }
        CHECK(found);
    }
    CHECK(write_opml({}).find("<body>") != std::string::npos && parse_opml(write_opml({})).empty());

    // import into a store: catalog feeds keep their catalog identity, duplicates are counted, junk is not fatal
    Catalog cat;
    CHECK(cat.load_json("[{\"id\":\"fr-x\",\"name\":\"Le X\",\"url\":\"https://x.example.fr/rss\",\"country\":\"FR\",\"lang\":\"fr\",\"topic\":\"science\"}]"));
    TempDir t;
    Store st(t.path + "/d");
    std::vector<OpmlFeed> imp = {{"Whatever", "http://X.example.fr/rss/", "", ""}, {"Mine", "https://mine.example.com/rss", "", "Folder"},
                                 {"Mine again", "https://mine.example.com/rss", "", ""}, {"Bad", "ftp://nope", "", ""}};
    OpmlImportResult res = import_opml(st, &cat, imp, 1234);
    CHECK(res.added == 2 && res.duplicates == 1 && res.failed == 1);
    std::vector<Subscription> subs = st.subscriptions();
    CHECK(subs.size() == 2 && subs[0].id == "fr-x" && !subs[0].custom && subs[0].name == "Le X" && subs[0].group == "science" && subs[0].added == 1234);
    CHECK(subs.size() == 2 && subs[1].custom && subs[1].id == custom_feed_id("https://mine.example.com/rss") && subs[1].group == "Folder");
    // importing the same file again changes nothing
    res = import_opml(st, &cat, imp, 5678);
    CHECK(res.added == 0 && res.duplicates == 3 && st.subscriptions().size() == 2);
    // without a catalog everything is custom
    TempDir t2;
    Store st2(t2.path + "/d");
    CHECK(import_opml(st2, nullptr, imp, 1).added == 2);

    // export from the store and back
    std::vector<OpmlFeed> exported = opml_from_subscriptions(st.subscriptions());
    CHECK(exported.size() == 2 && exported[0].url == "https://x.example.fr/rss" && exported[0].group == "science");
    std::vector<OpmlFeed> back = parse_opml(write_opml(exported));
    CHECK_EQ(back.size(), std::size_t(2));
}
