/*
 * SPDX-License-Identifier: MIT
 */
#include "catalog.hpp"

#include "json.hpp"
#include "url.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace rssreader {

const std::vector<std::string> &known_topics()
{
    static const std::vector<std::string> t = {"general", "world", "business", "technology", "science", "space", "health", "sport",
                                               "culture", "security", "linux", "maker", "aviation", "maritime", "weather", "radio",
                                               "podcasts"};
    return t;
}

namespace {

struct Pair { const char *key; const char *label; };

std::string lookup(const Pair *table, std::size_t n, const std::string &key)
{
    for (std::size_t i = 0; i < n; ++i)
        if (key == table[i].key) return table[i].label;
    return key;
}

std::string norm_url(const std::string &u)
{
    std::string s = lower(trim(u));
    if (starts_with(s, "https://")) s = s.substr(8);
    else if (starts_with(s, "http://")) s = s.substr(7);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

bool by_label(const CatalogGroup &a, const CatalogGroup &b) { return fold_ascii(a.label) < fold_ascii(b.label); }

} // namespace

std::string country_label(const std::string &c)
{
    static const Pair t[] = {{"INT", "International"}, {"US", "United States"}, {"GB", "United Kingdom"}, {"FR", "France"},
                             {"DE", "Germany"}, {"CA", "Canada"}, {"ES", "Spain"}, {"JP", "Japan"}, {"IT", "Italy"},
                             {"BR", "Brazil"}, {"NL", "Netherlands"}, {"PT", "Portugal"}, {"AU", "Australia"}, {"BE", "Belgium"},
                             {"CH", "Switzerland"}, {"AT", "Austria"}, {"IE", "Ireland"}, {"MX", "Mexico"}, {"AR", "Argentina"},
                             {"IN", "India"}, {"SE", "Sweden"}, {"NO", "Norway"}, {"DK", "Denmark"}, {"FI", "Finland"},
                             {"PL", "Poland"}, {"NZ", "New Zealand"}, {"ZA", "South Africa"}};
    return lookup(t, sizeof t / sizeof t[0], c);
}

std::string language_label(const std::string &c)
{
    static const Pair t[] = {{"en", "English"}, {"fr", "French"}, {"de", "German"}, {"es", "Spanish"}, {"it", "Italian"},
                             {"pt", "Portuguese"}, {"nl", "Dutch"}, {"ja", "Japanese"}, {"sv", "Swedish"}, {"no", "Norwegian"},
                             {"da", "Danish"}, {"fi", "Finnish"}, {"pl", "Polish"}, {"ca", "Catalan"}};
    return lookup(t, sizeof t / sizeof t[0], c);
}

std::string topic_label(const std::string &topic)
{
    if (topic.empty()) return topic;
    if (topic == "linux") return "Linux";
    std::string s = topic;
    if (s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 32);
    return s;
}

bool Catalog::load_json(const std::string &text)
{
    feeds_.clear();
    version_ = 0;
    generated_.clear();
    Json root;
    if (!json_parse(text, root)) return false;
    const Json *list = nullptr;
    if (root.is_array()) {
        list = &root;                               /* a bare array of feeds is accepted too */
    } else if (root.is_object()) {
        version_ = static_cast<int>(root.get_int("version", 0));
        generated_ = root.get_string("generated");
        list = root.get("feeds");
    }
    if (!list || !list->is_array()) return false;
    std::set<std::string> ids;
    for (const Json &f : list->arr) {
        if (!f.is_object()) continue;
        CatalogFeed c;
        c.id = f.get_string("id");
        c.name = collapse_ws(f.get_string("name"));
        c.url = trim(f.get_string("url"));
        if (c.id.empty() || c.name.empty() || !is_http_url(c.url) || !ids.insert(c.id).second) continue;
        c.country = f.get_string("country", "INT");
        c.lang = f.get_string("lang", "en");
        c.topic = f.get_string("topic", "general");
        c.site = f.get_string("site");
        c.format = f.get_string("format", "rss");
        c.content = f.get_string("content", "summary");
        c.items = static_cast<int>(f.get_int("items"));
        c.newest = f.get_string("newest");
        c.checked = f.get_string("checked");
        c.notes = f.get_string("notes");
        feeds_.push_back(std::move(c));
    }
    if (version_ == 0) version_ = 1;
    return true;
}

bool Catalog::load_file(const std::string &path)
{
    feeds_.clear();
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[8192];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < 8u * 1024 * 1024) text.append(buf, n);
    std::fclose(f);
    return load_json(text);
}

std::vector<CatalogGroup> Catalog::countries() const
{
    std::map<std::string, std::size_t> n;
    for (const CatalogFeed &f : feeds_) ++n[f.country];
    std::vector<CatalogGroup> out;
    for (const auto &kv : n) out.push_back({kv.first, country_label(kv.first), kv.second});
    std::sort(out.begin(), out.end(), [](const CatalogGroup &a, const CatalogGroup &b) {
        if ((a.key == "INT") != (b.key == "INT")) return a.key == "INT";
        return by_label(a, b);
    });
    return out;
}

std::vector<CatalogGroup> Catalog::languages() const
{
    std::map<std::string, std::size_t> n;
    for (const CatalogFeed &f : feeds_) ++n[f.lang];
    std::vector<CatalogGroup> out;
    for (const auto &kv : n) out.push_back({kv.first, language_label(kv.first), kv.second});
    std::sort(out.begin(), out.end(), by_label);
    return out;
}

static std::vector<CatalogGroup> topic_groups(const std::vector<CatalogFeed> &feeds, const std::string &field, const std::string &key)
{
    std::map<std::string, std::size_t> n;
    for (const CatalogFeed &f : feeds) {
        const std::string &v = field == "country" ? f.country : f.lang;
        if (key.empty() || v == key) ++n[f.topic];
    }
    std::vector<CatalogGroup> out;
    for (const auto &kv : n) out.push_back({kv.first, topic_label(kv.first), kv.second});
    std::sort(out.begin(), out.end(), [](const CatalogGroup &a, const CatalogGroup &b) {
        if (a.feeds != b.feeds) return a.feeds > b.feeds;
        return a.label < b.label;
    });
    return out;
}

std::vector<CatalogGroup> Catalog::topics_in_country(const std::string &country) const { return topic_groups(feeds_, "country", country); }
std::vector<CatalogGroup> Catalog::topics_in_language(const std::string &lang) const { return topic_groups(feeds_, "lang", lang); }

static void sort_by_name(std::vector<const CatalogFeed *> &v)
{
    std::sort(v.begin(), v.end(), [](const CatalogFeed *a, const CatalogFeed *b) {
        std::string x = fold_ascii(a->name), y = fold_ascii(b->name);
        return x != y ? x < y : a->id < b->id;
    });
}

std::vector<const CatalogFeed *> Catalog::feeds_in(const std::string &country, const std::string &topic) const
{
    std::vector<const CatalogFeed *> v;
    for (const CatalogFeed &f : feeds_)
        if ((country.empty() || f.country == country) && (topic.empty() || f.topic == topic)) v.push_back(&f);
    sort_by_name(v);
    return v;
}

std::vector<const CatalogFeed *> Catalog::feeds_in_language(const std::string &lang, const std::string &topic) const
{
    std::vector<const CatalogFeed *> v;
    for (const CatalogFeed &f : feeds_)
        if ((lang.empty() || f.lang == lang) && (topic.empty() || f.topic == topic)) v.push_back(&f);
    sort_by_name(v);
    return v;
}

std::vector<const CatalogFeed *> Catalog::search(const std::string &query, std::size_t limit) const
{
    std::vector<std::string> words;
    std::istringstream in(fold_ascii(collapse_ws(query)));
    for (std::string w; in >> w;) words.push_back(w);
    std::vector<const CatalogFeed *> v;
    if (words.empty()) return v;
    for (const CatalogFeed &f : feeds_) {
        std::string hay = fold_ascii(f.name) + " " + url_host(f.site) + " " + f.topic;
        bool all = true;
        for (const std::string &w : words)
            if (hay.find(w) == std::string::npos) { all = false; break; }
        if (all) v.push_back(&f);
    }
    sort_by_name(v);
    if (v.size() > limit) v.resize(limit);
    return v;
}

const CatalogFeed *Catalog::find(const std::string &id) const
{
    for (const CatalogFeed &f : feeds_)
        if (f.id == id) return &f;
    return nullptr;
}

const CatalogFeed *Catalog::find_by_url(const std::string &url) const
{
    std::string want = norm_url(url);
    for (const CatalogFeed &f : feeds_)
        if (norm_url(f.url) == want) return &f;
    return nullptr;
}

} // namespace rssreader
