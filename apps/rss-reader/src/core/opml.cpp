/*
 * SPDX-License-Identifier: MIT
 */
#include "opml.hpp"

#include "refresh.hpp"
#include "url.hpp"
#include "util.hpp"
#include "xml.hpp"

#include <algorithm>
#include <set>

namespace rssreader {

namespace {

void walk(const XmlNode &node, const std::string &group, std::vector<OpmlFeed> &out, std::set<std::string> &seen, int depth)
{
    if (depth > kMaxXmlDepth) return;
    for (const auto &k : node.kids) {
        if (k->is_text || !iequals(k->name, "outline")) continue;
        std::string url = trim(k->attr("xmlUrl"));
        std::string title = collapse_ws(k->attr("title"));
        if (title.empty()) title = collapse_ws(k->attr("text"));
        if (!url.empty()) {
            if (is_http_url(url) && seen.insert(url).second) {
                OpmlFeed f;
                f.title = title.empty() ? url_host(url) : title;
                f.url = url;
                f.site = trim(k->attr("htmlUrl"));
                f.group = group;
                out.push_back(std::move(f));
            }
        }
        /* an outline can be both a feed and a folder in sloppy files; walk its children either way */
        std::string sub = group;
        if (url.empty() && !title.empty()) sub = group.empty() ? title : group + " / " + title;
        walk(*k, sub, out, seen, depth + 1);
    }
}

} // namespace

std::vector<OpmlFeed> parse_opml(const std::string &text)
{
    std::vector<OpmlFeed> out;
    XmlDoc doc;
    if (xml_parse(text, doc) != XmlStatus::Ok || !doc.root || !iequals(doc.root->name, "opml")) return out;
    const XmlNode *body = doc.root->child("body");
    if (!body) return out;
    std::set<std::string> seen;
    walk(*body, "", out, seen, 0);
    return out;
}

std::string write_opml(const std::vector<OpmlFeed> &feeds, const std::string &title)
{
    auto outline = [](const OpmlFeed &f, const std::string &indent) {
        std::string s = indent + "<outline type=\"rss\" text=\"" + html_escape(f.title) + "\" title=\"" + html_escape(f.title) +
                        "\" xmlUrl=\"" + html_escape(f.url) + "\"";
        if (!f.site.empty()) s += " htmlUrl=\"" + html_escape(f.site) + "\"";
        return s + "/>\n";
    };
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<opml version=\"2.0\">\n  <head><title>" + html_escape(title) +
                      "</title></head>\n  <body>\n";
    std::vector<std::string> groups;
    for (const OpmlFeed &f : feeds)
        if (!f.group.empty() && std::find(groups.begin(), groups.end(), f.group) == groups.end()) groups.push_back(f.group);
    for (const OpmlFeed &f : feeds)
        if (f.group.empty()) out += outline(f, "    ");
    for (const std::string &g : groups) {
        out += "    <outline text=\"" + html_escape(g) + "\" title=\"" + html_escape(g) + "\">\n";
        for (const OpmlFeed &f : feeds)
            if (f.group == g) out += outline(f, "      ");
        out += "    </outline>\n";
    }
    return out + "  </body>\n</opml>\n";
}

std::vector<OpmlFeed> opml_from_subscriptions(const std::vector<Subscription> &subs)
{
    std::vector<OpmlFeed> v;
    for (const Subscription &s : subs) {
        OpmlFeed f;
        f.title = s.name;
        f.url = s.url;
        f.site = s.site;
        f.group = s.group;
        v.push_back(std::move(f));
    }
    return v;
}

OpmlImportResult import_opml(Store &store, const Catalog *catalog, const std::vector<OpmlFeed> &feeds, std::int64_t now)
{
    OpmlImportResult r;
    for (const OpmlFeed &f : feeds) {
        Subscription s;
        const CatalogFeed *c = catalog ? catalog->find_by_url(f.url) : nullptr;
        if (c) {
            s = make_subscription(*c, now);
        } else {
            s.id = custom_feed_id(f.url);
            s.name = f.title.empty() ? url_host(f.url) : f.title;
            s.url = f.url;
            s.site = f.site;
            s.group = f.group;
            s.custom = true;
            s.added = now;
        }
        switch (store.add_subscription(s)) {
        case StoreError::Ok: ++r.added; break;
        case StoreError::Duplicate: ++r.duplicates; break;
        default: ++r.failed; break;
        }
    }
    return r;
}

} // namespace rssreader
