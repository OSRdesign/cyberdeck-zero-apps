# Making the screenshots of the user guide

The pictures in [USER-GUIDE.md](USER-GUIDE.md) are rendered by the app's scene harness on a PC: no board and no
sound hardware needed. One script renders all of them again, so you can refresh the guide after a feature changes
the screen.

## Refresh everything

From Windows, in a WSL shell (the build runs in WSL, as `build.sh host` does):

```sh
export LAUNCHER_SRC=<path to a checkout of the launcher repository>   # the launcher tree the apps build against
export LAUNCHER_TREE=$LAUNCHER_SRC
wsl -e sh <path to this repository>/apps/cassette/docs/make-doc-images.sh
```

Optional: `CDK_BUILD=~/cdk-build/<new folder>` (default `~/cdk-build/cassette-docs-img`; use a new folder per
worktree so builds do not collide) and `LVBUILD` (the LVGL host build; the first run builds it, a few minutes;
copy an earlier `_lvgl-host` folder into `$CDK_BUILD` to skip that).

What the script does:

1. Stages the app like `build.sh host` (an LF copy under `$CDK_BUILD/stage-docs`) and keeps only the scenes named in
   [`doc-images.tsv`](doc-images.tsv), so it renders only those scenes (about 60), not all of them (about 160).
2. Builds the headless host with `_common/build_app.sh` and runs those scenes. Their own checks (`expect text ...`,
   the layout check) must pass, or the script stops. The PNGs are written to `$CDK_BUILD/host/out/<scene>/<size>/<shot>.png`.
3. Copies each wanted PNG to `docs/img/<name>` (the table below is `doc-images.tsv`).
   The header clock is frozen at 12:00 (UTC) for the run by `freeze-clock.c`, a four-line `LD_PRELOAD` shim that
   `make-doc-images.sh` compiles with the host's `cc`; so two renders of the same UI give the same bytes. Without a C
   compiler the script says so and the clock shows the time of the run.
4. Runs [`finish-images.py`](finish-images.py) with the first Python that has Pillow (`python3`, `python`,
   `python.exe`): draws the numbered markers on `annot-*` images, makes `layouts.png` (the screen sizes side by
   side) and shrinks any PNG over 100 KB. If there is no Pillow, the raw PNGs stay in place and the script says
   so; then run `python docs/finish-images.py docs/doc-images.tsv docs/img` on any machine with Pillow
   (`pip install pillow`).

The images are 10 to 70 KB each, so the folder stays small. Commit `docs/img` with the change that caused it.

## The images

Source of truth: `docs/doc-images.tsv` (tab separated: image, scene, size, shot). The table is the same list (the rows after `bt-call-quality.png` were added later, so they are not in render order).

| Image (in `docs/img`) | Scene (`tests/scenes`) | Size | Shot | Shows |
|---|---|---|---|---|
| `recorder-landscape.png` | `x8-state-idle` | 480x320 | `state-idle` | Idle recorder with a take loaded, landscape |
| `annot-recorder-deck.png` | `x1-deck-recording` | 640x480 | `deck-recording` | Recording at the deck's 640x480 density, numbered regions (markers drawn by finish-images.py) |
| `recording.png` | `a-recording` | 480x320 | `recording` | Recording, level inside the TARGET zone |
| `recording-low.png` | `a2-recording-low` | 480x320 | `recording-low` | Recording, level too low |
| `recording-mon-lit.png` | `doc1-recording-mon-lit` | 480x320 | `recording-mon-lit` | Recording with the MON lamp lit |
| `recording-stereo.png` | `q-stereo` | 480x320 | `stereo` | Stereo card, two meter rows |
| `recording-paused.png` | `f-paused` | 480x320 | `paused` | Recording paused |
| `recording-lost-audio.png` | `z5-lost-audio` | 480x320 | `lost-audio` | Red `lost 0.3 s` in the strip |
| `recording-server.png` | `z2-server-recording` | 480x320 | `server-recording` | Recording through the sound server |
| `countdown.png` | `c-countdown` | 480x320 | `countdown` | Record timer countdown |
| `playing.png` | `d-playing` | 480x320 | `playing` | A take playing |
| `play-paused.png` | `x10-state-play-paused` | 480x320 | `state-play-paused` | A take paused |
| `play-rew.png` | `e-wind` | 480x320 | `wind` | REW held, 5x badge |
| `blank-cassette.png` | `y2-blank-idle` | 480x320 | `blank-idle` | Blank cassette, no take |
| `no-input.png` | `g-noinput` | 480x320 | `noinput` | No sound input found |
| `no-card-level.png` | `b-nocard` | 480x320 | `nocard` | Card without a level control (SOFT full width) |
| `load-0000ms.png` | `w1-load-t000` | 480x320 | `load-t000ms` | LOAD animation, frame 0 ms |
| `load-0190ms.png` | `w3-load-t190` | 480x320 | `load-t190ms` | LOAD animation, frame 190 ms |
| `load-0420ms.png` | `w4-load-t420` | 480x320 | `load-t420ms` | LOAD animation, frame 420 ms |
| `load-0750ms.png` | `w5-load-t750` | 480x320 | `load-t750ms` | LOAD animation, frame 750 ms |
| `list.png` | `i-list` | 480x320 | `list` | Recordings list |
| `list-preview.png` | `w6-list-load` | 480x320 | `list-load-previewing` | List, a take previewing |
| `rename.png` | `r-rename` | 480x320 | `rename` | Rename text box |
| `confirm-delete.png` | `s2-keys-list` | 480x320 | `keys-delete` | Delete confirm box (scene wording differs from the app's: see note) |
| `trim.png` | `j-trim` | 480x320 | `trim` | Edit page (trim and split), two splits |
| `trim-zoomed.png` | `j2-trim-tapflag` | 480x320 | `trim-zoomed` | Edit page zoomed, one flag |
| `save-box.png` | `k-save` | 480x320 | `save` | Save box, three files |
| `confirm-replace.png` | `l-confirm` | 480x320 | `confirm` | Replace original confirm |
| `list-keys-320x480.png` | `mg1-merge-small` | 320x480 | `list-keys` | The list's five keys on the narrow screen |
| `merge-mode.png` | `mg1-merge-small` | 480x320 | `mode-3` | Merge mode: three takes chosen, the takes that cannot join dimmed |
| `merge-mode-320x480.png` | `mg1-merge-small` | 320x480 | `mode-3` | The same, 320x480 (a reason wraps, its row grows) |
| `merge-box.png` | `mg1-merge-small` | 480x320 | `merge-box` | The merge box |
| `merge-confirm.png` | `mg1-merge-small` | 480x320 | `confirm-delete-originals` | Delete the originals? |
| `merge-progress.png` | `mg1-merge-small` | 480x320 | `progress` | Merging... 37% with CANCEL |
| `merged.png` | `mg2-merged-small` | 480x320 | `merged` | The list after a merge, the notice |
| `settings-top.png` | `h-settings` | 480x320 | `settings-top` | Settings, top |
| `settings-picker.png` | `o-settings-picker` | 480x320 | `picker` | A choice row's list (Record timer) |
| `settings-monitor.png` | `r7a-settings-all-rows` | 480x320 | `all-rows-monitor` | Settings with Monitor and Monitor level |
| `settings-bottom.png` | `r7a-settings-all-rows` | 480x320 | `all-rows-bottom` | Settings, Sync every to Auto-split |
| `take-number.png` | `z7a-take-number` | 480x320 | `take-number-page` | Take number page |
| `size-640x480.png` | `x8-state-idle` | 640x480 | `state-idle` | Idle recorder 640x480 |
| `size-320x480.png` | `x8-state-idle` | 320x480 | `state-idle` | Idle recorder 320x480 portrait |
| `size-800x480.png` | `y-other-sizes` | 800x480 | `other-recording` | Recording 800x480 |
| `size-720x720.png` | `sq720-a-idle` | 720x720 | `idle` | Idle recorder 720x720 square |
| `size-480x480.png` | `sq480-a-idle` | 480x480 | `idle` | Idle recorder 480x480 square |
| `square-stereo.png` | `sq720-i-stereo` | 720x720 | `stereo` | 720x720 with a stereo card |
| `bt-input-picker.png` | `bt1-input-picker` | 480x320 | `input-picker` | Bluetooth: the Input picker with a headset (two-line row, 16 kHz) |
| `bt-input-not-connected.png` | `bt1-input-picker` | 480x320 | `input-picker-not-connected` | The chosen headset not connected (dimmed, the reason) |
| `bt-settings.png` | `bt1-input-picker` | 480x320 | `settings` | Settings with the headset as Input, Input level Not applicable |
| `bt-input-notsetup.png` | `bt2-input-notsetup` | 480x320 | `input-notsetup` | No sound server: "Bluetooth audio is not set up on this board" |
| `bt-output-picker.png` | `bt3-output-picker` | 480x320 | `output-picker` | The Output picker: music quality, call quality, not connected |
| `bt-monitor-delay.png` | `bt4-monitor-delay` | 480x320 | `monitor-delay` | "Delay about 90 ms" under Monitor |
| `bt-monitor-to-picker.png` | `bt4-monitor-delay` | 480x320 | `monitor-to-picker` | The Monitor to picker |
| `bt-monitor-echo.png` | `bt5-monitor-echo` | 480x320 | `monitor-echo` | The echo notice in dark amber (Monitor to a speaker) |
| `bt-monitor-none.png` | `bt6-monitor-none` | 480x320 | `monitor-none` | Monitor to greyed with the reason |
| `bt-recorder.png` | `bt7-rec-label` | 480x320 | `rec-bt-label` | The recorder with BT instead of IN |
| `bt-connecting.png` | `bt8-rec-connecting` | 480x320 | `rec-connecting` | "Connecting the headset..." |
| `bt-no-sound.png` | `bt9-rec-nosound` | 480x320 | `rec-nosound` | "The headset is connected but sends no sound." |
| `bt-headset-gone.png` | `bt10-rec-gone` | 480x320 | `rec-gone` | The headset disconnected during a take |
| `bt-switching.png` | `bt11-switching` | 480x320 | `switching` | "Switching to music mode... 1 s" |
| `bt-call-quality.png` | `bt12-callq` | 480x320 | `callq` | "Playing in call quality: this headset has no music mode." |
| `rename-counter-mid.png` | `r2-rename-counter` | 480x320 | `rename-count-mid` | The rename box with its character counter, part way |
| `rename-counter-amber.png` | `r2-rename-counter` | 480x320 | `rename-count-amber` | The rename box, counter near the limit (amber) |
| `rename-counter-full.png` | `r2-rename-counter` | 480x320 | `rename-count-full` | The rename box, counter at the limit |
| `list-tags.png` | `r8-list-tags` | 480x320 | `list-tags` | The recordings list with the format tag in each row |
| `settings-codec-rows.png` | `fmt1-codec-all` | 480x320 | `settings-rows` | Settings with the Codec, Sample rate and Bit depth rows |
| `settings-codec-flac.png` | `fmt6-codec-current-flac` | 480x320 | `settings-flac` | Settings with FLAC chosen |
| `codec-picker-640x480.png` | `fmt1-codec-all` | 640x480 | `picker-top` | The Codec picker, 640x480 |
| `codec-picker-480x320.png` | `fmt1-codec-all` | 480x320 | `picker-top` | The Codec picker, top, 480x320 |
| `codec-picker-bottom.png` | `fmt1-codec-all` | 480x320 | `picker-bottom` | The Codec picker, bottom (the quick presets) |
| `codec-picker-current-flac.png` | `fmt6-codec-current-flac` | 480x320 | `picker-current-flac` | The Codec picker with FLAC as the current choice |
| `codec-picker-unavailable.png` | `fmt2-codec-44k-only` | 480x320 | `picker-top` | The Codec picker on a 44.1 kHz-only card: Opus dimmed |
| `codec-picker-notice.png` | `fmt2-codec-44k-only` | 480x320 | `picker-notice` | A tap on a dimmed entry: "Not available: ..." |
| `codec-picker-nolib.png` | `fmt3-codec-nolib` | 480x320 | `picker-top` | The Codec picker with a codec library missing |
| `rate-picker.png` | `fmt8-rate-picker` | 480x320 | `picker-top` | The Sample rate picker |
| `depth-picker.png` | `fmt9-quality-depth` | 480x320 | `picker-top` | The Bit depth picker |
| `bitrate-picker.png` | `fmt10-codec-change-toast` | 480x320 | `bitrate-picker` | The Bit rate picker (Opus) |
| `rate-picker-opus.png` | `fmt10-codec-change-toast` | 480x320 | `rate-opus` | The Sample rate picker with Opus chosen |
| `codec-change-toast.png` | `fmt10-codec-change-toast` | 480x320 | `settings-opus-toast` | Settings after a codec change, with its notice |
| `trim-fit.png` | `wfq-fit-small` | 480x320 | `trim-fit` | Edit page with the FIT waveform scale |
| `bt-input-paired.png` | `bt16-input-paired` | 480x320 | `input-paired` | The Input picker with a paired headset that is not connected (dimmed) |
| `bt-output-paired.png` | `bt17-output-paired` | 480x320 | `output-paired-refused` | The Output picker, a paired device refused (not connected) |
| `bt-input-notsetup-server.png` | `bt18-notsetup-sound-server` | 480x320 | `input-notsetup-server` | The Input footer "The sound server is not running" |

Not rendered by the script:

- `layouts.png`: made by `finish-images.py` from `recorder-landscape.png`, `size-*.png`.
- `real-hackberry-720.png` and `real-pi3a-portrait.png`: real board screenshots, copied by hand: the HackBerry (an Opus take, the lamps lit) and
  the Pi 3A+ (a FLAC take; 480x800), both taken with the cassette9l build. Replace them with newer captures when a layout
  changes (take a screenshot of the board's screen).
- `../screenshots/01..10-*.png`: the ten Store pictures, copied by hand from the same render
  (`$CDK_BUILD/host/out/<scene>/<size>/`: `a-recording` 640x480, 480x320 and 320x480, `c-countdown`, `i-list`, `j-trim`
  at 640x480 and 320x480, `k-save`, `h-settings` 320x480 and `b-nocard`; the scene names are in the README's table).

## Change what an image shows

- **A different state**: change the scene, size or shot in `doc-images.tsv` (the shot names are the `shot` lines of the
  scene file). Scenes: `tests/scenes/*.scene` (the UI on its synthetic model) and `tests/bind/scenes/*.scene` (the bound app
  on the scripted sound system). A scene is a few lines: `sizes`, `ppmm` (pixels per mm), `env CASSETTE_DEMO <state>`,
  `shot <name>` and `expect` checks; copy `tests/scenes/doc1-recording-mon-lit.scene` as a start.
- **A new image**: add a line to `doc-images.tsv` and the `![...](img/<name>)` line to `USER-GUIDE.md`.
- **Markers on an annotated image**: the positions are in `MARKERS` in `finish-images.py` (pixels of the picture). Look
  at the new render and move the numbers; the numbered table in the guide ("The recorder screen") must match.
- **Wording that comes from a scene, not the app**: scenes in `tests/scenes` use the UI's demo model, so a few texts
  differ from the bound app (for example the delete box title, `Delete Take 005?` in the picture, `Delete this take?`
  in the app). The guide's text follows the code; check the picture against the app when in doubt.

## Check before you commit

- Every `img/...` link in `USER-GUIDE.md` has a file: `grep -o 'img/[a-z0-9-]*\.png' docs/USER-GUIDE.md | sort -u | while
  read f; do [ -f docs/$f ] || echo missing $f; done`.
- No PNG over 250 KB (`find docs/img -size +250k`).
- The facts in the guide: re-read the sections that touch what you changed (the Settings table against
  `src/bind/view_map.cpp` `settings_rows()`, the keys against `docs/KEYS.md`).
