# Vinyl (`vinyl`)

A music player for CyberdeckZero Launcher boards: it plays the music in `~/Music` and on USB drives, folder by folder or
from M3U playlists, with gapless playback, shuffle, repeat, embedded cover art and resume where you stopped. It plays on
while you use other apps (on boards with multitasking), to wired outputs and, on boards with a sound server, to
Bluetooth headsets and speakers; headset, speaker and keyboard play keys work. Version **0.1.0** (phase P1 of the launcher
repo's report 059, the MVP: folder first, no library database yet).

Kind: a native `cp0_lvgl` app (kind A of the guidelines): its own screens, touch and keys. The design is the owner-approved
drafts in [`docs/design`](docs/design) (the `.dc.html` files): a cream panel in Cassette's family with its own teal accent,
the record sliding out of its square sleeve and turning while it plays.

## Screenshots

Headless harness, demonstration world (`VINYL_DEMO=1`):

| | |
|---|---|
| ![now playing](screenshots/01-now-playing-480x320.png) | ![browser](screenshots/02-browser-480x320.png) |
| ![queue](screenshots/03-queue-480x320.png) | ![output](screenshots/04-output-480x320.png) |
| ![portrait](screenshots/05-now-playing-320x480.png) | ![disconnected](screenshots/08-output-lost-480x320.png) |
| ![muted](screenshots/10-muted-480x320.png) | ![dragging the seek bar](screenshots/11-scrub-480x320.png) |

The deck (640x480), square (480x480) and large (800x480, two panes):
![deck](screenshots/09-now-playing-640x480.png) ![square](screenshots/06-now-playing-480x480.png)
![large](screenshots/07-now-playing-800x480.png)

## What it does

| Page | What |
|---|---|
| **Browser** (the root) | `~/Music` and the mounted drives (`/media/<user>`, `/run/media/<user>`, `/mnt`): folders A-Z, playlists (`.m3u`, `.m3u8`), tracks A-Z (natural order) with their title tags and lengths; the mini player at the bottom. A track plays its folder as the queue; a playlist plays its entries. |
| **Now playing** | cover art (embedded JPEG or PNG, else the drawn sleeve) with the record sliding out (its label about a third out, with a dot and a centre ring that show it turning), title (one line that scrolls, or two lines on the 640x480, square and large screens), artist, album and year, the format and Gapless as information (one line at 480x320, flat chips at 640x480, plain text elsewhere), the output (a full-width button with the device's name and a chevron; a chip on 800x480 and wider), the volume group ("-", the level on teal, "+"); the position with a seek bar (a tap seeks, a drag scrubs), previous, play / pause, next, shuffle, repeat (off, all, one: a teal mark, and a "1" for one); the queue key (480x320) or the Browse and Queue keys of the bottom row; up next (square), the queue pane (large). An empty queue dims the transport keys, which then do nothing (the Queue key stays: it opens the empty queue). |
| **Queue** | the play order from the playing track, "3 OF 20, 1 H 24 MIN LEFT"; Move up, Move down, Remove, Clear all (asks first: "Clear the queue?", Cancel, the default, or Clear; it keeps the playing track). An empty queue dims the four buttons; with only the playing track, Move up, Move down and Clear all are dimmed. No dragging in the lists (the seek bar is the one control that drags). |
| **Output** | Automatic (the system's default output), the Bluetooth devices (music or call quality), the wired outputs, a paired device that is off (dimmed). Vinyl never switches output by itself: when the output in use goes away, playback pauses ("Evolve 65 disconnected. Paused."). |

The top band is the launcher's: the toolkit's header with the title (none at 320x480, the toolkit's narrow portrait), the
Esc and the shared status strip. No page has a busy state: nothing waits in front of the user (tags, covers and the
output lists are read in the background).

### Keys and touch

| Function | Key | Touch |
|---|---|---|
| Play / pause | Space (Enter on now playing) | the teal key; the mini player's key |
| Previous / next track | Left / Right on now playing; `,` / `.` on every page; the media keys | the keys beside play; the mini player's next key |
| Scan back / ahead (muted, 3x) | hold Left / Right on now playing (or `,` / `.`, or a media key) | hold the previous / next key |
| Seek | Shift+Left / Shift+Right: back / forward 10 s (now playing) | tap the seek bar where to go, or press on it and drag: the knob follows the finger, the times show where it will land, and the seek happens when you let go (playing or paused; nothing else starts from a press on the bar) |
| Shuffle / repeat | S / R | the keys left and right of the transport |
| Queue, Output, Browser | Q, O, B | the Queue key; the output button (the only way to Output by touch on every layout but 800x480 and wider, which have the output chip and the Output key); Browse (or Esc from now playing) |
| Lists: move, act, back | Up / Down, Enter; Right acts as Enter and Left goes back (queue, output; in the browser Right opens a folder) | browser: tap a row; queue: tap selects, tap again plays; output: tap chooses |
| Folder up | Left or Backspace (browser) | the ".." row |
| Queue pane (800x480 and wider) | Up / Down pick a track, Enter plays it | tap picks, tap again plays |
| Queue editing | U move up, D move down, Delete or X remove, C clear all (then Y clears; Enter or Esc cancels, as the dialog says) | the four buttons; Clear all asks "Clear the queue?" (the toolkit's dialog: Cancel, Clear, or the header's Esc; a tap outside does nothing). A button that cannot act is dimmed and does nothing |
| Volume | `-` / `=`: the volume of the output Vinyl plays on, one of its levels a press (about 2 dB) | "-" and "+" of the volume group, the level between them (teal): in the bottom row (640x480, square, portrait), under the output button (480x320), where the Equalizer key was (800x480). It sets and shows the playing output's real level as a percent of that output's levels; muted (by anything), it shows "Muted" with the speaker-off mark, "-" does nothing and "+" brings the sound back at its level; "-" at the first level mutes. "--" when the level is not known; where nothing can change it a tap shows "Volume is not available on this board" |
| Close a notice | Esc (it also goes back as Esc always does: at the browser Vinyl quits) | tap it (it also goes after 6 s, 8 s for a disconnect) |
| Back; at the browser, quit | Esc | the header's Esc |
| Headset, speaker, keyboard media keys | Play/Pause, PlayCD, PauseCD (a toggle), Next, Previous (as Right / Left above, held too), Stop (pause and back to the start) | |

Previous and next act when the key is released, the same from every path (the transport press, owner ruling
2026-11-12): next plays the next track (at the end of the queue with repeat off it stays on the last one; repeat all and
repeat one wrap to the first); previous goes back to the start of the track when more than 2 s of it have played, else
to the previous track (the first track starts again). Held for half a second they scan through the track at three times
its speed with the sound muted; on release the sound comes back from there, playing or paused as before. A forward scan
goes on into the next track; a backward one stops at 0:00. Left / Right on now playing are the one exception to the
Left = back, Right = Enter rule of the other pages. The volume group and `-` / `=` set the output Vinyl plays on; the
launcher's volume, mute and brightness keys stay the launcher's and act on the system's default output, so when Vinyl
plays on the default output the group follows them (with a sound server, within about half a second). A media key seen
twice within 250 ms (the same button on two input nodes) acts once.

## Formats

| Tier | Formats | Library (loaded at run time) |
|---|---|---|
| 1 (always) | MP3 (gapless with the LAME header), FLAC (16/24/32 bit kept), Ogg Vorbis, Ogg Opus, WAV (PCM, float, RF64), AIFF / AIFC | libmpg123, libvorbisfile, libFLAC, libopus (package Depends); WAV/AIFF: own reader |
| 2 (FFmpeg, installed with Vinyl) | AAC / ALAC / M4A, WMA, APE, WavPack, TTA, Musepack, DSF (as PCM), Ogg FLAC, trackers | libavformat, libavcodec, libswresample, libavutil 61 (Debian 13, Raspberry Pi OS) or 62 (Ubuntu 26.04): package **Depends** (decision D20), loaded only when such a file is opened, never at start |

Every board plays the common formats out of the box (the owner's decision D20, 2026-11-13). FFmpeg costs about 22.5 MB
of memory once loaded (report 059, 14.6), nothing before. On a Lite image the FFmpeg libraries and what they need are
about 104 MB of disk with `apt install --no-install-recommends`; the launcher's installer today also installs their own
recommends, about 462 MB (a launcher change to pass `--no-install-recommends` is proposed separately). If the libraries
are there but cannot be loaded, a tier-2 file shows "The FFmpeg libraries could not be loaded. Reinstall Vinyl." and the
next track plays.

Gapless: the next track continues on the same stream when its format (rate, channels, sample size) is the same; another
format drains the stream and opens a new one (a short gap). Tags come from Vinyl's own readers (ID3v2.2-2.4 and v1, FLAC
and Ogg comments, MP4 items, WAV INFO, AIFF), never from a process per file; text is UTF-8 and Chinese, Japanese and
Korean titles draw with the launcher's Noto Sans CJK.

## Outputs, Bluetooth and volume

- **With a sound server** (PipeWire or PulseAudio: the HackBerry, the Pi 5 boards, the tablet): every output of the server,
  opened by name with DONT_MOVE (Cassette's layer), so the server never moves the music elsewhere. Automatic plays to the
  server's default output, looked up when playback starts and at each track change (never in the middle of a track).
  Bluetooth headsets and speakers are listed with their quality; one A2DP device at a time on the HackBerry, a second one
  gets call quality ("Call quality (16 kHz): the headset is in call mode." while it plays). A Bluetooth profile switch
  (the node re-created) is followed on the same device; a device that goes away pauses playback.
- **Without one** (the Lite boards: the deck, zero8, Pi 3A+): ALSA `default` (Automatic) and the cards with a playback
  device (through `plughw`, never `hw:`); the output page says "Bluetooth audio is not set up on this board."
- **Volume** (decision D11 as the owner changed it, 2026-11-12; the levels as the owner decided on 2026-11-13): the
  volume group and `-` / `=` set the volume of the output Vinyl plays on and show its real level. Each output gets its
  own levels from its real range: the top 60 dB of it at most (a sensible usable range, after Dr. Lex's
  [volume control article](https://www.dr-lex.be/info-stuff/implement-a-volume-control.html)), cut in levels about 2 dB
  apart, linear in dB: level 1 is the bottom of that window (it sounds), the last level the output's own top (100 %), 0
  is mute; the group shows the level as a percent of the output's levels, so 50 % is the middle of that output's range
  and a press moves one level, not 5 %. The same output (the same DAC or card model) gets the same levels on every
  board; different outputs differ, as their hardware does.

  | Output | Range | Window | Levels | dB a level | 50 % |
  |---|---|---|---|---|---|
  | Sharkoon USB DAC (`PCM`, 1 dB steps) | -45..0 dB | -45..0 dB | 23 | 2.05 | 52 % = -22.5 dB (the middle) |
  | Raspberry Pi headphone jack (`PCM`, 0.01 dB steps, mute at min) | -102.39..+4.00 dB | -56..+4 dB | 30 | 2.07 | -27.0 dB |
  | Apple USB-C adapter (`Headset`, 0.5 dB steps) | -60..0 dB | -60..0 dB | 30 | 2.07 | -31.0 dB |
  | a sink with no known range (Bluetooth, software sinks) | | -60..0 dB | 30 | 2.07 | -31.0 dB |

  Without a sound server it is the card's playback mixer control (`Master`, `PCM`, `Speaker`, `Headphone`,
  `Headset`...), set by raw value and read back fresh; the control's switch mutes. A control whose lowest step is "mute"
  (the Pi jack's) has that step read as Muted, and its dB range is taken from the step above (ALSA reports the mute step
  as -99999.99 dB); a control without dB data gets its own raw steps (at most 30). With a sound server it is the sink's
  own volume, set as the server's dB (`pa_sw_volume_from_dB`) whether it is the default sink or not; the window is the
  range of the ALSA card behind the sink (found from the sink's properties), placed under the sink's 0 dB as the server
  maps it, so a DAC whose floor is -45 dB never gets a level below it; a sink with no card (a Bluetooth headset) gets
  -60..0 dB. A level changed elsewhere (a headset's buttons, the launcher's keys on the default sink, alsamixer) shows as
  the nearest level within about half a second. Each set is logged with what the output holds after it
  (`volume: set <output> step i/N P % target X dB raw=M read back raw=K`). Only a card with no playback mixer gets
  Vinyl's software volume, over -60..0 dB. Vinyl's stream itself always plays at 100 %, with its own restore key, so a
  server never brings back another app's stream level. A muted output shows "Muted" in the group, whoever muted it; the
  sound server's placeholder output at start (`auto_null`) never shows a level.

## Planned settings

Not in 0.1.0 (the owner's roadmap, 2026-11-13; `docs/decisions.md`, "Later (not built)"), in the Playback settings of the
Menu key (P2):

- **Gain boost**: 0 to +4 dB of software gain in 1 dB steps, with a limiter against clipping, for outputs that are quiet
  even at their maximum (the Raspberry Pi headphone jack beside a USB DAC).
- **Volume steps**: Normal (about 2 dB, the percent shown, as now) / 1 dB / 0.5 dB / 0.1 dB; in the dB modes the volume
  group shows the output's real level in dB ("-22.5 dB"), as some hi-fi amplifiers do.

## Background playback

`.desktop` `X-Background=run`: on a launcher with multitasking (canvases 480 px and more on the short side) Super, Alt+Tab
or the three-finger hold hide Vinyl and the music plays on; nothing is drawn while hidden. Below 480 px (zero8 in
landscape or portrait) the app owns the panel and Esc at the browser ends playback. The queue and the position are saved
as they change, so a close by the launcher (memory) resumes paused at the same place. Where the launcher answers `dim`
(board.state `requests`: the battery boards with backlight levels), Vinyl asks it to let the idle panel dim while it
plays in front (`dim allow`, decision D16).

## Requirements

- A screen of 480x320 or more (`X-MinSize=480x320`): the smallest approved draft is 480x320, where the sleeve beside the
  title, artist, format line, output button and volume group and a row of six transport keys already take the whole
  width; a smaller screen gets the launcher's notice. A portrait layout for 320x480 and taller (`X-Portrait=320x480`).
- The layouts (owner-approved drafts): 480x320 (and landscape screens of that shape under 480 px high or under 640 px
  wide), 640x480 (4:3 screens 640 to 799 px wide and 480 px or more high: the deck), square (480x480, 720x720),
  portrait, and two panes from 800x480 (800x480, 1024x600, and the 4:3 screens from 800 wide and 480 high: 800x600,
  1024x768). Where a layout has less room than the 7 mm
  touch size (the deck's 640x480 at 11.3 px/mm: 70 px rows; a 3.5-inch 480x320: 43 px for the volume group and the seek
  row), the touch areas share the room without overlapping (owner rule R-4).
- CyberdeckZero Launcher with the app contract; built at the shared pin (`apps/_common/LAUNCHER_REF`).
- Packages (Debian 13 names, all Depends): libmpg123-0t64, libvorbisfile3, libflac14, libopus0, libasound2t64,
  libpulse0, libavformat61, libavcodec61, libavutil59, libswresample5 (Ubuntu 26.04: libavformat62, libavcodec62,
  libavutil60, libswresample6).
- Reading the media keys needs read access to `/dev/input/event*` (the `input` group, as the launcher's user has).

## Install

Settings > Apps > Available > Vinyl, or by hand: `sudo apt install ./vinyl_0.1.0_arm64.deb` (apt installs the
dependencies), `sudo dpkg -r vinyl` to remove it.

## Files the app keeps

| Path | What |
|---|---|
| `~/.config/vinyl/state.json` | the queue, its order, shuffle, repeat, the position, the output, the browser's folder, the software level of an output without a mixer |

Delete it to start fresh. Vinyl writes nothing else (it never writes tags or music files).

## Known limits (0.1.0)

- Folder browsing only: no library database, no search, no artist / album views (P2).
- No EQ, ReplayGain, crossfade or sleep timer: the drafts' sleep-timer key is drawn dimmed (the Equalizer key of 800x480
  gave its place to the volume group), and the "Gapless" chip shows without the ReplayGain figure (P2). The Menu key (tools
  and settings) is P2 too: the 640x480 bottom row and the 480x320 transport row leave it out until then. No sorting
  control in the browser (folders and tracks are A-Z).
- No metadata to headsets or car displays (MPRIS / AVRCP, P2); headset keys work when BlueZ hands them to an input node.
- A cover larger than 1200 px on its long side is not decoded (the memory cap on the Pi Zero 2 W): the drawn sleeve shows.
- FFmpeg seeks land on the packet at or before the position; the AAC length is the container's.
- The amd64 package targets Ubuntu 26.04, where FFmpeg is libavformat 62 (FFmpeg 8): Vinyl's FFmpeg 62 structure
  offsets were checked against the FFmpeg 8 headers (`tests/abi`), not yet played on the tablet.
- MIDI, native DSD, SACD images, tag editing and video are out of scope (decision D14).

## Credits and licences

Vinyl is MIT (`2026 OSRdesign`). Linked into the binary: cp0_lvgl and LVGL (MIT), stb_image 2.30 (Sean Barrett and the
stb contributors, public domain / MIT; `third_party/stb`). Loaded at run time from the system, not shipped: libmpg123
(LGPL-2.1), libvorbisfile / libvorbis / libogg, libFLAC and libopus (BSD-3-Clause), libasound and libpulse
(LGPL-2.1-or-later), FFmpeg (libavformat, libavcodec, libavutil, libswresample: GPL-2.0-or-later as Debian and Raspberry
Pi OS build it; source https://ffmpeg.org/ and the Debian source package `ffmpeg`). Fonts: the launcher's Montserrat,
JetBrains Mono and Noto Sans CJK (SIL OFL 1.1) and DejaVu Sans, read at run time. The sound-server and ALSA layers and
the FLAC / Opus readers come from this repository's Cassette (same author, MIT). Full text:
`root/usr/share/doc/vinyl/copyright`.

## Build and test

```sh
wsl -e sh apps/vinyl/tests/run.sh               # the unit tests (-Werror) and the real-library fixtures
wsl -e sh apps/vinyl/build.sh host              # harness, unit tests, 49 scenes (qa* for the design QA), sheets
wsl -e sh apps/vinyl/build.sh cross             # the arm64 binary; --arch amd64 for PCs
wsl -e sh apps/vinyl/tests/abi/run.sh [root]    # the hand-written FFmpeg / mpg123 / vorbisfile ABI against headers
python3 tools/make_registry.py --only vinyl --out <folder>    # a local .deb (not published)
python apps/vinyl/tools/make_icon.py            # the icon
wsl -e sh apps/vinyl/tools/mixer-probe/build.sh # vinyl-mixer-probe (arm64, amd64): a card's mixer as Vinyl sees and sets it
```

`vinyl-mixer-probe hw:CARD=<id> [--set PCT] [--selem-raw PCT] [--selem-db PCT] [--loop N] [--keep]` (on a board, Vinyl
stopped) lists the card's controls as Vinyl reads them and as alsa-lib's simple mixer (amixer) reports them, Vinyl's
choice and level, and with `--set` writes a level the way Vinyl does and reads the control back; the value from before
is put back at the end unless `--keep`. It is a board-probe tool, not part of the package.

Board protocol: [`tests/BOARD-TESTS.md`](tests/BOARD-TESTS.md).

## Changes

- **0.1.0** (not published yet): the P1 MVP of report 059 (decisions D1-D18), with the design-QA rounds: the transport
  press (owner ruling 2026-11-12), the volume group on every layout with the output button (owner, 2026-11-13), the
  approved 640x480 and 480x320 layouts, the record's label with its spin marks, Muted shown as muted, Clear all asks,
  the Raspberry Pi headphone jack's volume, Left / Right on the queue and output pages, the output rows' action dot,
  the title as the toolkit places it, the icon inside its margin.
