# viz1090 0.3.2: board tests of the intro screen

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
python deck.py put C:/CLAUDE/zero7/pkg-out/viz1090_0.3.2_arm64.deb /tmp/viz1090.deb
python deck.py sudo 'dpkg -i /tmp/viz1090.deb || apt-get -f install -y'
python deck.py run '/opt/viz1090/vizsetup --version; /opt/viz1090/vizsetup --screen'
```

`--version`: `viz1090 (viz1090) 0.3.0`, the launcher commit and `cp0_lvgl pinned` or `same-tree`. `--screen`: the
panel, e.g. `640 480 2 1130 screen.state` on the deck. Logs while testing:

```
python deck.py run 'journalctl --user -u cyberdeckzero.service -n 80 --no-pager | grep -E "cdk|viz1090|External app"'
```

## T1. Deck: fullscreen, launcher look, status strip

1. Open viz1090 from the home grid. Expect the intro: gold title "viz1090", the grey Esc button, the clock / Wi-Fi /
   Bluetooth strip where the home grid has it, a grey status line, a wide Start button, then SDR receiver, GPS
   receiver and Position rows (compare `receivers.png` / `root.png`, column 640x480).
2. Log: `[cdk] viz1090 0.3.0: launcher contract 1, mode fullscreen, canvas 640x480` and
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

## P. Portrait (PDA mode, 0.3.0; launcher v0.8.11, Settings > Screen > PDA mode)

On the Pi 3A+ with the 4.3" DSI panel (480x800 in PDA mode; touch only, SDR dongle plugged in if you have one). Expected
pictures: `portrait.png`, `portrait-keyboard.png`, `portrait-band.png`, `portrait-messages.png` in the host build's
`out/sheets`, and the README's two portrait shots (layout rules: README, *Portrait (PDA mode)*). Main path only; note
anything cut, overlapping or too small to read.

1. **Upright start.** Settings > Screen > PDA mode On, rotate left (the launcher restarts). The **viz1090** tile starts the
   intro **upright**: the header (gold title, Esc, the status strip) does not overlap, the status line under it has one or
   two lines whole, Start, then SDR receiver, GPS receiver and Position. Log: `canvas 480x800`. [ ] pass
2. **Rotate right.** Rotate the other way (right): the same intro upright, and a tap on a row highlights the row under the
   finger. [ ] pass
3. **Airport code by touch.** Tap Position twice, tap the field: the on-screen keyboard shows across the bottom with the
   field and its line above it; type `cdg` ("found: CDG"), Enter. [ ] pass
4. **Airport code by band.** Settings > On-screen keyboard = Handwriting + keyboard: Position, the field, write `jfk` in
   the band, its Enter. Then **Latitude** and **Longitude** by the band (`40.64`, `-73.78`): the intro shows both. [ ] pass
5. **Lat/lon by touch.** With the keyboard rule back on the keyboard: Latitude `48.8566` and Longitude `2.3522` on the `123`
   page; Esc (header) back on the intro. [ ] pass
6. **Start.** The map comes (`viz1090` and `readsb` running) upright, centred on the position, under the narrow header
   (title, Esc, the strip; log `[viz1090] map 480x...` under the header); nothing of the map is drawn over the header;
   the scale and the status line at the bottom whole. Compare `docs/screenshots/viz1090-portrait-map.png`. [ ] pass
7. **Pan, pinch, Esc.** One-finger drag pans the way the finger moves; two fingers zoom (a bit uneven is known); a double
   tap zooms and moves the map only about half way to the tapped point (known). The header's Esc: back to the launcher, no
   `readsb` left (`pgrep -x readsb` prints nothing). Open again, Start, the keyboard's Esc on the map: the same. [ ] pass
8. **Back to the launcher.** Home grid intact in portrait, the tile still there. [ ] pass
9. **Landscape unchanged.** Settings > Screen > PDA mode Off (the launcher restarts): the intro and the map are the
   0.2.3 ones at 800x480 (T1, T2 step 4), nothing turned or cut. [ ] pass

## T7. Tablet (amd64, FZ-M1; 0.3.2, `viz1090_0.3.2_amd64.deb`)

Needs Wi-Fi once (the package pulls libsdl2, libsdl2-ttf, libsdl2-gfx, librtlsdr0 and their dependencies) and, for aircraft, an RTL-SDR dongle with a 1090 MHz antenna (the deck's will do). Not run yet.

1. **Install.** `sudo apt install ./viz1090_0.3.2_amd64.deb`; Settings > Apps lists viz1090 0.3.2; the tile is on the home grid. `file /opt/viz1090/readsb` says x86-64; `ldd /opt/viz1090/readsb /opt/viz1090/viz1090` shows no "not found". [ ] pass
2. **Intro.** PDA mode on: the intro upright at 800x1280, header Esc and strip as in the other apps; the SDR row "Not found" (red) without the dongle, "Found" about a second after plugging it in (the kernel DVB driver is detached by librtlsdr). [ ] pass
3. **Map.** Start with a position: the map upright under the narrow header; with the dongle near a window, aircraft within a minute. One-finger pan, two-finger pinch, tap an aircraft; the header's Esc: back to the home grid, `pgrep -x readsb` prints nothing. [ ] pass
4. **Landscape.** PDA mode off (the launcher restarts): the same at 1280x800. [ ] pass
5. **No dongle.** Start anyway: the map with no aircraft ("connecting"), Esc back. [ ] pass
6. **Memory.** While the map runs: `grep VmRSS /proc/$(pgrep -x viz1090)/status; free -m`. [ ] pass
