# System Monitor 0.1.0: board tests

For the Controller and the user (controller-led testing: the Controller deploys and reads logs, the user does the
physical steps). `deck.py` is `C:\CLAUDE\zero7\launcher\docs\dev\deck\deck.py` (password in `PIPW`; `DECK_HOST=<ip>`
points it at another board). The app's process name is `system-monitor` (`pgrep -x system-monitor`; never `pkill -f`).

Expected pictures: the contact sheets of `build.sh host` in
`\\wsl.localhost\Ubuntu\home\osrde\cdk-build\system-monitor\host\out\sheets\` (`overview.png`, `cpu.png`,
`memory.png`, `thermal-uv.png`, `thermal-hot.png`, `storage.png`, `network.png`, `processes.png`, `about.png`,
`period.png`, `touch.png`, `novc.png`, `offline.png`, `nocontract.png`, `notice.png`).

Wanted: the deck (Pi Zero 2 W) with its Bluetooth keyboard; the Pi 3A+ (touch only); if possible a weak or long USB
cable / a 1 A supply for the under-voltage test (B4), `stress-ng` (`sudo apt install stress-ng`, remove it after) for
B3. Write down every value that differs from the reference command by more than the tolerance given.

## 0. Deploy

```
python tools/make_registry.py --only system-monitor --out C:/CLAUDE/zero7/pkg-out
python deck.py put C:/CLAUDE/zero7/pkg-out/system-monitor_0.1.0_arm64.deb /tmp/system-monitor.deb
python deck.py sudo 'dpkg -i /tmp/system-monitor.deb'
python deck.py run '/usr/share/APPLaunch/bin/system-monitor --version; id -nG; command -v vcgencmd iw'
```

`--version`: `System Monitor (system-monitor) 0.1.0`, the launcher commit, `cp0_lvgl pinned`. `id -nG` should list
`video` (else B6 first); `command -v` shows which of `vcgencmd` and `iw` exist. Logs:
`python deck.py run 'journalctl --user -n 80 --no-pager | grep -E "cdk|system-monitor"'` (expect one line
`[system-monitor] system, every 1 s, screen 640x480`, nothing else unless a tool is missing).

## B1. Deck (640x480, Bluetooth keyboard): the home tile, the overview against the system's own tools

1. On the home grid the tile reads **Monitor** on one line under its icon (no second line over the icon; compare with
   *Serial Console* and *LAN Scan*).
2. Open it. Within a second the overview fills the screen with no scrolling: Power (top left, green frame, *Power
   OK*), CPU, Temperature, Memory, Storage, Network in 2 columns x 3 rows, the footer Processes / System / Every 1 s;
   no `...` left after 2 s; no text cut with dots; the history lines fill from the right over 2 minutes.
3. Keys: the blue ring starts on Power; Right / Left / Up / Down move it by position (Down from the bottom tiles goes
   to the footer item under them); Enter opens; Esc comes back with the ring on the same tile. Touch: one tap on a
   tile opens it at once (no highlight-first tap); the header Esc comes back.
4. Over ssh, compare (the app refreshes every second; allow for that):

   | Tile / line | Command | Tolerance |
   |---|---|---|
   | CPU % | `top -b -n 2 -d 1 \| grep '%Cpu'` (second line: 100 - id) | +-10 points |
   | CPU small line: frequency, load | `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq`, `cat /proc/loadavg`; `vcgencmd measure_clock arm` | see note |
   | Temperature | `vcgencmd measure_temp`, `cat /sys/class/thermal/thermal_zone0/temp` | +-1 °C |
   | Memory %, used / total, swap | `free -m` (used column; the app's MB are MiB) | +-5 MB |
   | Storage | `df -h /` (Use%) | +-1 % |
   | Power | `vcgencmd get_throttled` (`0x0` = *Power OK*) | exact |
   | Network: address, interface, signal | `ip -4 addr`, `ip route` (the default route's interface), `iw dev wlan0 link` | exact / a few dBm |
   | Footer: Processes N | `ls /proc \| grep -c '^[0-9]'` (refreshed every 5 s) | +-5 |

   Note for the frequency: if `measure_clock arm` is lower than the app's value while a flag is set, write it down
   (known limit: the app shows the kernel's requested frequency).
5. At the overview a short Esc (or the header's Esc) ends the app; the home grid is intact.

## B2. Deck: every page

1. **CPU**: the chart fills from the right; per-core values follow `top` (press `1` in `top` for each core);
   *Governor* = `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`; top 5 by CPU roughly as `top -o %CPU`;
   *All processes* opens Processes sorted by CPU.
2. **Memory**: used / available / cache / buffers / free / shared against `free -k` and `/proc/meminfo`; top 5 by
   memory as `ps -eo comm,rss --sort=-rss | head -6` (RSS in MB).
3. **Temp & power**: the 70 °C (amber) and 80 °C (red) lines; the flags say *never since boot* on a good supply;
   *Core voltage* = `vcgencmd measure_volts core`; *Firmware flags* = `vcgencmd get_throttled` and `(vcgencmd)`.
4. **Storage**: `/` and `/boot/firmware` as `df -h`; copy a big file (`dd if=/dev/zero of=~/t bs=1M count=200
   conv=fsync; rm ~/t`): the blue write line rises, *Writing* shows MB/s, then falls back.
5. **Network**: `wlan0 (Wi-Fi)`, the address as `ip -4 addr show wlan0`; *Network* = `iwgetid -r` (or `iw dev wlan0
   link`), *Signal* within a few dBm of `iw dev wlan0 link`; download something (`curl -o /dev/null
   http://speedtest.tele2.net/10MB.zip`): *Receiving* and the green line rise. *Bluetooth* *On* with the keyboard paired.
6. **Processes** (footer): 15 rows; Sort by switches CPU / Memory (Enter and tap); the status line shows the
   highlighted pid (`ps -p <pid> -o comm,rss,stat`); the list refreshes every 2 s without losing the highlight's place.
7. **System** (footer): kernel `uname -r`, OS `grep PRETTY /etc/os-release`, hostname, serial = the last 8 of
   `cat /proc/device-tree/serial-number`, firmware date = `vcgencmd version` (first line), *This app* (B5), Screen
   `640x480 standard, 11.3 px/mm`, Launcher `contract 1, fullscreen`.
8. **Sampling** (footer): choose *Every 5 seconds*: the footer says *Every 5 s*, values change every 5 s, the CPU chart
   says *last 10 min*. Quit and open again: still 5 s; `cat ~/.config/cyberdeckzero/system-monitor.conf` shows `period=5`.
   Set it back to 1 s.

## B3. Deck: load and heat (stress-ng)

`python deck.py run 'stress-ng --cpu 4 --timeout 120s'` (or `yes > /dev/null &` four times, then `kill %1 %2 %3 %4`).

1. The CPU tile goes near 100 % (its line climbs), the load toward 4; Processes (by CPU) lists `stress-ng-cpu` first,
   each near 100 %; the CPU page's cores near 100 %.
2. Temperature rises on its tile's line; at 70 °C the value, the line and the small line turn amber (*Warm, hot from
   80 °C*); at 80 °C red (*Hot: the Pi slows down*) (a bare Zero 2 W may not reach it in 2 min: write down the
   highest value). If the firmware soft-limits, the Power tile turns amber *Temp limit* / red *Throttled*.
3. The UI stays responsive during the load (the arrows, Enter, Esc at once).
4. After the load: values fall back; flags that occurred show *last h:mm:ss after boot* (compare with `uptime`).

## B4. Pi 3A+ (800x480, touch only): power

```
set DECK_HOST=<pi3a ip>
python deck.py put C:/CLAUDE/zero7/pkg-out/system-monitor_0.1.0_arm64.deb /tmp/system-monitor.deb
python deck.py sudo 'dpkg -i /tmp/system-monitor.deb'
```

1. The home tile reads *Monitor* on one line; open it by touch. The overview as on `thermal-uv.png` 800x480 (3 x 2,
   with your values). One tap on a tile opens its page; on a page a tap highlights a row, a second tap opens it, a
   drag scrolls; the header Esc goes back; at the overview it quits.
2. `vcgencmd get_throttled` over ssh and the Power tile agree. If the board shows under-voltage (lightning bolt,
   `0x50005` or `0x50000`): the Power tile is red (frame and tint) *Under-voltage* with *Check the supply* (or amber
   *Under-voltage*, *Happened since boot, last h:mm:ss*), Temp & power starts with the sentence and lists the flags
   with the time since boot of the last event.
3. To provoke it (only with a supply you may stress): run `stress-ng --cpu 4 --timeout 60s` on a weak supply or a
   long thin cable: watch *Under-voltage* go to *now* within 2 s of the bolt appearing, and back to *last ... after
   boot* when the load stops. Write down what you saw.
4. RSS and CPU as B5 (the 3A+ has 512 MB too).

## B5. Deck: resources and exit

1. The app's own cost, two ways, on each of: the overview, the CPU page, the Processes page (one minute each):
   - on screen: System page, *This app* (CPU of one core over the last 10 s, its vcgencmd runs included, and memory);
     open the System page from the footer, read it, go back and wait a minute on the page to measure, then read it
     again (the value covers the last 10 s, so read it within a few seconds of arriving);
   - over ssh: `python deck.py run 'P=$(pgrep -x system-monitor); grep VmRSS /proc/$P/status; top -b -n 3 -d 10 -p $P |
     grep system-mon; free -m'`.
   Target: about 1 % or less on the overview at 1 s (the brief's goal), VmRSS under 25 MB. Write down the three pairs
   of numbers and the same at *Every 5 s*. (0.1.0 before this round showed about 4 % on the CPU page.)
2. Hold Esc 3 s: the launcher's watchdog ends it; `pgrep -x system-monitor` and `pgrep -x vcgencmd` print nothing.
3. Started over ssh without the launcher: `python deck.py run 'timeout 5 /usr/share/APPLaunch/bin/system-monitor;
   echo rc=$?'`: one `[cdk] system-monitor 0.1.0: no launcher contract (started by hand ...` line, no crash (rc 124
   from the timeout).

## B6. Without the video group / without vcgencmd

1. `sudo gpasswd -d $USER video`, log out and in (or reboot). Open the app: the Power tile says *No access* / *Needs
   the video group* (amber), Temp & power starts with *Needs the video group: sudo usermod -aG video $USER*, or, if the
   kernel has the `rpi_volt` alarm, the alarm only (Power OK or Under-voltage; Temp & power: *Source: rpi_volt alarm
   only*). The journal has one line `vcgencmd refused (not in the video group?)`. Restore: `sudo usermod -aG video
   $USER`, log in again.
2. (Optional, a spare board) without `vcgencmd` and without the alarm: the Power tile says *Unknown* in grey, no
   error; the rest works (as on `novc.png`).
3. Wi-Fi off (Settings or `nmcli radio wifi off`): the Network tile says *Not connected* in amber within 5 s, rates 0
   (as on `offline.png`); on again: the address comes back within 5 s.

## B7. Pi 5 / HackBerry CM5 (if at hand)

Temp & power shows *Fan* (level and rpm; compare `cat /sys/class/thermal/cooling_device0/cur_state`) and *Supply (5 V
input)* = `vcgencmd pmic_read_adc EXT5V_V`; Storage shows the NVMe or eMMC; at 720x720 (HackBerry, keyboard rule
forced) every page as on the 720x720 column of the sheets.

Cleanup: `sudo dpkg -r system-monitor`; `rm -f ~/.config/cyberdeckzero/system-monitor.conf`; `sudo apt remove
stress-ng` if it was installed for B3.

## Unverified until a board run

| # | Assumption |
| --- | --- |
| U-1 | `vcgencmd get_throttled` every 2 s costs little CPU on the Zero 2 W and never takes near its 1 s timeout |
| U-2 | The rpi_volt hwmon `in0_lcrit_alarm` follows the under-voltage state as the firmware flag does |
| U-3 | The firmware's sysfs `get_throttled` node, where a kernel has it, holds the same hex value as vcgencmd |
| U-4 | `pmic_read_adc EXT5V_V` prints `EXT5V_V volt(N)=5.1...V` on the Pi 5 / CM5 firmware in use |
| U-5 | Reading every `/proc/<pid>/stat` every 2 s (Processes, CPU and Memory pages only) stays cheap with ~150 processes |
| U-6 | VmRSS stays under 25 MB with the charts and the FreeType sizes of the roller, the tiles and the panels |
| U-9 | The app's own CPU on the deck is about 1 % or less on the overview at 1 s (measured on the PC only: see the report) |
| U-10 | The home tile label *Monitor* stays on one line on every board's home grid (measured: 96 px of 176 px on the deck) |
| U-7 | `iw dev wlan0 link` works without root on Raspberry Pi OS (it does on Debian) |
| U-8 | The pwm-fan cooling device and the `pwmfan` hwmon are named so on the Pi 5 kernel in use |
