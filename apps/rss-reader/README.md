# RSS Reader

Package `rss-reader`, title "RSS Reader", version 0.1.1. Part of
[cyberdeck-zero-apps](../../README.md).

A text-feed reader for the CyberdeckZero handheld: pin your favourite news, tech and science feeds from a bundled catalog
(country or International, then topic, then feed), or add any site by its address (its feed is found for you), then read
the articles full screen, by touch or by keyboard, on any screen from 640x480 up. Text feeds only (no podcasts, no video);
feed content only: the app fetches the feeds, never the web pages behind them (the `Article::body_html()` seam is where a
page fetcher would plug in). A native app (kind A of the [guidelines](../../docs/APP-GUIDELINES.md)): its own screens,
lists and forms on the launcher's `cp0_lvgl`.

**Status: phase 3 of 3**, published. Phase 1: the core library (feeds, catalog, store, refresh,
OPML). Phase 2: the app, every page and a minimal reader. Phase 3 (this version): the images in the reader, the full-screen
image, Save for offline and the Saved articles page.

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
| Favorites | the Refresh bar ("Refresh all", or "Stop" while it runs; under it "Updating 3/6..." or "Updated 2 min ago"), then your feeds in your order: a blue dot and a bold name while a feed has unread articles, "N new", the topic (or, for a feed added by its address, its site) and the age of its newest article; a feed that could not be updated says why in amber. Options: Move up, Move down, Unpin (asked first), Mark all read, Refresh this feed, Details. With **Refresh when opening** (Settings) the page updates every feed as it opens (at most every 10 minutes); leaving the page or Stop cancels |
| Articles | a feed's articles, newest first: unread ones with the blue dot and a bright title, read ones grey; the age; "saved" in blue for a saved one. Options: Mark as read / unread, Save for offline / Remove from saved, Mark all read, Refresh this feed, Article info. A feed never fetched (just pinned or imported) is fetched when it opens |
| Reader | the article's title, the source and the date in the grey line, then its text: headings, paragraphs, lists, quotes, preformatted text, and its pictures where they stand (an article whose text has none shows the feed's own picture under the title); the article's link at the end. The column takes the whole width of the screen. Opening an article marks it read. The first six pictures of an article are downloaded (Download images = Always) and shown as thumbnails (the column's width, a third of the screen's height at most); until then, or when one cannot be shown, a tile with its description says why: Downloading..., Image unavailable, Too large to show, Format not supported, Images are off, Not loaded. A tap on a picture, or Enter, shows it full screen. Options: Text size (Small, Medium, Large), Save for offline / Remove from saved (asked first), Mark as unread, Article info |
| Full-screen picture | black, the picture fitted to the screen, no header; "Esc or tap to close · 1 / 2" at the bottom fades after 2 s and comes back with any key. Esc or a tap closes; Left / Right or a swipe show the previous / next picture of the article; no zoom. Works offline for a saved article |
| Browse feeds | the countries (International first, the languages of each), a country's topics, a topic's feeds with what they carry (full text or a summary) and the favorite toggle at the end: a blue dot when pinned, an empty ring when not; a tap on the selected feed pins or unpins it. The search field above the countries finds a feed by its name, its site or its topic in the whole catalog. Podcasts are not shown. The title follows the path ("Browse > France > Technology", shortened in the middle when it would reach Esc) |
| Saved articles | the articles saved for offline reading, newest saved first: the title, the feed, when it was saved, its size; a line at the bottom: "3 articles, 4.2 MB of 200 MB". The chevron opens the saved copy (text and pictures, without the network). Options: Remove from saved (asked first), Article info. Saving keeps the article and its pictures (2 MB each at most) until removed: a refresh of its feed or unpinning the feed does not touch it. Saving is refused when it would pass the storage cap: "Saved articles use 198 of 200 MB. Delete some or raise the limit in Settings." With Download images = Off only the text is saved, and the notice says so |
| Add a feed | an address (a site, `example.com`, or a feed's own address), Find feed (Enter): "Found: title · N articles" with Pin and Cancel, or why nothing was found: no network, timed out, not a web address, no feed on that page |
| Settings | Text size, Refresh when opening (On / Off), Download images (Always / Off: Off downloads nothing, pictures already kept still show), Clear image cache (its size), Storage used and Storage cap for saved articles (50, 100, 200 or 500 MB), Clear read history (asked first), Export favorites (to `~/rss-reader-favorites.opml`), Import favorites (an `.opml` file of your home folder: what is missing is added), About (version, catalog, launcher pin, data folder) |

Until the options were opened once, a grey line under the title of the reader and of the article lists says "Hold or press
Ctrl+O for options". Favorites: the second line of a feed is its topic and the age of its newest article ("Technology · 1 h";
"General" for a general feed, never its country), or for a feed added by its address its site ("korben.info · 1 h").

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
| A picture of the article | a ring marks the picture nearest the middle of the screen (it moves as the text scrolls); Enter (or a pointer click) shows it full screen; Tab / Shift+Tab bring the next / previous picture to the middle | a tap on it |
| The full-screen picture | Left / Right the previous / next one, Esc closes, any other key shows the line again | a swipe; a tap closes |
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

Settings > Apps. By hand: `sudo dpkg -i rss-reader_0.1.1_arm64.deb`.

## Files the app keeps

| Path | What |
|---|---|
| `~/.local/share/rss-reader/` (or `$RSSREADER_DATA`) | `subscriptions.json` (your favourites), `settings.json`, `cache/` (the articles of each feed), `read/` (what you read), `saved/` (saved articles with their pictures, under the storage cap); the formats are documented in `src/core/store.hpp` |
| `~/.cache/rss-reader/images/` (or `$RSSREADER_CACHE`) | the downloaded pictures, 50 MB at most (the least recently used go first); Settings > Clear image cache empties it ([docs/IMAGES.md](docs/IMAGES.md)) |
| `~/rss-reader-favorites.opml` | written by Settings > Export favorites |

Reset: close the app and remove `~/.local/share/rss-reader` and `~/.cache/rss-reader`.

## Known limits (this version)

- Pictures: JPEG (baseline and progressive), PNG, GIF (the first frame only: GIFs do not move) and BMP, decoded by
  stb_image (see [docs/IMAGES.md](docs/IMAGES.md)). WebP, SVG and AVIF show "Format not supported"; a JPEG over 16 MP
  (progressive: 8 MP) or a PNG, GIF or BMP over 4.2 MP shows "Too large to show"; a photo turned by its EXIF tag is shown
  as stored, not turned; at most six pictures per article, 2 MB each. No zoom in the full-screen view.
- Articles show what the feed carries: a feed that gives only a summary shows only the summary (and only its pictures).
- Article titles are one line in the lists, cut with dots.
- The lists and the key bridge are copies of Mesh Hop's (a shared one in `apps/_common` is a follow-up).
- Untested on a board so far: see [tests/BOARD-TESTS.md](tests/BOARD-TESTS.md).

## Layout of the source

| Path | What |
|---|---|
| `src/core/` | plain C++17, namespace `rssreader`, no LVGL: the XML and feed parsers (RSS 2.0 / 0.9x, RDF, Atom), HTML to text blocks, feed discovery, HTTP through the `curl` command, the catalog, the store, the refresh, OPML (phase 1; see the headers) |
| `src/ui/` | namespace `rssui`: `ui_logic` (the rows' texts, the rules of the dialogs, the 3 s hold, the title that fits, which pictures an article shows; no LVGL), `keys` and `key_bridge` (the keys), `jobs` (the worker threads), `row_list` (the virtual list), `image_codecs.c` and `image_decode` (stb_image, `third_party/stb`, off the UI thread, scaled to the thumbnail), `rss_app*` and `page_*` (the pages; `page_viewer` the full-screen picture), `app_spec` (the `cdk::AppSpec`, minimum 640x480) |
| `data/catalog.json` | 269 feeds (see `data/CATALOG-SOURCES.md`); the package installs a copy in `/usr/share/APPLaunch/share/rss-reader/` |
| `tests/unit/` | host tests of the core (about 10000 checks, with the image headers, the image cache and the read marks made during a refresh), of the UI logic and the package files (`ui_tests.cpp`, about 520), and of the decoding (`img_tests.cpp`: `rss-reader-imgtest`, with LVGL, a mutation test of the decoders; `--bench` measures one picture) |
| `tests/flows/` | the scripted runner (`rss-reader-script`: a fake network answering from `tests/data/ui` by `routes.txt`, pictures in `tests/data/ui/img`, a fixed clock, a seeded store) and thirteen flows |
| `tests/scenes/` | scenes of the shared headless harness (Home, Browse, Add a feed with the on-screen keyboard, Settings, the quit question, the notice under 640x480, a start without the launcher) |
| `tools/fetch_smoke.cpp` | optional: fetches three real feeds with the real curl client (not part of the tests) |
| `tools/make_img_fixtures.py` | makes the picture fixtures and the large pictures of the measurements (Python with Pillow; not part of the tests) |
| `docs/IMAGES.md` | the decoders of the pinned launcher, what the app uses, the memory and time measured |

## Build and test

In WSL, from Windows (the launcher checkout and its build tree as for every app: see `apps/_common/build_app.sh -h`):

```
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/rss-reader/build.sh all --strict   # the arm64 binary and the host build
wsl -e sh .../apps/rss-reader/build.sh test      # unit tests only (core + UI logic)
wsl -e sh .../apps/rss-reader/build.sh asan      # the same under AddressSanitizer + UBSan
wsl -e sh .../apps/rss-reader/build.sh sim       # the thirteen flows against the fake network at 640x480 (or: sim 800x480)
wsl -e sh .../apps/rss-reader/build.sh ui        # the flows at the five sizes (about 3 minutes, the sizes side by side)
python tools/make_registry.py --only rss-reader --out <a folder outside packages/>   # the .deb
```

`cross` writes `root/usr/share/APPLaunch/bin/rss-reader`, the catalog copy and `root/usr/share/doc/rss-reader/build-info`;
`--strict` stops when the launcher is not the pinned one (`apps/_common/LAUNCHER_REF`). The unit tests need `g++`,
`python3` and `curl` (the curl client is exercised against a throw-away HTTP server on 127.0.0.1; no internet is used). The
board protocol is [tests/BOARD-TESTS.md](tests/BOARD-TESTS.md).

## Credits and licences

Our code: MIT, 2026 OSRdesign. The binary links the launcher's `cp0_lvgl` runtime and LVGL (MIT); the fonts are the
launcher's, read at run time (Montserrat and Noto Sans CJK: SIL OFL 1.1; DejaVu: the Bitstream Vera / DejaVu licence).
HTTP goes through the `curl` program (curl licence). Pictures are decoded by stb_image v2.30 (Sean Barrett and the stb contributors,
public domain or MIT at your choice; `third_party/stb`, one header compiled into the binary). The catalog lists the publishers' own feed addresses
([CATALOG-SOURCES.md](data/CATALOG-SOURCES.md)); the feeds' content belongs to their publishers and is only fetched and
shown on the device. Details in `root/usr/share/doc/rss-reader/copyright`.

## Changes

- 0.1.1: multitasking, `X-Background=freeze` (APP-CONTRACT section 13 of the launcher; [guidelines](../../docs/APP-GUIDELINES.md#multitasking-hidden-and-shown-again-x-background)): under a launcher with multitasking on, Super, Alt+Tab or
  three fingers held leave the reader open in the background, stopped; a refresh in flight waits with it (its curl is
  stopped too) and goes on, or fails and is retried at the next refresh. An older launcher: as before.
- 0.1.0 (phase 3): the pictures in the reader (downloaded on worker threads into a 50 MB image cache, decoded off the UI
  thread, placeholders that say why), the full-screen picture, Save for offline with the pictures and the Saved articles
  page (sizes, the use of the cap, remove, info), Settings > Clear image cache; the first-run hint; the Favorites' second
  line is the topic (or the site); a hairline under the lists' top; read marks made during a refresh are kept by the store.
- 0.1.0 (phase 2): the app: Home, Favorites with the background refresh, the article list, a minimal reader, Browse with
  the search, Add a feed, Settings with OPML export and import, the saved-articles list; options by a 3 s hold or Ctrl+O.
- phase 1: the core library, the catalog and their host tests.
