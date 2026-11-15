# VHS: decisions

The full record (D1-D38 with the owner's words, the D6 ruling of 2026-11-15, R-7, the whole-frame design call, the
resolutions of brief section 20 and the answers to Q1-Q3) is written at milestone M7 (brief section 17). This file
starts with the points resolved during the build.

## Resolved during the build

| Date | Point | Resolution | By |
|---|---|---|---|
| 2026-11-15 | C270 and the Light rule (brief 4.1 rule 3) | Kept exactly as rule 3 reads: Light streams the listed MJPEG mode nearest 1280x720 in the Photo size's orientation with at most 1280 on the long side. On the Logitech C270 that is 1280x720 while its Photo size (the largest mode) is 1280x960, so Light is a real stream switch there. The brief's note "Instant = Light, Light-like by construction" is about cost, not the rule. Instant stays the default by SoC class, so the C270's default behaves as 1280x960 (the preview streams the Photo size). | Controller, G1 review |
| 2026-11-15 | Capture page in the Square class (480x480, 720x720) | The side column and the text block left the 480x480 picture far too small: in the layout service's Square class the buttons go in one row under the picture (Photo / Video, the shutter, Scenes, Settings, each TARGET high) and the picture takes the panel's width (432x243 at 480x480, 8 px/mm; 672x378 at 720x720). 640x480 (4:3, not Square) keeps the column: the row would give it a smaller picture. The canvas sizes (480x320, 320x480) are unchanged. | Controller, G1 review |
| 2026-11-15 | Settings in M2 | The Settings page is M6: until then Photo (`photo_mode`), Photo size and the self-timer (`self_timer_s`) are read from `~/.config/vhs/settings.json` only (docs/SETTINGS.md comes with M7). | Controller, M2 brief |

| 2026-11-15 | REC start indicator (owner idea, camcorder style; M4 item 1) | From the REC press (or the self-timer's zero) until the first frame is written the REC tag's red dot blinks, 500 ms on / 500 ms off starting lit (app/rec_indicator.hpp: the phase from a passed-in clock, the UI's frame timer drives it); the tag reads "REC 00:00:00", Sound on is hidden, Photo now is dimmed and inert. At the first written frame the dot is solid and the timer runs. The stop button works during the start: with nothing written the empty file and its journal are removed (no row, no toast); with frames written it is a normal stop. A hide ends the recording, so the blink never applies after a show or reopen; no new text. | Controller, G3 |
| 2026-11-15 | REC start delay (M4 item 2) | One `[vhs] rec-start:` line per REC: stream close, reopen, mode set, controls, buffers + STREAMON, first camera frame, warm-up (15 frames), mic open and writer open (both run while the stream switches), first frame written. A mode change now switches on the open device (STREAMOFF, REQBUFS 0, S_FMT, S_PARM, new buffers, STREAMON; the controls kept) instead of closing and reopening it; any refusal falls back to the full reopen. The warm-up stays 15 frames (no evidence for less). | Controller, G3 |
| 2026-11-15 | Mic log on a normal stop (M4 item 3) | A user stop no longer logs "the microphone stopped (none)"; the line is for a real microphone loss only, with its reason. | Controller, G3 |

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
