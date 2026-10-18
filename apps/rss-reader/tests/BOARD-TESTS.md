# RSS Reader 0.1.0 (phase 2): board tests

Skeleton for phase 2 (the pages and the minimal reader); phase 3 (images, the full reader, Save for offline) adds its own
steps. For the Controller and the owner (controller-led testing: the Controller deploys and reads the logs, the owner does
the physical steps and ticks the boxes). Commands run from Windows: `deck.py` is
`C:\CLAUDE\zero7\launcher\docs\dev\deck\deck.py` (password in `PIPW`; `DECK_HOST=<ip>` points it at another board). The
app's process name is `rss-reader` (`pgrep -x rss-reader`; never `pkill -f`).

Boards: **deck** (Pi Zero 2 W, 640x480, Bluetooth keyboard), **Pi 3A+** (800x480 touch, no keyboard: the on-screen
keyboard), **HackBerry** (CM5, 720x720 square touch, built-in keyboard, the on-screen keyboard rule forced). Every board
needs the network (Wi-Fi) for the refresh steps; R3 turns it off.

Expected pictures: the flow shots of `build.sh ui` in `\\wsl.localhost\Ubuntu\home\osrde\cdk-build\rss-reader\flows-sizes\<WxH>\<flow>\`
(or the `CDK_BUILD` folder used), made with a fake network and a clock fixed at 2026-10-18 16:00 UTC: the feeds, the ages
and the counts differ on a board, the layout does not.

## 0. Build and deploy (each board)

```
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/rss-reader/build.sh all --strict
python tools/make_registry.py --only rss-reader --out C:/CLAUDE/zero7/pkg-out
set DECK_HOST=<board ip>        (deck: leave it unset)
python deck.py put C:/CLAUDE/zero7/pkg-out/rss-reader_0.1.0_arm64.deb /tmp/rss-reader.deb
python deck.py sudo 'dpkg -i /tmp/rss-reader.deb || apt-get -f install -y'
python deck.py run '/usr/share/APPLaunch/bin/rss-reader --version; which curl; ls -l /usr/share/APPLaunch/share/rss-reader/'
```

`--version`: `RSS Reader (rss-reader) 0.1.0`, the launcher commit `f35c2d6...` with `cp0_lvgl pinned`, "needs a screen of at
least 640x480". `curl` is found; `catalog.json` is there. Logs while testing:

```
python deck.py run 'journalctl --user -n 80 --no-pager | grep -E "cdk|rss-reader"'
```

The first line of a start says the data folder and `catalog ... 269 feeds`. A clean start: `rm -rf ~/.local/share/rss-reader`
(the app closed).

## M. Memory and CPU (each board)

With the app open, after Favorites with 6 feeds refreshed, a feed's articles, two articles (one Japanese: NHK) and Browse
were each shown once:

```
python deck.py run 'p=$(pgrep -x rss-reader); grep -E "VmRSS|VmHWM" /proc/$p/status; top -b -n 2 -d 3 -p $p | tail -1; free -m'
```

Both VmRSS and VmHWM under 25 MB (25600 kB); idle %CPU a few percent at most. The Noto CJK fallback font is loaded for the
reader and the lists: watch VmHWM after the NHK article.

| Board | VmRSS kB | VmHWM kB | idle %CPU | Pass | Fail |
| --- | --- | --- | --- | --- | --- |
| deck | | | | [ ] | [ ] |
| Pi 3A+ | | | | [ ] | [ ] |
| HackBerry | | | | [ ] | [ ] |

## H. Home and quitting (deck)

1. Open RSS Reader from the launcher (a clean start). Expect `home/01-home.png`: the header "RSS Reader" with Esc and the
   status strip identical to the home grid's, "No favorites yet" under it, five rows with chevrons. [ ] pass
2. Up / Down move the highlight; Enter opens; Esc of a page comes back to Home. [ ] pass
3. Esc at Home asks "Quit RSS Reader?" (`home/03-quit-dialog.png`); Esc or Cancel keeps the app, Quit ends it; the header's
   Esc button does the same by touch. [ ] pass
4. Hold Esc 3 s anywhere: the launcher ends the app; the home grid is intact; `pgrep -x rss-reader` prints nothing. [ ] pass

## B. Browse and pin (deck, then Pi 3A+ by touch)

1. Browse feeds: International first, the languages under each country, the counts (`browse/01-countries.png`); no Podcasts
   topic anywhere. [ ] pass
2. A country, a topic, the feeds with an empty ring (`browse/03-feeds.png`); the title "Browse > ... > Technology" never runs
   under Esc. [ ] pass
3. A first tap selects a feed, a tap on the selected feed pins it (blue dot, "Added to favorites"), again unpins it. [ ] pass
4. Search: type "linux" (keyboard) or tap the field (Pi 3A+: the on-screen keyboard shows, Enter puts it away and the
   results show). [ ] pass

## F. Favorites and the refresh (deck)

1. Pin 4-6 feeds (one Japanese: Japan > General > NHK). Favorites opens and, Refresh when opening being On, updates them one
   after the other: "Stop" and "Updating 2/6..." on the bar, "Updating..." on the row in flight (`refresh/01-updating.png`).
   The UI stays responsive while it runs (scroll the list). [ ] pass
2. Unread feeds: blue dot, bold name, "N new" in blue; all read: grey name, no dot (`favorites/02-favorites.png`). [ ] pass
3. Hold a row 3 s: the cue "Keep holding: options for ..." then Options (`favorites/05-options.png`); Ctrl+O does the same for
   the selected row. Move down, Details, Mark all read, Unpin (asked first). [ ] pass
4. Leave Favorites during a refresh: it stops ("Refresh stopped"). [ ] pass

## R. Network problems (deck)

1. A feed that fails shows why on its row in amber ("Not updated: Server error (HTTP 503)"). [ ] pass
2. Wi-Fi off, Refresh all: "Not updated: No network", every feed keeps its articles and they open. [ ] pass
3. Wi-Fi back: Refresh all works again. [ ] pass

## A. Articles and the reader (deck, then HackBerry)

1. A feed's articles: unread with the blue dot, read ones grey (`articles/01-articles.png`); options by a 3 s hold or Ctrl+O:
   Mark as read / unread, Mark all read, Refresh this feed, Article info. [ ] pass
2. Open an article: title, source and date, the text; it is marked read (back in the list: grey). [ ] pass
3. Up / Down, Page Up / Page Down, a drag and the wheel (pointer navigation on) scroll; Left / Right go to the previous / next
   article. [ ] pass
4. Hold the text 3 s (or Ctrl+O): Text size (arrows, OK / Cancel), Small / Medium / Large readable at 640x480. [ ] pass
5. A French article (accents, guillemets) and a Japanese one (NHK) show every character. [ ] pass

## D. Add a feed (Pi 3A+ by touch, then deck with the keyboard)

1. Add a feed: the on-screen keyboard shows at once on the Pi 3A+ (none with a keyboard); type `korben.info`, Enter: "Found:
   Korben · N articles" with Pin / Cancel (`add/03-found.png`). Pin: it is in Favorites. [ ] pass
2. A site without a feed (`example.com`): "No feed found..."; Wi-Fi off: "No network..."; `nothing`: "That is not a web
   address...". [ ] pass

## S. Settings and OPML (deck)

1. Every row: values with a chevron (Text size, Refresh when opening, Download images, Storage cap), dots (Clear read history,
   Export favorites), grey read-outs (Storage used, About) (`settings/01-settings.png`, `settings/10-about.png`). [ ] pass
2. Export favorites: `~/rss-reader-favorites.opml` exists and lists the favorites
   (`python deck.py run 'head -c 600 ~/rss-reader-favorites.opml'`). [ ] pass
3. Copy an OPML file of another reader to the home folder; Import favorites lists it; the import adds what is missing and says
   so. [ ] pass
4. Clear read history: every article unread again. [ ] pass

## N. Without the launcher, small screens

1. Started over ssh (`python deck.py run 'timeout 5 /usr/share/APPLaunch/bin/rss-reader; echo rc=$?'`): one log line, no
   crash. [ ] pass
2. The 480x320 notice: covered by `tests/scenes/notice.scene` (harness); on the Pi 3A+ with its SPI panel if available. [ ] pass

## Cleanup

`python deck.py sudo 'dpkg -r rss-reader'`; the user's data stays in `~/.local/share/rss-reader` (remove it by hand).
