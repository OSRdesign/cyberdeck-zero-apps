# cyberdeck-zero-apps

Apps for the [CyberdeckZero Launcher](https://github.com/OSRdesign/cyberdeck-zero-launcher) (a touch launcher for
Raspberry Pi cyberdecks), installable from the deck's **Settings > Apps** menu.

<!-- branding-allow-begin: disclaimer -->
An independent hobby project, not affiliated with M5Stack (see the
[launcher's disclaimer](https://github.com/OSRdesign/cyberdeck-zero-launcher#readme)).
<!-- branding-allow-end -->

**Writing an app? Read the [app guidelines](docs/APP-GUIDELINES.md) first** (screen sizes, input, packaging, testing, checklist). List rows must show what can be tapped: see
[row kinds](docs/APP-GUIDELINES.md#row-kinds-what-can-be-tapped). What the launcher gives an app (the status strip,
the state files, requests such as a beep or a toast, the launcher versions and the pin):
[section 3](docs/APP-GUIDELINES.md#3-the-launcher-contract) of the guidelines; the normative text is the launcher's
[APP-CONTRACT.md](https://github.com/OSRdesign/cyberdeck-zero-launcher/blob/main/docs/APP-CONTRACT.md).

Publishing a version (the one pull request, the registries, the checks): [PUBLISHING.md](docs/PUBLISHING.md). Every `app.json`
field: [APP-JSON.md](docs/APP-JSON.md). The scripts: [`tools/README.md`](tools/README.md).

For developers: the guidelines end with [lessons learned on the deck](docs/APP-GUIDELINES.md#14-lessons-learned) (the
on-screen keyboard rule, options by a hold, heavy work at start, logs after a reset, text size, building the packages),
and the shared code of the native apps is described in [`apps/_common`](apps/_common/README.md).

This repository is both the **app source** of that menu and a **template**: copy it and you can host your own
apps for the deck in a few minutes, with nothing but a public GitHub repository.

## Apps

Each app has its own page with the full description, keys, requirements, known limits, credits and build notes.

| App | Version | Packages | What it does |
| --- | --- | --- | --- |
| [**Cassette**](apps/cassette/README.md) (`cassette`) | 0.3.1 | arm64, amd64 | A sound recorder with the look of a tape deck: records from any USB sound card or headset, or a Bluetooth headset on boards with a sound server (PipeWire), to WAV, FLAC or Ogg Opus, crash-safe (a take survives a power cut up to the last sync), with a level meter and target zone, card and software gain, a record timer, a live monitor for headphones, playback with a 5x scan, trim and split on a waveform Edit page, and merging of takes of one format. No cloud: nothing leaves the board. From 480x320 up, portrait (PDA mode), landscape and square layouts. Needs a USB sound card or headset (or a Bluetooth headset); the Store installs the codec libraries (`libopus0`, `libopusenc0`, `libflac`) and `libpulse0` with it. Category Tools. |
| [**Deck Atlas**](apps/deck-atlas/README.md) (`deck-atlas`) | 0.2.0 | arm64, amd64 | Offline maps, by touch or keyboard: import your own MBTiles or PMTiles files, or a z/x/y tile folder from a tile downloader (converted on the board), from the SD card or a USB stick, then pan and zoom without any network. Three online tile sources (OpenTopoMap, and Thunderforest and Geoapify with your own key) stay off until you switch one on and accept its terms; the tiles you viewed are kept on the SD card. The OpenStreetMap Foundation's tile servers are never used. From 480x320 up, portrait (PDA mode) layout at 320x480. An optional USB GPS (NMEA, listen-only; off until you switch it on) shows your position, with follow mode and a status page (fix, sky plot, signal bars), and writes `position.state` for other apps; the position never leaves the deck. Category Tools. |
| [**Home Assistant**](apps/home-assistant/README.md) (`home-assistant`) | 0.3.0 | arm64, amd64 | A client of your own Home Assistant server on the local network, signed in by scanning a QR code with a phone: your stored dashboard drawn natively (entities, sensor and weather cards), then tiles for your favourites and each area, with Home Assistant's icons and state colours; switch a light, run a scene, locks and garage doors ask first. By touch or keyboard, from 480x320 up, portrait (PDA mode) layout. Needs the Home Assistant server. Category Home. Not affiliated with the Home Assistant project. |
| [**Klondike Solitaire**](apps/klondike-solitaire/README.md) (`klondike-solitaire`) | 0.1.1 | arm64, amd64 | The classic patience game, by touch or keyboard: draw one or three, Standard, Vegas or no scoring, unlimited undo and redo, hints, deals that are the same on every board and winnable deals proven by a built-in solver, statistics, the win cascade, portrait (PDA mode) and left-handed layouts. Category Games, no network. |
| [**Mine Field**](apps/mine-field/README.md) (`mine-field`) | 0.1.1 | arm64, amd64 | A mine-clearing puzzle, by touch or keyboard: three levels whose boards are derived from the screen and always fit it (no pan, no scroll), fair boards that never need a guess, hints, long press to flag, chord, best five times, statistics, the game in progress kept, portrait (PDA mode) layout. Category Games, no network. |
| [**Markdown Notes**](apps/markdown-notes/README.md) (`markdown-notes`) | 0.2.1 | arm64, amd64 | Notes as plain Markdown files in a folder on the deck (tile name "Notes"): write them, read them with headings, lists, task boxes and links, open any note by name, and keep the folder in sync with Syncthing, rsync or git. Category Productivity, no network. |
| [**Mesh Hop**](apps/mesh-hop/README.md) (`mesh-hop`) | 0.8.3 | arm64, amd64 | Full-screen client for MeshCore LoRa mesh networks: chats, contacts, repeater login, the meshcore-cli command line over the mesh, nearby nodes, radio settings, packet log and position, with a MeshCore companion radio board on USB, Bluetooth LE or IP, by touch or keyboard, on any screen from 640x480 up. Category Radio. |
| [**mmWave Radar**](apps/mmwave-radar/README.md) (`mmwave-radar`) | 0.1.1 | arm64, amd64 | A viewer for the Hi-Link 24 GHz radars: up to three targets seen from above with trails and speed arrows, a table of the raw numbers, a 30 s graph of distance and speed and the sensor's state, from 480x320 up, portrait (PDA mode) layout. Supports the HLK-LD2452 (and the LD2450, the same frame) and the HLK-LD2451, on a USB-serial adapter, or the LD2451 over Bluetooth LE (the package recommends `bluez`). Listen only: it sends nothing to the sensor. The LD2410 is only recognised, not supported. Category Tools. |
| [**Net Toolbox**](apps/net-toolbox/README.md) (`net-toolbox`) | 0.5.1 | arm64, amd64 | The tools you reach for when the network misbehaves: a step-by-step diagnosis, ping, traceroute, DNS lookup, port check, HTTP check, public IP, whois and the interfaces, a speed test with a bufferbloat grade, Wake-on-LAN, a Bluetooth radar, a local device scanner, Wi-Fi networks with their channels and a Wi-Fi walk test. It replaces LAN Scan and Wi-Fi Survey (unpublished on 2026-11-02). Category Tools. |
| [**Net Walk**](apps/net-walk/README.md) (`net-walk`) | 0.1.0 | arm64, amd64 | A network-wiring puzzle, by touch or keyboard: turn the tiles until the server's power reaches every computer. Four sizes (5x5, 7x7, 9x9 and a 9x9 board whose edges wrap), padlocks, best moves and times per size, drawn in a Palm OS colour look, from 480x320 up, portrait (PDA mode) layout. Category Games, no network. |
| [**RSS Reader**](apps/rss-reader/README.md) (`rss-reader`) | 0.2.1 | arm64, amd64 | A reader of RSS and Atom text feeds: pin your favorite news, tech and science feeds from a catalog of about 260 by country and topic, or add any site by its address, then read the articles full screen with their pictures, and save them to read offline, by touch or keyboard, on any screen from 640x480 up. Category Internet. |
| [**Serial Console**](apps/serial-console/README.md) (`serial-console`) | 0.6.1 | arm64, amd64 | Finds your USB-serial adapters and boards, tells what is plugged in and what is behind it, recommends the settings, and talks to it. Category Tools. |
| [**System Monitor**](apps/system-monitor/README.md) (`system-monitor`) | 0.3.1 | arm64, amd64 | A live look at the board or PC it runs on: CPU, temperature, memory, storage, network, processes, and the power supply or battery. Category Tools. |
| [**Vinyl**](apps/vinyl/README.md) (`vinyl`) | 0.1.0 | arm64, amd64 | A music player for the music in `~/Music` and on USB drives: folder by folder or from M3U playlists, gapless, shuffle, repeat, embedded cover art and resume where you stopped, with a record that slides out of its sleeve and turns while it plays. MP3, FLAC, Ogg Vorbis and Opus, WAV and AIFF, and through FFmpeg (installed with it) AAC, ALAC, WMA, APE and more. Plays on while you use other apps; wired outputs, and Bluetooth headsets and speakers on boards with a sound server; the volume of the playing output in steps fitted to it; headset and keyboard play keys; drag the seek bar to a point. From 480x320 up: 480x320, 640x480, square, portrait (PDA mode) and two-pane layouts. Category Tools. |
| [**viz1090**](apps/viz1090/README.md) (`viz1090`) | 0.3.2 | arm64, amd64 | The real viz1090 ADS-B display, full screen on any supported screen, fed by an RTL-SDR dongle, with an intro screen, the app header (Esc) and a world map, upright on portrait screens (PDA mode). Needs the dongle and a launcher with full-screen app support. Category Radio. |

`arm64` packages run on the Raspberry Pi boards (the decks). The apps marked `amd64` also have a package for x86-64 PCs
running the launcher, listed in their own registry ([amd64](#amd64-x86-64-pcs)).

An upgrade through the Apps menu keeps the app's tile where it was on the launcher grid (launcher v0.4.0 and later), as
long as the `.desktop` file name does not change.

## Install apps from this repository on a deck

On the deck: **Settings > Apps > Sources > + Add a GitHub source**, enter `OSRdesign/cyberdeck-zero-apps`, then
go to the **Apps** tab, pick an app and press Enter to install. Installing asks for the deck user's sudo password
(a `.deb` package is installed as root). Remove an app from the same menu.

(The Store app can use the same repository: add the registry URL
`https://raw.githubusercontent.com/OSRdesign/cyberdeck-zero-apps/main/registry.json` in its settings.)

On an x86-64 PC (amd64) running the launcher, the apps built for it are in a registry of their own,
`https://raw.githubusercontent.com/OSRdesign/cyberdeck-zero-apps/main/registry-amd64.json` (see [amd64](#amd64-x86-64-pcs)).

---

# Host your own apps

An app source is just a **public GitHub repository** that contains:

```
registry.json                 the catalogue the deck reads (generated by tools/make_registry.py)
packages/<name>_<ver>_arm64.deb   one Debian package per app version
apps/<id>/app.json            what you write: name, description, version, ...
apps/<id>/icon.png            optional icon, 100x100 square transparent PNG (see docs/ICON-PALETTE.md)
apps/<id>/screenshots/*.png   optional screenshots (PNG, the real screen size: 480x320, 320x480, 640x480 and 800x480 are in use);
                              the registry lists every PNG in sorted file-name order and the Store shows them in that order
apps/<id>/root/...            the files of the app, laid out as they must be on the device
tools/                        make_registry.py, build_deb.py, check_registry.py, check_descriptors.py, screen_info.py,
                              test_registry_tools.py, branding/, icons/ (pure Python 3; see tools/README.md)

and, for apps also built for x86-64 PCs (optional, see amd64 below):
registry-amd64.json           the catalogue of the amd64 packages
packages/<name>_<ver>_amd64.deb
apps/<id>/root.amd64/...      the amd64 files (the binary) that replace those of root/ in the amd64 package
```

Each registry entry also carries the app's screen requirements, copied from its packaged `.desktop` by
`tools/make_registry.py`, so that the launcher shows only the apps that fit the board's screen:
`"min_size": "WxH"` (from `X-MinSize`, the smallest landscape screen) and `"portrait_min_size": "WxH"` (from `X-Portrait`,
the smallest canvas on a portrait launcher), the second only when declared. An app without `X-MinSize` has no field and is
listed everywhere. From launcher v0.9.2, Settings > Apps > Available leaves out an app whose `min_size` does not fit
the screen (a portrait screen is judged turned, as the app would run, so orientation hides nothing; installed apps stay
listed), and an app's page shows the rows Screen (Landscape, or Landscape and portrait) and Minimum size.
`python3 tools/check_registry.py` checks the registries against the `.desktop` files and
`python3 tools/test_registry_tools.py` is the generator's self test; a plain `make_registry.py` run keeps the dates and
`device_targets` of unchanged entries.

The deck downloads `registry.json`, lists the apps in it, and installs the `.deb` an entry points to after
checking its MD5. The format is the registry format (schema 2) of M5Stack's CardputerZero Store, which the
launcher bundles, so a source you create here also works in the Store app.

## 1. Create your repository

1. On GitHub, click **Use this template / Fork** on this repository (or copy its `tools/` folder into a new
   public repository). The repository must be **public** and its default branch must be **`main`**.
2. Delete the apps you do not want (`apps/*`; keep `apps/_common` and `apps/cyberdeck-app-template` if you build native
   apps), `packages/*` (this repository holds 17 app folders and about 120 packages) and `docs/screenshots/`, then clear
   `registry.json` and `registry-amd64.json` (`"apps": []`) and replace the table of apps above. Or keep a small app as an example.

## 2. Build your program for the deck

Your app is a normal **aarch64 Linux executable**. **New apps are native full-screen apps** built from
[`apps/cyberdeck-app-template`](apps/cyberdeck-app-template/README.md) on the launcher's LVGL based `cp0_lvgl`
runtime: they fill the whole screen they are given and adapt their layout to it (640x480 is the deck's reference size,
not a minimum; 480x320 and 320x480 are normal sizes), and take keys and touch directly. An app declares `X-MinSize` in
its `.desktop` file only when its UI cannot fit a smaller screen. The [app guidelines](docs/APP-GUIDELINES.md) give the screen
sizes, the input rules, the packaging and the tests. Native apps build against one launcher commit,
[`apps/_common/LAUNCHER_REF`](apps/_common/LAUNCHER_REF) (today `28817a5`, launcher main after v0.9.4 with the map engine and its first fixes, off by default; Cassette 0.3.1, Deck Atlas 0.2.0, Mesh Hop 0.8.3 and mmWave Radar 0.1.1 are built at `28817a5`, the other published apps (Home Assistant, Klondike Solitaire, Markdown Notes, Mine Field, Net Toolbox, Net Walk, RSS Reader, Serial Console, System Monitor, viz1090) are still built at `3f6579f`, the SSH indicator commit of launcher v0.8.12); what each launcher version adds for
apps, and when it needs a rebuild: [launcher versions](docs/APP-GUIDELINES.md#launcher-versions-and-what-each-added-for-apps).

Wrapped (compat) apps, such as the first ones here ([LAN Scan](apps/lanscan/README.md),
[Wi-Fi Survey](apps/wifi-survey/README.md), unpublished on 2026-11-02 and kept as drafts), still work: such an app draws a fixed 320x170 picture into the
launcher's virtual framebuffer, and the launcher scales it into its window. Keys arrive as normal keyboard events
(arrows, Enter, Esc; hold Esc 3 s to quit), touch is turned into keys by the launcher (Settings > Touch picks the
behaviour per app). Do not start a new app this way.

A program that does not use the runtime works too: the launcher starts any executable. Terminal programs can use
`Terminal=true` in the `.desktop` file (below) and run in the launcher's full-screen terminal.

## 3. Describe the app

`apps/<id>/app.json`:

```json
{
  "share_code": "myapp",
  "package": "myapp",
  "version": "0.1.0",
  "title": "My App",
  "summary": "One line shown in the list (at most 80 characters).",
  "description": "What the app does, its main features and what it needs.",
  "categories": ["Tools"],
  "author": {"github": "your-github-name", "display_name": "Your Name"},
  "license": "MIT",
  "source_repo": "https://github.com/your-github-name/myapp",
  "depends": "",
  "permissions": {"network": true}
}
```

Every field, its rule and what the registry does with it (the descriptor limits: summary at most 80 characters, summary +
description at most 500; `depends` and `recommends` syntax and the run-time library policy; `permissions` keys;
`categories`; `arch`; `draft`; `min_size`; icon and screenshot rules): **[docs/APP-JSON.md](docs/APP-JSON.md)**.
`share_code` is the app's stable identity, keep it the same across versions, and `version` must grow for every release.

## 4. Lay out the files

Put the files of the app under `apps/<id>/root/`, exactly where they must end up on the device:

```
apps/myapp/root/usr/share/APPLaunch/bin/myapp                        <- your executable
apps/myapp/root/usr/share/APPLaunch/applications/myapp.desktop       <- the launcher entry
apps/myapp/root/usr/share/APPLaunch/share/images/myapp.png           <- the tile icon
```

`myapp.desktop` (file name = `share_code` + `.desktop`):

```ini
[Desktop Entry]
Name=My App
Exec=/usr/share/APPLaunch/bin/myapp
Terminal=false
Icon=share/images/myapp.png
Type=Application
```

The tile icon (and `apps/<id>/icon.png`) is a 100x100 square transparent PNG; the [icon system](docs/ICON-PALETTE.md)
gives the rules, the palette and the generator.

`Exec` must be an absolute path to an executable (no shell tricks). Name the executable after the app id
(`share_code`), with no prefix. (Older apps here still carry a prefix inherited from the upstream launcher; they
drop it at their next version.) `Icon` is relative to
`/usr/share/APPLaunch/`. Anything else your app needs (data files, fonts) goes in the same tree, under
`usr/share/APPLaunch/share/<your-app>/` to avoid clashing with other apps. Files under a `bin/` directory
become executable automatically.

## 5. Build the package and the registry

```
python3 tools/make_registry.py
```

This builds `packages/myapp_0.1.0_arm64.deb` from `apps/myapp/root/`, computes its MD5/SHA-256/size and writes
`registry.json` with raw.githubusercontent.com URLs (owner and repository are read from your git remote; pass
`--owner` / `--repo` if needed). It prints the registry address, for example
`https://raw.githubusercontent.com/you/your-apps/main/registry.json`. Apps that also list amd64 get their amd64
package and `registry-amd64.json` in the same run ([amd64](#amd64-x86-64-pcs)).

For a release of this repository use `--no-build` and the procedure of [docs/PUBLISHING.md](docs/PUBLISHING.md): the
pull request holds the sources, the packages of both architectures, **both** registries and the table row, and older
packages stay in `packages/`.

Check a package without installing it, on Linux: `dpkg-deb -I packages/myapp_0.1.0_arm64.deb` and
`dpkg-deb -c ...`.

## 6. Publish

```
git add -A
git commit -m "myapp 0.1.0"
git push
```

That is all: no GitHub Pages, no release needed. Packages above 50 MB are better attached to a **GitHub
Release**: upload the `.deb` there and set `download.url` of that entry to the release asset URL (edit
`registry.json` after generating it; GitHub refuses files above 100 MB in a repository).

## 7. Add your source on the deck

**Settings > Apps > Sources > + Add a GitHub source**, then type `your-github-name/your-apps` (a full
`https://github.com/you/your-apps` address or a direct `registry.json` URL also work). The deck syncs the
registry and your apps appear in the **Apps** tab of **Settings > Apps**.

To publish an update, raise `version` in `app.json`, run `tools/make_registry.py`, commit and push; the deck
shows the new version after **Sync my sources** (GitHub's cache can delay it by a few minutes). This repository
publishes the exact `.deb` that passed the board tests and writes its entry with `--no-build`
([PUBLISHING.md](docs/PUBLISHING.md), [app guidelines](docs/APP-GUIDELINES.md#5-packaging-and-registry), section 5).

## amd64 (x86-64 PCs)

The launcher also runs on x86-64 PCs (the Panasonic FZ-M1 tablet is the reference). An app is offered there when its
`app.json` lists `"arch": ["arm64", "amd64"]`, from one app folder and one version number:

* `apps/<id>/root/` keeps the arm64 files (the aarch64 binary); `apps/<id>/root.amd64/` holds what differs on amd64,
  at the same paths: `usr/share/APPLaunch/bin/<id>` (the x86-64 binary) and `usr/share/doc/<id>/build-info`. A native
  app writes both with `build.sh cross --arch amd64` (see [`apps/_common`](apps/_common/README.md#amd64)).
* `tools/make_registry.py` builds `packages/<pkg>_<ver>_amd64.deb` from `root/` with `root.amd64/` laid over it (a
  file of the overlay replaces the one of `root/`; an aarch64 binary left without its replacement stops the build)
  and writes `registry-amd64.json`, the same format as `registry.json`, listing only the apps that name amd64.
  `registry.json` (arm64) stays as it was. `--arch amd64` builds only the amd64 packages and `registry-amd64.json`;
  `--only <id> --arch amd64 --out <folder>` builds one local amd64 package and writes no registry.
* The binaries need glibc 2.38 or newer, as the arm64 ones (Debian 13, Ubuntu 24.04 and later).

## Good to know

* **Trust.** Installing a package runs it as root (`dpkg`), and an app runs with your user's rights. Only add
  sources you trust and read what you install. The MD5 in the registry protects against a corrupted
  download, not against a malicious author.
* **`review.status`.** The Store installs only entries marked `approved`. `make_registry.py` sets it for you,
  because you are the reviewer of your own repository. What the other registry fields mean:
  [PUBLISHING.md](docs/PUBLISHING.md#what-the-registry-entry-holds).
* **Updates need a new version number.** The deck compares versions; re-uploading a `.deb` with the same
  version is not seen as an update.
* **Architecture.** Packages are `arm64` (aarch64) unless the app also lists amd64 ([amd64](#amd64-x86-64-pcs));
  a binary built for another CPU installs but does not run.
* **Several sources** can be enabled at once; an app with the same `share_code` in two sources is shown once
  (the source listed last wins).

## Third-party and run-time libraries

The libraries the apps use, none of them part of this repository. **linked** = a shared library the binary needs at
start (named in `depends`); **dlopen** = opened by the app at run time, only when the feature needs it (also named in
`depends` or `recommends`, so the Store installs it); **built in** = compiled into the app or shipped in its package.
System libraries are not shipped: `apt` installs them.

| Library | Licence | How | Apps |
| --- | --- | --- | --- |
| libfreetype6 | FreeType License or GPL-2.0-or-later | linked | every native app |
| libinput10 | MIT | linked | every native app |
| libxkbcommon0 | MIT | linked | every native app |
| libasound2 (ALSA) | LGPL-2.1-or-later | dlopen | Cassette, Vinyl |
| libFLAC | BSD-3-Clause | dlopen | Cassette, Vinyl |
| libopus | BSD-3-Clause | dlopen | Cassette, Vinyl |
| libopusenc | BSD-3-Clause | dlopen | Cassette |
| libpulse (PulseAudio and PipeWire client) | LGPL-2.1-or-later | dlopen, only with a sound server (Bluetooth audio) | Cassette, Vinyl |
| libmpg123 | LGPL-2.1 | dlopen | Vinyl |
| libvorbisfile, libvorbis, libogg | BSD-3-Clause | dlopen | Vinyl |
| FFmpeg (libavformat, libavcodec, libavutil, libswresample) | GPL-2.0-or-later as Debian and Raspberry Pi OS build it (LGPL-2.1-or-later upstream) | dlopen, only when a file needs it (AAC, ALAC, WMA, APE...) | Vinyl |
| libsqlite3 | public domain | dlopen | Deck Atlas |
| libcurl4t64 | curl license (MIT style) | dlopen | Deck Atlas, Home Assistant |
| libsystemd (sd-bus) | LGPL-2.1-or-later | dlopen, only for Bluetooth | Mesh Hop, mmWave Radar |
| libsdl2, libsdl2-ttf, libsdl2-gfx | zlib | linked | viz1090 |
| librtlsdr | GPL-2.0-or-later | linked | viz1090 |
| libusb-1.0 | LGPL-2.1-or-later | linked | viz1090 |
| libncurses6, zlib1g | X11 (MIT style), zlib | linked | viz1090 |
| libzstd | BSD-3-Clause or GPL-2.0 | linked | viz1090 |
| LVGL, the `cp0_lvgl` toolkit | MIT | built in | every native app |
| stb_image 2.30 | public domain or MIT | built in | RSS Reader, Deck Atlas, Vinyl |
| md4c 0.6.0 | MIT | built in | Markdown Notes |
| Nayuki QR Code generator 1.8.0 | MIT | built in | Home Assistant |
| Material Design Icons 7.4.47 | Apache-2.0 | shipped (webfont) | Home Assistant |
| Caveat SemiBold | SIL OFL 1.1 | built in (the cassette label) | Cassette |

Programs an app runs, which are not libraries: `curl` (RSS Reader, Net Toolbox, Mesh Hop), `python3` (Mesh Hop, viz1090),
`network-manager` (Net Toolbox), `bluez` (recommended by Mesh Hop and mmWave Radar). The launcher's fonts (Montserrat and
JetBrains Mono, SIL OFL 1.1) are read from the launcher at run time and not shipped.

A library an app opens at run time has to be named in that app's `copyright` file and README
([PUBLISHING.md](docs/PUBLISHING.md#1-before-you-start)). Cassette's `copyright` file names its run-time
and system libraries from 0.3.1 on (0.3.0 and older packages name only libasound). The licences of libFLAC, libopus, libopusenc,
libpulse and libasound were checked in the projects' own repositories (their COPYING and LICENSE files); the others are
from general knowledge of the projects and have not been re-checked.

## Licence and credits

The repository is MIT. Each app page has a "Credits and licences" section with its own credits and licences in full. The
list below gives, in short, the apps with third-party material that needs a credit; Klondike Solitaire, Mine Field, Net
Walk, Serial Console and System Monitor have none beyond the libraries of the
[table](#third-party-and-run-time-libraries) above.

* **Deck Atlas** shows map data (&copy; OpenStreetMap contributors, ODbL) and, when you switch the sources on, the styles
  of OpenTopoMap (CC-BY-SA, SRTM elevation), Thunderforest and Geoapify (OpenMapTiles), each credited on the map; imported
  maps are your own, under their own terms. The OpenStreetMap Foundation's tile servers are never used. stb_image (public
  domain or MIT) is inside its map engine; SQLite and libcurl come from the system, loaded at run time. See
  [its page](apps/deck-atlas/README.md).
* **mmWave Radar** is written from Hi-Link's published protocol documents (no code of other radar libraries) and opens
  `libsystemd` (sd-bus, LGPL-2.1-or-later) from the system at run time, only for Bluetooth; see
  [its page](apps/mmwave-radar/README.md).

* **Home Assistant** ships Material Design Icons 7.4.47 by Pictogrammers (Apache-2.0: the webfont unchanged, its licence
  in `/usr/share/doc/home-assistant/LICENSE.mdi`) and vendors Project Nayuki's QR Code generator library 1.8.0 (MIT,
  unchanged); no code of Home Assistant's frontend is used; see [its page](apps/home-assistant/README.md).
* **RSS Reader** decodes pictures with stb_image v2.30 (Sean Barrett and the stb contributors, public domain or MIT);
  see [its page](apps/rss-reader/README.md).
* **Net Toolbox** ships a shortened list of vendor names from the IEEE OUI registry (<https://standards-oui.ieee.org/>);
  see [its page](apps/net-toolbox/README.md).
* **LAN Scan** and **Wi-Fi Survey** (unpublished on 2026-11-02, kept as drafts) use the IEEE OUI registry (vendor names) published at
  <https://standards-oui.ieee.org/>. Both run on the LVGL based `cp0_lvgl` runtime, from the CardputerZero launcher by M5Stack
  (MIT).
* **viz1090** bundles viz1090 (BSD), readsb (GPL-3 or later), fonts (SIL OFL 1.1, Font Awesome icons CC BY 4.0) and
  map data (Natural Earth, OurAirports, public domain); see [its page](apps/viz1090/README.md).
* **Cassette** has its own code (MIT) on the LVGL based `cp0_lvgl` runtime (MIT); the handwriting on the cassette label is Caveat SemiBold (SIL OFL 1.1, compiled into the binary, licence in the package's copyright file). It opens libasound, libFLAC, libopus and libopusenc from the system at run time and ships none of them; see [its page](apps/cassette/README.md).
* **Vinyl** has its own code (MIT) on the LVGL based `cp0_lvgl` runtime (MIT), its sound-server and ALSA layers and the FLAC / Opus readers from this repository's Cassette (same author, MIT), and stb_image 2.30 (public domain or MIT) for embedded cover pictures. It opens libmpg123, libvorbisfile, libFLAC, libopus, libasound, libpulse and FFmpeg from the system at run time, never ships them; see [its page](apps/vinyl/README.md).
* **Markdown Notes** parses Markdown with md4c v0.6.0 by Martin Mitas (MIT, vendored unchanged with its licence) and runs on
  the LVGL based `cp0_lvgl` runtime (MIT); see [its page](apps/markdown-notes/README.md).
* **Mesh Hop** speaks the MeshCore protocol (MeshCore firmware and protocol, MIT; meshcore_py, MIT, as the
  reference of the wire format). Other MeshCore clients (two MIT, two GPL) were feature references only, no code was copied from
  the GPL ones. Its CLI page bundles, unmodified, meshcore-cli (MIT) and its libraries meshcore_py (MIT), pycayennelpp (MIT),
  pyserial-asyncio-fast (BSD-3-Clause), pycryptodome (BSD-2-Clause and Unlicense), prompt-toolkit (BSD-3-Clause) and wcwidth
  (MIT), each with its licence file in the package; see [its page](apps/mesh-hop/README.md).
