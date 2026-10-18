/*
 * SPDX-License-Identifier: MIT
 */
#include "feed.hpp"

#include "datetime.hpp"
#include "html_text.hpp"
#include "url.hpp"
#include "util.hpp"
#include "xml.hpp"

#include <algorithm>
#include <set>

namespace rssreader {

const char *feed_status_text(FeedStatus s)
{
    switch (s) {
    case FeedStatus::Ok: return "ok";
    case FeedStatus::NotAFeed: return "not a feed";
    case FeedStatus::TooBig: return "feed too big";
    case FeedStatus::TooDeep: return "feed too deeply nested";
    case FeedStatus::BadEncoding: return "unsupported text encoding";
    }
    return "unknown";
}

std::string make_article_id(const std::string &scope, const std::string &raw) { return hash16(scope + "\n" + raw); }

bool looks_like_feed(const std::string &body)
{
    std::string head = lower(body.substr(0, 2048));
    std::size_t a = head.find("<rss");
    std::size_t b = head.find("<feed");
    std::size_t c = head.find("<rdf:rdf");
    std::size_t h = head.find("<html");
    std::size_t d = head.find("<!doctype html");
    std::size_t first = std::min({a, b, c});
    if (first == std::string::npos) return false;
    std::size_t html = std::min(h, d);
    return html == std::string::npos || first < html;
}

namespace {

std::string one_line(const std::string &s) { return html_to_inline(s); }

bool image_type(const std::string &type) { return starts_with(lower(type), "image/"); }

bool image_ext(const std::string &url)
{
    std::string path = lower(url.substr(0, url.find_first_of("?#")));
    for (const char *e : {".jpg", ".jpeg", ".png", ".gif", ".webp"})
        if (ends_with(path, e)) return true;
    return false;
}

/* "editor@x.com (Jane Doe)" -> "Jane Doe"; "Jane Doe <editor@x.com>" -> "Jane Doe"; a bare address stays out. */
std::string clean_author(const std::string &raw)
{
    std::string a = one_line(raw);
    std::size_t p = a.find('(');
    std::size_t q = a.find(')', p == std::string::npos ? 0 : p);
    if (p != std::string::npos && q != std::string::npos && q > p + 1 && a.find('@') < p) return trim(a.substr(p + 1, q - p - 1));
    std::size_t lt = a.find('<');
    if (lt != std::string::npos) a = trim(a.substr(0, lt));
    if (a.find('@') != std::string::npos && a.find(' ') == std::string::npos) return "";
    return a;
}

std::string text_to_html(const std::string &text)
{
    std::string out = "<p>";
    std::string t = trim(text);
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\n' && i + 1 < t.size() && t[i + 1] == '\n') {
            out += "</p><p>";
            while (i + 1 < t.size() && t[i + 1] == '\n') ++i;
        } else if (t[i] == '\n') {
            out += "<br>";
        } else {
            out += html_escape(std::string(1, t[i]));
        }
    }
    return out + "</p>";
}

/* Atom text construct: type="html" -> as is; "xhtml" -> its markup; "text" (default) -> escaped. */
std::string atom_text_html(const XmlNode &n)
{
    std::string type = lower(n.attr("type"));
    if (type == "xhtml") {
        const XmlNode *div = n.child("div");
        return xml_inner(div ? *div : n);
    }
    if (type == "html" || type == "text/html" || type == "application/xhtml+xml") return n.text_content();
    if (type.empty() || type == "text") return text_to_html(n.text_content());
    return "";     /* binary / other content types are not article text */
}

std::string pick_lang(const std::string &l)
{
    std::string r = lower(trim(l));
    for (char &c : r)
        if (c == '_') c = '-';
    return r.size() > 12 ? r.substr(0, 12) : r;
}

void add_image(std::vector<std::string> &images, std::set<std::string> &seen, const std::string &base, const std::string &raw)
{
    if (images.size() >= kMaxImagesPerArticle) return;
    std::string u = resolve_url(base, raw);
    if (u.empty() || !seen.insert(u).second) return;
    images.push_back(u);
}

void collect_media(const XmlNode &item, const std::string &base, std::vector<std::string> &images, std::set<std::string> &seen)
{
    std::vector<const XmlNode *> stack{&item};
    int budget = 200;
    while (!stack.empty() && budget-- > 0) {
        const XmlNode *n = stack.back();
        stack.pop_back();
        for (const auto &k : n->kids) {
            if (k->is_text) continue;
            const std::string &nm = k->name;
            if (nm == "enclosure") {
                std::string type = k->attr("type");
                std::string url = k->attr("url");
                if (image_type(type) || (type.empty() && image_ext(url))) add_image(images, seen, base, url);
            } else if (nm == "media:content") {
                std::string type = k->attr("type"), medium = lower(k->attr("medium")), url = k->attr("url");
                if (medium == "image" || image_type(type) || (type.empty() && medium.empty() && image_ext(url))) add_image(images, seen, base, url);
            } else if (nm == "media:thumbnail") {
                add_image(images, seen, base, k->attr("url"));
            } else if (nm == "media:group") {
                stack.push_back(k.get());
            } else if (nm == "link" && lower(k->attr("rel")) == "enclosure" && image_type(k->attr("type"))) {
                add_image(images, seen, base, k->attr("href"));
            }
        }
    }
}

void finish_article(Article &a, const XmlNode &node, const std::string &base, const std::string &scope, std::int64_t fetch_time, std::size_t index)
{
    a.summary_html = truncate_utf8(a.summary_html, kMaxSummaryBytes);
    a.content_html = truncate_utf8(a.content_html, kMaxContentBytes);
    std::string raw = !trim(a.guid).empty() ? trim(a.guid) : (!a.link.empty() ? a.link : std::string());
    if (raw.empty()) raw = "title:" + a.title + (a.dated ? "\n" + std::to_string(a.published) : std::string());
    a.guid = raw;
    a.id = make_article_id(scope, raw);
    std::set<std::string> seen;
    collect_media(node, base, a.images, seen);
    for (const std::string &u : html_image_urls(a.content_html, base)) add_image(a.images, seen, base, u);
    for (const std::string &u : html_image_urls(a.summary_html, base)) add_image(a.images, seen, base, u);
    if (!a.dated) a.published = fetch_time - static_cast<std::int64_t>(index);
    if (!a.content_html.empty()) a.has_full_text = true;
    else if (html_to_inline(a.summary_html).size() > 1500) a.has_full_text = true;
}

std::string first_date_text(const XmlNode &n, std::initializer_list<const char *> names)
{
    for (const char *nm : names) {
        std::string t = n.child_text(nm);
        std::int64_t e;
        if (!t.empty() && parse_date(t, e)) return t;
    }
    return "";
}

void parse_rss_item(const XmlNode &item, const std::string &base, const std::string &scope, std::int64_t fetch_time, std::size_t index, Article &a)
{
    a.title = one_line(item.child_text("title"));
    std::string link = item.child_text("link");
    if (link.empty()) {
        for (const XmlNode *l : item.children("atom:link"))
            if (lower(l->attr("rel")).empty() || lower(l->attr("rel")) == "alternate") { link = l->attr("href"); break; }
    }
    a.guid = item.child_text("guid");
    if (link.empty() && !a.guid.empty()) {
        const XmlNode *g = item.child("guid");
        if (g && lower(g->attr("ispermalink")) != "false") link = a.guid;
    }
    a.link = resolve_url(base, link);
    if (a.link.empty() && is_http_url(trim(link))) a.link = trim(link);
    std::string dt = first_date_text(item, {"pubDate", "dc:date", "published", "date", "updated"});
    std::int64_t e;
    a.dated = !dt.empty() && parse_date(dt, e);
    if (a.dated) a.published = e;
    std::string au = item.child_text("dc:creator");
    if (au.empty()) au = item.child_text("author");
    if (au.empty()) au = item.child_text("itunes:author");
    a.author = clean_author(au);
    const XmlNode *d = item.child("description");
    if (d) a.summary_html = d->text_content();
    const XmlNode *c = item.child("content:encoded");
    if (c) a.content_html = trim(c->text_content());
    std::string art_base = a.link.empty() ? base : a.link;
    finish_article(a, item, art_base, scope, fetch_time, index);
}

void parse_atom_entry(const XmlNode &entry, const std::string &base, const std::string &scope, std::int64_t fetch_time, std::size_t index, Article &a)
{
    const XmlNode *tn = entry.child("title");
    a.title = tn ? one_line(atom_text_html(*tn)) : std::string();
    std::string link, alt_any;
    for (const XmlNode *l : entry.children("link")) {
        std::string rel = lower(l->attr("rel"));
        std::string href = l->attr("href");
        if (href.empty()) continue;
        if (rel.empty() || rel == "alternate") {
            std::string type = lower(l->attr("type"));
            if (type.empty() || type.find("html") != std::string::npos) { if (link.empty()) link = href; }
            else if (alt_any.empty()) alt_any = href;
        }
    }
    if (link.empty()) link = alt_any;
    a.link = resolve_url(base, link);
    a.guid = entry.child_text("id");
    std::string dt = first_date_text(entry, {"published", "updated", "issued", "created", "modified", "dc:date"});
    std::int64_t e;
    a.dated = !dt.empty() && parse_date(dt, e);
    if (a.dated) a.published = e;
    const XmlNode *au = entry.child("author");
    std::string name = au ? au->child_text("name") : "";
    if (name.empty() && au) name = trim(au->text_content());
    if (name.empty()) name = entry.child_text("dc:creator");
    a.author = clean_author(name);
    if (const XmlNode *s = entry.child("summary")) a.summary_html = atom_text_html(*s);
    if (const XmlNode *c = entry.child("content")) {
        if (c->has_attr("src") && trim(c->text_content()).empty()) { /* out-of-line content: nothing to show */ }
        else a.content_html = trim(atom_text_html(*c));
    }
    std::string art_base = a.link.empty() ? base : a.link;
    finish_article(a, entry, art_base, scope, fetch_time, index);
}

void finish_feed(Feed &f, const std::string &scope_unused)
{
    (void)scope_unused;
    /* dedupe by id (first wins), newest first (stable: feed order breaks ties), cap */
    std::vector<Article> uniq;
    std::set<std::string> ids;
    for (Article &a : f.articles)
        if (ids.insert(a.id).second) uniq.push_back(std::move(a));
    std::stable_sort(uniq.begin(), uniq.end(), [](const Article &x, const Article &y) { return x.published > y.published; });
    if (uniq.size() > kMaxArticlesPerFeed) uniq.resize(kMaxArticlesPerFeed);
    f.articles = std::move(uniq);
}

} // namespace

FeedStatus parse_feed(const std::string &body, const std::string &feed_url, const std::string &scope, std::int64_t fetch_time, Feed &out)
{
    out = Feed();
    XmlDoc doc;
    switch (xml_parse(body, doc)) {
    case XmlStatus::Ok: break;
    case XmlStatus::TooBig: return FeedStatus::TooBig;
    case XmlStatus::TooDeep: return FeedStatus::TooDeep;
    case XmlStatus::BadEncoding: return FeedStatus::BadEncoding;
    case XmlStatus::Empty: return FeedStatus::NotAFeed;
    }
    const XmlNode &root = *doc.root;
    out.recovered = doc.recovered;
    std::string rn = lower(root.name);

    if (rn == "rss" || rn == "rdf:rdf") {
        out.format = rn == "rss" ? "rss" : "rdf";
        const XmlNode *ch = root.child("channel");
        const XmlNode &c = ch ? *ch : root;
        out.title = one_line(c.child_text("title"));
        out.description = one_line(c.child_text("description"));
        out.lang = pick_lang(c.child_text("language").empty() ? c.child_text("dc:language") : c.child_text("language"));
        out.site_url = resolve_url(feed_url, c.child_text("link"));
        if (out.site_url.empty()) {
            for (const XmlNode *l : c.children("atom:link"))
                if (lower(l->attr("rel")) == "alternate") { out.site_url = resolve_url(feed_url, l->attr("href")); break; }
        }
        std::string base = out.site_url.empty() ? feed_url : out.site_url;
        std::vector<const XmlNode *> items = c.children("item");
        if (rn == "rdf:rdf") {
            for (const XmlNode *i : root.children("item")) items.push_back(i);
        }
        out.articles.resize(items.size() > 600 ? 600 : items.size());
        for (std::size_t k = 0; k < out.articles.size(); ++k) parse_rss_item(*items[k], base, scope, fetch_time, k, out.articles[k]);
        if (!ch && items.empty()) return FeedStatus::NotAFeed;
    } else if (rn == "feed") {
        out.format = "atom";
        out.title = one_line(root.child("title") ? atom_text_html(*root.child("title")) : "");
        out.description = one_line(root.child("subtitle") ? atom_text_html(*root.child("subtitle")) : "");
        out.lang = pick_lang(root.attr("xml:lang"));
        for (const XmlNode *l : root.children("link")) {
            std::string rel = lower(l->attr("rel"));
            if (rel.empty() || rel == "alternate") { out.site_url = resolve_url(feed_url, l->attr("href")); break; }
        }
        std::string base = out.site_url.empty() ? feed_url : out.site_url;
        std::vector<const XmlNode *> entries = root.children("entry");
        out.articles.resize(entries.size() > 600 ? 600 : entries.size());
        for (std::size_t k = 0; k < out.articles.size(); ++k) parse_atom_entry(*entries[k], base, scope, fetch_time, k, out.articles[k]);
    } else {
        return FeedStatus::NotAFeed;
    }
    finish_feed(out, scope);
    return FeedStatus::Ok;
}

} // namespace rssreader
