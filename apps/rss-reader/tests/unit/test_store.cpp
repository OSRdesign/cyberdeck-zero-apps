// store: settings, subscriptions, article caches, read state, saved articles; atomic writes and corrupt files.
#include "test_common.hpp"

#include "fsutil.hpp"
#include "json.hpp"
#include "store.hpp"
#include "util.hpp"

#include <thread>

using namespace rssreader;
using namespace rssreader_test;

static Subscription sub(const std::string &id, const std::string &url = "")
{
    Subscription s;
    s.id = id;
    s.name = "Feed " + id;
    s.url = url.empty() ? "https://" + id + ".example.com/rss" : url;
    return s;
}

static StoredArticle art(int n, bool read = false)
{
    StoredArticle sa;
    sa.article.id = hash16("art" + std::to_string(n));
    sa.article.guid = "g" + std::to_string(n);
    sa.article.title = "Article " + std::to_string(n);
    sa.article.link = "https://x.example.com/" + std::to_string(n);
    sa.article.published = 1700000000 + n;
    sa.article.summary_html = "<p>s</p>";
    sa.article.content_html = "<p>c \xC3\xA9 \"q\" \\ \n</p>";
    sa.article.images = {"https://x.example.com/i" + std::to_string(n) + ".png"};
    sa.article.has_full_text = true;
    sa.read = read;
    return sa;
}

static std::vector<StoredArticle> arts(int from, int to)    // newest first
{
    std::vector<StoredArticle> v;
    for (int i = to; i >= from; --i) v.push_back(art(i));
    return v;
}

static Article saved_article(int n)
{
    return art(n).article;
}

void test_atomic_files()
{
    TempDir t;
    const std::string p = t.path + "/f.txt";
    CHECK(write_file_atomic(p, "one"));
    std::string s;
    CHECK(read_file(p, s) && s == "one");
    CHECK(write_file_atomic(p, std::string(100000, 'x')) && file_size(p) == 100000);
    CHECK(!path_exists(p + ".tmp"));
    CHECK(write_file_atomic(p, "") && file_size(p) == 0);

    // a stale temp file from a crash does not matter
    write_file_atomic(p, "good");
    {
        std::FILE *f = std::fopen((p + ".tmp").c_str(), "wb");
        std::fputs("half-written garbage", f);
        std::fclose(f);
    }
    CHECK(write_file_atomic(p, "newer") && read_file(p, s) && s == "newer" && !path_exists(p + ".tmp"));

    // a failure leaves the old file untouched and leaves no temp file: make the temp name unusable (a directory)
    CHECK(make_dirs(p + ".tmp"));
    CHECK(!write_file_atomic(p, "lost"));
    CHECK(read_file(p, s) && s == "newer");
    remove_tree(p + ".tmp");
    CHECK(!write_file_atomic(t.path + "/missing-dir/f.txt", "x"));

    // fs helpers
    CHECK(make_dirs(t.path + "/a/b/c") && is_directory(t.path + "/a/b/c") && make_dirs(t.path + "/a/b/c"));
    CHECK(!make_dirs(p + "/sub"));                                   // a file in the way
    write_file_atomic(t.path + "/a/b/c/x", "12345");
    write_file_atomic(t.path + "/a/y", "123");
    CHECK_EQ(tree_size(t.path + "/a"), std::uint64_t(8));
    CHECK_EQ(list_dir(t.path + "/a").size(), std::size_t(2));
    CHECK(remove_tree(t.path + "/a") && !path_exists(t.path + "/a"));
    CHECK(remove_tree(t.path + "/never-existed"));
    CHECK(file_size(t.path + "/nothing") == -1);
    CHECK(!read_file(t.path + "/nothing", s));
    CHECK(!read_file(p, s, 2));                                       // over the size limit
}

void test_store_settings()
{
    TempDir t;
    {
        Store st(t.path + "/data");
        CHECK(st.open());
        Settings d = st.settings();
        CHECK(d.text_size == TextSize::Medium && d.image_download == ImageDownload::Always && d.storage_cap_mb == 200 && d.refresh_on_open);
        Settings s;
        s.text_size = TextSize::Large;
        s.image_download = ImageDownload::Off;
        s.storage_cap_mb = 500;
        s.refresh_on_open = false;
        CHECK(st.set_settings(s));
        CHECK_EQ(st.storage_cap_bytes(), std::uint64_t(500) * 1024 * 1024);
    }
    {
        Store st(t.path + "/data");
        Settings s = st.settings();
        CHECK(s.text_size == TextSize::Large && s.image_download == ImageDownload::Off && s.storage_cap_mb == 500 && !s.refresh_on_open);
        s.storage_cap_mb = 1;
        st.set_settings(s);
        CHECK_EQ(st.settings().storage_cap_mb, 10);                  // clamped
        s.storage_cap_mb = 1000000;
        st.set_settings(s);
        CHECK_EQ(st.settings().storage_cap_mb, 4096);
        std::string text;
        CHECK(read_file(st.settings_path(), text) && text.find("\"version\"") != std::string::npos && text.find("\"text_size\": \"large\"") != std::string::npos);
    }
    // unknown values fall back to the defaults; a newer schema is set aside
    write_file_atomic(t.path + "/data/settings.json", "{\"version\":1,\"text_size\":\"gigantic\",\"image_download\":\"maybe\",\"storage_cap_mb\":\"lots\"}");
    {
        Store st(t.path + "/data");
        Settings s = st.settings();
        CHECK(s.text_size == TextSize::Medium && s.image_download == ImageDownload::Always && s.storage_cap_mb == 200);
    }
    write_file_atomic(t.path + "/data/settings.json", "{\"version\":99,\"text_size\":\"large\"}");
    {
        Store st(t.path + "/data");
        CHECK(st.settings().text_size == TextSize::Medium);
        CHECK(path_exists(t.path + "/data/settings.json.bad"));
        CHECK(!path_exists(t.path + "/data/settings.json"));
        CHECK(st.set_settings(st.settings()) && path_exists(t.path + "/data/settings.json"));     // usable again
    }
    // corrupt: set aside as .bad, replacing an older .bad
    write_file_atomic(t.path + "/data/settings.json", "{\"version\":1, \"text_size\": \"large\"");   // truncated
    {
        Store st(t.path + "/data");
        CHECK(st.settings().text_size == TextSize::Medium);
        std::string bad;
        CHECK(read_file(t.path + "/data/settings.json.bad", bad) && bad.find("large") != std::string::npos);
    }
    // not writable: open fails, nothing throws
    {
        Store st(t.path + "/data/settings.json.bad/impossible");   // a file in the path
        CHECK(!st.open());
        CHECK(st.subscriptions().empty());
        CHECK(st.add_subscription(sub("a")) == StoreError::IoError);
        CHECK(!st.set_settings(Settings()));
        CHECK(st.load_articles("a").empty());
        CHECK(st.saved_usage_bytes() == 0);
    }
    // default root honours the environment
    ::setenv("RSSREADER_DATA", "/tmp/rss-data-x", 1);
    CHECK_EQ(Store::default_root(), "/tmp/rss-data-x");
    ::unsetenv("RSSREADER_DATA");
    ::setenv("XDG_DATA_HOME", "/tmp/xdg-x", 1);
    CHECK_EQ(Store::default_root(), "/tmp/xdg-x/rss-reader");
    ::unsetenv("XDG_DATA_HOME");
    CHECK(Store::default_root().find("rss-reader") != std::string::npos);
}

void test_store_subscriptions()
{
    TempDir t;
    const std::string root = t.path + "/d";
    {
        Store st(root);
        CHECK(st.subscriptions().empty());
        CHECK(st.add_subscription(sub("one")) == StoreError::Ok);
        CHECK(st.add_subscription(sub("two")) == StoreError::Ok);
        CHECK(st.add_subscription(sub("three")) == StoreError::Ok);
        CHECK(st.add_subscription(sub("one")) == StoreError::Duplicate);                          // same id
        CHECK(st.add_subscription(sub("four", "https://one.example.com/rss")) == StoreError::Duplicate);   // same url
        CHECK(st.add_subscription(sub("../evil")) == StoreError::BadArgument);
        CHECK(st.add_subscription(sub("")) == StoreError::BadArgument);
        CHECK(st.add_subscription(sub("_hidden")) == StoreError::BadArgument);
        CHECK(st.add_subscription(sub("bad-url", "ftp://x/y")) == StoreError::BadArgument);
        CHECK(st.add_subscription(sub("bad id")) == StoreError::BadArgument);
        std::vector<Subscription> v = st.subscriptions();
        CHECK(v.size() == 3 && v[0].id == "one" && v[1].id == "two" && v[2].id == "three");
        CHECK(st.move_subscription("three", 0));
        CHECK(st.move_subscription("one", 99));
        v = st.subscriptions();
        CHECK(v.size() == 3 && v[0].id == "three" && v[1].id == "two" && v[2].id == "one");
        CHECK(!st.move_subscription("nope", 0));

        Subscription f;
        CHECK(st.find_subscription("two", f) && f.name == "Feed two");
        CHECK(!st.find_subscription("nope", f));

        // fetch bookkeeping: validators only change on success
        CHECK(st.record_fetch("two", 1000, true, "\"e1\"", "lm1", ""));
        CHECK(st.find_subscription("two", f) && f.etag == "\"e1\"" && f.last_modified == "lm1" && f.last_fetch == 1000 && f.last_attempt == 1000 && f.last_error.empty());
        CHECK(st.record_fetch("two", 2000, false, "", "", "HTTP 503"));
        CHECK(st.find_subscription("two", f) && f.etag == "\"e1\"" && f.last_fetch == 1000 && f.last_attempt == 2000 && f.last_error == "HTTP 503");
        CHECK(st.record_fetch("two", 3000, true, "\"e2\"", "", ""));
        CHECK(st.find_subscription("two", f) && f.etag == "\"e2\"" && f.last_modified.empty() && f.last_error.empty());
        CHECK(!st.record_fetch("nope", 1, true, "", "", ""));
        CHECK(st.record_fetch("two", 4000, false, "", "", ""));
        CHECK(st.find_subscription("two", f) && f.last_error == "failed");
    }
    {
        Store st(root);                                              // persisted, in order
        std::vector<Subscription> v = st.subscriptions();
        CHECK(v.size() == 3 && v[0].id == "three" && v[2].id == "one");
        Subscription f;
        CHECK(st.find_subscription("two", f) && f.etag == "\"e2\"" && f.last_attempt == 4000);
        // removing deletes the cache but not the order of the others
        CHECK(st.store_articles("two", arts(1, 3)));
        CHECK(path_exists(st.cache_path("two")));
        CHECK(st.remove_subscription("two"));
        CHECK(!path_exists(st.cache_path("two")) && !st.remove_subscription("two"));
        CHECK(st.subscriptions().size() == 2);
        CHECK(st.add_subscription(sub("two")) == StoreError::Ok);   // can come back, empty
        CHECK(st.load_articles("two").empty());
    }
    // a damaged file: set aside, the app starts empty and can write again
    write_file_atomic(root + "/subscriptions.json", "{\"version\":1,\"feeds\":[{\"id\":\"a\",\"url\":\"https://a/\"},");
    {
        Store st(root);
        CHECK(st.subscriptions().empty());
        CHECK(path_exists(root + "/subscriptions.json.bad"));
        CHECK(st.add_subscription(sub("fresh")) == StoreError::Ok);
    }
    // damaged records inside a good file are skipped
    write_file_atomic(root + "/subscriptions.json",
                      "{\"version\":1,\"feeds\":[{\"id\":\"good\",\"url\":\"https://g.org/r\",\"name\":\"G\"},42,{\"id\":\"../x\",\"url\":\"https://h.org\"},"
                      "{\"id\":\"good\",\"url\":\"https://dup.org\"},{\"id\":\"nourl\"},{\"id\":\"neg\",\"url\":\"https://n.org\",\"unread\":-5,\"total\":-1}]}");
    {
        Store st(root);
        std::vector<Subscription> v = st.subscriptions();
        CHECK(v.size() == 2 && v[0].id == "good" && v[1].id == "neg" && v[1].unread == 0 && v[1].total == 0);
    }
    // an empty file, a JSON array, binary junk
    for (const char *junk : {"", "[]", "\x01\x02\xff", "null", "{}"}) {
        write_file_atomic(root + "/subscriptions.json", junk);
        Store st(root);
        CHECK(st.subscriptions().empty());
    }
}

void test_store_articles()
{
    TempDir t;
    Store st(t.path + "/d");
    CHECK(st.add_subscription(sub("f1")) == StoreError::Ok);
    CHECK(st.add_subscription(sub("f2")) == StoreError::Ok);

    std::vector<StoredArticle> in = arts(1, 5);
    in[1].read = true;                                               // article 4 read
    CHECK(st.store_articles("f1", in));
    std::vector<StoredArticle> out = st.load_articles("f1");
    CHECK_EQ(out.size(), std::size_t(5));
    if (out.size() == 5) {
        CHECK(out[0].article.title == "Article 5" && !out[0].read && out[1].read && !out[2].read);
        const Article &a = out[0].article;
        const Article &b = in[0].article;
        CHECK(a.id == b.id && a.guid == b.guid && a.link == b.link && a.published == b.published && a.summary_html == b.summary_html &&
              a.content_html == b.content_html && a.images == b.images && a.has_full_text == b.has_full_text && a.dated == b.dated);
    }
    CHECK_EQ(st.unread_count("f1"), 4);
    CHECK_EQ(st.unread_count("f2"), 0);
    CHECK_EQ(st.unread_count("nope"), 0);

    // read state
    CHECK(st.set_read("f1", art(5).article.id, true));
    CHECK_EQ(st.unread_count("f1"), 3);
    CHECK(st.set_read("f1", art(5).article.id, true));               // idempotent
    CHECK_EQ(st.unread_count("f1"), 3);
    CHECK(st.set_read("f1", art(4).article.id, false));
    CHECK_EQ(st.unread_count("f1"), 4);
    out = st.load_articles("f1");
    CHECK(out[0].read && !out[1].read);
    CHECK_EQ(st.total_unread(), 4);
    CHECK(!st.set_read("../x", "id", true));
    CHECK(!st.set_read("f1", "", true));
    CHECK(st.mark_all_read("f1"));
    CHECK_EQ(st.unread_count("f1"), 0);
    out = st.load_articles("f1");
    bool all = true;
    for (const StoredArticle &a : out) all = all && a.read;
    CHECK(all && out.size() == 5);
    CHECK(!st.mark_all_read("../x"));

    // persists; the read file and the cache are separate small files
    {
        Store again(t.path + "/d");
        CHECK_EQ(again.unread_count("f1"), 0);
        CHECK_EQ(again.load_articles("f1").size(), std::size_t(5));
        CHECK(again.cache_usage_bytes() > 0);
    }
    // read state is pruned with the articles: a new list without article 1 drops its read flag
    std::vector<StoredArticle> next = arts(2, 8);
    for (StoredArticle &a : next) a.read = (a.article.id == art(3).article.id);
    CHECK(st.store_articles("f1", next));
    std::string rtext;
    Json rj;
    CHECK(read_file(t.path + "/d/read/f1.json", rtext) && json_parse(rtext, rj) && rj.get("read") && rj.get("read")->arr.size() == 1);
    CHECK_EQ(st.unread_count("f1"), 6);

    // duplicates in the input are dropped, later calls replace the whole list
    std::vector<StoredArticle> dup = arts(1, 3);
    dup.push_back(art(2));
    CHECK(st.store_articles("f2", dup));
    CHECK_EQ(st.load_articles("f2").size(), std::size_t(3));
    CHECK(st.store_articles("f2", {}));
    CHECK(st.load_articles("f2").empty());
    CHECK_EQ(st.unread_count("f2"), 0);

    // 250 in, 200 kept (the newest, in the given order)
    CHECK(st.store_articles("f1", arts(1, 250)));
    out = st.load_articles("f1");
    CHECK_EQ(out.size(), std::size_t(200));
    CHECK(out.front().article.title == "Article 250" && out.back().article.title == "Article 51");
    CHECK_EQ(st.unread_count("f1"), 200);

    // damaged cache: set aside, empty list, the next store works
    write_file_atomic(st.cache_path("f1"), "{\"version\":1,\"articles\":[{\"id\":\"x\"");
    CHECK(st.load_articles("f1").empty());
    CHECK(path_exists(st.cache_path("f1") + ".bad"));
    CHECK(st.store_articles("f1", arts(1, 2)));
    CHECK_EQ(st.load_articles("f1").size(), std::size_t(2));
    // damaged read file: everything unread, nothing lost
    write_file_atomic(t.path + "/d/read/f1.json", "garbage");
    out = st.load_articles("f1");
    CHECK(out.size() == 2 && !out[0].read);
    CHECK(path_exists(t.path + "/d/read/f1.json.bad"));
    // damaged records inside a good cache are skipped
    write_file_atomic(st.cache_path("f2"), "{\"version\":1,\"articles\":[42,{\"title\":\"no id\"},{\"id\":\"ok1\",\"title\":\"fine\",\"images\":[1,\"u\",\"\"]},{\"id\":\"ok1\"}]}");
    out = st.load_articles("f2");
    CHECK(out.size() == 1 && out[0].article.title == "fine" && out[0].article.images.size() == 1);
    CHECK(st.store_articles("not a valid id", arts(1, 1)) == false);
}

void test_store_saved()
{
    TempDir t;
    Store st(t.path + "/d");
    std::vector<SavedImage> imgs = {{"https://x.example.com/a.png", "image/png", std::string(1000, 'p')},
                                    {"https://x.example.com/b.jpg", "image/jpeg", std::string(2000, 'j')},
                                    {"https://x.example.com/empty.png", "image/png", ""}};
    Article a = saved_article(1);
    CHECK(!st.is_saved(a.id));
    CHECK(st.save_article(a, "f1", "Feed One", imgs, 5000) == StoreError::Ok);
    CHECK(st.is_saved(a.id));
    CHECK(path_exists(st.saved_dir(a.id) + "/article.json") && path_exists(st.saved_dir(a.id) + "/images/001.png") && path_exists(st.saved_dir(a.id) + "/images/002.jpg"));
    CHECK(!path_exists(st.saved_dir(a.id) + "/images/003.png"));                // empty images are skipped
    CHECK(!path_exists(t.path + "/d/saved/.tmp-" + a.id));
    CHECK_EQ(file_size(st.saved_dir(a.id) + "/images/001.png"), 1000);

    SavedArticle sa;
    CHECK(st.load_saved(a.id, sa));
    CHECK(sa.article.title == a.title && sa.article.content_html == a.content_html && sa.feed_id == "f1" && sa.feed_name == "Feed One" && sa.saved_at == 5000);
    CHECK_EQ(sa.images.size(), std::size_t(2));
    if (sa.images.size() == 2) {
        CHECK(sa.images[0].url == "https://x.example.com/a.png" && sa.images[0].file == "images/001.png" && sa.images[0].bytes == 1000 && sa.images[0].content_type == "image/png");
        CHECK(sa.images[1].file == "images/002.jpg");
    }
    std::string bytes;
    CHECK(st.read_saved_image(a.id, "images/002.jpg", bytes) && bytes == std::string(2000, 'j'));
    CHECK(!st.read_saved_image(a.id, "images/../article.json", bytes));
    CHECK(!st.read_saved_image(a.id, "../../subscriptions.json", bytes));
    CHECK(!st.read_saved_image(a.id, "article.json", bytes));
    CHECK(!st.read_saved_image(a.id, "images/9.png", bytes));
    CHECK(!st.read_saved_image("nope", "images/001.png", bytes));
    CHECK(!st.load_saved("nope", sa) && !st.load_saved("../x", sa));

    std::uint64_t usage = st.saved_usage_bytes();
    CHECK(usage >= 3000 && usage < 3000 + 20000);
    CHECK_EQ(sa.bytes, usage);

    // saving again replaces, it does not double the usage
    CHECK(st.save_article(a, "f1", "Feed One", imgs, 6000) == StoreError::Ok);
    CHECK_EQ(st.saved_usage_bytes(), usage);
    CHECK(st.save_article(a, "f1", "Feed One", {}, 7000) == StoreError::Ok);
    CHECK(st.saved_usage_bytes() < 20000 && !path_exists(st.saved_dir(a.id) + "/images/001.png"));
    CHECK(st.save_article(a, "f1", "Feed One", imgs, 8000) == StoreError::Ok);

    Article b = saved_article(2);
    CHECK(st.save_article(b, "f2", "Feed Two", {}, 9000) == StoreError::Ok);
    std::vector<SavedSummary> list = st.list_saved();
    CHECK(list.size() == 2 && list[0].id == b.id && list[1].id == a.id);        // newest saved first
    CHECK(list.size() == 2 && list[1].image_count == 2 && list[1].title == a.title && list[1].feed_name == "Feed One" && list[1].bytes >= 3000);

    // persisted
    {
        Store again(t.path + "/d");
        CHECK(again.is_saved(a.id) && again.list_saved().size() == 2);
        CHECK_EQ(again.saved_usage_bytes(), st.saved_usage_bytes());
    }

    // bad ids
    Article bad = saved_article(3);
    bad.id = "../escape";
    CHECK(st.save_article(bad, "f", "F", {}, 1) == StoreError::BadArgument);
    bad.id = "";
    CHECK(st.save_article(bad, "f", "F", {}, 1) == StoreError::BadArgument);
    CHECK(!path_exists(t.path + "/escape"));

    // delete
    CHECK(st.delete_saved(b.id) && !st.is_saved(b.id) && !path_exists(st.saved_dir(b.id)));
    CHECK(!st.delete_saved(b.id) && !st.delete_saved("../x"));
    CHECK_EQ(st.list_saved().size(), std::size_t(1));
}

void test_store_cap()
{
    TempDir t;
    Store st(t.path + "/d");
    Settings s;
    s.storage_cap_mb = 10;
    CHECK(st.set_settings(s));
    const std::string meg(1024 * 1024, 'z');

    auto big = [&](int n, int mb) {
        std::vector<SavedImage> v;
        for (int i = 0; i < mb; ++i) v.push_back({"https://x.example.com/" + std::to_string(i), "image/jpeg", meg});
        return st.save_article(saved_article(n), "f", "F", v, 100 + n);
    };
    CHECK(big(1, 4) == StoreError::Ok);
    CHECK(big(2, 4) == StoreError::Ok);
    CHECK(big(3, 4) == StoreError::CapExceeded);                       // would take it to 12 MB
    CHECK(!st.is_saved(saved_article(3).id) && !path_exists(st.saved_dir(saved_article(3).id)));
    CHECK(!path_exists(t.path + "/d/saved/.tmp-" + saved_article(3).id));
    CHECK(big(3, 1) == StoreError::Ok);                                // 9 MB in all
    CHECK(st.saved_usage_bytes() <= st.storage_cap_bytes());           // never above the cap
    CHECK(st.list_saved().size() == 3);                                // nothing is evicted

    // replacing an article may grow it only while the total stays under the cap; a refusal keeps the old copy
    const std::uint64_t before = st.saved_usage_bytes();
    CHECK(big(1, 6) == StoreError::CapExceeded);
    CHECK_EQ(st.saved_usage_bytes(), before);
    SavedArticle old_copy;
    CHECK(st.load_saved(saved_article(1).id, old_copy) && old_copy.images.size() == 4);

    // raising the cap lets it through; deleting makes room
    s.storage_cap_mb = 100;
    CHECK(st.set_settings(s));
    CHECK(big(4, 6) == StoreError::Ok);
    CHECK(st.saved_usage_bytes() > 10u * 1024 * 1024);
    s.storage_cap_mb = 10;
    CHECK(st.set_settings(s));
    CHECK(big(5, 1) == StoreError::CapExceeded);                       // already over the (lowered) cap: new saves refused, nothing deleted
    CHECK(st.is_saved(saved_article(4).id));
    CHECK(st.delete_saved(saved_article(4).id));
    CHECK(st.delete_saved(saved_article(1).id));
    CHECK(big(5, 1) == StoreError::Ok);

    // per-article limit
    s.storage_cap_mb = 4096;
    CHECK(st.set_settings(s));
    CHECK(big(6, 21) == StoreError::ArticleTooLarge);
    CHECK(!st.is_saved(saved_article(6).id));
    // an article with huge text
    Article huge = saved_article(7);
    huge.content_html.assign(kMaxSavedArticleBytes + 10, 'a');
    CHECK(st.save_article(huge, "f", "F", {}, 1) == StoreError::ArticleTooLarge);
}

void test_store_recovery()
{
    TempDir t;
    const std::string root = t.path + "/d";
    Article a = saved_article(1), b = saved_article(2), c = saved_article(3);
    {
        Store st(root);
        CHECK(st.save_article(a, "f", "F", {{"https://x/i.png", "image/png", "abc"}}, 1) == StoreError::Ok);
        CHECK(st.save_article(b, "f", "F", {}, 2) == StoreError::Ok);
        CHECK(st.save_article(c, "f", "F", {}, 3) == StoreError::Ok);
    }
    // an interrupted replacement: the old copy was moved aside, the new one never arrived
    rename_path(root + "/saved/" + a.id, root + "/saved/.old-" + a.id);
    // an interrupted first save: staging left behind
    make_dirs(root + "/saved/.tmp-" + b.id + "/images");
    write_file_atomic(root + "/saved/.tmp-" + b.id + "/article.json", "{}");
    // an old copy left next to a good new one
    make_dirs(root + "/saved/.old-" + c.id);
    // a damaged saved article
    make_dirs(root + "/saved/0123456789abcdef");
    write_file_atomic(root + "/saved/0123456789abcdef/article.json", "{not json");
    // a folder that is not an article at all
    write_file_atomic(root + "/saved/stray-file.txt", "x");
    {
        Store st(root);
        CHECK(st.is_saved(a.id));                                       // restored from .old
        CHECK(st.is_saved(b.id) && st.is_saved(c.id));
        CHECK(!path_exists(root + "/saved/.old-" + a.id) && !path_exists(root + "/saved/.tmp-" + b.id) && !path_exists(root + "/saved/.old-" + c.id));
        CHECK(!st.is_saved("0123456789abcdef") && path_exists(root + "/saved/0123456789abcdef.bad"));
        CHECK_EQ(st.list_saved().size(), std::size_t(3));
        SavedArticle sa;
        CHECK(st.load_saved(a.id, sa) && sa.images.size() == 1);
        std::string bytes;
        CHECK(st.read_saved_image(a.id, "images/001.png", bytes) && bytes == "abc");
    }
    // an article whose folder name does not match its id is not trusted
    rename_path(root + "/saved/" + b.id, root + "/saved/aaaaaaaaaaaaaaaa");
    {
        Store st(root);
        CHECK(!st.is_saved(b.id) && !st.is_saved("aaaaaaaaaaaaaaaa"));
    }
}

void test_store_saved_kept_in_cache()
{
    TempDir t;
    Store st(t.path + "/d");
    CHECK(st.add_subscription(sub("f1")) == StoreError::Ok);
    // article 1 (the oldest) is saved: it survives the 200 cut
    CHECK(st.save_article(art(1).article, "f1", "F", {}, 1) == StoreError::Ok);
    CHECK(st.store_articles("f1", arts(1, 260)));
    std::vector<StoredArticle> out = st.load_articles("f1");
    CHECK_EQ(out.size(), std::size_t(201));
    CHECK(out.back().article.id == art(1).article.id);
    CHECK_EQ(st.unread_count("f1"), 201);
    // deleting the saved copy lets it go at the next store
    CHECK(st.delete_saved(art(1).article.id));
    CHECK(st.store_articles("f1", out));
    CHECK_EQ(st.load_articles("f1").size(), std::size_t(200));
    // at most 50 saved articles beyond the 200
    for (int i = 1; i <= 60; ++i) CHECK(st.save_article(art(i).article, "f1", "F", {}, i) == StoreError::Ok);
    CHECK(st.store_articles("f1", arts(1, 300)));
    CHECK_EQ(st.load_articles("f1").size(), std::size_t(250));
}

void test_store_threads()
{
    TempDir t;
    Store st(t.path + "/d");
    for (int i = 0; i < 4; ++i) CHECK(st.add_subscription(sub("f" + std::to_string(i))) == StoreError::Ok);
    for (int i = 0; i < 4; ++i) CHECK(st.store_articles("f" + std::to_string(i), arts(1, 30)));
    std::vector<std::thread> th;
    for (int w = 0; w < 4; ++w)
        th.emplace_back([&st, w] {
            const std::string f = "f" + std::to_string(w);
            for (int i = 1; i <= 30; ++i) st.set_read(f, art(i).article.id, true);
            for (int i = 0; i < 20; ++i) {
                st.subscriptions();
                st.total_unread();
                st.load_articles(f);
            }
        });
    for (std::thread &x : th) x.join();
    CHECK_EQ(st.total_unread(), 0);
    for (int i = 0; i < 4; ++i) CHECK_EQ(st.unread_count("f" + std::to_string(i)), 0);
    Store again(t.path + "/d");
    CHECK_EQ(again.total_unread(), 0);
}
