# Making the screenshots of the user guide

The pictures in [USER-GUIDE.md](USER-GUIDE.md) are rendered by the app's scene harness on a PC: no board and no
sound hardware needed. One script renders all of them again, so you can refresh the guide after a feature changes
the screen.

## Refresh everything

From Windows, in a WSL shell (the build runs in WSL, as `build.sh host` does):

```sh
export LAUNCHER_SRC=/home/<you>/zero7/pinfix-tree      # the launcher tree the apps build against
export LAUNCHER_TREE=$LAUNCHER_SRC
wsl -e sh /mnt/c/CLAUDE/zero7/<worktree>/apps/cassette/docs/make-doc-images.sh
```

Optional: `CDK_BUILD=~/cdk-build/<new folder>` (default `~/cdk-build/cassette-docs-img`; use a new folder per
worktree so builds do not collide) and `LVBUILD` (the LVGL host build; the first run builds it, a few minutes;
copy an earlier `_lvgl-host` folder into `$CDK_BUILD` to skip that).

What the script does:

1. Stages the app like `build.sh host` (an LF copy under `$CDK_BUILD/stage-docs`) and keeps only the scenes named in
   [`doc-images.tsv`](doc-images.tsv), so it renders about 36 scenes, not 160.
2. Builds the headless host with `_common/build_app.sh` and runs those scenes. Their own checks (`expect text ...`,
   the layout check) must pass, or the script stops. The PNGs are written to `$CDK_BUILD/host/out/<scene>/<size>/<shot>.png`.
3. Copies each wanted PNG to `docs/img/<name>` (the table below is `doc-images.tsv`).
4. Runs [`finish-images.py`](finish-images.py) with the first Python that has Pillow (`python3`, `python`,
   `python.exe`): draws the numbered markers on `annot-*` images, makes `layouts.png` (the screen sizes side by
   side) and shrinks any PNG over 100 KB. If there is no Pillow, the raw PNGs stay in place and the script says
   so; then run `python docs/finish-images.py docs/doc-images.tsv docs/img` on any machine with Pillow
   (`pip install pillow`).

The images are 10 to 70 KB each, so the folder stays small. Commit `docs/img` with the change that caused it.

## The images

Source of truth: `docs/doc-images.tsv` (tab separated: image, scene, size, shot). The table is the same list.

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
| `trim.png` | `j-trim` | 480x320 | `trim` | Trim and split page, two splits |
| `trim-zoomed.png` | `j2-trim-tapflag` | 480x320 | `trim-zoomed` | Trim page zoomed, one flag |
| `save-box.png` | `k-save` | 480x320 | `save` | Save box, three files |
| `confirm-replace.png` | `l-confirm` | 480x320 | `confirm` | Replace original confirm |
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

Not rendered by the script:

- `layouts.png`: made by `finish-images.py` from `recorder-landscape.png`, `size-*.png`.
- `real-hackberry-720.png` and `real-pi3a-portrait.png`: real board screenshots, copied by hand from
  `atlas-board-work/hackberry/20261110-014928-cassette8.png` and `atlas-board-work/pi3a/20261110-002511-monitor.png`.
  Replace them with newer captures when a layout changes (the Pi 3A+ one is 480x800).

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
