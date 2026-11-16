# VHS: decisions

The full record (D1-D38 with the owner's words, the D6 ruling of 2026-11-15, R-7, the whole-frame design call, the
resolutions of brief section 20 and the answers to Q1-Q3) is written at milestone M7 (brief section 17). This file
starts with the points resolved during the build.

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

## Provisional in M6 (for the Controller at G6)

| Point | What the build does now | Basis |
|---|---|---|
| Settings rows | the toolkit's list (cp0::ui::CompactList, LIST_ROW rows: the canvas's 34 px at 480x320) under the canvas's head line; a row's value grey, then the chevron (Edit); Playback output without a chevron (canvas), drawn with Info's look though it opens Vinyl's chooser | canvas Settings-480x320 |
| Tap rule in Settings | the toolkit's list rule: a tap selects a row, a tap on the selected row opens it (Vinyl's own rows act on the first tap) | cp0::ui::CompactList |
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
| Send to Notes | cut from M6 (optional): the sheet's row still shows (canvas) and only logs | brief 9.4, coordinator |
| Settings head line | the list's section row (upper case, grey) cut with dots where it does not fit: at 480x320 it reads "VHS SETTINGS. SOUND: THE CAMERA MICROPHONE (CONT..." (the section font is larger than the canvas's 12 px) | canvas Settings-480x320 |
| Copy progress before the size is known | "Preparing" for the first moment, then "630 MB of 1.4 GB, about 40 s left" ("almost done" under 1 s) | canvas CopyToUsb |
| Copy dialog on narrow panels | the canvas's places; a line that wraps to more rows moves the bar, the bytes line and Cancel down | brief 12.4 item 7 |
| Volume line after a first use | "Volume 50% (was off)" until the next press of "-" / "+" | G6 owner item |

## Provisional in M5 (for the Controller at G5)

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

## Provisional in M4 (for the Controller at G4)

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

## Provisional in M3 (for the Controller at G3)

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
