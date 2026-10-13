# System Monitor

Version 0.1.0 (draft, not published yet), package `system-monitor`. Part of [cyberdeck-zero-apps](../../README.md).

System Monitor is a live, read-only look at the Raspberry Pi it runs on: CPU load per core, temperature, frequency,
memory, storage, network, processes and the **power supply** (under-voltage and throttling from the firmware's flags,
in plain words). It is a full-screen touch app made for the deck's small screen (Pi Zero 2 W, 640x480) and works the
same on the Pi 3A+ (800x480, touch only), the Pi 5 and the HackBerry CM5. It changes nothing on the system: no kill,
no writes except its own one-line settings file.

Kind: a native `cp0_lvgl` app (kind A of the [guidelines](../../docs/APP-GUIDELINES.md)), because it has its own
pages, live charts and must be complete by touch and by keys; no existing program fits the small screen.

## Screenshots

To be added from the deck and the Pi 3A+ (640x480 and 800x480). Until then the headless contact sheets of
`build.sh host` show every page at the five sizes (see [Build and test](#build-and-test)).

## What it does

* **Overview** (the first page). At the top, a bar per CPU core with its load, and one status line: *Power OK* in
  green, or the most urgent problem in amber or red (*Under-voltage now: check the power supply*, *Throttled now: the
  CPU is too hot*, *CPU hot: 82 °C, the Pi slows down from 80 to 85 °C*, *Under-voltage happened since boot*...).
  Under it, one row per headline, each opening its page: **CPU** (total %), **Temperature** (amber from 70 °C, red
  from 80 °C), **Frequency** (current / maximum), **Memory** (used / total), **Swap**, **Load** (1, 5, 15 min),
  **Uptime**, **Storage** (the system disk: % used of its size; amber from 85 %, red from 95 %), **Power** (*OK*,
  *Under-voltage now*, *Under-voltage since boot*, *Throttled now*...; only when the system tells it, see
  [Requirements](#requirements)), **Network** (the interface of the default route and its address), **Processes**
  (how many), **System** (the model), **Sampling** (every 1, 2 or 5 s).
* **CPU**: a chart of the total load over the last 120 samples (2 minutes at 1 s), each core's %, the frequency,
  the governor, the load average, running / total tasks, the top 5 processes by CPU and *All processes*.
* **Memory**: a chart of RAM (green) and swap (blue) use; used (with %), available, cache, buffers, free, shared,
  swap; the top 5 processes by memory and *All processes*. "Used" is counted as `free` counts it: total minus available.
* **Temp & power**: a chart of the CPU temperature with the 70 °C (amber) and 80 °C (red) marks; the power sentence
  first; temperature, state (OK / warm / hot), the highest value of the chart; the fan (level of the cooling device and
  speed in rpm) when there is one; then each firmware flag (*Under-voltage*, *Frequency capped*, *Throttled*, *Soft temp
  limit*) with *now*, the time since boot of the last event the app saw (*last 0:41:12 after boot*), *before the app
  started* (the firmware's "has occurred" bit with no event seen) or *never since boot*; the 5 V input on a Pi 5 /
  CM5, the core voltage, the raw flags (`0x50005`).
* **Storage**: a chart of the system disk's read (green) and write (blue) rates; each real filesystem (on a block
  device; network, RAM and loop filesystems are left out) with used / size and %; the disk's rates and the bytes read
  and written since boot.
* **Network**: a chart of the main interface's in (green) and out (blue) rates; per interface its state, IPv4
  address, for Wi-Fi the network name, signal and bit rate (from `iw`, while the page is open) or the signal from
  `/proc/net/wireless`, the rates and the totals since boot; Bluetooth (on, off, connected, no adapter).
* **Processes**: the top 15 by CPU or by memory (*Sort by*, Enter or a tap switches), name, %CPU and memory (RSS);
  the status line under the title gives the highlighted one's pid, state, memory and CPU. %CPU is the share of one
  core, as `top` shows it (a process busy on two cores shows 200 %). Read-only.
* **System**: model, kernel, OS, hostname, the last 8 digits of the serial number, firmware date (from
  `vcgencmd version`), CPU, RAM, uptime, the screen as the launcher's layout service sees it, the launcher contract,
  the app version.
* **Sampling**: every second, 2 or 5 seconds; remembered. Changing it starts the charts again (their samples would no
  longer be evenly spaced); at 5 s a chart covers 10 minutes.

Values refresh once per period on a thread of their own; the screen never waits for them. Rates and percentages need
two samples: they appear a third of a second after the start.

## Keys and touch

| Function | Keys | Touch |
|---|---|---|
| Move through the rows | Up / Down | a tap on a row highlights it; drag to scroll |
| Open a page, switch Sort by, choose a period | Enter or Right | a tap on the highlighted row |
| Back one page | Esc or Left | the header's Esc |
| Quit (at the overview) | a short Esc | the header's Esc |
| Stop a stuck app | hold Esc 3 s (the launcher's watchdog) | |

The panels (bars, charts) are not touch targets: every function is a row.

## Requirements

* A screen of at least **640x480** (`X-MinSize=640x480`; under it the app says so and closes on any key) and a
  CyberdeckZero Launcher with full-screen app support (contract 1).
* A Raspberry Pi running Linux (tested in the harness with fixtures of a Pi Zero 2 W, a Pi 3A+ and a Pi 5). Every
  number comes from `/proc` and `/sys` except:
  * the firmware's power flags, firmware date and voltages: **`vcgencmd`** (package `raspi-utils` on Raspberry Pi OS),
    run with a 1 s timeout on the sampler thread: `get_throttled` every 2 s, the voltage every 10 s
    (`pmic_read_adc EXT5V_V` on a Pi 5 / CM5, else `measure_volts core`). It needs the **video** group:
    `sudo usermod -aG video $USER`, then log out and in. Without `vcgencmd` the app says nothing and leaves the flags
    out; with the `rpi_volt` hwmon alarm (most Pi kernels) it still shows *under-voltage now* / *since the app
    started*. Refused (not in the group): the Power row says *Needs the video group* with the command.
  * the Wi-Fi network name and bit rate: **`iw`** (optional; without it only the signal level shows).
* No new package dependency (`depends` = the template's `libfreetype6, libinput10, libxkbcommon0`), no network, no
  root, no special hardware.

## Install

From **Settings > Apps** once published (the app is a draft for now). By hand:

```sh
python3 tools/make_registry.py --only system-monitor --out <a folder outside packages/>
sudo dpkg -i system-monitor_0.1.0_arm64.deb      # remove: sudo dpkg -r system-monitor
```

## Files the app keeps

| Path | What |
|---|---|
| `${XDG_CONFIG_HOME:-~/.config}/cyberdeckzero/system-monitor.conf` | the sampling period (`period=1`); written through a temporary file and a rename. Delete it to reset. `SYSTEM_MONITOR_CONFIG` names another file |

Nothing else is written. Logs go to stderr with the tag `[system-monitor]` (the launcher's journal): the source and
period at start, `vcgencmd` or `iw` missing or refused (once), a settings file that could not be saved.

## Known limits

* **Fixture-tested only so far**: the parsers and the pages ran on recorded `/proc` and `/sys` files of a Pi Zero 2 W,
  a Pi 3A+ and a Pi 5 and on the build PC's own `/proc`; the board protocol (`tests/BOARD-TESTS.md`) is still to run.
* The firmware keeps no time for its "has occurred" flags: the time of the last event is the app's own observation
  (the last sample that showed it). An event that happened before the app started shows as *before the app started*.
* The current frequency is the kernel's `scaling_cur_freq`; when the firmware caps the clock (under-voltage, heat) the
  real ARM clock can be lower than shown (`vcgencmd measure_clock arm` tells it). The flags say when that happens.
* Supply voltage: only the Pi 5 / CM5 measure their 5 V input. Older boards only have the under-voltage alarm; the
  *core voltage* shown there is the SoC's, not the supply.
* Bluetooth "on" means an adapter not blocked by rfkill (as the launcher's status strip counts it), not BlueZ's
  *Powered* property.
* The core bars are drawn for boards up to 4 cores in two rows; a board with more cores (the build PC) takes more
  room from the rows.
* `vcgencmd` every 2 s costs a process start (a few ms of CPU on the Zero 2 W); the firmware's sysfs node
  `get_throttled` is read instead when the kernel has it (path searched under `/sys/devices/platform`).
* No history across restarts; no process actions (read-only by design).

## Credits and licences

<!-- branding-allow-begin: credit of the cp0_lvgl runtime linked into the binary (MIT) -->
MIT, 2026 OSRdesign. The binary contains the launcher's `cp0_lvgl` runtime (M5Stack Technology CO LTD, MIT) and
LVGL (LVGL Kft, MIT); see `root/usr/share/doc/system-monitor/copyright`. The fonts are the launcher's, read at run
time; none is shipped.
<!-- branding-allow-end -->
At run time the app reads the Linux kernel's `/proc` and `/sys` interfaces and, when installed, runs `vcgencmd`
(Raspberry Pi firmware tools, `raspi-utils`, BSD-3-Clause) and `iw` (ISC); neither is shipped or modified. The
temperature thresholds follow the Raspberry Pi documentation (soft throttling from 80 °C, hard limit at 85 °C); the
meaning of the `get_throttled` bits is the firmware's (Raspberry Pi documentation, *vcgencmd*).

## Build and test

In WSL (the launcher build tree `~/zero7/launcher` with its SDK; see `../_common/build_app.sh -h`):

```sh
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/system-monitor/build.sh host           # tests + scenes
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/system-monitor/build.sh all --strict   # + aarch64 binary
python3 tools/make_registry.py --only system-monitor --out <a folder outside packages/>       # the .deb (draft)
```

* `src/core/`: no LVGL. `parsers` (stat, meminfo, loadavg, uptime, net/dev, route, wireless, `iw` link, diskstats,
  mounts, thermal, cpufreq, `get_throttled`, voltages, firmware version, `/proc/<pid>/stat`, model, OS, serial,
  cpuinfo), `stats` (history ring, counter rates, process table and top-N, chart scales), `power` (temperature
  state, flag tracker, the wording), `source` (the system, or a fixture folder), `command` (one command with a
  timeout), `sampler` (the thread), `settings`, `monitor_core` (navigation and every page's rows).
* `src/ui/`: `monitor_app` (pages on `cp0::ui::Page` and `cp0::ui::Roller`), `panels` (the core bars and the
  `lv_chart` history chart).
* `tests/`: unit tests (`test-system-monitor`, 515 checks) on `tests/fixtures/<board>/` (recorded `/proc` and `/sys`
  files of a Pi Zero 2 W, a Pi 3A+ with `throttled=0x50005`, a Pi 5 with a fan, NVMe and 16 kB pages; two snapshots
  one second apart; a process table; a missing and a refused `vcgencmd`); headless scenes in `tests/scenes` on the
  same fixtures with a scenario that makes the counters move (`SYSTEM_MONITOR_FIXTURE=<board>:<scenario>`: the deck
  at work, the Pi 3A+ under-voltage, a hot Pi 5) at 640x480, 800x480, 720x720, 1280x720 and 1024x600, the notice
  under 640x480 and a start without the launcher.
* Board protocol: [`tests/BOARD-TESTS.md`](tests/BOARD-TESTS.md).

The same fixture variable works on a board, for screenshots of a state that is hard to cause:
`SYSTEM_MONITOR_FIXTURE=/path/to/fixtures/pi-3a-plus:undervoltage /usr/share/APPLaunch/bin/system-monitor`.

## Changes

* 0.1.0 (draft): first version.
