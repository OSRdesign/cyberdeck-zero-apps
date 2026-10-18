# RSS Reader

Package `rss-reader`, title "RSS Reader". Part of [cyberdeck-zero-apps](../../README.md).

**Status: phase 1 of 3, core library only. There is no UI, no `app.json` and no package yet** (so the registry tool skips
this folder). Phase 2 adds the pages (catalog, feed list, article list), phase 3 the reader.

A text-feed reader for the CyberdeckZero handheld: pin favourite feeds from a bundled catalog (country or language, then
topic, then feed), add your own (type a site address, the feed is found for you), read articles with read / unread tracking, and
save articles with their images for offline reading (storage cap 200 MB by default, adjustable). Text feeds only (no podcasts or
video); feed content only, no full-page fetching (the `Article::body_html()` seam is where a page fetcher would plug in).

## Layout

| Path | What |
|---|---|
| `src/core/` | plain C++17, namespace `rssreader`, no LVGL, no library beyond the C++ runtime. HTTP is the `curl` command (the package will depend on `curl` and `ca-certificates`). |
| `src/core/xml.*` | tolerant XML parser (entities, CDATA, namespaces by canonical prefix, ISO-8859-1 / windows-1252 transcoding, 5 MB / depth 64 limits) |
| `src/core/feed.*`, `datetime.*` | RSS 2.0 / 0.9x, RSS 1.0 (RDF) and Atom 1.0 to `Feed` + `Article`; robust date parsing |
| `src/core/html_text.*` | article HTML to a list of text `Block`s (headings, paragraphs, list items, quotes, preformatted, images, rules) |
| `src/core/discover.*` | feed auto-discovery from a page's `<link rel=alternate>` plus the usual locations |
| `src/core/fetch.*` | `HttpClient` interface, `CurlClient`, image fetching (2 MB, image types only) |
| `src/core/catalog.*` | the bundled catalog and its queries (countries, languages, topics, search) |
| `src/core/store.*`, `fsutil.*` | persistence: subscriptions, settings, article caches, read state, saved articles with images, cap handling; atomic writes |
| `src/core/refresh.*`, `opml.*` | refresh / add-custom-feed orchestration with an injected client and clock; OPML import and export |
| `src/core/json.*`, `url.*`, `util.*` | small shared pieces |
| `data/catalog.json` | 269 feeds (see `data/CATALOG-SOURCES.md` for provenance and licences) |
| `tests/unit/` | host tests (about 9000 checks), `tests/data/` their fixtures |
| `tools/fetch_smoke.cpp` | optional: fetches three real feeds with the real curl client (not part of the tests) |

Data lives in `~/.local/share/rss-reader` (override with `$RSSREADER_DATA`); the file formats are documented in
`src/core/store.hpp`.

## Running the unit tests

They need `g++`, `python3` and `curl` (the curl client is exercised against a throw-away HTTP server on 127.0.0.1; no internet
is used). From Windows, in WSL:

```
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/rss-reader/build.sh            # the tests
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/rss-reader/build.sh asan       # the same under AddressSanitizer + UBSan
```

or directly `sh apps/rss-reader/tests/unit/run_tests.sh` (set `SANITIZE=1` for the sanitizer build). The build tree is
`$HOME/cdk-build/rss-reader` (`$CDK_BUILD` overrides it). The smoke tool:

```
cd apps/rss-reader/tools && g++ -std=c++17 -O1 -I ../src/core fetch_smoke.cpp ../src/core/*.cpp -lpthread -o fetch_smoke && ./fetch_smoke
```
