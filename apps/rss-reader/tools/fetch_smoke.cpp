// Smoke test of the real curl client against the internet (NOT part of the unit tests, which never leave the host).
// Fetches three feeds from the bundled catalog (or the URLs given on the command line), parses them, then repeats the
// first one with its ETag / Last-Modified to see a 304, and fetches one article image.
//
//   g++ -std=c++17 -O1 -I ../src/core fetch_smoke.cpp ../src/core/*.cpp -lpthread -o fetch_smoke
//   ./fetch_smoke [feed-url ...]
#include "catalog.hpp"
#include "feed.hpp"
#include "fetch.hpp"
#include "html_text.hpp"
#include "util.hpp"

#include <cstdio>

using namespace rssreader;

int main(int argc, char **argv)
{
    std::vector<std::string> urls;
    for (int i = 1; i < argc; ++i) urls.push_back(argv[i]);
    if (urls.empty()) {
        Catalog cat;
        if (!cat.load_file("../data/catalog.json")) {
            std::printf("cannot read ../data/catalog.json (run from the tools folder)\n");
            return 2;
        }
        for (const char *id : {"int-dw-english", "int-france-24-english", "int-al-jazeera-english"})
            if (const CatalogFeed *f = cat.find(id)) urls.push_back(f->url);
    }
    CurlClient curl;
    std::printf("curl available: %s, user agent: %s\n", curl.available() ? "yes" : "NO", curl.user_agent().c_str());
    int failures = 0;
    HttpResponse first;
    std::string image_url;
    for (const std::string &u : urls) {
        HttpRequest req;
        req.url = u;
        HttpResponse r = curl.get(req);
        if (!r.ok()) {
            std::printf("FAIL %s: %s (status %d) %s\n", u.c_str(), fetch_error_text(r.error), r.status, r.error_detail.c_str());
            ++failures;
            continue;
        }
        Feed f;
        FeedStatus fs = parse_feed(r.body, r.final_url, "smoke", 0, f);
        std::printf("%s %s\n  status %d, %zu bytes, type %s, etag [%s], last-modified [%s]\n  -> %s: \"%s\", %zu articles", fs == FeedStatus::Ok ? "OK  " : "FAIL", u.c_str(),
                    r.status, r.body.size(), r.content_type.c_str(), r.etag.c_str(), r.last_modified.c_str(), feed_status_text(fs), f.title.c_str(), f.articles.size());
        if (fs != FeedStatus::Ok) { std::printf("\n"); ++failures; continue; }
        if (!f.articles.empty()) std::printf(", newest \"%s\"", html_to_inline(f.articles[0].title).c_str());
        std::printf("\n");
        if (first.body.empty()) first = r;
        if (image_url.empty())
            for (const Article &a : f.articles)
                if (!a.images.empty()) { image_url = a.images[0]; break; }
    }
    if (!first.body.empty() && (!first.etag.empty() || !first.last_modified.empty())) {
        HttpRequest req;
        req.url = urls[0];
        req.etag = first.etag;
        req.last_modified = first.last_modified;
        HttpResponse r = curl.get(req);
        std::printf("conditional re-fetch of %s: %s (status %d)\n", urls[0].c_str(), fetch_error_text(r.error), r.status);
    } else {
        std::printf("the first feed gave no validators; conditional re-fetch skipped\n");
    }
    if (!image_url.empty()) {
        HttpResponse img = fetch_image(curl, image_url);
        std::printf("image %s: %s, %zu bytes, %s\n", image_url.c_str(), fetch_error_text(img.error), img.body.size(), img.content_type.c_str());
        if (!img.ok()) ++failures;
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
