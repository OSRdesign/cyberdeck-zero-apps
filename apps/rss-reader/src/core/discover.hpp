/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: feed auto-discovery. Given the HTML of a page and its URL, list the feed URLs it advertises
 * (<link rel="alternate" type="application/rss+xml | atom+xml | feed+json | rdf+xml">) followed by the usual
 * fallback locations. The caller tests the candidates through fetch + parse_feed (see refresh.hpp add_custom_feed).
 */
#pragma once

#include <string>
#include <vector>

namespace rssreader {

struct FeedCandidate {
    std::string url;     /* absolute */
    std::string title;   /* the link's title attribute, may be "" */
    std::string type;    /* declared MIME type, "" for a fallback guess */
    bool declared = false;
};

/* Declared links first (RSS, then Atom, then the rest; "comments" feeds last), then the guesses /feed, /rss, /atom.xml,
 * /feed.xml, /index.xml, /rss.xml at the site root (and /feed under the page's own path when it is not the root).
 * No duplicates; at most 12. Honors <base href>. */
std::vector<FeedCandidate> discover_feeds(const std::string &html, const std::string &page_url);

} // namespace rssreader
