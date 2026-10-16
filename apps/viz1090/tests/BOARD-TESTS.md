# viz1090 0.2.0: board tests of the intro screen

For the Controller and the owner (controller-led testing: the Controller deploys and reads logs, the owner does the
physical steps). Commands run from Windows: `deck.py` is `C:\CLAUDE\zero7\launcher\docs\dev\deck\deck.py` (password in
`PIPW`; `DECK_HOST=<ip>` points it at another board). Nothing here uses `pkill -f`: the intro's process name is
`vizsetup` (`pgrep -x vizsetup`), the map's `viz1090`, the decoder's `readsb`.

Expected pictures: the contact sheets of `build.sh host` in `\\wsl.localhost\Ubuntu\home\osrde\cdk-build\viz1090\host\out\sheets\`
(or the `CDK_BUILD` folder used): `root.png`, `receivers.png`, `gps-wait.png`, `position-keyboard.png`,
`position-osk.png`, `position-forced.png`, `degrees.png`, `recent.png`, `messages.png`, `not-installed.png`,
`nocontract.png`, `notice.png`.

## 0. Build and deploy

```
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/viz1090/build.sh all --strict
python tools/make_registry.py --only viz1090 --out C:/CLAUDE/zero7/pkg-out
python deck.py put C:/CLAUDE/zero7/pkg-out/viz1090_0.2.0_arm64.deb /tmp/viz1090.deb
python deck.py sudo 'dpkg -i /tmp/viz1090.deb || apt-get -f install -y'
python deck.py run '/opt/viz1090/vizsetup --version; /opt/viz1090/vizsetup --screen'
```

`--version`: `viz1090 (viz1090) 0.2.0`, the launcher commit and `cp0_lvgl pinned` or `same-tree`. `--screen`: the
panel, e.g. `640 480 2 1130 screen.state` on the deck. Logs while testing:

```
python deck.py run 'journalctl --user -u cyberdeckzero.service -n 80 --no-pager | grep -E "cdk|viz1090|External app"'
```

## T1. Deck: fullscreen, launcher look, status strip

1. Open viz1090 from the home grid. Expect the intro: gold title "viz1090", the grey Esc button, the clock / Wi-Fi /
   Bluetooth strip where the home grid has it, a grey status line, a wide Start button, then SDR receiver, GPS
   receiver and Position rows (compare `receivers.png` / `root.png`, column 640x480).
2. Log: `[cdk] viz1090 0.2.0: launcher contract 1, mode fullscreen, canvas 640x480` and
   `[viz1090] intro: position ..., SDR ..., viz1090 installed, settings /home/<user>/.config/cardputerzero/viz1090.conf`.

## T2. Esc from every source, Start, the contract with run_viz1090.sh

1. BT keyboard Esc at the intro: back to the home grid, no map, no `readsb` left (`pgrep -x readsb` prints nothing).
2. Open again, header Esc by touch: the same.
3. Open, Down Down Down Enter (Position), Esc: back on the intro; Esc: home grid.
4. Open, Start (Enter or a tap) with a position: the map comes (`viz1090` and `readsb` running), centred on it,
   under the same header as the intro (gold "viz1090", the grey Esc button, the strip in the same places; compare
   `docs/screenshots/viz1090-map.png`); log `[viz1090] map 640x424 under a 56 px header`. Nothing of the map is drawn
   over the header; one-finger drag pans, two fingers zoom. Tap the header's Esc (it turns blue while held): home
   grid, no `readsb` left. Open, Start, BT keyboard Esc on the map: the same. Open, Start, hold Esc 3 s on the map:
   the watchdog ends it, home grid, no `viz1090` or `readsb` left.
5. Hold Esc 3 s on the intro: the launcher's watchdog ends it; log `[cdk] viz1090: signal 15, ended cleanly`; the map
   does not start; home grid intact.

## T3. Touch only (Pi 3A+ 800x480, no keyboard): every function without typing

1. No saved position (`mv ~/.config/cardputerzero/viz1090.conf ~/viz1090.conf.bak` first): Start says "No position
   yet: choose one under Position first" and stays.
2. Tap Position twice: the Position page opens on its list, no keyboard. Tap the airport code field: the on-screen
   keyboard comes, the field and its line stay above it. Type `cdg`: "found: CDG"; Enter: back on the intro with
   "CDG  49.0097, 2.5478".
3. Tap Position twice, tap Latitude twice: type `48.8566` (digits on the keyboard's 123 page), Enter; Longitude
   `2.3522`, Enter; Esc (header): the intro shows "48.8566, 2.3522". Start: the map opens around Paris.
4. Again: the Position page lists RECENT (the places started with); tap one twice: it is set without typing.
5. Plug a USB keyboard while the on-screen keyboard is up: it goes within about a second.
6. Restore: `mv ~/viz1090.conf.bak ~/.config/cardputerzero/viz1090.conf`.

## T4. Receivers (a board with the RTL-SDR dongle and, if there is one, a USB GPS)

1. Without the dongle: "Not found" with a cross; plug it in: "Found" with a check within about a second (hot-plug);
   unplug: back to "Not found".
2. A GPS receiver indoors: "Waiting for a fix" (amber); Enter on the row: "GPS on /dev/ttyACM0: no fix yet, it needs
   open sky". Outdoors with a fix: "Fix: lat, lon" with a check; Enter or a tap twice: "Position set from the GPS".
3. If the user is not in `dialout`: "No access to /dev/ttyACM0" with a cross, and the log line with the
   `usermod` command.

## T5. Square screen with the keyboard rule forced (HackBerry 720x720)

Open the Position page with the built-in keyboard: it opens on its list; Up: the field edits with the on-screen
keyboard (forced rule) above the field's line; typing works with both keyboards; Esc stops editing, Esc goes back.
Compare `position-forced.png`, column 720x720.

## T6. Started by hand and memory

1. Over ssh, from the console: `python deck.py run 'timeout 5 /opt/viz1090/vizsetup; echo rc=$?'` while the launcher
   is stopped by the owner, or on a board where that is allowed: one log line `no launcher contract (started by hand
   ...)`, no crash (rc 124 from timeout, or 1 after Esc).
2. While the intro is on screen: `python deck.py run 'grep VmRSS /proc/$(pgrep -x vizsetup)/status; free -m'`:
   VmRSS under 25 MB.

Remove after the tests (when the deck should go back to the published version): `python deck.py sudo 'dpkg -r viz1090'`.
