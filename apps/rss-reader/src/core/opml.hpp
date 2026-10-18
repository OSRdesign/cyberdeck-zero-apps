/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: OPML import and export of the subscriptions.
 *
 * Import reads every <outline xmlUrl="..."> (title or text for the name, htmlUrl for the site). Nested categories are
 * flattened into a group name ("Tech / Linux"). Export writes one outline per feed, grouped by the subscription's group.
 */
#pragma once

#include "catalog.hpp"
#include "store.hpp"

#include <string>
#include <vector>

namespace rssreader {

struct OpmlFeed {
    std::string title;
    std::string url;
    std::string site;
    std::string group;   /* "" or the enclosing categories joined with " / " */
};

/* Empty when the text is not OPML / has no feeds. Feeds without an http(s) URL are skipped; duplicate URLs once. */
std::vector<OpmlFeed> parse_opml(const std::string &text);
std::string write_opml(const std::vector<OpmlFeed> &feeds, const std::string &title = "RSS Reader subscriptions");
std::vector<OpmlFeed> opml_from_subscriptions(const std::vector<Subscription> &subs);

struct OpmlImportResult {
    int added = 0;
    int duplicates = 0;   /* already subscribed */
    int failed = 0;       /* could not be added */
};

/* Subscribes to the feeds. A feed whose URL is in `catalog` becomes the catalog subscription (its id and name); the
 * others become custom feeds. */
OpmlImportResult import_opml(Store &store, const Catalog *catalog, const std::vector<OpmlFeed> &feeds, std::int64_t now);

} // namespace rssreader
