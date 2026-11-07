# Deck Atlas: board tests (the main path, about 15 minutes)

For the owner (controller-led testing: Claude deploys and reads the logs, you do the physical steps). The models
(settings, labels, the import progress, the online sources' flow, the file browser, the map geometry, the package files)
are tested on the PC (`build.sh host`: 1,912 checks), and 22 scenes draw every page at 640x480, 480x320, 320x480 and
480x800 (plus the zero8 density). What the PC cannot tell: **whether the map is readable on the glass**, how a real
import feels on a Zero 2 W's SD card, the memory use, and the network path. Keep it short: the main path must work.

Boards: the deck (640x480, 11.3 px/mm), zero8 (480x320 3.5", 6.3 px/mm; 320x480 with PDA mode on), and the FZ-M1 tablet
for the amd64 package. Install the `.deb` from the bundle (`sudo dpkg -i deck-atlas_0.2.0_arm64.deb`); it pulls
`libsqlite3-0`, `libcurl4t64` and `ca-certificates`.

Before: copy the tile folder (MapTileDl's `tiles`, about 276 MB) to the board, e.g. `scp -r tiles deck@zero7.local:Maps/`,
or put it on a USB stick.

## T0. Start, the look (every board, 1 min)

1. Open Deck Atlas from the home grid. The launcher's header (Esc, title "Deck Atlas", status strip), the tab strip at
   the bottom (Map, Sources, Settings, Map underlined in blue). With an empty store: a grey world grid and the plate
   "No map yet. Import a map file in Sources."
2. Log: `journalctl --user -u <launcher service> | grep deck-atlas` shows `[deck-atlas] 0.2.0 640x480 scale Auto (2x),
   store /home/<user>/.local/share/cyberdeckzero/maps, 0 map files, network on`.

## T1. Import the tile folder (3 min, the deck first, then zero8)

1. Sources > Import a map > Home folder > Maps > tiles. The folder shows one row "Import this tile folder z1-17".
2. Enter (or tap it twice): the dialog says the folder is converted into one MBTiles file in
   `~/.local/share/cyberdeckzero/maps` and that you are responsible for the terms of the maps you import. Import.
3. The import page: "Checking free space", "Counting tiles", then "Converting tiles" with the bar, "N of 10,326 tiles",
   the rate and the time left, then "Imported: 10,326 tiles, 260 MB, ... tiles/s". **Note the rate** (report 055: 784
   tiles/s on the deck for small tiles; these are about 26 KB) and whether the deck stays responsive (the clock in the
   strip keeps going, the Bluetooth keyboard keeps typing).
   If this folder was imported on the deck with an earlier build of the engine, remove it in Sources first and import it
   again: only a fresh import keeps every zoom's coverage (the Sources page of the file then shows the union as Area).
4. Show on map: the map over the imported area at the file's deepest level. Pan by dragging and by the arrow keys, zoom with the + / -
   buttons on the right, the + and - keys, Page Up / Down, a double tap.
5. Cancel once on a second try (import the same folder again from a USB stick: Cancel, then import it again; it goes on
   where it stopped, "N from before").

## T2. Legibility (the legibility check of report 054; 3 min, the deck and zero8 in both orientations)

At arm's length, at zoom 15 and zoom 17:

- Can you read the street names, the road numbers, the place names and the scale bar's figure?
- Settings > Display scale: Auto shows "Auto (2x)" on the deck and "Auto (1.5x)" on zero8. Try 1x, 1.5x and 2x on each
  board and tell which you prefer. The PC check's verdict is in the README (Known limits: legibility); confirm or correct it.

## T3. Online source (2 min, a board with Wi-Fi; optional)

1. Sources > OpenTopoMap: the terms dialog (what the source is and its rules in plain words, "Daily limit in
   this app: 2,000 requests", the "check the provider's current terms" line with the address, the map data credit). I agree, enable. The row says "On", the map shows OpenTopoMap and
   fills as you look; the attribution is in the bottom right corner.
2. Settings > Offline mode On: the map's top strip says "Offline: saved tiles only"; the tiles you saw stay.
3. Switch OpenTopoMap off and on again: no dialog the second time.
4. A keyed source (Thunderforest, a free Hobby key): the key page first, then the terms; the key row says "Key saved" and
   the key is never shown again. `ls -l ~/.local/share/cyberdeckzero/maps/sources/` shows the key file `-rw-------`.

## T4. Memory, exit, files (2 min, the deck)

1. With the map moving: `grep VmRSS /proc/$(pgrep -x deck-atlas)/status` (guidelines: under 25 MB; report 055 expected
   about 22-24 MB with the 8 MiB tile cache).
2. Esc at a tab ends the app; hold Esc 3 s also ends it. Open it again: the same view (centre, zoom) comes back.
3. `cat ~/.config/deck-atlas/settings.conf` shows the view and the settings, no key.
4. Started over ssh without the launcher (`/usr/share/APPLaunch/bin/deck-atlas` on the console of the deck): the app
   comes up and logs one line about the missing contract.

## T5. GPS (0.2.0; 15 minutes, outdoors or at a window, the u-blox puck on the deck and then on zero8)

The PC tests cover the parser, the fix logic, the baud sniff on a fake port and on a pseudo-terminal, follow mode and the
`position.state` writer, and eight scenes draw every state at 640x480, 480x320 and 320x480. What only a board can tell: the
real puck, the cold start, power, memory.

1. **Off is quiet.** GPS Off (the default): the Map tab has no chip and no follow button; `ls -l /proc/$(pgrep -x deck-atlas)/fd`
   shows no `/dev/tty*`. Settings > GPS receiver shows GPS Off, Port Auto, Speed Auto.
2. **Plug the puck, GPS Auto** (Settings > GPS receiver > GPS). `lsusb` and `ls /dev/ttyACM* /dev/ttyUSB*`: note which node it
   is and the USB id (u-blox 1546:01a7/01a8/01a9). If the chip says "No access to GPS port": `groups` must list `dialout`
   (`sudo usermod -aG dialout $USER`, log in again). The log (`[deck-atlas] gps: ...`) says which port and speed it locked at.
3. **Cold start.** Put the puck where it sees the sky. Note the time from plugging to "Searching", then "2D fix"/"3D fix N
   sats" (a cold start takes minutes; the chip must say "Searching" meanwhile, and no dot appears before a fix).
4. **The dot.** With the fix: the blue dot with its white outline and the accuracy ring at the right place; walk or drive: the
   heading wedge and the speed in the chip (km/h; mph with Units mi). Cover the puck or unplug its antenna side: after 5 s the dot
   is hollow and the chip says "Last fix N s ago".
5. **Follow.** Tap the crosshair button under the zoom buttons (or press G): the map recentres at once and keeps the dot in the
   middle while you move; zoom with the buttons: follow stays; drag the map: follow goes off (the button loses its highlight);
   tap it again: it recentres. Home does the same. Check on the deck and on zero8 that the button is below the zoom buttons, the
   same size, and clear of the map credit and the scale bar (also at 320x480 with PDA mode on).
6. **Unplug and replug** while the app runs: "No GPS device" within a few seconds, no dot, no crash; plug it back: it finds the
   receiver again within about 2 s (same node or another number) and the fix returns. Repeat three times quickly.
7. **Wrong speed.** Settings > Speed 4800 with the puck at 9600: "Searching" for ever (nothing locks, no garbage position);
   back to Auto: it locks at 9600. The log shows the speeds tried.
8. **Another app on the port.** Open Serial Console or Mesh Hop on the same node first: Deck Atlas says "Searching" (the port is
   held; no error, no crash) and takes it when the other app lets go (within the 30 s backoff).
9. **position.state.** While there is a fix: `cat $APPLAUNCH_STATE_DIR/position.state` (else
   `cat $XDG_RUNTIME_DIR/applaunch/position.state`): `fix=3`, `lat`, `lon`, `alt_m`, `utc` (the receiver's time), `mono_ms`, `pid`
   equal to `pgrep -x deck-atlas`. `watch -n1 cat ...` changes about once a second, never twice a second. Unplug: `fix=0` and no
   position. GPS Off or leaving the app: the file is gone (`ls`).
10. **Memory and power.** `grep VmRSS /proc/$(pgrep -x deck-atlas)/status` with a fix and the map moving (no more than a few
    MB above T4); with Wi-Fi tile fetching on, the deck does not reset (a powered hub if it does; note the board).
11. **Privacy.** `ss -tnp | grep deck-atlas` shows only the tile source's connections (none with Offline mode on), never anything
    about the position.

## T6. What to tell me

- The import rate and the time for the 10,326 tiles on the deck; anything slow while it ran.
- The legibility answers (T2) per board and scale.
- VmRSS on the deck, with and without a GPS fix (T5.10).
- The GPS: time to a cold fix, the puck's node and USB id, whether it locked at 9600, anything odd on unplug / replug.
- Anything cut, overlapping or hard to hit: the zoom buttons, the tab strip, the rows; the attribution line at the
  bottom right of the map.
- Cleanup: `sudo dpkg -r deck-atlas` (the maps stay in `~/.local/share/cyberdeckzero/maps`; delete that folder to reset).
