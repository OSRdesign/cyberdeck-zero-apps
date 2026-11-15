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
