// Phase 3 of the core: the image headers (image_probe), the image cache (LRU, cap, clear), the read marks made while a
// refresh runs (Store::store_fetched_articles), the first-run hint flag, saves that take turns.
#include "test_common.hpp"

#include "fsutil.hpp"
#include "image_cache.hpp"
#include "image_probe.hpp"
#include "refresh.hpp"
#include "store.hpp"
#include "util.hpp"

#include <atomic>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <thread>

using namespace rssreader;
using namespace rssreader_test;

namespace {

std::string img(const std::string &name) { return fixture("ui/img/" + name); }

}

void test_image_probe()
{
    ImageProbe p = probe_image(img("graph.jpg"));
    CHECK(p.format == ImageFormat::Jpeg && p.width == 640 && p.height == 360 && p.baseline && !p.progressive && p.jpeg_sof == 0xC0 && p.components == 3);
    p = probe_image(img("progressive.jpg"));
    CHECK(p.format == ImageFormat::Jpeg && p.width == 480 && p.height == 320 && !p.baseline && p.progressive && p.jpeg_sof == 0xC2);
    p = probe_image(img("cmyk.jpg"));
    CHECK(p.format == ImageFormat::Jpeg && p.components == 4);
    p = probe_image(img("huge.jpg"));
    CHECK(p.width == 20000 && p.height == 20000 && p.pixels() == 400000000ull);
    p = probe_image(img("logo.png"));
    CHECK(p.format == ImageFormat::Png && p.width == 360 && p.height == 160 && p.alpha && !p.interlaced);
    p = probe_image(img("huge.png"));
    CHECK(p.format == ImageFormat::Png && p.width == 30000 && p.height == 30000);
    p = probe_image(img("anim.gif"));
    CHECK(p.format == ImageFormat::Gif && p.width == 240 && p.height == 140);
    p = probe_image(img("pic.webp"));
    CHECK(p.format == ImageFormat::Webp && p.width == 300 && p.height == 200);
    p = probe_image(img("notimage.jpg"));
    CHECK(p.format == ImageFormat::Unknown && p.pixels() == 0);
    CHECK(probe_image("").format == ImageFormat::Unknown);
    CHECK(probe_image("\xFF\xD8\xFF").format == ImageFormat::Jpeg && probe_image("\xFF\xD8\xFF").width == 0);
    CHECK(probe_image("<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>").format == ImageFormat::Svg);
    CHECK(probe_image(std::string("\0\0\0\x1C" "ftypavif", 12)).format == ImageFormat::Avif);
    CHECK(probe_image(std::string("BM\x36\0\0\0\0\0\0\0\x36\0\0\0\x28\0\0\0\x10\0\0\0\xF0\xFF\xFF\xFF", 26)).height == 16);
    // every cut of a real file: no read past the end (ASan), never a size from bytes that are not there
    const std::string j = img("graph.jpg");
    for (std::size_t n = 0; n < 700 && n < j.size(); ++n) {
        const ImageProbe q = probe_image(j.substr(0, n));
        CHECK(q.width == 0 || q.width == 640);
    }
    const std::string g = img("logo.png");
    for (std::size_t n = 0; n < 40; ++n) probe_image(g.substr(0, n));
    CHECK_EQ(std::string(image_format_name(ImageFormat::Webp)), std::string("WebP"));
}

void test_image_cache()
{
    TempDir t;
    ImageCache c(t.path + "/img/", 1000);
    CHECK(c.dir() == t.path + "/img");
    std::string b, ct;
    CHECK(!c.get("https://a/1.jpg", b) && b.empty());
    CHECK(c.usage() == 0 && c.count() == 0);
    CHECK(c.put("https://a/1.jpg", std::string(300, 'a'), "image/jpeg"));
    CHECK(c.put("https://a/2.png", std::string(300, 'b'), "image/png"));
    CHECK(path_exists(t.path + "/img/" + ImageCache::key("https://a/1.jpg") + ".jpg"));
    CHECK(c.get("https://a/2.png", b, &ct) && b == std::string(300, 'b') && ct == "image/png");
    CHECK(c.contains("https://a/1.jpg") && !c.contains("https://a/3.gif"));
    CHECK_EQ(c.usage(), std::uint64_t(600));
    // the oldest use goes first: 1 is older than 2 (2 was just read), so a third file over the cap evicts 1
    struct timespec old[2] = {{1000, 0}, {1000, 0}};
    ::utimensat(AT_FDCWD, (t.path + "/img/" + ImageCache::key("https://a/1.jpg") + ".jpg").c_str(), old, 0);
    CHECK(c.put("https://a/3.gif", std::string(500, 'c'), "image/gif"));
    CHECK(!c.contains("https://a/1.jpg") && c.contains("https://a/2.png") && c.contains("https://a/3.gif"));
    CHECK(c.usage() <= 1000);
    // a get makes a file young again
    ::utimensat(AT_FDCWD, (t.path + "/img/" + ImageCache::key("https://a/2.png") + ".png").c_str(), old, 0);
    CHECK(c.get("https://a/2.png", b));
    struct timespec older[2] = {{900, 0}, {900, 0}};
    ::utimensat(AT_FDCWD, (t.path + "/img/" + ImageCache::key("https://a/3.gif") + ".gif").c_str(), older, 0);
    CHECK(c.put("https://a/4.jpg", std::string(400, 'd'), "image/jpeg"));
    CHECK(c.contains("https://a/2.png") && !c.contains("https://a/3.gif") && c.contains("https://a/4.jpg"));
    // the same URL again with another type: one file
    CHECK(c.put("https://a/4.jpg", std::string(100, 'e'), "image/png"));
    CHECK(c.get("https://a/4.jpg", b, &ct) && b.size() == 100 && ct == "image/png");
    CHECK_EQ(c.count(), std::size_t(2));
    // refused: empty, too large, no URL
    CHECK(!c.put("https://a/5.jpg", "", "image/jpeg"));
    CHECK(!c.put("https://a/5.jpg", std::string(kMaxImageBytes + 1, 'x'), "image/jpeg"));
    CHECK(!c.put("", "x", "image/jpeg"));
    // a file larger than the whole cap stays (the one just written is never evicted), the others go
    CHECK(c.put("https://a/6.jpg", std::string(1500, 'f'), "image/jpeg"));
    CHECK(c.contains("https://a/6.jpg") && c.count() == 1);
    // lowering the cap evicts; clear empties; a leftover temp file is cleaned
    c.set_cap(100);
    write_file_atomic(t.path + "/img/x.jpg.tmp", "zz");
    CHECK(c.put("https://a/7.jpg", std::string(50, 'g'), "image/jpeg"));
    CHECK(!path_exists(t.path + "/img/x.jpg.tmp") && c.contains("https://a/7.jpg") && !c.contains("https://a/6.jpg"));
    CHECK(c.remove("https://a/7.jpg") && !c.contains("https://a/7.jpg"));
    CHECK(c.put("https://a/8.jpg", std::string(50, 'h'), "image/jpeg"));
    CHECK(c.clear() && c.usage() == 0 && c.count() == 0);
    // an unwritable place: nothing cached, nothing fails loudly
    ImageCache bad("/proc/nope/images");
    CHECK(!bad.put("https://a/1.jpg", "x", "image/jpeg") && !bad.get("https://a/1.jpg", b) && bad.usage() == 0);
    // threads
    ImageCache tc(t.path + "/t", 20000);
    std::vector<std::thread> th;
    for (int w = 0; w < 4; ++w)
        th.emplace_back([&tc, w] {
            std::string x;
            for (int i = 0; i < 40; ++i) {
                tc.put("https://t/" + std::to_string(w) + "/" + std::to_string(i), std::string(700, 'a' + w), "image/png");
                tc.get("https://t/" + std::to_string(w) + "/" + std::to_string(i / 2), x);
                tc.usage();
            }
        });
    for (std::thread &x : th) x.join();
    CHECK(tc.usage() <= 20000);
    // the default place
    setenv("RSSREADER_CACHE", "/tmp/rsscache-test", 1);
    CHECK(ImageCache::default_dir() == "/tmp/rsscache-test");
    unsetenv("RSSREADER_CACHE");
    setenv("XDG_CACHE_HOME", "/x/cache", 1);
    CHECK(ImageCache::default_dir() == "/x/cache/rss-reader/images");
    unsetenv("XDG_CACHE_HOME");
}

namespace {

/* Answers every request with `body` after running `during` (what the user does while the feed downloads). */
class HookHttp : public HttpClient {
public:
    std::string body;
    std::function<void()> during;
    HttpResponse get(const HttpRequest &req) override
    {
        if (during) during();
        HttpResponse r;
        r.status = 200;
        r.body = body;
        r.final_url = req.url;
        r.content_type = "application/rss+xml";
        return r;
    }
};

std::string rss(const std::vector<int> &items)
{
    std::string s = "<?xml version=\"1.0\"?><rss version=\"2.0\"><channel><title>T</title><link>https://t.example.com</link>";
    for (int i : items)
        s += "<item><title>Item " + std::to_string(i) + "</title><guid>g" + std::to_string(i) + "</guid><pubDate>Sun, 18 Oct 2026 1" +
             std::to_string(i) + ":00:00 +0000</pubDate><description>x</description></item>";
    return s + "</channel></rss>";
}

} // namespace

/* The race of phase 2: a refresh loads the cache (with its read marks), downloads, merges, writes. A mark made during the
 * download must survive the write: read, unread, mark all read, clear the history. */
void test_store_fetched_race()
{
    TempDir t;
    Store st(t.path);
    Subscription s;
    s.id = "feed";
    s.name = "Feed";
    s.url = "https://t.example.com/rss";
    CHECK(st.add_subscription(s) == StoreError::Ok);
    HookHttp http;
    http.body = rss({1, 2, 3});
    Refresher rf(st, http, [] { return std::int64_t(1792339200); });
    CHECK(rf.refresh_feed(s).status == RefreshResult::Status::Updated);
    std::vector<StoredArticle> a = st.load_articles("feed");
    CHECK(a.size() == 3);
    const std::string id3 = a[0].article.id, id2 = a[1].article.id, id1 = a[2].article.id;
    CHECK(st.set_read("feed", id1, true));
    // read during the download: kept (phase 2 lost it: the merge carried the flags read before the download)
    http.body = rss({1, 2, 3, 4});
    http.during = [&] { st.set_read("feed", id3, true); };
    RefreshResult r = rf.refresh_feed(s);
    CHECK(r.status == RefreshResult::Status::Updated && r.new_articles == 1);
    a = st.load_articles("feed");
    auto read_of = [&](const std::string &id) {
        for (const StoredArticle &x : a)
            if (x.article.id == id) return x.read;
        return false;
    };
    CHECK(read_of(id3) && read_of(id1) && !read_of(id2));
    CHECK(a.size() == 4 && !a[0].read);                          // the new one: unread
    CHECK_EQ(st.unread_count("feed"), 2);
    // unread during the download: kept
    http.during = [&] { st.set_read("feed", id1, false); };
    rf.refresh_feed(s);
    a = st.load_articles("feed");
    CHECK(!read_of(id1) && read_of(id3));
    // mark all read during the download: everything that was there stays read, a new article is unread
    http.body = rss({1, 2, 3, 4, 5});
    http.during = [&] { st.mark_all_read("feed"); };
    r = rf.refresh_feed(s);
    a = st.load_articles("feed");
    CHECK(r.new_articles == 1 && a.size() == 5 && !a[0].read && a[1].read && a[2].read && a[3].read && a[4].read);
    CHECK_EQ(st.unread_count("feed"), 1);
    // clear the history during the download: everything unread
    http.during = [&] { st.clear_read_history(); };
    rf.refresh_feed(s);
    CHECK_EQ(st.unread_count("feed"), 5);
    // the direct call: the given flags do not count, the read file does
    std::vector<StoredArticle> mine = st.load_articles("feed");
    for (StoredArticle &x : mine) x.read = true;
    CHECK(st.set_read("feed", mine[2].article.id, true));
    CHECK(st.store_fetched_articles("feed", mine));
    CHECK_EQ(st.unread_count("feed"), 4);
    CHECK(!st.store_fetched_articles("../x", mine));
    // store_articles still writes the flags it is given (mark all read uses it)
    CHECK(st.store_articles("feed", mine));
    CHECK_EQ(st.unread_count("feed"), 0);
}

void test_store_options_seen()
{
    TempDir t;
    {
        Store st(t.path);
        CHECK(!st.settings().options_seen);
        Settings s = st.settings();
        s.options_seen = true;
        CHECK(st.set_settings(s));
        std::string text;
        CHECK(read_file(st.settings_path(), text) && text.find("\"options_seen\": true") != std::string::npos);
    }
    Store again(t.path);
    CHECK(again.settings().options_seen);
}

/* Saves from two threads take turns and both land; a delete while saving waits; the UI reads the store meanwhile. */
void test_store_saves_take_turns()
{
    TempDir t;
    Store st(t.path);
    std::vector<std::thread> th;
    std::atomic<int> ok{0};
    for (int w = 0; w < 3; ++w)
        th.emplace_back([&st, &ok, w] {
            for (int i = 0; i < 5; ++i) {
                Article a;
                a.id = hash16("s" + std::to_string(w) + "-" + std::to_string(i));
                a.title = "Saved " + std::to_string(i);
                SavedImage im;
                im.url = "https://x/" + a.id + ".jpg";
                im.content_type = "image/jpeg";
                im.bytes = std::string(200 * 1024, static_cast<char>('a' + w));
                if (st.save_article(a, "f", "Feed", {im}, 1000 + i) == StoreError::Ok) ++ok;
            }
        });
    for (int i = 0; i < 50; ++i) {
        st.list_saved();
        st.is_saved("nope");
        st.settings();
    }
    for (std::thread &x : th) x.join();
    CHECK_EQ(ok.load(), 15);
    CHECK_EQ(st.list_saved().size(), std::size_t(15));
    CHECK(st.saved_usage_bytes() >= 15u * 200 * 1024);
    // the files of each are whole
    for (const SavedSummary &s : st.list_saved()) {
        SavedArticle sa;
        std::string bytes;
        CHECK(st.load_saved(s.id, sa) && sa.images.size() == 1 && st.read_saved_image(s.id, sa.images[0].file, bytes) && bytes.size() == 200u * 1024);
    }
    Store again(t.path);
    CHECK_EQ(again.list_saved().size(), std::size_t(15));
}
