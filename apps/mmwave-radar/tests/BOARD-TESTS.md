# mmWave Radar: board tests (the main path, about 10 minutes)

For the owner (controller-led testing: Claude installs the draft package and reads the log, you do the physical steps).
The decoder, the listen-only detector, the link, the tracker and the geometry are tested on the PC (`build.sh host`), and
every page is rendered by the harness at 480x320, 320x480, 640x480 and larger sizes on scripted ports. What the PC cannot
tell: the real sensor on a real port, the speed of the drawing on a Pi, and whether left, right, near and far match the
room. Keep it short: the main path must work.

Board: the Pi 4B first (then the deck, zero8 at 480x320 and upright). The sensor on a USB-serial adapter (CH340), wired
5V-5V, GND-GND, TX-RX, RX-TX, mounted still on the deck facing away from the screen.

Log: `journalctl --user -u <launcher service> -f | grep mmwave-radar` (lines `[mmwave-radar] ...`). For the drawing time
start it once by hand with `MMWAVE_TIMING=1 /usr/share/APPLaunch/bin/mmwave-radar` (a line every 5 s).

## T1. Found (1 min)

1. Plug the adapter in, open mmWave Radar. Within about 2 s the chip in the top left turns green with about 19 fps; the
   log says `Found ttyUSB0 (CH340) 9600 baud`.
2. Sensor page (key 4 or the tab): Status Found, Port `ttyUSB0 (CH340)`, Baud 9600, Bad frames 0, the line "Detected
   by listening only". Close and open the app again: found at once (remembered).

## T2. Walking (3 min)

1. Walk toward the sensor from about 5 m: the dot comes down the plot toward the sensor, the arrow points at it, the
   label's distance falls; the Table's direction says Approaching and the speed is negative. Walk away: Receding.
2. Step to your right (seen from behind the sensor, looking where it looks): the dot moves right. If it moves left, set
   Orientation > Mirror and note it (the default may need to change).
3. Two people: two dots, T1 and T2 in different colours; note whether a person keeps the same T number.
4. Space: PAUSED, the plot freezes; Space again: it goes on. Graph page: the lines of the last 30 s.
5. Auto range: at 5 m the plot shows 6 m; come within 1 m and wait: it goes down to 4 then 2 m. + / - fix it; Range >
   Auto in the settings.

## T3. Speed of the drawing (2 min)

With `MMWAVE_TIMING=1` (above) and one or two people moving on the Radar page: note the `render avg` and `max`. On a
Pi 4B expect a few ms; on a Zero 2 W the redraws slow down by themselves (never more than half of the time). Does the
plot look smooth? Is the touch still quick (tabs, pause)?

## T4. No radar, busy, lost (3 min)

1. Unplug the adapter: within about 2 s "Signal lost, searching". Plug it back: found again by itself.
2. Open Serial Console on the radar's port, then mmWave Radar: "The port is used by another app"; close Serial
   Console: found within about 4 s. (Mesh Hop the same.)
3. A GPS or another serial board on the port only: "No radar found", the TRIED list names it (e.g. "GPS receiver
   (NMEA)"), the wiring table, Rescan works. The other device keeps working afterwards (nothing was sent to it).

## T4b. Tall screen and the diagnostic log (2 min, Pi 3A+ 480x800)

1. `touch ~/.config/mmwave-radar/diag`, then open mmWave Radar from the launcher with one person walking: the plot at
   the top at full width, the ring labels (`1 m`, `2 m` ...) along the left edge of the wedge, under it the T1-T3 list
   (distance, angle, speed, direction; empty slots `-`) and the 30 s distance strip; nothing under the tab strip.
2. Close the app: `~/.config/mmwave-radar/mmwave.log` begins with `log on (marker ...)` and has a line a second. Send it
   if something looked wrong, then `rm ~/.config/mmwave-radar/diag` (no log file is written without it).

## T4c. Bluetooth, HLK-LD2451 (5 min, Pi 3A+; the radar powered, its UART free)

`bluetoothctl show` says `Powered: yes` (the app never switches it on). Keep the diagnostic marker of T4b.

1. Sensor > Connection to **Bluetooth**, then the Radar page: "Searching for radars over Bluetooth..." and within a
   few seconds a row `HLK-LD2451_xxxx  -NN dBm  XX:XX` (no other device listed). After 10 s the scan stops ("Choose a
   radar"), another starts 20 s later. Does Wi-Fi (the status strip, an SSH session) stay usable during the scan?
2. Tap the row: "Connecting to HLK-LD2451_xxxx" (blue dot), then the plot within about 10 s; walk in front: the target
   moves as over USB, the Sensor page says `HLK-LD2451_xxxx (Bluetooth)`, Baud `-`, Bluetooth `HLK-LD2451_xxxx`. The
   log says `Found HLK-LD2451_xxxx (Bluetooth)` (no baud), within about 2 s of "Connecting" ending for an idle radar.
3. Power the radar off: "Signal lost, reconnecting" after about 5 s, retries at 2, 4, 8 ... s; power it on: found
   again by itself. End the app and start it again: it connects to the remembered radar without the list.
4. End the app (Esc): `bluetoothctl info <address>` says `Connected: no` within a few seconds; the phone app can reach
   the radar again. The log's `ble ...` line says AcquireNotify or StartNotify. `mmwave.log` (marker `diag`) has the
   `start ... connection=bluetooth ble=... name=...` line and the `ble-scan`, `ble-seen`, `ble-choose`, `ble-connect`,
   `ble-subscribe`, `ble-found` lines of this run in that order, and at the end one `ble-disconnect ... result=ok` (or
   `ok-already-disconnected`), never an `error:`. A target that jumps tens of metres in one frame gets a new id.
5. Connection **Auto** with the USB adapter plugged: the USB radar is used, Bluetooth stays off (`bluetoothctl info`:
   not connected); unplug it: after the scan the Bluetooth radar is connected. Back to **USB** at the end.

## T5. Exit (30 s)

Esc key and the header's Esc end the app; the port is free again (Serial Console can open it).

Cleanup: `sudo dpkg -r mmwave-radar`; settings in `~/.config/mmwave-radar/` (delete to reset).
