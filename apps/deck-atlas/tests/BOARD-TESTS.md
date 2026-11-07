# Deck Atlas: board tests (the main path, about 15 minutes)

For the owner (controller-led testing: Claude deploys and reads the logs, you do the physical steps). The models
(settings, labels, the import progress, the online sources' flow, the file browser, the map geometry, the package files)
are tested on the PC (`build.sh host`: 399 checks), and twelve scenes draw every page at 640x480, 480x320, 320x480 and
480x800 (plus the zero8 density). What the PC cannot tell: **whether the map is readable on the glass**, how a real
import feels on a Zero 2 W's SD card, the memory use, and the network path. Keep it short: the main path must work.

Boards: the deck (640x480, 11.3 px/mm), zero8 (480x320 3.5", 6.3 px/mm; 320x480 with PDA mode on), and the FZ-M1 tablet
for the amd64 package. Install the `.deb` from the bundle (`sudo dpkg -i deck-atlas_0.1.0_arm64.deb`); it pulls
`libsqlite3-0`, `libcurl4t64` and `ca-certificates`.

Before: copy the tile folder (MapTileDl's `tiles`, about 276 MB) to the board, e.g. `scp -r tiles deck@zero7.local:Maps/`,
or put it on a USB stick.

## T0. Start, the look (every board, 1 min)

1. Open Deck Atlas from the home grid. The launcher's header (Esc, title "Deck Atlas", status strip), the tab strip at
   the bottom (Map, Sources, Settings, Map underlined in blue). With an empty store: a grey world grid and the plate
   "No map yet. Import a map file in Sources."
2. Log: `journalctl --user -u <launcher service> | grep deck-atlas` shows `[deck-atlas] 0.1.0 640x480 scale Auto (2x),
   store /home/<user>/.local/share/cyberdeckzero/maps, 0 map files, network on`.

## T1. Import the tile folder (3 min, the deck first, then zero8)

1. Sources > Import a map > Home folder > Maps > tiles. The folder shows one row "Import this tile folder z1-17".
2. Enter (or tap it twice): the dialog says the folder is converted into one MBTiles file in
   `~/.local/share/cyberdeckzero/maps` and that you are responsible for the terms of the maps you import. Import.
3. The import page: "Checking free space", "Counting tiles", then "Converting tiles" with the bar, "N of 10,326 tiles",
   the rate and the time left, then "Imported: 10,326 tiles, 260 MB, ... tiles/s". **Note the rate** (report 055: 784
   tiles/s on the deck for small tiles; these are about 26 KB) and whether the deck stays responsive (the clock in the
   strip keeps going, the Bluetooth keyboard keeps typing).
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

## T5. What to tell me

- The import rate and the time for the 10,326 tiles on the deck; anything slow while it ran.
- The legibility answers (T2) per board and scale.
- VmRSS on the deck.
- Anything cut, overlapping or hard to hit: the zoom buttons, the tab strip, the rows; the attribution line at the
  bottom right of the map.
- Cleanup: `sudo dpkg -r deck-atlas` (the maps stay in `~/.local/share/cyberdeckzero/maps`; delete that folder to reset).
