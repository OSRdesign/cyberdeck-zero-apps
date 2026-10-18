/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: the bundled catalog of feeds (data/catalog.json), browsed as country (or language) > topic > feed.
 * Pure data and queries; nothing here touches the network.
 *
 * File format: {"version":1,"generated":"YYYY-MM-DD","feeds":[{id,name,country,lang,topic,url,site,format,content,items,
 * newest,checked,notes}, ...]}. A feed without id, name or an http(s) url is skipped; a duplicate id is skipped.
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rssreader {

struct CatalogFeed {
    std::string id;        /* "fr-le-monde": lower case, [a-z0-9-] */
    std::string name;
    std::string country;   /* ISO 3166 alpha-2, or "INT" for international */
    std::string lang;      /* ISO 639-1 */
    std::string topic;     /* one of known_topics() */
    std::string url;       /* the feed */
    std::string site;      /* the publisher's site */
    std::string format;    /* "rss" | "atom" */
    std::string content;   /* "summary" | "full": what the feed carries */
    int items = 0;         /* items seen when it was checked */
    std::string newest;    /* ISO date of the newest item when checked */
    std::string checked;   /* ISO date of the check */
    std::string notes;
};

struct CatalogGroup {
    std::string key;       /* country code / language code / topic */
    std::string label;     /* English display name ("France", "French", "Technology") */
    std::size_t feeds = 0;
};

const std::vector<std::string> &known_topics();            /* the topics the catalog uses */
std::string country_label(const std::string &code);        /* "FR" -> "France"; unknown code -> the code */
std::string language_label(const std::string &code);       /* "fr" -> "French"; unknown -> the code */
std::string topic_label(const std::string &topic);         /* "linux" -> "Linux"; others capitalised */

class Catalog {
public:
    /* Returns false (and leaves the catalog empty) when the text / file is not a catalog at all. */
    bool load_json(const std::string &text);
    bool load_file(const std::string &path);

    const std::vector<CatalogFeed> &all() const { return feeds_; }
    std::size_t size() const { return feeds_.size(); }
    int version() const { return version_; }
    const std::string &generated() const { return generated_; }

    /* countries (International first, then by name) and languages (by name), with their feed counts */
    std::vector<CatalogGroup> countries() const;
    std::vector<CatalogGroup> languages() const;
    /* topics present in a country / a language (by feed count, then name); an empty key = all feeds */
    std::vector<CatalogGroup> topics_in_country(const std::string &country) const;
    std::vector<CatalogGroup> topics_in_language(const std::string &lang) const;
    /* feeds of (country, topic) / (language, topic), by name; an empty topic = every topic of the country / language */
    std::vector<const CatalogFeed *> feeds_in(const std::string &country, const std::string &topic) const;
    std::vector<const CatalogFeed *> feeds_in_language(const std::string &lang, const std::string &topic) const;
    /* case- and accent-insensitive search in name, site host and topic; every word must match; by name */
    std::vector<const CatalogFeed *> search(const std::string &query, std::size_t limit = 50) const;
    const CatalogFeed *find(const std::string &id) const;
    const CatalogFeed *find_by_url(const std::string &url) const;     /* ignoring http/https and a trailing slash */

private:
    std::vector<CatalogFeed> feeds_;
    int version_ = 0;
    std::string generated_;
};

} // namespace rssreader
