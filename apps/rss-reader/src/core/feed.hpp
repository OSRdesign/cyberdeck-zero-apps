/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: feed parsing. RSS 2.0 and 0.9x, RSS 1.0 (RDF) and Atom 1.0 become one Feed with Articles. Tolerant:
 * a broken or truncated document gives the articles that could be read; a document that is not a feed, too big, too
 * deep or in an unsupported encoding gives a status and no articles. Never throws.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rssreader {

constexpr std::size_t kMaxArticlesPerFeed = 200;
constexpr std::size_t kMaxImagesPerArticle = 20;
constexpr std::size_t kMaxContentBytes = 100 * 1024;   /* per article content_html; the rest is cut */
constexpr std::size_t kMaxSummaryBytes = 20 * 1024;

struct Article {
    std::string id;            /* hash16(scope + "\n" + guid | link | title+date): stable, file-name safe, unique across feeds */
    std::string guid;          /* the raw guid / atom id / link the id was made from */
    std::string title;         /* plain text, one line */
    std::string link;          /* absolute URL of the article page ("" if none) */
    std::int64_t published = 0; /* epoch seconds; the fetch time (minus the item index) when the feed gave none */
    bool dated = true;         /* false: `published` is a stand-in because the feed had no usable date */
    std::string author;        /* plain text */
    std::string summary_html;  /* RSS description / atom summary */
    std::string content_html;  /* content:encoded / atom content ("" if none) */
    std::vector<std::string> images;   /* absolute URLs: enclosures, media:content / thumbnail, <img> of the html; no duplicates */
    bool has_full_text = false;  /* content_html present, or a summary long enough to be the whole article */

    /* The html to show: content_html when there is one, else summary_html. */
    const std::string &body_html() const { return content_html.empty() ? summary_html : content_html; }
};

struct Feed {
    std::string title;
    std::string site_url;      /* the site's own page (channel link / atom alternate link), absolute */
    std::string description;
    std::string lang;          /* "en", "fr-CA" ... lower case, "" when unknown */
    std::string format;        /* "rss", "rdf" or "atom" */
    std::vector<Article> articles;   /* newest first, at most kMaxArticlesPerFeed */
    bool recovered = false;    /* the XML was malformed and repaired */
};

enum class FeedStatus {
    Ok,
    NotAFeed,       /* well-formed enough, but neither RSS, RDF nor Atom (an HTML page, an error page) */
    TooBig,
    TooDeep,
    BadEncoding,
};

const char *feed_status_text(FeedStatus s);

/* `feed_url` resolves relative links; `scope` (the subscription id) makes article ids unique across feeds;
 * `fetch_time` stands in for missing dates. */
FeedStatus parse_feed(const std::string &body, const std::string &feed_url, const std::string &scope, std::int64_t fetch_time, Feed &out);

/* Cheap sniff on the start of a body: does it look like an RSS / RDF / Atom document (and not HTML)? */
bool looks_like_feed(const std::string &body);

/* Article id for a raw identity string. */
std::string make_article_id(const std::string &scope, const std::string &raw);

} // namespace rssreader
