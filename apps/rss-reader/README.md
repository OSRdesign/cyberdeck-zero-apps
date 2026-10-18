# RSS Reader

Package `rss-reader`, title "RSS Reader", version 0.1.0 (a draft: not in the registry yet). Part of
[cyberdeck-zero-apps](../../README.md).

A text-feed reader for the CyberdeckZero handheld: pin your favourite news, tech and science feeds from a bundled catalog
(country or International, then topic, then feed), or add any site by its address (its feed is found for you), then read
the articles full screen, by touch or by keyboard, on any screen from 640x480 up. Text feeds only (no podcasts, no video);
feed content only: the app fetches the feeds, never the web pages behind them (the `Article::body_html()` seam is where a
page fetcher would plug in). A native app (kind A of the [guidelines](../../docs/APP-GUIDELINES.md)): its own screens,
lists and forms on the launcher's `cp0_lvgl`.

**Status: phase 2 of 3.** Phase 1: the core library (feeds, catalog, store, refresh, OPML). Phase 2 (this version): the app,
every page and a minimal reader. Phase 3: the full reader (images, a full-screen image view) and Save for offline.

## Screenshots

Taken in the headless harness at the five sizes (`build.sh ui`, a fake network): the publishing PR adds the 640x480 ones
to `docs/screenshots/` and `screenshots/`. Board screenshots come with the board tests.

## Pages

One pane at every screen size: a page fills the screen under the shared header (the title, the Esc button, the clock and
status strip; nothing else) and a grey line under it says what the page shows, or a short notice for 4 s. The secondary
actions of a row, or of the article being read, are in an **Options** dialog: hold the row (or the text) for 3 s (a bar
fills while you hold), or press **Ctrl+O** (the launcher's pointer navigation gives Ctrl+O on a held click).

| Page | What |
|---|---|
| Home | Favorites (the count of new articles in blue), Browse feeds, Saved articles (their count), Add a feed, Settings. Esc asks "Quit RSS Reader?" |
| Favorites | the Refresh bar ("Refresh all", or "Stop" while it runs; under it "Updating 3/6..." or "Updated 2 min ago"), then your feeds in your order: a blue dot and a bold name while a feed has unread articles, "N new", the topic (or the country) and the age of its newest article; a feed that could not be updated says why in amber. Options: Move up, Move down, Unpin (asked first), Mark all read, Refresh this feed, Details. With **Refresh when opening** (Settings) the page updates every feed as it opens (at most every 10 minutes); leaving the page or Stop cancels |
| Articles | a feed's articles, newest first: unread ones with the blue dot and a bright title, read ones grey; the age; "saved" in blue for a saved one. Options: Mark as read / unread, Mark all read, Refresh this feed, Article info. A feed never fetched (just pinned or imported) is fetched when it opens |
| Reader | the article's title, the source and the date in the grey line, then its text: headings, paragraphs, lists, quotes, preformatted text; an image shows as "[image: its description]" in this version; the article's link at the end. The column takes the whole width of the screen. Opening an article marks it read. Options: Text size (Small, Medium, Large), Mark as unread, Article info |
| Browse feeds | the countries (International first, the languages of each), a country's topics, a topic's feeds with what they carry (full text or a summary) and the favorite toggle at the end: a blue dot when pinned, an empty ring when not; a tap on the selected feed pins or unpins it. The search field above the countries finds a feed by its name, its site or its topic in the whole catalog. Podcasts are not shown. The title follows the path ("Browse > France > Technology", shortened in the middle when it would reach Esc) |
| Saved articles | the articles saved for offline reading (saving comes in phase 3), newest first; one opens in the reader |
| Add a feed | an address (a site, `example.com`, or a feed's own address), Find feed (Enter): "Found: title · N articles" with Pin and Cancel, or why nothing was found: no network, timed out, not a web address, no feed on that page |
| Settings | Text size, Refresh when opening (On / Off), Download images (Always / Off: kept for phase 3), Storage used and Storage cap for saved articles (50, 100, 200 or 500 MB), Clear read history (asked first), Export favorites (to `~/rss-reader-favorites.opml`), Import favorites (an `.opml` file of your home folder: what is missing is added), About (version, catalog, launcher pin, data folder) |

Rows show what a tap does (the [row kinds](../../docs/APP-GUIDELINES.md#row-kinds-what-can-be-tapped)): a chevron opens
something or changes a value, a white dot acts right there, a grey value without a marker is a read-out. The blue dots are
states: unread (at the left of a row), pinned (the favorite toggle), the value in use in a pick list.

## Keys and touch

| Function | Keys | Touch |
|---|---|---|
| Move in a list | Up / Down, Page Up / Page Down, Home / End | drag; a tap selects a row |
| Open / act | Enter or Right | a tap on the selected row |
| Back | Esc (or Left in a list) | the header's Esc button |
| Quit | Esc at Home, then Y (or Right, Enter) | the header's Esc at Home, then Quit |
| Options of a row or of the article | Ctrl+O | hold 3 s on the row or the text |
| Refresh all (Favorites) | Ctrl+R or F5 | the Refresh bar |
| Scroll an article | Up / Down, Page Up / Page Down, Space, Home / End; the wheel | drag |
| Previous / next article | Left / Right | Options, or back to the list |
| Type (search, an address) | the keyboard; letters at Browse start the search | a tap on the field: the launcher's on-screen keyboard while no keyboard is connected |
| A dialog | Left / Right (the choices), Up / Down (the rows), Enter, Esc | the arrows, the rows, the buttons |

The launcher's 3 s Esc hold ends any app.

## Requirements

- A screen of at least 640x480 (`X-MinSize=640x480`; tested in the harness at 640x480, 800x480, 720x720, 1024x600 and
  1280x720) and a launcher with full-screen app support (`X-Fullscreen` in `.desktop` files).
- The packages `curl` and `ca-certificates` (dependencies of the package); the network to update the feeds (what was fetched
  stays readable offline).
- The launcher's fonts in `/usr/share/APPLaunch/share/font` (Montserrat; DejaVu Sans and Noto Sans CJK for accents and
  Japanese).

## Install

Settings > Apps once it is published. By hand: `sudo dpkg -i rss-reader_0.1.0_arm64.deb`.

## Files the app keeps

| Path | What |
|---|---|
| `~/.local/share/rss-reader/` (or `$RSSREADER_DATA`) | `subscriptions.json` (your favourites), `settings.json`, `cache/` (the articles of each feed), `read/` (what you read), `saved/` (saved articles); the formats are documented in `src/core/store.hpp` |
| `~/rss-reader-favorites.opml` | written by Settings > Export favorites |

Reset: close the app and remove `~/.local/share/rss-reader`.

## Known limits (this version)

- Images are placeholders; there is no Save for offline yet (phase 3). The saved-articles list and storage settings exist for
  phase 3.
- Articles show what the feed carries: a feed that gives only a summary shows only the summary.
- Article titles are one line in the lists, cut with dots.
- The lists and the key bridge are copies of Mesh Hop's (a shared one in `apps/_common` is a follow-up).
- Untested on a board so far: see [tests/BOARD-TESTS.md](tests/BOARD-TESTS.md).

## Layout of the source

| Path | What |
|---|---|
| `src/core/` | plain C++17, namespace `rssreader`, no LVGL: the XML and feed parsers (RSS 2.0 / 0.9x, RDF, Atom), HTML to text blocks, feed discovery, HTTP through the `curl` command, the catalog, the store, the refresh, OPML (phase 1; see the headers) |
| `src/ui/` | namespace `rssui`: `ui_logic` (the rows' texts, the rules of the dialogs, the 3 s hold, the title that fits; no LVGL), `keys` and `key_bridge` (the keys), `jobs` (the worker threads), `row_list` (the virtual list), `rss_app*` and `page_*` (the pages), `app_spec` (the `cdk::AppSpec`, minimum 640x480) |
| `data/catalog.json` | 269 feeds (see `data/CATALOG-SOURCES.md`); the package installs a copy in `/usr/share/APPLaunch/share/rss-reader/` |
| `tests/unit/` | host tests of the core (about 9200 checks) and of the UI logic and the package files (`ui_tests.cpp`, about 470) |
| `tests/flows/` | the scripted runner (`rss-reader-script`: a fake network answering from `tests/data/ui` by `routes.txt`, a fixed clock, a seeded store) and nine flows |
| `tests/scenes/` | scenes of the shared headless harness (Home, Browse, Add a feed with the on-screen keyboard, Settings, the quit question, the notice under 640x480, a start without the launcher) |
| `tools/fetch_smoke.cpp` | optional: fetches three real feeds with the real curl client (not part of the tests) |

## Build and test

In WSL, from Windows (the launcher checkout and its build tree as for every app: see `apps/_common/build_app.sh -h`):

```
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/rss-reader/build.sh all --strict   # the arm64 binary and the host build
wsl -e sh .../apps/rss-reader/build.sh test      # unit tests only (core + UI logic)
wsl -e sh .../apps/rss-reader/build.sh asan      # the same under AddressSanitizer + UBSan
wsl -e sh .../apps/rss-reader/build.sh sim       # the nine flows against the fake network at 640x480 (or: sim 800x480)
wsl -e sh .../apps/rss-reader/build.sh ui        # the flows at the five sizes (about 3 minutes, the sizes side by side)
python tools/make_registry.py --only rss-reader --out <a folder outside packages/>   # the .deb (app.json is a draft)
```

`cross` writes `root/usr/share/APPLaunch/bin/rss-reader`, the catalog copy and `root/usr/share/doc/rss-reader/build-info`;
`--strict` stops when the launcher is not the pinned one (`apps/_common/LAUNCHER_REF`). The unit tests need `g++`,
`python3` and `curl` (the curl client is exercised against a throw-away HTTP server on 127.0.0.1; no internet is used). The
board protocol is [tests/BOARD-TESTS.md](tests/BOARD-TESTS.md).

## Credits and licences

Our code: MIT, 2026 OSRdesign. The binary links the launcher's `cp0_lvgl` runtime and LVGL (MIT); the fonts are the
launcher's, read at run time (Montserrat and Noto Sans CJK: SIL OFL 1.1; DejaVu: the Bitstream Vera / DejaVu licence).
HTTP goes through the `curl` program (curl licence). The catalog lists the publishers' own feed addresses
([CATALOG-SOURCES.md](data/CATALOG-SOURCES.md)); the feeds' content belongs to their publishers and is only fetched and
shown on the device. Details in `root/usr/share/doc/rss-reader/copyright`.

## Changes

- 0.1.0 (phase 2): the app: Home, Favorites with the background refresh, the article list, a minimal reader, Browse with
  the search, Add a feed, Settings with OPML export and import, the saved-articles list; options by a 3 s hold or Ctrl+O.
- phase 1: the core library, the catalog and their host tests.
