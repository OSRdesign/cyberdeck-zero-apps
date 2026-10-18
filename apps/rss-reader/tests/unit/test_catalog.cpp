// The bundled catalog (data/catalog.json) and the catalog queries.
#include "test_common.hpp"

#include "catalog.hpp"
#include "json.hpp"
#include "url.hpp"
#include "util.hpp"

#include <algorithm>
#include <set>

using namespace rssreader;

#ifndef CATALOG_PATH
#define CATALOG_PATH "../../data/catalog.json"
#endif

static bool iso_date(const std::string &s)
{
    if (s.size() < 10) return false;
    for (std::size_t i = 0; i < 10; ++i) {
        bool dash = i == 4 || i == 7;
        if (dash ? s[i] != '-' : !(s[i] >= '0' && s[i] <= '9')) return false;
    }
    return true;
}

static bool id_ok(const std::string &s)
{
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return s[0] != '-' && s.back() != '-';
}

void test_catalog_data()
{
    Catalog cat;
    CHECK(cat.load_file(CATALOG_PATH));
    CHECK_EQ(cat.version(), 1);
    CHECK_EQ(cat.generated(), "2026-10-18");
    CHECK_EQ(cat.size(), std::size_t(269));

    // the file itself: every record must be accepted by the loader (none silently skipped) and have exactly the documented fields
    std::string text;
    CHECK(read_file(CATALOG_PATH, text));
    Json root;
    CHECK(json_parse(text, root));
    const Json *list = root.get("feeds");
    CHECK(list && list->is_array() && list->arr.size() == cat.size());
    static const char *fields[] = {"id", "name", "country", "lang", "topic", "url", "site", "format", "content", "items", "newest", "checked", "notes"};
    if (list)
        for (const Json &f : list->arr) {
            CHECK_EQ(f.obj.size(), std::size_t(13));
            for (const char *k : fields) CHECK(f.get(k) != nullptr);
        }

    std::set<std::string> ids, urls, topics(known_topics().begin(), known_topics().end());
    static const std::set<std::string> countries = {"INT", "US", "GB", "FR", "DE", "CA", "ES", "JP", "IT", "BR", "NL", "PT"};
    int http_only = 0;
    for (const CatalogFeed &f : cat.all()) {
        CHECK(ids.insert(f.id).second);
        CHECK(urls.insert(lower(f.url)).second);
        CHECK(id_ok(f.id));
        CHECK(!f.name.empty() && f.name == collapse_ws(f.name));
        CHECK(topics.count(f.topic) == 1);
        CHECK(countries.count(f.country) == 1);
        CHECK(f.lang.size() == 2);
        CHECK(is_http_url(f.url));
        CHECK(is_http_url(f.site));
        CHECK(f.format == "rss" || f.format == "atom");
        CHECK(f.content == "summary" || f.content == "full");
        CHECK(f.items > 0);
        CHECK(iso_date(f.checked) && f.checked == "2026-10-18");
        CHECK(f.newest.empty() || iso_date(f.newest));
        CHECK(starts_with(f.id, lower(f.country) + "-"));
        CHECK(utf8_sanitize(f.name) == f.name);
        if (starts_with(f.url, "http://")) ++http_only;
        if (starts_with(f.url, "http://")) std::printf("note: plain http in the catalog: %s\n", f.url.c_str());
    }
    CHECK(http_only <= 5);                                    // https is preferred; the few http ones are publisher-only feeds
    CHECK(http_only * 50 < static_cast<int>(cat.size()));     // at most 2%

    // every topic and country is used (no dead entries), every feed is reachable by browsing
    std::size_t by_country = 0;
    for (const CatalogGroup &g : cat.countries()) by_country += g.feeds;
    CHECK_EQ(by_country, cat.size());
    std::size_t by_lang = 0;
    for (const CatalogGroup &g : cat.languages()) by_lang += g.feeds;
    CHECK_EQ(by_lang, cat.size());
    std::size_t by_topic = 0;
    for (const CatalogGroup &g : cat.topics_in_country("")) by_topic += g.feeds;
    CHECK_EQ(by_topic, cat.size());
}

void test_catalog_queries()
{
    Catalog cat;
    CHECK(cat.load_file(CATALOG_PATH));

    std::vector<CatalogGroup> cs = cat.countries();
    CHECK_EQ(cs.size(), std::size_t(12));
    CHECK(!cs.empty() && cs[0].key == "INT" && cs[0].label == "International");     // International first
    for (std::size_t i = 2; i < cs.size(); ++i) CHECK(fold_ascii(cs[i - 1].label) <= fold_ascii(cs[i].label));
    bool fr = false;
    for (const CatalogGroup &g : cs)
        if (g.key == "FR") { fr = true; CHECK_EQ(g.label, "France"); CHECK(g.feeds == 27); }
    CHECK(fr);
    std::vector<CatalogGroup> ls = cat.languages();
    CHECK_EQ(ls.size(), std::size_t(8));
    for (const CatalogGroup &g : ls)
        if (g.key == "en") CHECK_EQ(g.feeds, std::size_t(129));
    CHECK_EQ(language_label("ja"), "Japanese");
    CHECK_EQ(language_label("xx"), "xx");
    CHECK_EQ(country_label("XX"), "XX");
    CHECK_EQ(topic_label("linux"), "Linux");
    CHECK_EQ(topic_label("technology"), "Technology");

    std::vector<CatalogGroup> ts = cat.topics_in_country("FR");
    CHECK(!ts.empty());
    for (std::size_t i = 1; i < ts.size(); ++i) CHECK(ts[i - 1].feeds >= ts[i].feeds);
    std::size_t sum = 0;
    for (const CatalogGroup &g : ts) sum += g.feeds;
    CHECK_EQ(sum, std::size_t(27));
    CHECK(cat.topics_in_country("ZZ").empty());
    CHECK(!cat.topics_in_language("fr").empty());

    std::vector<const CatalogFeed *> fs = cat.feeds_in("FR", ts[0].key);
    CHECK_EQ(fs.size(), ts[0].feeds);
    for (std::size_t i = 0; i < fs.size(); ++i) {
        CHECK(fs[i]->country == "FR" && fs[i]->topic == ts[0].key);
        if (i) CHECK(fold_ascii(fs[i - 1]->name) <= fold_ascii(fs[i]->name));
    }
    CHECK_EQ(cat.feeds_in("FR", "").size(), std::size_t(27));
    CHECK(cat.feeds_in("FR", "no-such-topic").empty());
    CHECK(cat.feeds_in("ZZ", "general").empty());
    CHECK_EQ(cat.feeds_in("", "").size(), cat.size());
    for (const CatalogFeed *f : cat.feeds_in_language("pt", "")) CHECK_EQ(f->lang, "pt");
    CHECK_EQ(cat.feeds_in_language("pt", "").size(), std::size_t(22));

    // lookup
    const CatalogFeed *a = cat.find("int-dw-english");
    CHECK(a && a->name == "DW English");
    CHECK(cat.find("nope") == nullptr);
    CHECK(cat.find("") == nullptr);
    CHECK(a && cat.find_by_url(a->url) == a);
    CHECK(a && cat.find_by_url("HTTP://RSS.DW.COM/xml/rss-en-all/") == a);          // scheme, case and trailing slash do not matter
    CHECK(cat.find_by_url("https://example.org/none") == nullptr);

    // search: names, host, topic; every word; case and accents ignored
    std::vector<const CatalogFeed *> r = cat.search("DW english");
    CHECK(!r.empty());
    bool found = false;
    for (const CatalogFeed *f : r) found = found || f->id == "int-dw-english";
    CHECK(found);
    CHECK(cat.search("").empty());
    CHECK(cat.search("   ").empty());
    CHECK(cat.search("zzzzqqq").empty());
    CHECK(cat.search("e", 5).size() == 5);                                       // limit
    CHECK(!cat.search("linux").empty());                                          // topic words match
    const CatalogFeed *accented = nullptr;
    for (const CatalogFeed &f : cat.all())
        if (fold_ascii(f.name) != lower(f.name)) { accented = &f; break; }
    if (accented) {
        std::vector<const CatalogFeed *> hit = cat.search(fold_ascii(accented->name));
        bool ok = false;
        for (const CatalogFeed *f : hit) ok = ok || f == accented;
        CHECK(ok);
        hit = cat.search(accented->name);
        ok = false;
        for (const CatalogFeed *f : hit) ok = ok || f == accented;
        CHECK(ok);
    } else {
        std::printf("note: no accented feed names in the catalog, accent search tested synthetically only\n");
    }
}

void test_catalog_loader()
{
    Catalog c;
    CHECK(!c.load_json(""));
    CHECK(!c.load_json("not json"));
    CHECK(!c.load_json("{\"version\":1}"));                      // no feeds
    CHECK(!c.load_json("{\"feeds\":{}}"));
    CHECK(!c.load_json("42"));
    CHECK(!c.load_file("/nonexistent/catalog.json"));
    CHECK(c.size() == 0);

    CHECK(c.load_json("[]") && c.size() == 0);
    CHECK(c.load_json("[{\"id\":\"a-x\",\"name\":\"X\",\"url\":\"https://x.org/rss\"}]") && c.size() == 1);
    CHECK(c.all()[0].country == "INT" && c.all()[0].lang == "en" && c.all()[0].topic == "general");   // defaults

    // bad records are skipped, good ones kept; duplicate ids dropped
    const std::string mixed =
        "{\"version\":1,\"feeds\":["
        "{\"id\":\"ok-1\",\"name\":\"Ok One\",\"url\":\"https://a.org/rss\",\"country\":\"FR\",\"lang\":\"fr\",\"topic\":\"science\"},"
        "{\"id\":\"ok-1\",\"name\":\"Duplicate\",\"url\":\"https://b.org/rss\"},"
        "{\"id\":\"no-url\",\"name\":\"No URL\"},"
        "{\"id\":\"bad-url\",\"name\":\"Bad\",\"url\":\"ftp://c.org/rss\"},"
        "{\"id\":\"\",\"name\":\"No id\",\"url\":\"https://d.org/rss\"},"
        "{\"id\":\"no-name\",\"url\":\"https://e.org/rss\"},"
        "42, null, \"str\", [],"
        "{\"id\":\"ok-2\",\"name\":\"  Spaced   Name \",\"url\":\" https://f.org/rss \",\"items\":\"7\"}"
        "]}";
    CHECK(c.load_json(mixed));
    CHECK_EQ(c.size(), std::size_t(2));
    CHECK(c.find("ok-2") && c.find("ok-2")->name == "Spaced Name" && c.find("ok-2")->url == "https://f.org/rss" && c.find("ok-2")->items == 0);
    CHECK(c.find("ok-1") && c.find("ok-1")->name == "Ok One");
    CHECK_EQ(c.countries().size(), std::size_t(2));
    CHECK(c.search("ok one").size() == 1);
    CHECK(c.search("SPACED").size() == 1);
    CHECK(c.load_json("{\"feeds\":[]}") && c.version() == 1);
}
