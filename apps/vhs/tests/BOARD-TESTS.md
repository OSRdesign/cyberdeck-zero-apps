# VHS: board tests (0.1.2, about 25 minutes per board; B39-B44 are the 0.1.2 choosers)

Controller-led: the Controller installs the draft `.deb` and reads the launcher's journal (the `[vhs]` lines); the owner
runs the steps and says what was seen. What the PC cannot tell: the real camera, the real microphone, the sound outputs,
the USB bus, the SD card, the supply and how the keys and the touch feel. The core (the camera states, the modes, the
recorder, the journal and repair, the scenes, the copy job, the presenter and the clock) and every page run on the PC with
a fake camera (`wsl -e sh apps/vhs/build.sh host`, the scenes of `tests/scenes`).

Numbers: B1-B12 keep the plan's P1 numbers (report 062, section 13), adjusted to the P0 probes and the spike. B13-B24 are
reserved for P2 and P3 (plan 13.4 and 13.5). The tests new in P1 start at B25.

## Boards

| Short | Board | Screen and sound | What it is for |
|---|---|---|---|
| **R5** | RPI52 (Pi 5) | 800x480, PipeWire, no wired out | the fast board; a USB or Bluetooth output for playback |
| **4F** | Pi 4B with the FineCam | 800x480, ALSA only | the supported camera on a mid board |
| **4C** | Pi 4B with the Logitech C270 | as 4F | the second camera: no 1080p (largest mode 1280x960), its microphone is a separate card |
| **PP** | palmpi (Pi 3A+) | 800x480, good supply | a slow board; dwc_otg USB; it needs the good supply (a weak one gave under-voltage and a reboot with the camera) |
| **HB** | HackBerry | 720x720 square, 6.68 px/mm, touch and keyboard | the Square class: the type 20 px at the smallest, the 688x440 picture, the 56 px buttons (tests B33-B38) |
| **FZ** | Panasonic FZ-M1 (amd64, Armbian / Ubuntu 26.04, desktop launcher) | 1280x800, PipeWire; two built-in USB cameras with no microphone ("USB FHD Webcam", "USB2.0 Camera"), one capture card "HDA Intel PCH" | the 0.1.2 choosers (B39-B44) |
| **Z8** | zero8 (Zero 2 W) | 320x480 PDA | the slowest board; dwc_otg USB; the touch controller is sometimes dead after boot, so power-cycle it 10-15 s before the tests (keys work everywhere) |

## Preparation (Controller)

1. `sudo apt install ./vhs_0.1.2_arm64.deb` (`_amd64.deb` on the FZ) (the package; apt adds `libturbojpeg0`, which was absent on the Pi 4B,
   and the FFmpeg libraries). `/usr/share/APPLaunch/bin/vhs --list` prints the cameras, the chosen one's modes, the board's
   selection and the codec nodes; `--selftest` and `--bench` are the other headless commands.
2. The camera in the named board. The FineCam's switch in position B (landscape) unless a test says otherwise. A phone
   with a stopwatch for B12 and a clap and some talking for B6: the repository holds no real recordings of people, so
   film a stopwatch, a clap or a test card.
3. Logs: the launcher's journal (`journalctl --user -u <launcher service> -f`); VHS's lines start with `[vhs]`. The stats
   line comes every 10 s; started with `VHS_STATS=1` it comes every second. "The journal" in the table below means this
   log; a recording's own journal is `~/VHS/.journal/<stem>.json`.
4. R5 has no wired output: a USB or Bluetooth output for B9. For B27 a FAT32 USB stick. For B26 and B30 the hidden keys
   `split_mib` and `min_free_mb` in `~/.config/vhs/settings.json` (see `docs/SETTINGS.md`). For B32 an ssh login. On a PC:
   `ffprobe` for the frame counts, mpv or VLC for playing the scenes.

## Budgets the stats lines are read against (brief 14)

Measured with the spike's page statistics (MJPEG, 30 fps); the Pi 4B's numbers were not read there.

| Item | Pass |
|---|---|
| Preview, zero8, Instant (1080p, 1/4) | <= 0.75 core, >= 26 fps shown |
| Preview, zero8, 720p (while recording) | <= 0.45 core, >= 26 fps |
| Preview, palmpi, 1080p 1/2 | <= 0.80 core, >= 29 fps |
| Preview, RPI52 | <= 0.35 core, >= 29 fps |
| Camera to screen (zero8, 1080p) | mean <= 60 ms, p95 <= 90 ms |
| Recorder extra CPU (writer, microphone, AAC), zero8, 720p | <= 0.25 core |
| Whole process while recording, zero8 | <= 0.9 core of 4 |
| REC to the first written frame | <= 1.5 s with a mode switch, <= 0.3 s without |
| Photo | Instant <= 0.3 s, Light <= 2 s |
| VmRSS, zero8, recording | <= 100 MB |
| Playback, MJPEG 720p, zero8 | >= 28 fps, <= 0.5 core |
| Playback, MJPEG 1080p, zero8 | >= 25 fps, <= 0.8 core |
| Thermal | no throttling in 10 minutes of recording (palmpi, zero8) |

## Steps (owner)

| # | Do | Pass | Boards | Result |
|---|---|---|---|---|
| B1 | Open VHS with the camera plugged | live picture within 2 s, upright, the whole frame (no crop), smooth; stats line within the budgets above | all | not run |
| B2 | Unplug, wait 10 s, replug (not recording) | "Camera unplugged.", the NoCamera page, the picture back within 3 s of the replug | R5, 4F, Z8 | not run |
| B3 | Flip the camera's switch while live | xhci: the Turned overlay, portrait picture within about 3 s, Settings sizes now portrait. dwc_otg: **optional, owner's word, may need a reboot**: the NoCamera page with the replug/restart hint | R5, 4F; PP or Z8 optional | not run |
| B4 | Photo by Space and by the on-screen shutter | in Scenes; opens on a PC at 1920x1080 (C270: 1280x960); EXIF date right with the clock set | all | not run |
| B5 | Settings > Photo = Light, take a photo | under 2 s; preview resumes; then back to Instant | PP, Z8 | not run |
| B6 | Record 1 min (MJPEG 720p, camera mic), clap at the start, talk | plays in VHS and on a PC (mpv or VLC), clap in sync, Sound-on lamp lit while talking | all (4C: the mic on its separate card) | not run |
| B7 | Unplug the camera while recording | the scene ends at the unplug, plays to the cut; toast | 4F, Z8 | not run |
| B8 | Scenes: newest first, filter, rename, lock, try to delete the locked one, delete another (asks first) | as described; a locked scene cannot be deleted | PP, Z8 (portrait layout) | not run |
| B9 | Windowed playback: play, pause, seek by tap, drag and Left/Right; volume -/+ and volume keys | works; level shows; R5 plays on a USB or Bluetooth output, or shows the no-output notice | R5, 4F, Z8 | not run (re-test, see Findings) |
| B10 | Super, then three fingers, during playback and during a recording | playback paused; recording finished and playable; on return the camera reopens | R5, 4F, PP | not run |
| B11 | Every page at 800x480 and at 320x480 | nothing off-screen; header per the band rules | PP, Z8 | not run |
| B12 | Film a phone stopwatch next to the screen | delay under 0.3 s | Z8, R5 | not run |
| B25 | Self-timer 3 s photo, 10 s video; cancel once by tap, once by Esc | photo flashes at zero; video starts at zero; a cancel takes nothing | PP | not run |
| B26 | Controller sets hidden `split_mib` = 200; record 2 min at 1080p | parts `_p2`... in Scenes; each plays in VHS and on a PC; across a cut at most a frame or two missing | R5 | not run |
| B27 | USB stick (FAT32): copy a scene and a photo; start another copy and cancel it | progress shown; the cancelled file is absent on the stick; copied files play on a PC. No udisks2: the notice | R5, Z8 | not run |
| B28 | C270: Settings sizes; record 1 min of 720p in indoor light | no 1080p offered, Photo size 1280x960; the journal says `exposure_auto_priority ... set 0`; ffprobe frame count about 1800 (30 fps) | 4C | not run |
| B29 | FineCam in dim light, record 1 min of 720p | about 1800 frames; journal shows the control state (C14 probe on the FineCam) | 4F | not run |
| B30 | Optional: Controller sets `min_free_mb` to free space minus 200, record | stops with the red toast after about 200 MB; the scene plays | R5 | not run |
| B31 | Optional, owner's word: power pull during a recording | at the next start "repaired"; plays to within about 2 s of the cut | 4F | not run |
| B32 | Optional: `arecord -D hw:CARD=CM969 ...` running over ssh, then record | video-only scene, mic-busy toast | 4F | not run |
| B33 | HB: live view in Photo and in Video mode, from arm's length | the status line, the size chip, the buttons and the hint text read without leaning in (smallest text 3 mm); the picture fills the panel's width; no empty band under the status line; the chosen mode (Photo or Video) has the amber ring; "Scenes" shows the count of the scenes beside it | HB | passed on HB, 2026-11-17 |
| B34 | HB: tap each of Photo, Video, the shutter, Scenes and Settings with a thumb, then with a finger | every tap lands on the right button on the first try (each is at least 56 px, 8.4 mm); Video switches the mode and the chip, Photo switches back; the key hints in the status line match the keys (Space, Tab, R) | HB | passed on HB, 2026-11-17 |
| B35 | HB: record 20 s in each mode | the REC tag with the time (large, at the picture's top left), "Sound on" at the top right, "Photo now" in the left column, the red Stop in the middle, Scenes and Settings dimmed; the status line shows the size and written bytes at the left, the time left on the disk at the right; nothing overlaps the picture's corners | HB | passed on HB, 2026-11-17 |
| B36 | HB: Scenes with 7 or more scenes, hold a row 1.5 s, Play / Rename / Lock / Copy / Delete from the sheet, then playback (seek by tap and drag, Pause, volume - and +) and a photo review | no text under 3 mm or cut off; the chips, the count, the hint line and the sheet's key letters (dark on cream) read; rows scroll; the seek bar is easy to hit (56 px); every page keeps the header clear; nothing overlaps on any of these pages | HB | passed on HB, 2026-11-17 |
| B37 | HB: Settings (open a pick list, choose), the copy dialog with a USB stick, the self-timer countdown (3 s), a toast (unplug the camera), the NoCamera page | text 20 px or more everywhere; the countdown ring sits in the picture's middle; Cancel is a full button; the toast is readable over the picture and does not cover the control row; the NoCamera block is centred with its Scenes button | HB | passed on HB, 2026-11-17 |
| B38 | A 480x480 or 640x640 panel when one is at hand (or the host harness sheets in `tests/scenes/*-square.scene`: `build.sh host`) | the same pages, scaled: text proportional (13 px at 480 is the floor of the type), buttons never under TARGET, nothing cut or overlapping | any Square board | passed on HB, 2026-11-17 |

### 0.1.2: the camera and sound input choosers (owner)

| # | Do | Pass | Boards | Result |
|---|---|---|---|---|
| B39 | FZ: open VHS; a camera chip with a name shows at the picture's top right ("USB FHD Webcam" or "USB2.0 Camera"); tap it, then press C | the picture switches to the other camera within about 2 s, the chip shows its name, the status text says "C changes the camera."; tapping or C again returns; no freeze, no crash; in the journal the `[vhs] camera: switch to ...` and `streaming` lines | FZ | passed on FZ, 2026-11-21 |
| B40 | FZ: Settings > Camera (tap a row, tap again); choose the other camera; Esc | the list names both cameras with "USB 9.3" / "USB 9.4" beside them, the one in use marked; the live view opens on the chosen camera; close VHS (Esc) and open it again: the same camera is in use | FZ | passed on FZ, 2026-11-21 |
| B41 | FZ: Settings > Sound input; then record 20 s of speaking with Automatic, play it back in VHS and on a PC | the list has Automatic ("HDA Intel PCH" beside it), "HDA Intel PCH" ("Built-in"), "No sound"; the Sound on lamp lights while speaking; the scene has sound at a normal level, in sync; the journal line `recording: REC ..., sound PCH ...` shows the rate and channel count the card gave (also try the card chosen by name, and with another program recording from the microphone) | FZ | passed on FZ, 2026-11-21 |
| B42 | FZ: Sound input = No sound, record 10 s; then a scene started while recording: press C | silent scene, no toast; Automatic again: sound again; C while recording: "Stop the recording to change the camera." and the camera stays (the chip is not drawn) | FZ | passed on FZ, 2026-11-21 |
| B43 | Pi 4B or RPI52 with the one FineCam: open VHS, take a photo, record 20 s, Settings | no camera chip, no "C changes the camera." text, the camera's own microphone records as in 0.1.1 (the lamp lights, the sound is there), Settings shows Camera = the FineCam and Sound input = Automatic | 4F or R5 | not run |
| B44 | Pi 4B with the FineCam and the C270 both plugged in (two USB cameras on one board) | the chip shows from two cameras; each switch opens the other with its own best mode and microphone (the FineCam's CM969, the C270's own "USB Audio" card); the Photo and Video sizes set for one are not applied to the other; unplug the one that was chosen and reopen VHS: the first camera opens and the toast says the chosen one is not plugged in | 4F + 4C | not run |

### 0.1.2 draft 2 and 3: exposure, the 8 MP photo and the focus wait (owner, FZ)

| # | Do | Pass | Boards | Result |
|---|---|---|---|---|
| B45 | FZ: open VHS on the back camera ("USB2.0 Camera"; C or the chip switches to it), Photo mode; look for the Exposure row; tap **+** once, wait 2 s, tap **+** again, then **-** three times; then press 0; point the camera at a window and at a dark corner in between; take a photo at **-1** and one at **+1**; switch to the front camera ("USB FHD Webcam") with C | the row "[-] Exposure Auto [+]" shows over the picture's bottom on the back camera only (not on the front one); the label reads Auto, +1, +2, +1, Auto, -1, ... and the format chip adds "EV +1" etc.; the picture follows within about 1-2 s after each tap (brighter at +, darker at -, about one stop each; at +2 in a bright scene it clips, that is fine); the buttons are at least 9 mm and the row does not hide the subject badly; tapping the label returns to Auto at once; the photos at -1 and +1 are about as dark and as bright as the preview was, and +1 is clearly brighter than -1; the UI never freezes at the slow shutters (the frame rate may drop); the front camera shows no row and its picture is unchanged; coming back to the back camera shows Auto; Tab (Video) hides the row but the EV chip stays; after quitting VHS another app (or `v4l2-ctl -d /dev/video2 -C auto_exposure`) finds auto_exposure 3 | FZ | passed on FZ, 2026-11-21 |
| B46 | FZ: back camera, Settings > Photo size: look at the list; choose **3264x2448 (8 MP)** with Photo = Light (the FZ default), take a photo; then with Photo = Instant take one; open the photo in the review and on a PC; then plug nothing else but try the same with the front camera streaming in another window if you can (to starve the USB bandwidth) | the list offers every MJPEG size the camera lists (3264x2448 (8 MP), 2592x1944 (5 MP), 2048x1536 (3 MP), 1920x1080, 1600x1200 ...), the front camera its own; the photo file is 3264x2448, its EXIF has the size, and with an exposure stop set it is as bright as the preview; Light: the preview stays 1280x720 and the shutter takes 1.5-3 s (the switch to 15 fps, 15 warm-up frames), then the preview is back and the stop is still on; Instant: the preview runs at 3264x2448 15 fps (heavy but alive); if the camera refuses the size (the journal shows `refused ... trying a smaller size`) the toast says "3264x2448 not available now: photo taken at 2592x1944." (or the next size), the photo is saved at that size and the preview returns, never frozen | FZ | passed on FZ, 2026-11-21 |
| B47 | A camera with continuous autofocus (a phone-sensor UVC camera; the FineCam is fixed-focus, so any other camera with `v4l2-ctl --list-ctrls` showing `focus_automatic_continuous`): Photo mode, aim at something near, then at something far, and press Space at once; also hold a hand in front of the lens, take it away and press Space | the photo is sharp (not the hunting frame): the journal line `[vhs] photo: ... frame N taken ... ms after the press, focus settled after M ms` (M up to about 1500; `focus still moving after the wait` only when the autofocus never settled, the photo is taken anyway); with the focus already steady (Instant) M is near 0; on the FineCam and the FZ's two cameras (fixed focus) the line has no focus text and there is no wait; there is no focus control in the UI | any AF camera | not run |
| B48 | FZ: back camera in a dim room, Photo mode; leave Auto with **+** once, then again; watch the picture's smoothness (move your hand in front of the lens) and `v4l2-ctl -d /dev/video2 -C exposure_time_absolute,gain,auto_exposure` while it runs; take a photo at +2; compare it with the preview; then **-** to Auto | the preview stays smooth (about 15 frames a second, never the 2-5 of a 200-400 ms shutter), its shutter never over 660 and the gain goes above 64 (the journal: `exposure: luma L of T at 660 gain G ...; photo shutter V (gain calibrated)`); it gets brighter with each + but may stay dimmer than the photo will be at +2 (`at the end of the shutter cap and the gain`); the photo takes 1-2 s longer than at Auto, its brightness is about the target (a little darker than the target is the known limit), `exposure_time_absolute` shows the long value only around the shot and 660 or less again after it; at Auto the gain is back to its own value (64) and auto_exposure 3 | FZ | passed on FZ, 2026-11-21 |

## What to read in the journal

The lines VHS writes, by test. A frame count, a duration or a sync check on the PC is `ffprobe` or the player, not the log.

| # | Lines |
|---|---|
| B1, B11 | `[vhs] page WxH (...), target N, picture box WxH, ..., refresh N ms, camera ..., TurboJPEG ...` once at the start; `[vhs] camera: <device> (... id ..., bus ...) streaming <mode>, N buffers` at each open; `[vhs] stats state=... mode="..." scale="..." fmt=... cap=.. dec=.. shown=.. drop=dec:N,ui:N,cam:N ... decode_ms= convert_ms= ui_ms= render_ms= lat_ms= cpu= threads=cap:,dec:,ui: rss_mb= temp_c= throttled=0x.. uv=` every 10 s |
| B2, B3, B7 | `[vhs] camera: gone (<where>: <reason>)` at the loss; `[vhs] camera: <state>` lines (not-back, turned ...); then the `streaming` line with the mode the camera now offers (portrait after a turn) |
| B4, B5, B25 | `[vhs] photo: <mode> frame N taken M ms after the press` (Instant under 0.3 s, Light under 2 s) and `[vhs] photo: saved <path> (N bytes ...)`; `[vhs] self-timer:` lines |
| B6, B29, B32 | `[vhs] recording: REC <mode>, ...`; the start breakdown `[vhs] rec-start: N ms from REC to the first written frame; switch WxH -> WxH in place or by reopening: stream close, reopen, mode set, controls, buffers + STREAMON, first camera frame, warm-up 15 frames; mic open ... and writer open ...; first frame written N ms` (or `no switch (the Video size was streaming and warm)`); `[vhs] recorder: part N <path> (...)`; the microphone lines (`the camera microphone <device>: <reason>: video only`, `no sound card on the camera's USB device: video only`, `the microphone stopped (<reason>): video goes on`); `[vhs] recording: finished <name>: N frames, S s, P part(s), N bytes, N frames dropped (ring) ...` |
| B39, B40, B44 | `[vhs] camera: switch to <label> (<id>)`; the `streaming` line with the new device and id; `[vhs] camera: <id> is not plugged in, using <id>` for the fallback |
| B45 | `[vhs] controls: manual exposure 50..10000 step 1 (default 166, auto_exposure was 3), fixed focus` (or `no manual exposure, fixed focus` on the front camera); `[vhs] exposure: +1 (base luma L0, target T), first try V`, then `exposure: luma L of T at V -> V2` ... `, done`; `[vhs] exposure: Auto`; `[vhs] exposure: another camera opened, back to Auto`; `exposure: writing V failed (...)` and the toast "The camera did not take that exposure." when the camera refuses |
| B46 | `[vhs] camera: switched in place to MJPG 3264x2448 15 fps`; `[vhs] camera: MJPG 3264x2448 15 fps refused (<reason>): trying a smaller size`; `[vhs] photo: MJPG 3264x2448 15 fps frame N taken ... ms after the press`; `[vhs] photo: ... was refused, taken at ...` |
| B47 | `[vhs] photo: ... taken N ms after the press, focus settled after M ms` / `focus still moving after the wait`; the `controls:` line ends with `continuous autofocus` or `fixed focus` |
| B48 | `[vhs] exposure: +2 (base luma L0, target T), first try V, preview shutter cap 660`; `exposure: luma L of T at 660 gain G -> 660 gain G2` ...; `, done; photo shutter V (gain calibrated)` or `, at the end of the shutter cap and the gain: the preview is dimmer than the photo will be; photo shutter V` |
| B41, B42 | `[vhs] recording: REC <mode>, sound <card> [via the sound server ]<rate> Hz <n> ch (<device>), ...` or `no sound`; `the sound input <device>: <reason>: not recording` (a chosen card) or `video only` (Automatic); `sound input is No sound (chosen): video only` |
| B7 | `[vhs] camera: gone`, `[vhs] recording: stop (<reason>)`, `[vhs] recording: finished ...` |
| B8 | `[vhs] scenes: N file(s) listed (N in the metadata cache)`; `[vhs] scenes: rename ...`, `lock` / `unlock`, `delete <name>: done`; the toast "Not deleted: the file is locked." |
| B9 | `[vhs] playback: opened <name>`; `<name>: <codec> WxH, <duration>, N fps`; `sound on <output>` or `no sound (<reason>): the picture keeps the monotonic clock`; once a second `[vhs] playback: clock N ms (sound N, delay N ms; <mode>, rate R), shown pts N; last second: N shown, N not shown, N dropped late (before decoding); decode X ms, queue N`; `frames late for 2 s at ...` when the drops toast fires; `[vhs] playback: closed after S s: N frames decoded (X ms each), N shown, N dropped late, N not shown, C core; clock <mode>, N jump(s)`; the volume lines `[vhs] volume: ...` (the output found, `first use of <output>: it was ...; set to N % (step i/N)`, every set with its read-back) |
| B10 | `[vhs] recording: finished ...` at the hide; `[vhs] playback: closed after ...`; `[vhs] camera opened` on return; `[vhs] launcher: dim deny` / `dim allow` where the launcher takes them |
| B12 | none: read the delay on the phone's film |
| B26 | `[vhs] recorder: part 2 <path> ...`; `[vhs] recording: finished ...: P part(s) ...` |
| B27 | `[vhs] copy: <name> (<size>), N drive(s)`; `copy: to <mount> on <label> (<fstype>)`; `copy: <name> to <label> started`; `... done: <size> in N s` or `... cancelled` and `copy: cancelled, nothing on the drive`; `copy: ... not copied: <reason>`; `[vhs] drives: unmount <device> at exit: ok` |
| B28, B29 | `controls: exposure_auto_priority absent`, `was N, set 0`, `read-only (N)`, `inactive (was N)` or `was N, set failed`; `[vhs] recorder: the camera delivers N fps (under 27 for 5 s)` when the rate drops |
| B30 | `[vhs] recording: refused, N MB free (the floor is N MB)` before REC; `[vhs] recorder: disk guard: ..., stopping`; the red toast "Disk almost full: recording stopped." |
| B31 | `[vhs] repair at start: N open journal(s): N part(s) repaired, N damaged` |

## Findings so far

Only what was measured or seen on a board; the numbers are from the builds named.

- **REC to the first written frame** (budget: 1.5 s with a mode switch, 0.3 s without). G3 build, the mode switched by
  closing and reopening the camera: 2456 ms on 4F and 2583 ms on PP. G4 build, the mode switched in place: `rec-start`
  1066 ms and 1081 ms on PP. Since the owner's rule of 2026-11-15 (Video mode previews the Video size), REC in Video mode
  needs no switch: on the build box's fake camera 130 ms, and 644 ms from Photo mode; not yet measured on a board.
- **Playback skipped visibly** on R5 and 4C (G5 build). R5, a FineCam scene, MJPEG 1280x720, 24.3 s: "393 frames decoded
  (8.4 ms each), 337 dropped late, 192 not shown, 0.15 core", the sound on "Dummy Output". The cause was found at G6 (the
  picture's clock was read every 250 ms) and fixed; see `docs/decisions.md`, 2026-11-16. To re-test with B9 on the G7
  build, on R5 and 4C (4C is not in B9's board column).
- **A silent output** (G5 build, 4C): the card's 'PCM Playback Volume' was found at 0 % (muted, -102.39 dB) and VHS left
  it there. G6 adds the first-use rule (the output starts near 50 %, the volume line reads "Volume 50% (was off)"). To
  re-test with B9.
- **The Square layout** (0.1.1 build, HB): B33-B38 passed on the HackBerry, 2026-11-17 (the owner's run).
- **The 0.1.2 choosers, exposure and photo size** (0.1.2~draft.3 build, FZ, two built-in USB cameras): B39-B42, B45,
  B46 and B48 passed on the FZ-M1, 2026-11-21 (the owner's run). B43, B44 (Pi boards) and B47 (an autofocus camera) not
  run yet.
- Everything else: not run yet.

## Cleanup

Delete the test scenes and photos the Controller does not want kept (`~/VHS`, `~/.cache/vhs`); put back `split_mib` and
`min_free_mb` (remove the two keys from `~/.config/vhs/settings.json`); `sudo dpkg -r vhs` removes the package;
`rm -r ~/.config/vhs ~/.cache/vhs` starts fresh.
