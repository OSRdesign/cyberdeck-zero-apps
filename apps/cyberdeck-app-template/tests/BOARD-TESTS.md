# App Template: board tests of spike S0 (report 024 section 4: T1-T4, T6-T8)

For the Controller and the user (controller-led testing: the Controller deploys and reads logs, the user does the
physical steps). Commands run from Windows: `deck.py` is `C:\CLAUDE\zero7\launcher\docs\dev\deck\deck.py` (password in
`PIPW`; `DECK_HOST=<ip>` points it at another board), `board.py` is `C:\CLAUDE\zero7\launcher\tools\boards\board.py`.
Remote commands are in single quotes (Git Bash and PowerShell pass them unchanged: `$(...)` and `$?` must reach the
board's shell). Nothing here uses `pkill -f`: the app's process name (`comm`) is its binary name cut to 15 characters,
`cyberdeck-app-t`, so it is found with `pgrep -x cyberdeck-app-t` (`pgrep -ax cyberdeck-app-t` to see the command
line). It no longer collides with the launcher, whose binary is now `cyberdeckzero`.

Expected pictures: the contact sheets `\\wsl.localhost\Ubuntu\home\osrde\cdk-build\cyberdeck-app-template\host\out\sheets\`
(`template.png`: every page at 640x480, 800x480, 720x720, 1280x720, 1024x600; `notice.png`; `nocontract.png`), made by
`build.sh host`.

## 0. Build and deploy

```
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/cyberdeck-app-template/build.sh all
python tools/make_registry.py --only cyberdeck-app-template --out C:/CLAUDE/zero7/pkg-out
```

Deck (zero7.local; launcher 0.6.0 or later, the user service):

```
python deck.py put C:/CLAUDE/zero7/pkg-out/cyberdeck-app-template_0.1.0_arm64.deb /tmp/cdk-template.deb
python deck.py sudo 'dpkg -i /tmp/cdk-template.deb'
python deck.py run '/usr/share/APPLaunch/bin/cyberdeck-app-template --version'
```

`--version`: `Template (cyberdeck-app-template) 0.1.0`, the launcher commit and `cp0_lvgl pinned` or `same-tree`.
The tile "App Template" appears on the home grid within a few seconds (directory watcher).

Logs while testing (the app's stderr goes to the launcher's journal):

```
python deck.py run 'journalctl --user -u APPLaunch.service -n 60 --no-pager | grep -E "cdk|External app|process|ESC"'
```

Remove after the tests: `python deck.py sudo 'dpkg -r cyberdeck-app-template'`.

## T1. Deck: fullscreen, launcher look, status strip identical

1. Open App Template from the home grid (Enter or a tap). Expect: the whole panel, black; gold title "Template" top
   left, the grey "Esc" button, the clock / Wi-Fi / Bluetooth strip top right exactly where the home grid has it; the
   roller with "Device info" highlighted, rows Name (green "Deck"), Example switch, Reset, Run a command, Screen
   ("640x480 standard"). Compare with `template.png`, column 640x480, row "root".
2. Log: `[cdk] cyberdeck-app-template 0.1.0: launcher contract 1, mode fullscreen, canvas 640x480` and
   `[cdk] cyberdeck-app-template: display 640x480 (dpi-scaled backend)`.
3. Strip identity, within one minute (the clock): with the app at its root
   `python deck.py shot C:/tmp/app-root.png`, then Esc, then on the home grid `python deck.py shot C:/tmp/home.png`, then
   ```
   python -c "from PIL import Image, ImageChops as C; a=Image.open('C:/tmp/home.png').convert('RGB').crop((421,0,640,56)); b=Image.open('C:/tmp/app-root.png').convert('RGB').crop((421,0,640,56)); d=C.difference(a,b).getbbox(); print('identical' if d is None else d)"
   ```
   (421 = the strip's left edge at 640x480, 56 = the header; from the harness report.) Expect `identical`. A
   difference only around x 450-520 means the Wi-Fi bars differ: see I-3.
4. Responsiveness: Up / Down on the BT keyboard 10 times fast, drag the roller with a finger: the highlight follows
   without lag, the scroll snaps to a row.

## T2. Deck: Esc from every source, home grid intact, screen.state

Before: `python deck.py run 'grep -E "^(w|h|gen)=" /run/user/1000/applaunch/screen.state'` (note gen).

1. Open the app, short Esc on the **Bluetooth keyboard** at the root: the app ends at once, the home grid is back with
   its tiles and strip. Log: `External app exited with code 0`.
2. Plug a **USB keyboard**, open the app, Down, Enter (Name opens), Esc (editing stops), Esc (back to the root), Esc:
   the app ends.
3. Touch only: open the app, tap "Device info" (moves the highlight), tap it again (the table opens), tap the header
   **Esc** button: back to the root (the app stays); tap Esc again: the app ends. The touch that ended it does not
   click a home tile.
4. Hold-Esc watchdog: open the app, open Name (the field edits), hold Esc 3 s. The app ends (by its own Esc handling or
   the watchdog). Log: either `exited with code 0`, or `[process] ESC timeout: SIGTERM` followed by
   `[cdk] cyberdeck-app-template: signal 15, ended cleanly` and no `SIGKILL` line.
5. SIGTERM directly: open the app, Run a command (Enter on "Run a command"), at once
   `python deck.py run 'kill -TERM $(pgrep -x cyberdeck-app-t); sleep 2; pgrep -af "[s]tep .i of 5" || echo no-child-left'`
   Expect `no-child-left` and the home grid back.
6. After: the screen.state command again: `w=640`, `h=480`, `gen` higher than before (the launcher rewrites it after
   every app; the app itself never writes it).

## T3. Deck: the app's own on-screen keyboard

The rule treats a Bluetooth keyboard that went away as asleep for 600 s, so switching it off is not enough; the
launcher's restart starts its timers again.

1. Bluetooth keyboard OFF, no USB keyboard. Restart the launcher:
   `python deck.py run 'setsid nohup systemctl --user restart APPLaunch.service > /dev/null 2>&1 < /dev/null &'`;
   check `python deck.py run 'grep -E "^(keyboards|vkb|gen)=" /run/user/1000/applaunch/input.state'`: `keyboards=0`,
   `vkb=auto`.
2. Open the app by touch, tap "Name" twice: the Name page, the field editing (blue border, cursor) and the keyboard at
   the bottom (Esc / Tab / Ctrl / arrows row, letters, Enter in blue); the field stays visible above it. Compare
   `template.png` row "form-osk".
3. Type `abc` on the on-screen keys, tap its Enter: back to the root, Name shows "Deckabc".
4. Open Name again, delete everything with the on-screen backspace, tap Enter: the red line "Type a name first" right
   under the field, above the keyboard (`form-error`).
5. With the keyboard still up, switch the Bluetooth keyboard ON. When it is connected (`keyboards=1` and a new `gen` in
   input.state), the on-screen keyboard goes within about 1 s; typing on the BT keyboard goes into the field.
6. Esc, Esc: back to the root.

## T4. HackBerry 720x720 (lightdm stopped), keyboard rule forced

```
python board.py test-start hackberry --env APPLAUNCH_OSK=force        (a staged launcher 0.6.0+ in ~/applaunch-test)
set DECK_HOST=192.168.68.214
python deck.py put C:/CLAUDE/zero7/apps-s0/apps/cyberdeck-app-template/root/usr/share/APPLaunch/bin/cyberdeck-app-template applaunch-test/bin/cyberdeck-app-template
python deck.py put C:/CLAUDE/zero7/apps-s0/apps/cyberdeck-app-template/root/usr/share/APPLaunch/applications/cyberdeck-app-template.desktop applaunch-test/applications/cyberdeck-app-template.desktop
python deck.py put C:/CLAUDE/zero7/apps-s0/apps/cyberdeck-app-template/root/usr/share/APPLaunch/share/images/cyberdeck-app-template.png applaunch-test/share/images/cyberdeck-app-template.png
python deck.py run 'chmod 755 ~/applaunch-test/bin/cyberdeck-app-template; ~/applaunch-test/bin/cyberdeck-app-template --version'
```

(/usr/share/APPLaunch is a link to ~/applaunch-test while test-start runs: do NOT dpkg-install there.)

1. Open the app: square layout as `template.png` column 720x720 (header 64 px, rows 70 px, the roller 560 px wide and
   centred).
2. Built-in keyboard: Up / Down / Enter / Esc work as on the deck.
3. Touch accuracy: tap each row (the highlight lands on the row tapped), tap the header Esc.
4. Open Name: the on-screen keyboard shows although the built-in keyboard is there (`vkb=force`, as `form-forced`);
   typing on either keyboard goes into the field.
5. Log: `python deck.py run 'grep -E "cdk|display-profile" ~/applaunch-test/run.log | tail'`: `display 720x720`.
6. `python board.py test-stop hackberry` (removes the test files, starts lightdm).

## T6. Pi 3A+ (480x320): hidden, and the notice when started anyway

```
set DECK_HOST=192.168.68.170
python deck.py put C:/CLAUDE/zero7/pkg-out/cyberdeck-app-template_0.1.0_arm64.deb /tmp/cdk-template.deb
python deck.py sudo 'dpkg -i /tmp/cdk-template.deb'
```

1. Home grid: no "App Template" tile; Settings > Apps does not list it either (X-MinSize=640x480).
2. Started by hand (an older launcher would show the tile): stop the launcher and run the app with the board profile:
   `python deck.py run 'systemctl --user stop APPLaunch.service; set -a; . /etc/applaunch/board.conf; set +a; APPLAUNCH_STD_KEYBOARD=1 timeout 60 /usr/share/APPLaunch/bin/cyberdeck-app-template; echo rc=$?'`
   The panel (upright) shows "Template needs a screen of at least 640x480. This screen is 480x320. Press any key or
   tap the screen to close." (`notice.png`). A key on the USB keyboard or a tap closes it: `rc=0`.
3. `python deck.py run 'setsid nohup systemctl --user restart APPLaunch.service > /dev/null 2>&1 < /dev/null &'`,
   then `python deck.py sudo 'dpkg -r cyberdeck-app-template'`.
   (The serial console part of T6 belongs to task A2.)

## T7. Deck: memory and CPU

With the app open at its root for 10 s, then again after visiting every page (Device info, Name with the on-screen
keyboard, the dialog, Run a command):

```
python deck.py run 'p=$(pgrep -x cyberdeck-app-t); grep -E "VmRSS|VmHWM|Threads" /proc/$p/status; top -b -n 2 -d 3 -p $p | tail -2; free -m'
```

Expect VmRSS and VmHWM under 25 MB, %CPU at idle (second `top` sample) a few percent at most, `free -m` with launcher
and app running leaving well over 250 MB available.

## T8. Deck: started over ssh without the launcher's environment

```
python deck.py run 'grep -E "^gen=" /run/user/1000/applaunch/screen.state; systemctl --user stop APPLaunch.service; sleep 1; timeout 45 /usr/share/APPLaunch/bin/cyberdeck-app-template; echo rc=$?; grep -E "^gen=" /run/user/1000/applaunch/screen.state'
```

1. The app draws full screen at 640x480 as under the launcher. Its first log line (in the command's output):
   `no launcher contract (started by hand or by an older launcher)`.
2. Keys (BT or USB keyboard) work. Device info: "Launcher: no contract". Touch may be transposed: the deck's touch axes
   come from the service's environment (`APPLAUNCH_TOUCH_SWAP_XY=1`, `APPLAUNCH_TOUCH_INVERT_Y=1`), absent here: an
   expected degrade. With them prefixed to the command, touch is right.
3. Esc at the root ends it with `rc=0`; left alone, `timeout` ends it after 45 s with SIGTERM (`rc=124`, and
   `signal 15, ended cleanly`). Both `gen=` lines are equal: the app wrote no screen.state.
4. `python deck.py run 'setsid nohup systemctl --user restart APPLaunch.service > /dev/null 2>&1 < /dev/null &'`

## Unverified assumptions (I) the first board run settles

| # | Assumption | Seen in |
|---|---|---|
| I-1 | The fullscreen child gets `APPLAUNCH_DISPLAY=dpi-scaled` and the board profile from the launcher's environment, and its backend opens the framebuffer and the touch device while the launcher keeps them open (no exclusive access) | T1, T4 |
| I-2 | `cp0_display_set_mode(NATIVE)` after init gives a 1:1 picture of the whole panel, rotation included, and the compat display never draws | T1, T4 |
| I-3 | The shared `cp0::ui::StatusStrip` fed by its default source (sysfs / procfs, `cp0_statusbar_read_state`) matches the home grid's (which feeds it NetworkManager signal and BlueZ): the Wi-Fi bar count may differ near a threshold | T1 |
| I-4 | The app's libinput reads every keyboard while the launcher's is suspended and its Esc watcher reads the nodes raw: no key lost, none doubled (the `/dev/input/bt-keyboard` link and the presence list name the same node) | T2, T3 |
| I-5 | The launcher, its timers off, does not act on touches made in the app; the touch that closes the app does not click a home tile (`touch_swallow(2)`) | T2.3 |
| I-6 | The launcher rewrites `input.state` while a fullscreen app runs (its presence watcher is a thread, not an LVGL timer: read in the code) and `APPLAUNCH_STATE_DIR` points at it | T3.5 |
| I-7 | On the watchdog's SIGTERM the app ends within the grace (no SIGKILL) and its commands' process groups die | T2.4, T2.5 |
| I-8 | At `-O2` the roller, the on-screen keyboard and the spinner are smooth on the Zero 2 W; idle CPU low; VmRSS < 25 MB | T1.4, T7 |
| I-9 | `/usr/share/APPLaunch/share/font/Montserrat-*.ttf` exist on every board (FreeType rows; else the builtin fallback, a different look) | T1, T4 |
| I-10 | The package's Depends names exist on trixie arm64: `libinput10`, `libxkbcommon0`, `libfreetype6` | install |
| I-11 | `X-MinSize=640x480` hides the tile on the Pi 3A+ home grid and in Settings > Apps | T6.1 |
| I-12 | HackBerry: its built-in keyboard counts as a real keyboard; touch (EP0110M09) maps right in buffer orientation | T4 |
| I-13 | After the app exits (its backend cleared the framebuffer at start), the launcher repaints the whole home grid | T2 |
| I-14 | `init_lvgl_env()` forces `XDG_RUNTIME_DIR=/run/user/1000` in every cp0_lvgl process: the deck user must be uid 1000 for a start by hand to find the state files | T8 |
| I-15 | A short Esc quits on its press; its release reaches nobody (the launcher's keyboard resumes after the app exits) | T2.1 |
| I-16 | Left at the root also ends the app (the Roller's "Left goes back"): acceptable or not? | T2 |
