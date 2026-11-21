# VHS (`vhs`)

A camcorder for CyberdeckZero Launcher boards and USB cameras: it shows the live picture of the camera (the whole frame,
never cropped), takes photos, records movies with sound from the camera's own microphone or any sound card, lists them as scenes, plays them back and
copies them to a USB drive. It was made for the UGREEN FineCam; the Logitech C270 is tested too. A photo is the camera's
own JPEG with an EXIF block; a movie is the camera's MJPEG frames copied as they come (no re-encode) with AAC sound, in an
MKV file. A self-timer (Off, 3, 5 or 10 s) works for photos and for video. Version **0.1.2**, published 2026-11-21 (phase P1 of the launcher repo's report 062, with
the Square layout, the camera chooser, the sound input chooser, the Exposure row and the 8 MP Photo size). Kind: a native `cp0_lvgl` app (kind A of the guidelines): its own screens,
touch and keys; the owner's words for it: "basically VHS is like cassette for videos (+ it can take photos)", so it
shares Cassette's naming, journal and repair, and Vinyl's drives and sound output.

## Screenshots

Board screenshots are added at the board round.

## What it does

| Page | What |
|---|---|
| **Capture** (the root) | the live picture inside its box: the whole frame, letterboxed or pillarboxed where the shapes differ, decoded at a JPEG scale and fitted in one pass. A chip with the size and rate ("WxH 30 fps"); the **Photo / Video** toggle (Tab), the shutter (Space), **Scenes** (with the number of scenes) and **Settings**. The preview follows the mode: Video mode streams the Video size (default 1280x720), Photo mode the Photo size (the largest mode the camera lists: 1920x1080 on the FineCam, 1280x960 on the C270) with **Instant**, or a lighter stream with **Light** (see Settings). The camera is open only while this page shows. |
| **Camera** (0.1.2) | VHS lists every usable capture camera (a USB camera with a video capture node; the metadata node and the second node of a device are skipped; MJPEG or YUYV). Each is known by a stable id (vendor, product and serial, else the USB port; a node that is not USB by its sysfs device path), never by `/dev/videoN`, and shown by its name; two cameras of one name are told apart by "(front)" / "(back)" when the kernel says where each looks, else the USB port ("(USB 9.3)"), else "#1", "#2". **Settings > Camera** lists them (a tap selects, a second tap chooses). With two or more cameras a chip with the camera's name sits at the picture's top right while the live view runs; a tap, or **C**, switches to the next camera (the old one is closed, the new one opened; nothing switches while recording or counting down: a toast says so). The choice is remembered by id. A remembered camera that is not plugged in gives way to the first one, with two or more present, and a toast says so ("<name> is not plugged in: using <name>."); with one camera that camera is used and remembered silently, as before. The Photo and Video sizes are kept per camera (a camera with none stored takes its best mode). |
| **Exposure** (0.1.2) | In Photo mode, with a camera that can do manual exposure (V4L2 `auto_exposure` with Manual Mode and a writable `exposure_time_absolute`: the tablet's back camera, not its front one), a row **[-] Exposure Auto [+]** sits over the picture's bottom while the live view runs: **-2, -1, Auto, +1, +2** stops around what the camera's automatic exposure picks (0 is Auto: the least confusing set). A UVC camera in automatic mode does not report the exposure it chose, so the stops are made in software: when you leave Auto, the mean luma of the live picture is measured once (L0); the camera goes to Manual and its shutter is driven by a small feedback loop (at most 6 steps, about a second or two) until the picture's luma is within 8 % of L0 x 2^stop (kept in 8..245); the base is not measured again while a stop is on. The preview follows live, a photo uses the same shutter (written again after the stream is switched to the Photo size, so it is as bright as the preview), and the setting stays through Photo / Video, the Photo size switch and a reopen; it is per camera and per run: another camera opening, or closing VHS, brings Auto (the camera is always left on Aperture Priority). In Video mode the row is not shown; the format chip says "EV +1" while a stop is on. A camera without these controls (no row) behaves as before. The preview never uses a shutter longer than 66 ms (about 15 frames a second), whatever the stop: what the stop asks beyond it the preview makes up with the camera's gain control when it has one (else it is as bright as the cap allows), so it stays smooth but is an approximation (about one stop beyond the cap at most); the **photo** uses the true, longer shutter, written when it is taken (a second or two longer at +2), and the preview's setting comes back at once. |
| **Photo** | Space or the shutter. Instant takes the next good frame of the stream; Light switches the stream to the Photo size for the shot and back. The file is the camera's JPEG with an EXIF block (make and model from the camera's USB names, orientation 1, Software "VHS 0.1.2", the pixel size, and the date only when the clock is synchronised) and with the standard Huffman tables added only when a frame has none. Photo size (0.1.2): **Settings > Photo size lists every MJPEG size the camera offers, at any frame rate**, labelled with its megapixels from 3 MP up ("3264x2448 (8 MP)"; the tablet's back camera offers 3264x2448, 2592x1944 and 2048x1536 at 15 fps); the default stays the largest at 30 fps. With Light (the tablet's default) the stream is switched to that size for the shot (in place, controls kept, the exposure written again) and back; with Instant the preview itself runs at that size and rate. A size the camera refuses when asked (a STREAMON error such as ENOSPC, the USB bandwidth shared with the other camera, or EBUSY or no memory) falls back to the next smaller size at once and the toast says so ("3264x2448 not available now: photo taken at 2592x1944."); the photo is never lost and the preview comes back. **Focus** (0.1.2): a camera with continuous autofocus (`focus_automatic_continuous`) gets time to settle: the photo is taken once the frames' sizes (a JPEG's size follows its sharpness) stop changing, at most 1.5 s after the first frame that would do; an already settled stream (Instant) loses no time, a camera with fixed focus never waits, and there is no manual focus. The file is `photo_<number>_<date and time>.jpg` in `~/VHS`. A camera that sends no MJPEG gets "Photos need a camera that sends MJPEG." |
| **Self-timer** | Settings > Self-timer: Off, 3, 5 or 10 s, for photos and for video. A big countdown runs over the live picture ("Photo in 3 s", "Video in 10 s"); a tap on the picture, Cancel, Esc or the Stop key cancels it and nothing is taken; no beep. At zero a video starts recording and a photo flashes once. |
| **Recording** | R (either mode), Space in Video mode or the red button starts a scene; the same stops it and asks nothing. MJPEG copy plus sound as AAC in MKV (**Settings > Sound input**, below), the sound as the clock the picture is matched to. From the press until the first frame is written the red dot of the REC tag blinks; then the tag shows the running time, the **Sound on** lamp lights while the microphone delivers a signal above a noise floor (no lamp without a microphone), and a line shows the size written and the time left on the disk. **Photo now** takes the current recording frame. Scenes and Settings are dimmed while recording. **Sound input** (0.1.2): Automatic (the default) uses the camera's own microphone when it has one (found by its USB parent, opened as `hw:`, the level left as found), else the first ALSA capture card, else no sound; or choose any capture card (USB, HDA and built-in, as Cassette lists them; remembered by name, by-id and USB id, never by index) or **No sound**. A card is opened as Cassette opens one (`hw:`, its own rate, mono or stereo, 16-bit; a card with only 24 or 32-bit formats is cut to 16 bits); a card that refuses what it lists is opened as `plughw:` at 48 kHz stereo; a card a sound server holds is reached through the server's `default` device. A card you chose that cannot be opened is shown ("Sound input <name> failed: <reason>.") and nothing is recorded; Automatic never blocks a recording. A microphone that is busy or missing records video only, with a notice; 3 s of exact silence says "No sound from the camera microphone." once per scene. A scene is cut into parts at 4 GiB (`_p2`, `_p3`...; Settings > Split long scenes: Off for one file); the disk guard stops it when less than 500 MB is free, or less than 2 minutes at the rate written, with the red notice "Disk almost full: recording stopped.". A journal is kept from the first byte, and a scene cut short by a crash or a power loss is remuxed up to the cut at the next start (the row says "repaired"; tested on the PC with truncated files, the power pull on a board is board test B31). |
| **Camera states** | no camera: "No camera found" with "Plug the camera into a USB port. VHS shows the picture as soon as it appears." (VHS looks again every second; Scenes stays); unplugged: "Camera unplugged.", the last frame stays dimmed for up to 5 s and then the NoCamera page shows; the camera's switch moved: "Camera turned. Restarting..." and the picture back upright at the sizes the camera now offers; busy: "Camera busy: another program uses it."; no access: "No access to the camera: add your user to the video group."; weak supply on a Raspberry Pi: "Power too weak for the camera." (at most once a minute). A recording ends at an unplug; it never stops for the supply. VHS never resets the USB bus. |
| **Scenes** | one list of movies and photos, newest first, with the filter All / Movies / Photos (Tab): a thumbnail, the name, "12:34 - 1.4 GB - MJPEG 720p" or "1920x1080 - 98 KB", the date and a "locked" mark. Each auto-split part is a row of its own. Any `.mkv`, `.mp4`, `.mov`, `.jpg` or `.jpeg` in the folder is listed; a name outside VHS's grammar cannot be renamed. A **1.5 s hold** on a row (or Ctrl+O) opens the options sheet. |
| **Options sheet** | **Play** (Enter: a movie opens playback, a photo the review), **Rename** (N: the prefix of a movie, `scene` by default; a photo keeps the name `photo`), **Lock** (L; "Unlock" on a locked file), **Copy to a USB drive** (U), **Delete** (Del: asks first, "Y = Delete, Enter or Esc = Cancel"; refused on a locked file; removes the thumbnail, the journal and the lock entry with it). |
| **Copy to a USB drive** | One drive: the copy starts at once; several: a list ("Copy to which drive?"); a drive that is not mounted is mounted on request through udisks2 (see Requirements; unmounted at exit when VHS mounted it, never one the system mounted). The file is written as `.<name>.part` in `<drive>/VHS/`, flushed and renamed, so it appears on the drive only when the copy is complete; the dialog shows the bytes and the time left; Cancel deletes the `.part` and, when the copy made the `VHS/` folder and it is still empty, that folder too. Refused: not enough room, a file of 4 GiB or more to a FAT drive, a name already on the drive ("Not copied: a file of that name is on the drive."). The copy goes on while VHS is hidden. |
| **Photo review** | the photo fitted whole in the box, "n of N" (the photos of the filter), Previous and Next (Left / Right), Delete (asks first), Esc back to Scenes. |
| **Playback** (windowed) | the movie in its box, **Pause / Play**, the seek bar (a tap seeks, a drag scrubs and seeks once on release), the times, "-" and "+" with "Volume N%", and the scene's name. The sound is Vinyl's output stack: Settings > Playback output (Automatic, Bluetooth with a sound server, the wired cards), a volume per output, the first use of an output found silent starting near 50 % ("Volume 50% (was off)"). The picture follows the sound: its clock is steered toward the heard position, and a picture at most two frame periods late is still shown. MJPEG scenes (VHS's own) decode through TurboJPEG at the box scale; other codecs through FFmpeg in software, with the picture's colours converted by VHS's own code. A movie pauses on its last frame. "This video cannot be played." for a file that does not open or has no usable decoder or pixel format. After 2 s of dropped frames: "This board drops frames at this size.". Esc stops playback and goes back to Scenes. Where the launcher takes `dim`, the screen is kept lit while playing or recording. |
| **Settings** | the toolkit's list: **Camera** (0.1.2), Folder (the current folder or `~/VHS`), Name prefix, **Photo** (Instant / Light), Photo size, Video size (the MJPEG 30 fps sizes the camera lists, shown with "MJPEG"), Self-timer, Split long scenes, **Sound input** (0.1.2), Playback output. Rows that change the stream apply when the camera opens again: opening Settings releases the camera and leaving it reopens it. Every key is in [`docs/SETTINGS.md`](docs/SETTINGS.md). |

The top band is the launcher's: the toolkit's header with the title, the Esc and the shared status strip; REC, the lamp and
the countdown live in the body. The canvas draws 480x320 and 320x480; the other screens (640x480, 800x480, taller
portrait) use the same layout rules. The Square class (the HackBerry's 720x720, and 480x480, 640x640 and the like) has its
own layout, drawn at 720x720 and scaled by the panel (k = side / 720): the picture across the panel (688x440 at 720), then
one control row 120 px high (Photo and Video stacked at the left, the chosen mode with an amber ring; the shutter in the
middle; Scenes with its count and Settings stacked at the right; while recording, "Photo now" takes the left column) and a
40 px status line (the camera and its format at the left, the key hints at the right). The type there is 20 px at the
smallest (3 mm at 6.7 px/mm), 24 px for body text, 26 px on buttons and 28 to 34 px for names and the REC time; touch
targets are 56 px at 720 and never under the launcher's TARGET. Below 720 the sizes shrink in proportion (never under
12 px). Scenes, the options sheet, playback, the photo review, the dialogs and Settings follow the same scale; details in
[`docs/decisions.md`](docs/decisions.md) (2026-11-17).

### Keys and touch

| Function | Key | Touch |
|---|---|---|
| Capture: photo, or start / stop in Video mode | Space | the shutter |
| Capture: start / stop a recording | R | the red button |
| Capture: Photo / Video | Tab | the Photo / Video toggle |
| Capture: next camera (two or more cameras) | C | the camera chip at the picture's top right |
| Capture: exposure a stop darker / brighter / Auto (Photo mode, a camera with manual exposure) | - / + (or =) / 0 | the Exposure row over the picture's bottom: [-], the label (a tap brings Auto back), [+] |
| Capture: Settings | S | Settings |
| Capture: cancel the countdown | Esc, the Stop key | a tap on the picture, Cancel |
| Scenes: move, open | Up / Down, Enter | tap selects a row, tap on the selected row opens it |
| Scenes: filter | Tab (All, Movies, Photos) | the chips |
| Scenes: options sheet | Ctrl+O | hold a row 1.5 s |
| Sheet and rows: act | Enter (Play), N (Rename), L (Lock), U (Copy), Del (Delete); the letters also work on the selected row without the sheet; Up / Down move in the sheet | tap an action |
| Delete question | Y deletes, Enter or Esc cancels | the dialog's buttons |
| Copy: pick a drive, cancel | Up / Down and Enter pick; Esc closes the list; during the copy Enter or Esc press Cancel | tap a drive; tap outside closes the list; the Cancel button (a tap outside the copy dialog does nothing) |
| Photo review | Left / Right previous / next, Del deletes | the Previous, Next and Delete buttons |
| Playback: pause / play | Space; a headset's, speaker's or keyboard's play, pause and stop keys | Pause / Play |
| Playback: seek | Left / Right 5 s, Shift+Left / Shift+Right 30 s | tap the seek bar, or press and drag |
| Playback: volume | | "-" and "+" (the launcher's own volume keys stay the launcher's) |
| Settings | the toolkit's list takes its keys itself; Esc closes a pick list, then the page | a tap selects a row, a tap on the selected row opens it; a choice in a pick list applies and closes it |
| Back; at the root, quit | Esc | the header's Esc |

Esc while recording finishes the scene, then quits. Esc held 3 s ends the app (the launcher's rule); the scene is finished
first. Zoom (Up / Down, F) is not in 0.1.2; + and - move the exposure in Photo mode on a camera that has it (they were reserved for zoom: the
owner decides which wins when zoom comes).

## Requirements

- A UVC camera that sends **MJPEG** at 30 fps. VHS lists only the modes the camera reports and never asks for one it does
  not list; a camera with no MJPEG shows its live picture (YUYV) and nothing else (see Known limits). Cameras are found by
  capability, not by name; the FineCam (UGREEN CM969) is the supported one and the Logitech C270 is tested.
- A screen of any size: the `.desktop` has no `X-MinSize`; `X-Portrait=320x480` gives the portrait layout.
- CyberdeckZero Launcher with the app contract; built at the shared pin (`apps/_common/LAUNCHER_REF`). Permissions asked
  (`app.json`): camera, microphone, additional hardware; no network. Your user needs the `video` group for the camera.
- Packages (Debian 13 names, all Depends): libstdc++6, libgcc-s1, libfreetype6, libinput10, libxkbcommon0, libturbojpeg0
  (Ubuntu: libturbojpeg), libasound2t64, libpulse0, libavformat61, libavcodec61, libavutil59, libswresample5 (Ubuntu
  26.04: libavformat62, libavcodec62, libavutil60, libswresample6). TurboJPEG, alsa-lib, libpulse and FFmpeg are opened at
  run time; FFmpeg only at the first recording, playback or scene scan, never at start.
- `udisks2` is a run-time need for USB drives, not a dependency: without it a copy says "Drives need the udisks2
  package."; mounting needs a session allowed to mount (the launcher's polkit rule, as for Vinyl).
- Reading the media keys needs read access to `/dev/input/event*` (the `input` group, as the launcher's user has).
- Boards: RPI52 (Pi 5), a Pi 4B with the FineCam and with the C270, palmpi (Pi 3A+) and zero8 (Zero 2 W, 320x480).
  Builds of 0.1.0 have been tried on some of them; the board round has not been run, except the Square tests B33-B38,
  passed on the HackBerry with 0.1.1 on 2026-11-17, and the 0.1.2 tests B39-B42, B45, B46 and B48 passed on the
  Panasonic FZ-M1 (amd64, two built-in USB cameras) on 2026-11-21 (see
  [`tests/BOARD-TESTS.md`](tests/BOARD-TESTS.md), "Findings so far").

## Install

Settings > Apps > Available > VHS, or by hand: `sudo apt install ./vhs_0.1.2_arm64.deb` (apt installs
the dependencies), `sudo dpkg -r vhs` to remove it.

## Files the app keeps

| Path | What |
|---|---|
| `~/VHS/` (mode 0700; Settings > Folder) | the scenes and photos: `scene_014_20261114-185200.mkv`, parts `scene_014_20261114-185200_p2.mkv`, photos `photo_013_20261114-184710.jpg`; `nodate` in place of the date when the clock is not synchronised. One counter numbers scenes and photos and never repeats a number after a delete |
| `~/VHS/.counter`, `.locked`, `.journal/<stem>.json` | the last number handed out; the locked names; a recording's journal (open while recording, closed after its trailer, repaired or damaged after a repair) |
| `~/.config/vhs/settings.json` | the Settings rows and the hidden keys (`split_mib`, `min_free_mb`, `camera_id`, `camera_name`, `camera_sizes`); written atomically and only when a value changed, read tolerantly |
| `~/.config/vhs/output-levels` | the volume state per playback output |
| `~/.cache/vhs/index.json`, `~/.cache/vhs/thumbs/` | the scenes' details and the row thumbnails; rebuilt when missing, a thumbnail deleted with its file |

Every key and file is described in [`docs/SETTINGS.md`](docs/SETTINGS.md). Delete the config and the cache to start
fresh; the scenes and photos in `~/VHS` stay until you delete them. The app writes nothing else on the board; a copy
writes to the drive you chose.

## Background

`.desktop` `X-Background=run` (the owner's answer to question 1, 2026-11-15: "finish and save the scene, hide with the camera
released, come back live"). On a launcher with multitasking (canvases 480 px and more on the short side) Super, Alt+Tab or
the three-finger hold hide VHS and:

1. a countdown is cancelled;
2. a recording is stopped and finished (the trailer, the journal closed), so the scene is saved and playable;
3. the camera and the microphone are released;
4. playback is paused;
5. the preview stops; a copy to a USB drive goes on.

When VHS is shown again the camera opens and warms up (about 15 frames, half a second). Nothing records while VHS is
hidden. Why `run`: with `freeze` the writer would be stopped right after the acknowledgement, in the middle of the file's
trailer; with no `X-Background` three fingers would freeze VHS mid-scene. Below 480 px on the short side (zero8) there is
no multitasking: VHS owns the panel and Esc at the root quits. Where the launcher answers `dim`, VHS asks it not to dim
the panel while it plays or records and lets it dim again at the stop.

## Privacy

The recordings are private: the app has no network code, sends nothing anywhere and keeps the scenes and photos in the
folder you chose (`~/VHS`). The camera and the microphone are open only while the Capture page shows (and, for the
microphone, while a scene is recorded); Scenes, Settings, playback and a hide release them. The EXIF block of a photo holds
the camera's make and model, the program's name, the pixel size and the date; no location. The test fixtures of the
repository are synthetic (generated frames and tones); no real recording is part of it.

## Known limits (0.1.2)

- Settings rows take two taps: a tap selects a row and a tap on the selected row opens it (the toolkit's list rule; the
  owner's ruling, 2026-11-16).
- **Recording is MJPEG copy only**, and MJPEG is large: 720p comes to about 7.5 GB per hour on RPI52 (a moving indoor
  scene), 12.6 GB on zero8, 7.2 GB on the Pi 3A+ and 5.7 GB on the HackBerry; 1080p about 14.3 GB per hour (RPI52, moving
  scene). A quiet scene is far smaller, real footage varies by several times, and VHS shows the size it writes and the
  time left. A scene is cut at 4 GiB by default (about 17 minutes of 1080p, 20-34 minutes of 720p in a moving scene).
  There is no H.264 recording and no full-screen playback until P2, and no camera controls but the exposure row (no focus, white balance or gain), no microphone controls, no zoom, no
  codec choice and no Bluetooth microphone (the sound input chooser lists ALSA capture cards only).
- A camera that sends **YUYV only** gives a live view and nothing else: no photos ("Photos need a camera that sends
  MJPEG.") and no recording ("Recording needs a camera that sends MJPEG.").
- **The dwc_otg limit** (the Pi 3A+ and the Zero 2 W): flipping the FineCam's switch while it is live does not make the
  camera enumerate again on those boards. Replug the camera or restart the board; the NoCamera page says so: "If you moved
  the camera's switch, unplug and replug it, or restart the board." On the xhci boards (RPI52, Pi 4B) the flip is
  expected to show the Turned overlay instead (board test B3, not run yet). VHS never unbinds a driver or resets the USB
  bus.
- **The C270**: the camera lowers its own frame rate to 15 fps in indoor light unless its exposure priority control is
  off. VHS sets `exposure_auto_priority` to 0 whenever a 30 fps mode streams and puts the value back when it closes (best
  effort), so the C270 keeps 30 fps (the journal line `controls: exposure_auto_priority was N, set 0`; board test B28
  checks it). The C270 has no 1080p mode: its Photo size is 1280x960 and its photo setting Light streams 1280x720.
- **Send to Notes** is not in 0.1.2 (the photo action of the plan's D21 is hidden until it is built; a candidate for P2).
- A **foreign H.264 or HEVC file** decodes in software (VHS's own scenes are MJPEG). The Zero class is expected to drop
  frames at 1080p, and after 2 s of drops the notice "This board drops frames at this size." shows; HEVC on the Zero 2 W
  and the Pi 3A+ was not measured. Hardware decode is P2.
- RPI52 has no wired audio output: playback there is silent unless a USB or Bluetooth output exists.
- Mounting a drive needs the `udisks2` package and a session allowed to mount (an ssh login is refused). A copy of 4 GiB or
  more to a FAT drive is refused (it is only possible with Split long scenes off).
- The hidden keys `split_mib`, `min_free_mb`, `camera_id`, `camera_name` and `camera_sizes` are changed by editing `settings.json`.
- **Cameras found**: UVC cameras (the `uvcvideo` driver, video index 0, or a `/dev/v4l/by-id/...-video-index0` link). A platform or ISP camera that is not `uvcvideo` (a Raspberry Pi CSI camera) is not listed: it sends raw formats, and VHS has never opened those nodes. The kernel's front / back hint (`V4L2_CID_CAMERA_ORIENTATION`) is read when a UVC camera exposes it; the FZ-M1's two cameras do not.

## Credits and licences

VHS is MIT (`2026 OSRdesign`). Linked into the binary: the cp0_lvgl runtime of the CardputerZero launcher by M5Stack (MIT)
and LVGL (MIT). Linked from the system, not shipped: libfreetype6 (FreeType License or GPL-2.0-or-later), libinput10 and
libxkbcommon0 (MIT), libstdc++6 and libgcc-s1 (GPL-3.0-or-later with the GCC Runtime Library Exception). Opened at run time
from the system, not shipped (the package depends on them): TurboJPEG, the API of libjpeg-turbo (IJG License, BSD-3-Clause
and zlib License), libasound (alsa-lib) and libpulse (LGPL-2.1-or-later), and FFmpeg's libavformat, libavcodec, libavutil
and libswresample (GPL-2.0-or-later as Debian and Raspberry Pi OS build it; source https://ffmpeg.org/ and the Debian
source package `ffmpeg`). Programs run when present: `udisksctl` (udisks2) for the drives and `vcgencmd` (Raspberry Pi
firmware tools) for the supply flags. Fonts: the launcher's, read at run time, none shipped. The camera code, the
conversions and the pages are the app's own; the sound output, drive and copy code come from this repository's Vinyl
0.3.0, the naming, journal, repair, ALSA capture and file helpers from Cassette (same author, MIT). Full text:
`root/usr/share/doc/vhs/copyright`.

## Build and test

```sh
wsl -e sh apps/vhs/tests/run.sh                         # the unit tests (-Werror, fake camera)
wsl -e sh apps/vhs/tests/run.sh SAN=address,undefined   # the same under the sanitizers
wsl -e sh apps/vhs/tests/run.sh SAN=thread              # the same under the thread sanitizer
wsl -e sh apps/vhs/tests/abi/run.sh [root]              # the hand-written FFmpeg ABI against the 61 and 62 header sets
wsl -e sh apps/vhs/build.sh host                        # harness, unit tests, the headless scenes (VHS_FAKE), sheets
wsl -e sh apps/vhs/build.sh cross                       # the arm64 binary; --arch amd64 for PCs
python tools/make_registry.py --only vhs --out <folder> # a local draft .deb (not published); --arch amd64 for PCs
```

`vhs --list`, `vhs --selftest` and `vhs --bench` run without a screen (the cameras and their modes, a self-test, a
capture benchmark); `VHS_STATS=1` makes the stats line come every second instead of every 10 s. Board protocol:
[`tests/BOARD-TESTS.md`](tests/BOARD-TESTS.md). Decisions and the owner's rulings: [`docs/decisions.md`](docs/decisions.md).

## Changes

- **0.1.2** (published 2026-11-21): the **camera chooser** (Settings > Camera, a camera chip on the live view with two or more
  cameras, the C key, the choice remembered by a stable id, a toast when the remembered camera is not plugged in, the
  photo and video sizes kept per camera) and the **sound input chooser** (Settings > Sound input: Automatic, any ALSA
  capture card including built-in ones, No sound; a built-in microphone at 44.1 or 48 kHz, mono or stereo, 16 or 32 bit,
  `plughw:` and the sound server's route; the camera's own microphone is still the Automatic choice). Made for the
  Panasonic FZ-M1, whose two built-in cameras have no microphone. In Photo mode an **Exposure row** (-2, -1, Auto, +1, +2
  stops, software metering, only on cameras with manual exposure; the preview's shutter is capped at 66 ms with the gain
  making up the rest so it stays smooth, the photo gets the true shutter), the **8 MP Photo size** (every MJPEG size in
  Settings > Photo size, with a fallback to a smaller one when the camera refuses a size) and the **focus wait** before a
  photo on autofocus cameras. One camera and the camera's own microphone behave as in 0.1.1. Board tests B39-B42, B45,
  B46 and B48 passed on the FZ-M1.

- **0.1.1** (2026-11-17): the Square layout for large square screens (the HackBerry's 720x720, and 480x480, 640x640
  and the like): the picture across the panel, one control row of 56 px buttons and a status line, type 20 px at the
  smallest at 720, and Scenes, the options sheet, playback, the photo review, the dialogs and Settings at the same scale
  (board tests B33-B38 passed on the HackBerry). Nothing changes on the other screens.
- **0.1.0** (2026-11-16): the first release, phase P1 of report 062. The live view of the whole frame, photos
  (Instant and Light, EXIF), the self-timer, MJPEG movies with the camera's microphone in MKV (auto-split, journal and
  repair, the disk guard, the Sound-on lamp), the Scenes list with its options sheet, copy to a USB drive, photo review,
  windowed playback on Vinyl's output stack, Settings, and the hide that finishes and saves a recording
  (`X-Background=run`).
