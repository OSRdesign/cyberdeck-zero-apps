# VHS: settings and the files it keeps

## `~/.config/vhs/settings.json`

`$XDG_CONFIG_HOME/vhs/settings.json` when `XDG_CONFIG_HOME` is set; `VHS_SETTINGS` names another file (tests).
Written atomically (temporary file, fsync, rename) and only when a value changed. Read tolerantly: a missing or
damaged file means the defaults, a bad value means that key's default, numbers are clamped, and keys VHS does not know
are kept when it writes the file again.

| Key | Settings row | Values | Default |
|---|---|---|---|
| `folder` | Folder | the current folder or `~/VHS` (Cassette's rule: other folders by editing this file) | `~/VHS` |
| `prefix` | Name prefix | 1-24 of `A-Z a-z 0-9`, space, `_`, `-`; no space first or last; not starting with `.` | `scene` |
| `photo_mode` | Photo | `instant` (the next frame of the photo stream) or `light` (the stream switches for the photo); absent: by the board's SoC (Instant on BCM2712 / BCM2711 / BCM2837, Light elsewhere) | per SoC |
| `photo_size` | Photo size | `WxH` of a listed MJPEG 30 fps size of the camera; absent: the largest | largest |
| `video_size` | Video size | `WxH` of a listed MJPEG 30 fps size, shown with "MJPEG" | `1280x720` |
| `self_timer_s` | Self-timer | `0` (Off), `3`, `5`, `10`; other numbers move to the nearest | `0` |
| `split` | Split long scenes | `true` (cut at `split_mib`), `false` (one file) | `true` |
| `output` | Playback output | Vinyl's output target: `""` Automatic, `bt:<MAC>`, `pulse:<sink>`, `alsa:<device>` | `""` |
| `split_mib` | hidden | 64..4096 (board test B26) | `4096` |
| `min_free_mb` | hidden | 100..100000: the disk floor (board test B30) | `500` |
| `camera_id` | Camera (0.1.2) | the camera remembered, by its stable id: `vid:pid:serial`, else `vid:pid@usb-path`, for a node that is not USB `sys:<sysfs device path>`; written by Settings > Camera, by the quick switch (chip, C) and, for a lone camera, as it is plugged in. A remembered camera that is not plugged in is not overwritten while two or more cameras are present (the first one is used and a toast says so) | none |
| `camera_name` | hidden | the remembered camera's name, for the "not plugged in" toast | none |
| `camera_sizes` | hidden | per camera id: `{"<id>": {"photo": "WxH", "video": "WxH"}}`, at most 16 (the oldest goes first). `photo_size` and `video_size` above are the sizes of the camera in use; a file of 0.1.1 (the one pair, no `camera_sizes`) keeps working: the pair belongs to the camera in use until a second camera is chosen, then it is stored for the camera that is left. A camera with no entry takes its best mode | none |
| `sound_input` | Sound input (0.1.2) | `""` Automatic (the camera's own microphone, else the first capture card, else none), `none` for No sound, else Cassette's card descriptor `name|usb by-id|vid:pid` (the card's number is never stored; `0000:0000` is left out). A stored card that is not there falls back to Automatic with a toast | `""` |

Settings rows (0.1.2): **Camera** first, **Sound input** before Playback output. Both open a pick list (a tap selects, a second tap
chooses); the Camera list is scanned when Settings opens (the camera is released then), the Sound input list names every
ALSA capture card and marks the one in force.

The preview follows the mode (the owner's decision of 2026-11-15): Video mode streams the Video size, Photo mode the
Photo size (Instant) or the Light stream. Rows that change the stream apply when the camera opens again (the Settings
page releases the camera; leaving it reopens). Nothing can change while recording (the Settings button is dimmed).

## `~/.config/vhs/output-levels`

`$XDG_CONFIG_HOME/vhs/output-levels` when set; `VHS_OUTPUT_LEVELS` names another file (tests). Vinyl's per-output
volume state, kept in a file of its own (VHS has no queue or resume file):

```
v1
<percent>	<m|u>	<output key>
```

One line per output after `v1`, keyed by the output (`<percent>`, then `m` when VHS muted it itself, `u` otherwise):
- an output without a playback mixer (an ALSA card without a volume control): its software level, the one VHS plays at;
- an output with a mixer (a card's volume control, a sound server's sink): the level VHS last saw or set there. The
  mixer itself keeps the level in force; the line marks the output as known. At an output's first use (no line yet) a
  mixer or sink found at 0 % or muted starts at the level nearest 50 % and the volume line reads "Volume 50% (was
  off)"; a level above 0 is kept as found. Once the line exists VHS leaves a silent output alone.

Written atomically and only when a level or a mute changed (a press of "-" / "+", a first use); read tolerantly (a
missing or damaged file is a fresh start: no output known, the software levels at 100 %, nothing muted). At most 32
outputs are kept (the oldest goes first).

## Other files

| Path | What | Written |
|---|---|---|
| `<folder>/.counter` | the last scene number handed out (one counter for movies and photos) | atomically, at every new name |
| `<folder>/.locked` | the locked file names, one per line | atomically, by Lock / Unlock |
| `<folder>/.journal/<stem>.json` | a recording's journal (`open` while recording, `closed` after the trailer, `damaged` / `repaired` after the repair at start) | at every part, at the end |
| `~/.cache/vhs/index.json` | the scenes' metadata cache (duration, codec, size by name + size + mtime) | when the Scenes page found new files |
| `~/.cache/vhs/thumbs/<name>-<size>-<mtime>.raw` | a row's thumbnail in the display's format | once per file and size; deleted with the file |

`XDG_CACHE_HOME` moves the cache. The fake camera of the headless scenes (`VHS_FAKE`) never touches these: it uses a
folder and a cache next to the harness's state directory.
