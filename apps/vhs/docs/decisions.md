# VHS: decisions in force

The owner's rulings and the accepted points that shape VHS, newest last within each section. Where the owner spoke, the
owner's words are quoted exactly; everything else says who decided. The study behind the app is the launcher repo's
`docs/dev/reports/062-vhs-plan.md` ("plan n.n" below is a section of it; D1-D38 are its decisions). "Brief n" is a
section of the P1 build brief of 2026-11-15. The settings and the files the app keeps are in
[`SETTINGS.md`](SETTINGS.md).

## Name

- **2026-11-14: the name and id `vhs` ("VHS").** Plan D1 ("Name and id: VHS, id `vhs`", not in the launcher's built-in
  tile list), accepted with D1-D29 (below). No wording of the owner about the name is recorded here.

## Plan decisions D1-D29 (accepted 2026-11-14)

The owner answered the decision list of plan 15.3 on 2026-11-14 with: "go with your recommendations on VHS". D1 to D29
are accepted as recommended, each as worded in 15.3 (plan section 17). Plan section 17 names D1, D2, D3, D8, D15
(ruling R-7, below), D24, D26 and D28 in particular; no other wording of the owner is recorded for them. The lines
below are that list's wording, one per decision, without its "Rec: yes".

- **D1 Name and id:** VHS, id `vhs` (not in the built-in tile list). See "Name" above.
- **D2 Interpretation:** the points (a) to (h) at the top of the plan.
- **D3 One camera:** the CM969 only; the code finds cameras by capability, others "may work, unsupported"; more UVC,
  tablet and CSI cameras later.
- **D4 Capture:** own V4L2 code; no libv4l, GStreamer, libavdevice or the toolkit's camera service.
- **D5 Decode for the live view and MJPEG playback:** TurboJPEG (Depends `libturbojpeg0`) with DCT scaling; stb_image
  fallback; VideoCore hardware decode later if P0 shows the need.
- **D6 Photos on the Zero class and Pi 3A+:** full resolution by mode switch (about 1-1.5 s) plus a Quick photo
  setting; instant 1080p elsewhere and during recordings. **Revised by the owner's ruling of 2026-11-15: see "D6
  ruling" below.**
- **D7 Files:** one folder `~/VHS/` (setting), Cassette-style names (`scene_`, `photo_`), `nodate` until the clock is
  set, mode 0700.
- **D8 Containers:** MKV + AAC for MJPEG, fragmented MP4 + AAC for H.264; FFmpeg as hard Depends from P1 (recording with
  sound and playback both need it).
- **D9 Codec picker:** the badge rules of 4.1, the table of 4.2, the defaults per board, the live encoder watchdog.
  P2: plan 16.3 proposes revisions of D5 and D9 that the owner has not accepted yet (brief 1.2, answer to Q3).
- **D10 H.265 and AV1:** conversion targets on the Pi 4/5 class only, never live, hidden on the Zero class and the
  Pi 3A+.
- **D11 Sound input:** Cassette's list and guards, camera mic first and default, "No sound" as a choice, the mic's
  level left as found and shown as a percentage. P1 camera mic only, the chooser in P2.
- **D12 A/V sync:** the audio clock as master, video timestamps mapped onto it.
- **D13 Convert queue:** inside VHS (in front, or hidden with `run`), no daemon; nice 10 and idle I/O on 2 threads,
  Fast optional; pause above 75 C, optional "only while charging"; disk check; keep the original by default, Replace
  after verification with one confirmation; a crashed job restarts from zero.
- **D14 Long conversions:** show the ETA; above 60 minutes suggest a lower resolution or Fast; above 4 hours show the PC
  alternative.
- **D15 Full-screen playback:** adopt ruling **R-7** as worded in 8.2 and record it in `apps/vhs/docs/decisions.md` and
  the checklist's rulings. See "Ruling R-7" below.
- **D16 Playback decode:** FFmpeg software, hardware H.264 on VideoCore boards and HEVC hardware on the Pi 4/5 where
  the Pi's FFmpeg supports it; audio-master frame dropping with a hint to convert; MJPEG exact seeks, H.264 keyframe
  seeks while scrubbing and exact on release.
- **D17 Audio output:** Vinyl's stack (outputs, per-output volume, Bluetooth, media keys, seek bar).
- **D18 Multitasking:** `X-Background=run`; on hide: finish recordings, release the camera, pause playback, keep
  converting; background recording later. (The answer to Q1 below settles the recording part.)
- **D19 Switch and unplug:** finish the file, notice, reopen by USB identity in the new orientation.
- **D20 Power:** read the Pi's under-voltage alarm when present and give supply or powered-hub advice; ask for L3.
- **D21 Scene actions:** play, convert, rename (Cassette's rules), delete (asks), lock, copy to a mounted drive; photos
  also "Send to Notes". (Send to Notes: see the entry of 2026-11-16 under "Resolved during the build".)
- **D22 Time-lapse** as a recording mode in P3, with the camera idle between shots of 30 s or more.
- **D23 Camera controls:** basic in P2, full in P3, saved per camera and re-applied.
- **D24 Phase order:** the owner's P1-P3 with basic controls moved to P2 and the queue first in P3 (plan 13.1).
- **D25 Launcher requests:** L1 with VHS 1.0, L2 with P3, L3 any time, L4-L6 later; none for full screen.
- **D26 Memory:** accept VmRSS above the 25 MB guideline while recording, playing or converting on the Zero 2 W (as
  Vinyl does with FFmpeg); measure in P1.
- **D27 Screen sizes:** Vinyl's classes (Small, Medium, Portrait with `X-Portrait=320x480`, Square, Large), no
  `X-MinSize`; layouts are the coordinator's design.
- **D28 P0 permissions:** the temporary static ffmpeg and two test tools on zero8, Pi 3A+, Pi 4B, RPI52, HackBerry and
  the tablet (removed after, with a rollback list); a few saved frames; the owner flips the camera.
- **D29 Later items:** QR, document scan, motion, LAN stream and Home Assistant, baby monitor, geotag, magnifier, more
  cameras, each decided on its own after 1.0 (plan section 14).

## Cassette features carried over (D30-D38, owner, 2026-11-14)

The owner: "basically VHS is like cassette for videos (+ it can take photos)". The Controller listed the features of
Cassette 0.3.1 that carry over and the owner placed each (plan 17.1). The owner's answers are quoted; the rest of each
cell is the plan's wording.

| # | Decision | Owner's answer | Release |
|---|---|---|---|
| D30 | **Self-timer** for photos and video (Cassette's Record timer): Off / 3 / 5 / 10 s, a big countdown over the picture, a tap, Esc or the stop key cancels, no beep, a brief flash at zero for a photo. The owner asked for it: "timer to start recording/take photos 3 / 5 / 10 secs" | "Off/3/5/10 s in P1" | P1 |
| D31 | **Camera microphone controls** (Input level, SOFT gain up to +24 dB, Mic mute, Low cut 80 Hz, Limiter, the LED meter with its target zone) | "All of them in P2" | P2 |
| D32 | **Auto-split** of long scenes at 4 GiB (part files, Cassette's `_pN` rule, seamless to a frame or two; Off for ext4 folders) | "In P1, default 4 GiB" | P1 |
| D33 | **Copy a scene to a USB drive**, reusing the mount-on-request code of Vinyl 0.3.0 (udisks, the launcher's polkit rule) | "P1, reuse Vinyl's mount-on-request" | P1 |
| D34 | **Edit** (trim and split: several split points, no dragging, 'save as new files' by default, 'replace original' asks twice; for MJPEG in MKV the cuts need no re-encoding) | "Its own release after P3" | after P3 (0.4.0) |
| D35 | **Merge** scenes (pick in order, only the same codec, size and rate join, keep or delete originals) | "Together with Edit" | with D34 |
| D36 | **Live monitor** (hear the microphone in the headphones while filming) | "Not wanted" | dropped |
| D37 | **Background recording**: foreground only, as Cassette's D10 (the app owns the screen while recording); this replaces the plan's `X-Background=run` for recording | "Foreground only in P1" | P1, revisit on multitasking boards later |
| D38 | **Sound-on lamp** in P1 (a small lamp on the recording screen that lights while the camera microphone delivers a signal above a noise floor; no meter and no gain controls, those come in P2 with D31). The Controller offered it because D31 leaves P1 without any sound level display | "ok for sound on" | P1 |

- The plan's remark under D31 that the P1 recording screen draws a meter without the target zone is overtaken by D38:
  P1 has the lamp and no meter (brief 1.1).
- D37 and the answer to Q1 (below) together: nothing records in the background; on a hide the scene is finished and
  saved.
- Still open in the plan after the owner's answers (plan 17.1): Cassette's scene look (a VHS look of its own, a design
  call of the Controller), the 5x scan and 5 s jump keys in playback, and the take number page. Brief 10 builds
  Left / Right as 5 s and Shift as 30 s seeks in playback; brief 20 leaves the take number page out of P1 (a P2
  candidate). No decision on the look is recorded in the sources of this file.

## D6 ruling (owner, 2026-11-15): Instant / Light

The plan's spike showed the 1080p preview is affordable on every board measured (0.64 core on the Zero 2 W, 0.70 on the
Pi 3A+). The plan proposed a Photo setting with Instant and Light; the owner ruled (plan 16.5): "accepted as
proposed, Instant on boards that previewed 1080p under 0.75 core (RPI52, Pi 4B, palmpi, zero8), Light elsewhere; the
setting is shown in Settings > Photo." No other wording of the owner is recorded.

- **Instant:** the preview streams the Photo size and a photo is the next good frame. **Light:** the preview streams the
  Light stream (the listed MJPEG mode nearest 1280x720 in the Photo size's orientation, at most 1280 on the long side)
  and a photo switches the stream: stop, set the Photo size, start, drop 15 frames, keep one, switch back (brief 4.1
  rule 3). During a recording a photo is the recording's current frame, in both.
- **The "Quick photo" row of the canvas becomes "Photo: Instant / Light"** (settings key `photo_mode`; brief 11 and 20).
  A choice the user stores always wins; absent, the default follows the board's SoC class from a table, never a board
  name: Instant on BCM2712, BCM2711 and BCM2837 (the Pi 5 and CM5, the Pi 4B, the Pi 3A+ and the Zero 2 W), Light
  elsewhere (brief 4.1 rule 4). The ruling named RPI52, Pi 4B, palmpi and zero8; extending Instant to the deck, the
  HackBerry, RPI53 and pi5b because they share those SoCs is the brief's inference, not a measurement. The amd64
  tablet gets Light (not measured).
- On the Logitech C270 (largest mode 1280x960, no 1080p) Light is a real stream switch: see the entry of 2026-11-15
  under "Resolved during the build".

## Ruling R-7: full-screen playback (D15, accepted 2026-11-14; recorded now, used in P2)

The owner accepted D15 with the rest of D1-D29, "as worded" in plan 8.2 (plan section 17). Full-screen playback is P2
(the answer to Q2); 0.1.0 has windowed playback only. The ruling is recorded here, as the plan asks, and goes into the
Design QA checklist's rulings in its next revision (plan 17.1). Why it is needed: a full-screen page without the
toolkit's header and strip fails the checklist's D-02, D-03, D-07, D-08, D-10 and D-12 (plan 8.2); D-28, D-30, D-34 and
D-35 still apply and can be met.

The wording (plan 8.2):

"Full-screen video playback is exempt from the header-band rules (D-02, D-03, D-07, D-08, D-10, D-12). Conditions:
- it is entered only by a deliberate action (a key, a button, a double tap);
- Esc and the overlay's Close always leave full screen (back to the windowed page); the 3 s Esc hold still ends the app;
- every overlay control has a key (Space play/pause, Left/Right seek, Up/Down or +/- volume through the launcher, F
  toggle, Esc leave), and the seek bar works by tap as well as drag;
- the overlay appears on any tap or key and shows volume changes from `status.state`;
- nothing else is drawn when the overlay is hidden."

Whether the live capture view may also go full screen is a separate question (plan 12); R-7 does not cover it. The
canvas board `PlaybackFullscreen` is the P2 reference.

## The whole frame (the Controller's design call, 2026-11-15)

Plan 16.5: "the preview always shows the **whole frame**; a camera preview that crops the field of view misleads
framing." The page takes the smallest TurboJPEG scale whose output covers the picture box, then fits it during the
RGB565 or XRGB8888 conversion (a border to the box where the shape differs). The spike's "auto" crops, which was fine for
measuring.

- Built as brief 4.1 rule 7 says: fit the whole frame inside the picture box with the same aspect ratio; decode at the
  largest denominator d in {1, 2, 4, 8} whose scaled size is still at least the fit size; resample to the fit size
  during the conversion (box filter or bilinear, fixed point), centred, with a border; LVGL never scales. The pixel
  format follows the display (RGB565 on 16-bit panels, XRGB8888 on the Pi 5).
- The same rule fits a photo in the review page and a movie in playback.

## Resolutions of brief section 20

The points the plan and the canvas left open, and how the brief settled them.

| Point | Resolution | Basis |
|---|---|---|
| Recording codec in P1 | MJPEG copy only; detection of the hardware codec nodes built and logged | D24, plan 1.1, canvas "MJPEG"; Q3 (answered, below) |
| Full-screen playback | P2; the canvas board is the reference | D24; Q2 (answered, below) |
| Instant / Light for unmeasured boards | by SoC class (brief 4.1 rule 4); the tablet is Light | plan 4.1 "data per SoC class"; D6 ruling |
| "Quick photo" canvas row | becomes "Photo: Instant / Light" | D6 ruling text |
| Camera discovery | by capability, not by name (the spike matched UGREEN only) | D3, C270 in the test set |
| Camera mic | matched by USB ancestry; `hw:` only; busy means video-only + toast | C14 (separate card), plan 3.6 |
| AAC rate | the card's (16 kHz on the CM969), 64 kbit/s at <= 24 kHz | P0-b / P0-f; D8 |
| Journal and repair | in P1 (plan 13.3 lists them); power-pull test optional | plan 13.3 |
| Parts | separate rows and files, Cassette labels | Cassette precedent |
| Counter and prefix | one counter for scenes and photos; prefix for movies only, photos `photo` | canvas names |
| Seconds in names | `YYYYMMDD-HHMMSS` | canvas, plan 6 |
| Foreign files | listed; rename refused | Cassette precedent |
| Camera while off the Capture page | released | CPU on the slow boards |
| Playback demux | two demuxers (Vinyl's decoder unchanged for audio) | simplest reuse; checked at G5 |
| No libswscale | own YUV-to-RGB with fit | not in plan 13.3 Depends |
| HEVC in P1 | FFmpeg software; drop rule + notice on slow boards | P0-m numbers; DRM hwaccel is P2 (plan 16.4) |
| Test hooks | hidden `split_mib`, `min_free_mb` | Cassette's `split_bytes` override precedent |
| Out of P1 | burst, 60 fps, take-number page (P2 candidate), shutter beep, YUYV recording, PCM option, Eject button, automatic Instant-to-Light fallback | not in the owner's P1 lists or the canvas; quality bar "main path first" |
| Send to Notes | optional end of M6 (then hidden until built: see the entry of 2026-11-16 below) | D21 accepted, canvas shows it, but it is not in the plan's P1 tier |
| `X-MinSize` | none | D27 (Vinyl declares 480x320; D27 chose none for VHS) |

## Owner answers to Q1-Q3 (2026-11-15, final)

Brief 21 asked three questions; the owner's answers, in the brief's words:

- **Q1 (D37 "foreground only"): what happens on Super or three fingers during a recording?** Options: (a)
  `X-Background=run`: the scene is finished and saved, VHS hides with the camera released and comes back live; (b)
  Cassette's way, no `X-Background`: Super does nothing and three fingers freeze VHS mid-scene behind a "Close VHS?"
  sheet. Answer: "D37: Super or three fingers during a recording = finish and save the scene, hide with the camera
  released, come back live (`X-Background=run`)." Built that way: the `.desktop` has `X-Background=run`. Why not freeze
  or none (brief 13): `freeze` SIGSTOPs the writer right after the ack, mid-trailer; with no `X-Background` three
  fingers freeze VHS mid-scene and the file has a gap or depends on SIGTERM handling. SIGTERM finishing the file is
  built either way.
- **Q2: full-screen playback.** The canvas drew it among the P1 screens, but D24 places it in P2. Answer: "Full-screen
  playback stays in P2 (D24)."
- **Q3: hardware H.264 recording on the Zero 2 W, Pi 3A+ and Pi 4B.** Answer: "P1 records MJPEG copy only; hardware
  H.264 is P2 (after D5/D9 revisions are accepted)."

## Later (not built)

From brief 1.2; none of these is in 0.1.0.

- **P2 (0.2.0):** the codec picker with badges (D9); H.264 recording in fragmented MP4 (hardware `h264_v4l2m2m` on the
  Zero 2 W, Pi 3A+ and Pi 4B, x264 elsewhere); the live encoder watchdog; the sound input chooser (USB and Bluetooth
  mics, server routing, "No sound" as a choice); the microphone controls (D31: level, SOFT gain, mute, low cut,
  limiter, the LED meter with its target zone); full-screen playback with the tap overlay (R-7); basic camera controls
  (exposure, white balance, zoom, power line) saved per camera; hardware decode (H.264 `h264_v4l2m2m`, HEVC through the
  DRM hwaccel); the owner's acceptance of the D5/D9 revisions of plan 16.3. Zoom (Up / Down, +/-) and F come with them.
- **P3 (0.3.0):** the convert queue; time-lapse; the full controls page.
- **0.4.0:** Edit (trim, split) and Merge (D34, D35).
- **Later or dropped:** QR, document scan, motion clips, LAN stream and Home Assistant, baby monitor, geotag, magnifier,
  CSI and other cameras, the launcher requests L1-L6, background recording. The live monitor is dropped (D36).
- **Not in P1 (settled in the table above):** burst; 60 fps modes; a take-number settings page; a shutter beep;
  recording from YUYV-only cameras; a PCM audio option; an Eject button; an automatic fallback from Instant to Light.

## Resolved during the build

| Date | Point | Resolution | By |
|---|---|---|---|
| 2026-11-15 | C270 and the Light rule (brief 4.1 rule 3) | Kept exactly as rule 3 reads: Light streams the listed MJPEG mode nearest 1280x720 in the Photo size's orientation with at most 1280 on the long side. On the Logitech C270 that is 1280x720 while its Photo size (the largest mode) is 1280x960, so Light is a real stream switch there. The brief's note "Instant = Light, Light-like by construction" is about cost, not the rule. Instant stays the default by SoC class, so the C270's default behaves as 1280x960 (the preview streams the Photo size). | Controller, G1 review |
| 2026-11-15 | Capture page in the Square class (480x480, 720x720) | The side column and the text block left the 480x480 picture far too small: in the layout service's Square class the buttons go in one row under the picture (Photo / Video, the shutter, Scenes, Settings, each TARGET high) and the picture takes the panel's width (432x243 at 480x480, 8 px/mm; 672x378 at 720x720). 640x480 (4:3, not Square) keeps the column: the row would give it a smaller picture. The canvas sizes (480x320, 320x480) are unchanged. | Controller, G1 review |
| 2026-11-15 | Settings in M2 | The Settings page is M6: until then Photo (`photo_mode`), Photo size and the self-timer (`self_timer_s`) are read from `~/.config/vhs/settings.json` only. M6 built the page and docs/SETTINGS.md. | Controller, M2 brief |
| 2026-11-15 | REC start indicator (owner idea, camcorder style; M4 item 1) | From the REC press (or the self-timer's zero) until the first frame is written the REC tag's red dot blinks, 500 ms on / 500 ms off starting lit (app/rec_indicator.hpp: the phase from a passed-in clock, the UI's frame timer drives it); the tag reads "REC 00:00:00", Sound on is hidden, Photo now is dimmed and inert. At the first written frame the dot is solid and the timer runs. The stop button works during the start: with nothing written the empty file and its journal are removed (no row, no toast); with frames written it is a normal stop. A hide ends the recording, so the blink never applies after a show or reopen; no new text. | Controller, G3 |
| 2026-11-15 | REC start delay (M4 item 2) | One `[vhs] rec-start:` line per REC: stream close, reopen, mode set, controls, buffers + STREAMON, first camera frame, warm-up (15 frames), mic open and writer open (both run while the stream switches), first frame written. A mode change now switches on the open device (STREAMOFF, REQBUFS 0, S_FMT, S_PARM, new buffers, STREAMON; the controls kept) instead of closing and reopening it; any refusal falls back to the full reopen. The warm-up stays 15 frames (no evidence for less). | Controller, G3 |
| 2026-11-15 | Mic log on a normal stop (M4 item 3) | A user stop no longer logs "the microphone stopped (none)"; the line is for a real microphone loss only, with its reason. | Controller, G3 |
| 2026-11-15 | Preview stream per mode (owner; replaces brief 4.1 rules 5 / 6 for the preview) | Video mode previews the Video size stream (default 1280x720); Photo mode previews the Photo size (Instant) or the Light stream (Light). No mode picking by window size. Tab switches the stream (in place; the last picture stays until the new one comes, the existing behaviour). R in Video mode needs no switch, so the scene starts from a warm stream; R in Photo mode still switches (the rec-start line says which). A photo during a recording ("Photo now") stays the recording's current frame. | Owner via the Controller, M5 |
| 2026-11-16 | The drops toast (G6 item 1) | It auto-hides after the toasts' 4 s (NoticeQueue) and fires again only after a new 2 s of continuous drops that start after it hid (DropWindow ignores drops for 4 s after it fired). It appeared on the build box because of the clock bug below, not the harness or a missing sound device. | Controller, G6 |
| 2026-11-16 | Skipping playback on the boards (owner, G5 build; Pi 5 "337 dropped late, 192 not shown" in 24 s, CPU 0.15 core) | Cause: with sound, the picture's clock was the sound's heard position as play_step() last read it, and play_step() runs on the 250 ms status timer: the clock stood still for 250 ms, then jumped 250 ms. At each jump the presenter skipped the due pictures ("not shown") and the movie thread dropped the frames more than a period behind ("dropped late"); the unit test called play_step() every few ms and never saw it. Fix: the picture's clock is computed at every call (monotonic) and steered toward the sound (a difference over 300 ms moves it at once, a smaller one is taken out at most 5 % faster or slower); it waits at 0 (or a seek's target) until the first picture is decoded; the sound steers only after a 1 s window found its position moving at real time within 15 % (three bad windows: never for that movie) and never on the sound server's null sink (Dummy Output); a picture up to two frame periods late is still shown, the drop rule drops only beyond two periods. A stats line once a second: clock, the sound's position and delay, the clock's mode and rate, the shown pts, shown / not shown / dropped late, decode ms, queue. Build box, fake outputs (real time, a stepping delay, an instant sink, the null sink), the app's cadence: 0 dropped late, 0 not shown in 5 s (before: the harness's 60 dropped late, 36 not shown in 8 s). | Controller, G6 |
| 2026-11-16 | First use of a silent output (owner, Pi 4B mixer at 0 %) | An output (a card's mixer or a sink) found at 0 % or muted for which VHS has no line in output-levels starts at the level nearest 50 % and the volume line reads "Volume 50% (was off)"; a level above 0 is kept as found. Either way the output's level goes into output-levels (and at every press of "-" / "+"), so a silent output known to VHS is left alone later. | Controller, G6 |
| 2026-11-16 | "Send to Notes" row (D21's photo action; brief 9.4, optional) | Hidden in the options sheet until the action is built; it is a P2 candidate. This replaces the M6 provisional row ("the sheet's row still shows (canvas) and only logs"). | Owner via the Controller, G6 |
| 2026-11-16 | Tap rule in Settings | The Settings rows keep the toolkit's two-tap rule: a tap selects a row, a tap on the selected row opens it (cp0::ui::CompactList; Vinyl's own pages act on the first tap). Owner ruling; it replaces the M6 provisional row "Tap rule in Settings". | Owner via the Controller, G6 |
| 2026-11-16 | The icon | Drawn by the Controller, the owner picked the camcorder: a 100x100 RGBA PNG, byte-identical in `icon.png` and `root/usr/share/APPLaunch/share/images/vhs.png` (installed at M7; the spike's placeholder is gone; brief 12.4 item 8). | Owner via the Controller, G6 / M7 |
| 2026-11-16 | A cancelled copy's empty folder | The empty `VHS/` folder on the drive is removed when the copy made it and it is still empty (the `.part` is deleted by the cancel, as before). | Owner via the Controller, G6 |

## Provisional defaults (for the Controller at the gates)

What each milestone's build does where the brief or the canvas is silent, newest first. The Controller confirms or
changes each at its gate; none is an owner question.

### Provisional in M6 (for the Controller at G6)

| Point | What the build does now | Basis |
|---|---|---|
| Settings rows | the toolkit's list (cp0::ui::CompactList, LIST_ROW rows: the canvas's 34 px at 480x320) under the canvas's head line; a row's value grey, then the chevron (Edit); Playback output without a chevron (canvas), drawn with Info's look though it opens Vinyl's chooser | canvas Settings-480x320 |
| Pick lists | the choices replace the rows under the row's name in capitals ("SELF-TIMER", "PHOTO SIZE" ...), Vinyl's dots (white, the chosen one blue); a choice applies and closes the list; Esc closes it unchanged | Vinyl's pick list (owner-approved 2026-11-13) |
| Folder choices | the current folder and `~/VHS` (Cassette's rule) | brief 11 |
| Photo size / Video size without a camera | the stored or last selected size only; "Largest" when nothing is known | brief 11 |
| Name prefix box | the rename's text box, title "Name prefix", label "Prefix"; refused: "Not saved: <the rename's reason>." | Cassette's prefix rule |
| Settings and the camera | opening Settings releases the camera (as Scenes does); leaving it reopens it with the new values | brief 11 "apply at the next open" |
| Toast on Settings | the Scenes page's place (the panel's bottom) | no canvas toast there |
| Drive list | "Copy to which drive?" over the dimmed list; Vinyl's row values ("not mounted", "<size>, mounted", "mounted by VHS"); a tap or Enter picks; Esc or a tap outside closes it | brief 12.4 item 5 |
| Copy notices | "No USB drive found.", "Copied to SANDISK."; Vinyl's "Mounting SANDISK...", "Could not mount it: <reason>.", "Drives need the udisks2 package."; "Not copied: <reason>." | brief 9.3, Vinyl |
| A name already on the drive | refused: "Not copied: a file of that name is on the drive." (no "(2)" names) | brief 9.3 is silent |
| Copy dialog keys | Esc and Enter press Cancel (the toolkit's Dialog with one button); a tap outside does nothing | toolkit Dialog |
| Cancel button height | at least TARGET (the canvas's 34u is under the touch rule) | harness touch rule |
| Settings head line | the list's section row (upper case, grey) cut with dots where it does not fit: at 480x320 it reads "VHS SETTINGS. SOUND: THE CAMERA MICROPHONE (CONT..." (the section font is larger than the canvas's 12 px) | canvas Settings-480x320 |
| Copy progress before the size is known | "Preparing" for the first moment, then "630 MB of 1.4 GB, about 40 s left" ("almost done" under 1 s) | canvas CopyToUsb |
| Copy dialog on narrow panels | the canvas's places; a line that wraps to more rows moves the bar, the bytes line and Cancel down | brief 12.4 item 7 |
| Volume line after a first use | "Volume 50% (was off)" until the next press of "-" / "+" | G6 owner item |

### Provisional in M5 (for the Controller at G5)

| Point | What the build does now | Basis |
|---|---|---|
| "Cannot be played" toast | "This video cannot be played." (no decoder, a pixel format the own converter does not take, a file that does not open) | brief 4.4 "wording: Controller" |
| Space on Playback | pauses / plays (Vinyl's habit); Left / Right 5 s, Shift 30 s (brief) | brief 12.3 has no playback row for Space |
| Volume line without an output | "Volume" alone (no percent) while the engine has no output (the harness, a board without sound) | canvas "Volume 72%" |
| The end of a movie | pauses on the last frame; Play starts again from the beginning | Vinyl's QueueEnd pause |
| "-" and "+" height | at least TARGET (the canvas's 40u is under the 44u touch rule); "Volume" moves 4u down | harness touch rule |
| Playback in the undrawn classes | portrait / square: one row under the picture (Pause half, "-" and "+" a quarter each), then volume, seek bar, times, name | brief 12.4 item 7 |
| Toast place on Playback | over the picture's bottom (the seek bar, the times and the name stay readable) | no canvas toast there |
| Output setting | Settings > Playback output read from settings.json (`output`, Vinyl's target string) until the Settings page (M6) | coordinator, M5 |

## The Square class layout (2026-11-17)

The owner, on the HackBerry (720x720, 6.68 px/mm, touch target 47 px): the texts were far too small (the hint line 11 px,
1.6 mm) and the lower 40 percent of the panel was empty; the owner asked for a proposed layout, and the Controller drew a
design canvas of six boards at 720x720 (live view, recording, Scenes, playback, options sheet, Settings). This build
implements the canvas as drawn; the owner has not commented on it yet, so everything here is the canvas's, not a ruling.

- **Cause:** the layouts scale by `u = TARGET / 44`, the 480x320 canvas's unit. At a 47 px target `u` is 1.07, so the type
  stayed at the canvas's 10 to 14 px (1.5 to 2.1 mm at 6.7 px/mm) while the Square panel is 720 px across: the picture,
  16:9 and a multiple of 16, was 624 px wide and the lower 40 percent held nothing.
- **Rule for the Square class only:** `k = min(w, h) / 720` (kept between 0.4 and 2.5); every size is the canvas's 720x720
  px times `k`, rounded. Type: 20 px at 720 as the smallest, 24 body, 26 buttons, 28 to 34 names and the REC time; below
  720 in proportion, never under 12 px (13 px at 480x480). Targets: 56 px times `k`, never under the launcher's TARGET
  (so at 480x480 the buttons are TARGET high and the control row grows to hold two of them). The older pages that still
  scale by `u` (the copy dialog, the toast, the NoCamera block, the review) get `u = 2k` in this class, which doubles their
  canvas-480 sizes (11 to 14 px become 22 to 28 at 720). The other classes keep `u = TARGET / 44` and are unchanged
  (checked by comparing the harness's PNGs of the 480x320, 320x480, 640x480, 800x480, 480x800 and 480x640 sizes before and
  after).
- **Live view:** picture box the panel's width, 688x440 at 720 (the frame is centred in it, pillarboxed); one control row
  120 px high: Photo and Video as two stacked 56 px buttons (the chosen one grey with an amber ring), the 120 px shutter,
  Scenes (with its count beside it, "Scenes 7") and Settings stacked; a 40 px status line under it. Recording: the REC tag (34 px,
  26 px dot) at the top left, the Sound-on lamp at the top right, "Photo now" as a 270x120 button in the left column.
- **Scenes:** filter chips 56 px (equal width: the layout code cannot measure text), "N scenes" at the right, rows 92 px
  (a 96 px pitch) with a 104x78 thumbnail, a 26 px name, a 20 px detail line, the date at the right and the "locked"
  mark after the name; the hint line under the list. **Options sheet:** across the panel's bottom, five rows of 72 px
  (76 px pitch), 28 px labels, 22 px key letters in `#4A4535` (darker than the other classes' `#6B6552`, for contrast), the
  name and its detail in the header, no note line. **Playback:** picture 688x400, name line (the codec at the right), a
  seek row (times 24 px at both ends, a 56 px touch area, a 32 px knob), Pause 230x80, volume buttons 80x80 with "Volume N%".
  **Settings:** rows 68 px, labels 26 px, values 24 px, a 30 px "SETTINGS" head, the hint line at the bottom.
- **Not on the canvas, scaled the same way (provisional):** the photo review (the picture as the live view's, Previous /
  Next / Delete 80 px high under it, the name at 28 px, the note 22 px); the copy dialog, the delete question, the toast
  (24 px, 56 px high, over the picture's bottom), the countdown (a 192 px ring, a 104 px number, Cancel in Scenes' place),
  the NoCamera page (36 px title, 26 px text, 22 px hint); the status line's texts for the countdown ("Photo in 3 s" and
  "A tap, Esc or Stop cancels"), the turned camera ("The camera switch was moved" and "Back in about 3 s") and the
  recording ("MJPEG 720p - 148 MB written", "2 h 10 min left on disk").
- **Departures from the canvas** (the toolkit or the code cannot do them, or the other classes' rules say otherwise):
  the Scenes rows have no chevron (the canvas draws none; the owner's row-affordance rule would add one); "locked" sits
  after the name, not under the date; the filter chips have one width; the sheet's highlighted row spans the sheet,
  not 12 px inside it; the Settings head is the toolkit's grey upper-case section line, not 30 px white bold; the toolkit's
  list draws its own chevrons; the font has one weight (the canvas's 600 weight is not used) and no middle dot, so the canvas's "·" is a hyphen ("C270 HD WEBCAM - MJPEG") and the Scenes count is grey beside the name; the status line's key hints fall back to "Space - Tab - R" when the camera's name and the full hints do not fit one line.
- **Tests:** `tests/test_layout.cpp` (the canvas's numbers at 720x720; the invariants of every page at 480, 640, 720, 800
  and 1024 squares and five touch targets); the host scenes `*-square.scene` at 720x720 (6.68 px/mm), 640x640 and 480x480;
  board tests B33 to B38.

### Provisional in M4 (for the Controller at G4)

| Point | What the build does now | Basis |
|---|---|---|
| Empty Scenes page | one grey line: "No scenes yet. Record with R or take a photo with Space." | no canvas board; brief 12.4 item 3 |
| Head count | "SCENES, N" counts the whole folder, whatever the filter | canvas "SCENES, 14" |
| Toast place on Scenes | the toast line at the panel's bottom | the canvas draws toasts on the Capture page only |
| Tab on Scenes | the next filter (All, Movies, Photos) | brief 12.3 has no filter key |
| Sheet "Lock" on a locked file | reads "Unlock" | brief 9.2 "toggles" |
| Rename on a photo | refused: "Not renamed: photos keep the name photo." (the grammar keeps photos "photo") | brief 8 |
| Refusal toasts | "Not renamed: <Cassette's reason>.", "Not deleted: the file is locked." | Cassette's pattern |
| Delete question | "Delete this scene?" / "Delete this photo?" with "Y = Delete, Enter or Esc = Cancel" | M2's photo dialog |
| Play on a movie, Copy, Send to Notes | log only until M5 (playback) and M6 (copy, notes) | milestones |
| Thumbnails of other codecs | none until M5 (FFmpeg's decoder comes with playback): the empty box | brief 9.1, milestone order |
| Tap rule on rows | a tap selects, a tap on the selected row opens (the toolkit's list rule) | cp0::ui::CompactList |

### Provisional in M3 (for the Controller at G3)

| Point | What the build does now | Basis |
|---|---|---|
| Toast texts not on the canvas | "Camera microphone busy: recording without sound." (the brief's example), "No camera microphone: recording without sound.", "Camera microphone failed: recording without sound.", "Recording needs a camera that sends MJPEG.", "Recording needs FFmpeg: its libraries are missing.", "Recording stopped: <reason>." | brief 5.4 "wording: Controller", 12.4 item 3 |
| Sound-on lamp while off | the tag is hidden while the lamp is off (and never drawn without a microphone); the canvas draws only the lit state | canvas Recording-480x320, brief 6.6 |
| Recording note without a microphone | "MJPEG 720p without sound. Stop with R, Space or the red button; it asks nothing and saves the scene." (the canvas's note with "without sound") | canvas note, brief 12.4 item 3 |
| Size and time left before the rate is known | the size alone ("4.2 MB") for the first 2 s; then "1.4 GB, 3 h 20 min left" | brief 6.4 (rate over the last 10 s) |
| R in Photo mode | starts a scene in either mode (canvas: "R records"); the Photo / Video mode is kept: after the stop Space takes photos again | brief 12.3 |
| Self-timer and R | the self-timer runs before every video start (R, Space in Video mode, the red shutter); "Video in N s" | brief 6.7, D30 |
| "Camera turned: recording stopped" | a loss stops the scene at once ("Camera unplugged." with the loss); the turned toast follows when the camera comes back turned within 15 s | brief 5.5 |
| Scenes and Settings while recording | dimmed (canvas) and they do nothing | canvas Recording-480x320 |
| Startup repair | the repair (record/journal.*) is built and tested; running it at start on the scenes worker is the M4 "startup repair hookup" | brief section 18, M4 row |
