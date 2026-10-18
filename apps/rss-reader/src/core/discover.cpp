/*
 * SPDX-License-Identifier: MIT
 */
#include "discover.hpp"

#include "html_text.hpp"
#include "url.hpp"
#include "util.hpp"

#include <set>
#include <sstream>

namespace rssreader {

namespace {

int type_rank(const std::string &type, const std::string &href)
{
    std::string t = lower(trim(type));
    t = t.substr(0, t.find(';'));
    if (t == "application/rss+xml") return 0;
    if (t == "application/atom+xml") return 1;
    if (t == "application/feed+json" || t == "application/json") return t == "application/json" && !icontains(href, "feed") ? -1 : 3;
    if (t == "application/rdf+xml") return 2;
    if ((t == "text/xml" || t == "application/xml") && (icontains(href, "rss") || icontains(href, "feed") || icontains(href, "atom"))) return 2;
    return -1;
}

bool rel_has(const std::string &rel, const std::string &word)
{
    std::istringstream in(lower(rel));
    std::string w;
    while (in >> w)
        if (w == word) return true;
    return false;
}

} // namespace

std::vector<FeedCandidate> discover_feeds(const std::string &html, const std::string &page_url)
{
    std::vector<HtmlToken> toks = html_tokenize(html);
    std::string base = page_url;
    for (const HtmlToken &t : toks) {
        if (t.kind == HtmlToken::Kind::Start && t.name == "base" && t.has_attr("href")) {
            std::string b = resolve_url(page_url, t.attr("href"));
            if (!b.empty()) base = b;
            break;
        }
    }
    struct Found { FeedCandidate c; int rank; bool comments; };
    std::vector<Found> found;
    std::set<std::string> seen;
    for (const HtmlToken &t : toks) {
        if (t.kind != HtmlToken::Kind::Start || t.name != "link") continue;
        if (!rel_has(t.attr("rel"), "alternate")) continue;
        std::string href = t.attr("href");
        int rank = type_rank(t.attr("type"), href);
        if (rank < 0) continue;
        std::string url = resolve_url(base, href);
        if (url.empty() || !seen.insert(url).second) continue;
        FeedCandidate c;
        c.url = url;
        c.title = collapse_ws(t.attr("title"));
        c.type = lower(trim(t.attr("type")));
        c.declared = true;
        bool comments = icontains(c.title, "comment") || icontains(url, "comments");
        found.push_back({c, rank, comments});
    }
    std::vector<FeedCandidate> out;
    for (int pass = 0; pass < 2; ++pass)                 /* normal feeds, then the comment feeds */
        for (int rank = 0; rank <= 3; ++rank)
            for (const Found &f : found)
                if (f.rank == rank && f.comments == (pass == 1)) out.push_back(f.c);

    std::string origin = url_origin(page_url);
    std::vector<std::string> guesses;
    std::string path = url_path(page_url);
    if (!origin.empty()) {
        std::string dir = path.substr(0, path.rfind('/') + 1);    /* the page's own folder: /blog/post.html -> /blog/ */
        if (dir.size() > 1) guesses.push_back(origin + dir + "feed");
        for (const char *g : {"/feed", "/rss", "/atom.xml", "/feed.xml", "/index.xml", "/rss.xml"}) guesses.push_back(origin + g);
    }
    for (const std::string &g : guesses) {
        if (out.size() >= 12) break;
        if (!seen.insert(g).second) continue;
        FeedCandidate c;
        c.url = g;
        out.push_back(c);
    }
    if (out.size() > 12) out.resize(12);
    return out;
}

} // namespace rssreader
