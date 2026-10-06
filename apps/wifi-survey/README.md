# Wi-Fi Survey

Version 0.1.0, package `wifi-survey`. Part of [cyberdeck-zero-apps](../../README.md).

Wi-Fi Survey shows the Wi-Fi networks around the deck: a **Networks** list (name, signal, channel, band, security,
the network you are connected to marked) and a **Channels** chart that points out the least crowded channel.

Passive only: it reads the scan results of NetworkManager (`nmcli`). Nothing is sent, captured or connected, and
it needs no root.

## Features

* **Networks**: the networks in range sorted by signal, with SSID, signal bar and strength, channel, band and
  security. The network the deck is connected to is marked.
* **Detail**: Enter (or a tap) on a network shows its BSSID, vendor, frequency and rate.
* **Channels**: the number of networks per channel on 2.4 and 5 GHz, with the least crowded channel highlighted.
* A scan runs about every 5 seconds; R rescans at once.

## Install

On the deck: **Settings > Apps > Sources > + Add a GitHub source**, enter `OSRdesign/cyberdeck-zero-apps`, go to the
**Apps** tab, pick **Wi-Fi Survey** and press Enter. Installing asks for the deck user's sudo password; the package depends on
`network-manager`. See the [repository README](../../README.md) for details.

## Use

The app runs in the launcher's normal 640x340 window, under the top bar.

| Key | Action |
| --- | --- |
| Left / Right / Tab | Switch between Networks and Channels |
| Up / Down | Move in the list |
| Enter | Open the detail of the selected network (Left / Right then show the previous / next one) |
| R | Rescan now |
| Esc | Detail: back to the list you came from. Networks / Channels: nothing, only a short hint "Hold Esc 3 s to exit". A short Esc never quits; hold Esc for 3 seconds to exit (handled by the launcher) |

The detail view follows the access point by its BSSID, so it stays on the same network when the list reorders
after a scan. If the network disappears, the title shows "- gone" and the last known values stay on screen until
it returns. Without a Wi-Fi adapter the app shows "No Wi-Fi adapter found" and "tap to retry"; if a scan cannot
start the list stays and the header shows "(old)".

Touch: tap a row for its detail and the tabs to switch screens.

## Screenshots

Taken on the deck with test scan data.

![Wi-Fi Survey, Networks](../../docs/screenshots/wifi-survey-networks.png)
![Wi-Fi Survey, Channels](../../docs/screenshots/wifi-survey-channels.png)

## Requirements

* `network-manager` (the package depends on it) and a Wi-Fi adapter managed by it.

## Known limits

* The touch behaviour was checked with test touch events, not with a finger, until the owner of the deck has tried it.
* After an upgrade through the Apps menu the tile moves to the end of the launcher grid.
* The app uses the launcher's standard window (640x340), not the full screen.

## Build from source

The source is in [`src`](src) (an LVGL application for the launcher's `cp0_lvgl` runtime; `src/tests` holds a scan
test and an `nmcli` stub). The build runs in WSL (Ubuntu) and needs a checkout of the launcher repository with its
`.venv-pizero2w` (the script looks in `/mnt/c/CLAUDE/zero7/launcher`; set `LAUNCHER` to change it):

```
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/wifi-survey/build/build.sh
```

[`build/build.sh`](build/build.sh) copies `src/` into the scratch project `projects/WifiSurvey` of the launcher
checkout (kept out of the launcher's git), runs `scons` under the shared build lock, and installs the binary as
`root/usr/share/APPLaunch/bin/M5CardputerZero-wifi-survey`. Then, at the root of this repository, run
`python3 tools/make_registry.py`. Only `root/` goes into the `.deb`; this page, `src/` and `build/` do not.

## Credits and licence

MIT. Vendor names come from the IEEE OUI registry published at <https://standards-oui.ieee.org/> (the app ships a
derived `oui.tsv`). The app runs on the LVGL based `cp0_lvgl` runtime of the M5CardputerZero launcher (M5Stack, MIT).
