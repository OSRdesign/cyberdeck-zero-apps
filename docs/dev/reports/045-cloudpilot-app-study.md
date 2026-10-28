# Report 045: CloudpilotEmu as a launcher app (study)

Status: study 2026-10-28, branch `study-cloudpilot` of the apps repo (from `origin/main` 31779ac); no product code.
Upstream: [CloudpilotEmu](https://github.com/cloudpilot-emu/cloudpilot-emu) tag 2.3.0, commit 064d5d5 (2026-10-02),
cloned with its submodule into WSL `~/zero7/cloudpilot-study` (outside every repo). (S) = read in the source,
(M) = measured here, (I) = inferred. **No ROM was fetched, opened or used.** Naming: this report writes "the OS name",
"the alphabet's trademark" and "the company name" for the three words of section 5 (rule of the owner).

## 1. Summary

- **Feasible, and the core is a clean library.** Each emulator is a core (`SOURCE_C`/`SOURCE_CXX` of its Makefile, no
  SDL) plus a small SDL frontend (`native/`, 6-20 files). Both cores compiled for arm64 with the stock cross gcc,
  without SDL or curl (readline only in the CLI file, which B leaves out) (M). API: `EmSession` (OS 4), `SoC` (OS 5).
- **The upstream native frontends are developer tools**, not products: CLI on stdin (end of input = quit), fixed
  2x/3x window, mouse only, no on-screen buttons, no audio for OS 4, no keyboard text on OS 5 (S: README).
- **Fit:** portrait is natural. At 480x800 (Pi 4 B, PDA mode) a 160x160 device with its writing area is 3x
  (480x660), the 240x320 device 2x, the paravirtualised OS 5 build 1x at 480x720. Landscape 640x480 / 800x480 under
  the standard 56 px header does not fit a 2x 160x220 device (needs 440 of 424 px).
- **Speed (M, no ROM):** a synthetic loop through the OS 5 interpreter runs at 84-97 MIPS on one i5-12450H core;
  upstream's default target is 100 MIPS. OS 5 on the Pi Zero 2 W and Pi 3A+ is likely too slow (I); OS 4 (16-33 MHz
  68k) should be fine everywhere (I). A ROM-free arm64 probe is built for the owner to run on the boards.
- **Recommendation: option B**, a native `cp0_lvgl` app linking the cores (picker, canvas, pen, keys, buttons,
  sessions), OS 4 first (about 3-3.5 sessions), OS 5 as a second binary after a board speed check. Option A (wrap the
  SDL frontends with the viz1090 shim) costs about as much (3-4 sessions, both OS generations) for a worse app.
- **Branding:** never commit upstream source (under `src/` it names the OS in 1,931 lines and 117 file paths, the
  trademark in 88 lines): fetch it at build time; never use upstream device IDs (they carry the OS name); neutral name.

## 2. What upstream offers

### 2.1 Layout (S)

| Path | What | Used by B |
| --- | --- | --- |
| `src/cloudpilot/emulator/` | OS 4 core: POSE-derived, UAE 68k CPU, Dragonball EZ/VZ/SZ/328 and device registers, sessions, savestates, database install/export, cards, network proxy glue | yes |
| `src/cloudpilot/native/` | SDL2 frontend `cloudpilot-emu`: `main.cpp`, `MainLoop.cpp`, `EventHandler.cpp`, readline CLI, GDB stub, network proxy (curl websocket) and native sockets (`network-backend` submodule) | no (network backend maybe) |
| `src/uarm/uarm/` + `MainLoop.cpp` | OS 5 core: uARM fork (ARM interpreter, PXA SoC of the 320x320 ARM device, the paravirtualised build "pv"), host-side 68k for legacy apps, audio queue, savestates | yes |
| `src/uarm/native/` | SDL2 frontend `cp-uarm`: renderer, events, SDL audio, CLI | no |
| `src/common/` | sessions, savestate chunks, zip/gzip, ROM info for OS 5, CLI, file utils (`libcommon.a`) | yes (not `Cli.cpp`) |
| `src/skins/`, `web/`, `server/` | SVG skins, Angular web app, Python network proxy | no |

### 2.2 How a core is driven (S)

| Need | OS 4 core | OS 5 core |
| --- | --- | --- |
| Start | `EmROMReader` + `EmDevice::SupportsROM` auto-detects the device from the ROM; `gSession->Initialize(device, rom)` | `RomInfo5` detects; `SoC` built from NOR (+ NAND, RAM size, display mode) |
| Run | `gSession->RunEmulation(cycles)`, paced by `GetClocksPerSecond()` | `MainLoop::Cycle(now)` → `soc->Run`, target `--mips` (default 100), max load 97 % |
| Frame out | `EmHAL::CopyLCDFrame(frame)`: 1/2/4 bpp grey or 24 bpp, dirty lines, `IsScreenDirty()` | framebuffer from the SoC, dirty flag |
| Pen in | `QueuePenEvent(PenEvent::down(x, y) / up())`, digitiser covers the writing area under the LCD | pen events, same idea |
| Keys in | `QueueKeyboardEvent(char)` (ASCII/Latin-1, arrows, backspace, tab); `QueueButtonEvent` (app 1-4, rocker, power, cradle, contrast, antenna, jog) | hardware keys only (`keyIdHard1-4`, 5-way, power); **no text** (README: not available on OS 5) |
| Audio | event `EmHAL::onPwmChange(freq, duty)` (beeper); the native frontend ignores it | PCM `AudioQueue` (44.1 kHz) |
| State | `SessionImage` = ROM + RAM + savestate + metadata in one file (`SaveImage` / `LoadImage`) | `SessionFile5` = NOR + NAND + RAM + savestate |
| Storage | SD / Memory Stick images (`ExternalStorage`), `.gz` read transparently | SD image (`--sd`) |
| Install | `.prc`/`.pdb` via `DbInstaller` (CLI `install`) | `db_installer` (CLI `install`) |
| Network | NetLib redirected to host sockets (`--net-native`) or to the Python proxy | none (README) |

Screens (S): OS 4 `ScreenDimensions` 160x160 (+60 writing area), 320x320 (+120), 240x320 and 320x480 (writing area
drawn by the OS); OS 5 320x320 (+120) for the ARM device, the pv build any of 160x160 ... 480x720 (8 modes).

### 2.3 The native frontends as they are (S, M)

- Binaries `cloudpilot-emu image` and `cp-uarm nor_or_session`; both reject a non-ROM file cleanly (M).
- A readline CLI thread reads **stdin**; at end of input it runs `quit` (`Cli.cpp`), so started by the launcher with an
  empty stdin it would exit at once (I). Saving is only by CLI (`save-image`, `save-session`); nothing is saved at quit.
- Window = device size x2 or x3 (`--small-window`: x1/x2); pen from `SDL_MOUSE*` + `SDL_GetMouseState`; no touch.
- Keys: OS 4 text from `SDL_TEXTINPUT`, hardware buttons on Ctrl+Shift+U/I/O/P/J/K/L/M/W/S/X; OS 5 F1-F4 = app
  buttons, arrows, Enter = select, **Esc = power**. Silkscreen art (embedded PNG): neutral icons and "abc"/"123".
- Audio: none in `cloudpilot-emu` (SDL video + events only); SDL audio in `cp-uarm`.

### 2.4 Licences (S)

Repository `LICENSE`: GPL-3.0. Bundled: nanopb (zlib), argparse (MIT), ArduinoJson (MIT), FatFs (BSD-style),
dosfstools (GPL-3.0), `network-backend` submodule (Unlicense), uri.h. The uARM-derived files carry only a
"(c) uARM project" header and no licence file of their own: **check upstream's statement on uARM before publishing**.
Consequences: B links GPL code into our binary, so the shipped binary is GPL-3.0 (our frontend source may stay MIT,
which is compatible); A ships upstream binaries next to our MIT shim. Either way a separate app folder and package
(as viz1090 with GPL readsb), `license` e.g. `GPL-3.0-only AND MIT` (SPDX to confirm), a Debian `copyright` with every
component, and the GPL source offer: upstream URL and exact commit, our repo, the build script; safest is the upstream
source tarball attached to the GitHub Release that hosts the `.deb`. Credits: CloudpilotEmu (Christian Speckner),
POSE, uARM (Dmitry Grinberg), the libraries above.

### 2.5 Build (M)

| Item | Result |
| --- | --- |
| Native deps (upstream) | SDL2, SDL2_image, readline, libcurl (proxy needs curl >= 8 with websockets), pthread, resolv; C++17 |
| Native build, x86-64 WSL | `make bin` with a private root of the `-dev` packages (no sudo): about 2 min clean on 12 threads; both binaries run |
| SDK arm64 sysroot | SDL2, SDL2_image, ALSA runtime libraries and curl, but no SDL, readline or ALSA headers or link names (M) |
| Stripped size x86-64 | `cloudpilot-emu` 2.59 MB, `cp-uarm` 2.37 MB; NEEDED: SDL2, SDL2_image, readline, curl (OS 4), libstdc++ |
| Cores only, arm64 | `aarch64-linux-gnu-g++` 13, `-O2`: OS 4 core 1.82 MB text+data, OS 5 core 2.04 MB (+ 1.6 MB bss); only the SDL frontends failed (no SDL headers), as expected |
| Cores only, x86-64 `-O3` | 2.07 MB and 2.45 MB |
| **Both cores in one binary** | **fails: duplicate symbols** (`op_0_3` ...): each core has its own copy of the UAE 68k CPU. One binary per core, or renamed symbols |

## 3. Targets and fit

### 3.1 Screens: integer scale under a 56 px header (I, from the sizes)

Emulated sizes include the writing area: 160x220, 320x440 (both cores), 240x320, 320x480, 480x720 (pv).

| Canvas (board) | Room | 160x220 | 320x440 | 240x320 | 320x480 | 480x720 |
| --- | --- | --- | --- | --- | --- | --- |
| 480x800 portrait (Pi 4 B PDA; 3A+, Pi 5 in PDA mode) | 480x744 | **3x** 480x660 | 1x (1.5x = 480x660) | **2x** 480x640 | 1x (1.5x = 480x720) | **1x** |
| 480x640 portrait (deck PDA) | 480x584 | 2x 320x440 | 1x | 1x (2x without header) | 1x | no |
| 800x1280 portrait (tablet PDA) | 800x1224 | **5x** 800x1100 | 2x 640x880 | 3x 720x960 | 2x 640x960 | 1x |
| 640x480 deck, 800x480 3A+ / Pi 5 / Pi 4 B landscape | w x424 | 1x (2x needs 440) | **does not fit** (440) | 1x | does not fit | no |
| 720x720 HackBerry | 720x664 | 3x 480x660 | 1x (1.5x) | 2x 480x640 | 1x | no |
| 1280x800 tablet landscape | 1280x744 | 3x | 1x (1.5x) | 2x | 1x (1.5x) | 1x |

Portrait-native is the best fit (`X-Portrait=480x640`, `X-MinSize=640x480`). In landscape the emulated screen is tall
and narrow: it fits at 2x only if the page drops the top header for a side column (Esc, buttons): a layout decision
for the owner (question 3). Non-integer 1.5x is possible (nearest or filtered) but uneven for 1-bit text.

### 3.2 Pen, keys, buttons (I)

- **Pen = touch:** canvas point minus the screen's origin, divided by the scale; the writing area and its four icons
  are part of the emulated digitiser, so they need nothing more. A stylus (Pi 4 B) works the same way.
- **Keys:** text, Enter, Backspace, Tab, arrows to `QueueKeyboardEvent` (OS 4 only); F1-F4 app buttons, Page Up/Down
  rocker. Esc stays ours (header Esc: save and back to the picker; hold 3 s: the launcher). Super, volume and
  brightness keys are the launcher's (guidelines section 3); upstream's Esc = power must not be kept.
- **On-screen buttons** (app 1-4, up/down, power) in the free row below the screen (84 px at 480x800 3x); there is no
  other way to press them by touch.

### 3.3 Handwriting: the device's own writing area, our band later (S, I)

The emulated OS recognises strokes itself in its writing area (OS 4) or its own input area (240x320, 320x480, pv
modes), so pen = touch already gives handwriting with no work. The launcher's band (report 044, phase 1 on launcher
branch `pda-s4-plan`) runs in a `cp0_lvgl` app's own process and, after owner decision 1, shows only while a
`TextField` edits; an emulator canvas is not a text field, and its Strokes keys could only reach an OS 4 session
(`QueueKeyboardEvent`; OS 5 has no text input). So: v1 uses the device's writing area; a "write with Strokes" toggle
for OS 4 needs a toolkit hook "text wanted without a TextField" after report 044 P2 (question 4). Option A cannot have
the band at all (no `cp0_lvgl`, and the launcher never draws over a fullscreen app).

### 3.4 Speed and memory

Probe (M): scratch `armbench.cpp` (60 lines, `~/zero7/cloudpilot-study/bench`, not in a repo) runs upstream's ARM
interpreter (`cpuCycle()`, MPU, icache, RAM) on a 9-instruction loop (ALU, load, store, conditional, shifted operand,
branch) with no ROM. i5-12450H in WSL: **84-97 MIPS** with upstream's `-O2`, 95-97 with `-O3`; about 45 MIPS in runs
that landed on an efficiency core (I). A static arm64 build (`armbench-arm64`, 4.5 MB) runs correctly under
qemu-user (speed there is meaningless); `./armbench-arm64 3` on a board gives its real number in 3 s.

| Board | OS 5 synthetic MIPS (I, scaled by single-core interpreter speed) | OS 4 (I) |
| --- | --- | --- |
| Pi 5 (A76 2.4 GHz) | 35-50 | fine |
| tablet (brief: Atom x5; board notes: a Core-m CPU) | 15-25 / 35-50 | fine |
| Pi 4 B (A72 1.5-1.8 GHz) | 20-30 | fine |
| Pi 3A+ (A53 1.4 GHz), deck (A53 1 GHz) | 8-15 | probably fine |

Upstream (README "Performance"): OS 4-era apps run fast even in OS 5 because legacy 68k code runs on a host-side 68k,
but native ARM apps need a fast host; slow phones "may struggle". The pv build is about 40 % faster than the ARM
device. **Not measurable without a ROM:** boot time, real OS 5 MIPS, OS 4 speed, frame rate, a session's VmRSS, audio.
Memory (S sizes, I totals): OS 4 device RAM up to 16 MB + ROM, about 30-50 MB VmRSS; OS 5 16/32 MB RAM + 33 MB NAND +
NOR, about 80-120 MB. The guideline for native apps is 25 MB, and section 12 lists "an emulator" under "when not to
build": this app is an owner-approved exception; the deck (about 414 MB usable) can hold an OS 4 session (I).

## 4. Two integration options

### A. Wrap the upstream SDL frontends (kind C, like viz1090)

| Work | Detail |
| --- | --- |
| Build | upstream `make bin` for Debian 13 arm64 and amd64: the SDK sysroot lacks the SDL and readline headers (2.5), so build on a Pi (viz1090's `build_on_pi.sh`) or add the `-dev` packages to the sysroot |
| Display, no desktop | `SDL_VIDEODRIVER=offscreen` + the viz1090 `LD_PRELOAD` bridge (`libviz_fb.so`: software renderer, frame to the contract's framebuffer, rotation, 16/32 bpp, header with Esc and status strip) |
| Touch → pen | the bridge sends finger events; upstream reads mouse: synthesise `SDL_MOUSEBUTTON*`/`MOTION` and interpose `SDL_GetMouseState` |
| Window size | upstream picks x2/x3; the bridge must scale the window to fit (new, per frame) or pass `--small-window` |
| Keys | Esc to the bridge (quit), never to `cp-uarm` (power); text events exist in the bridge |
| Session save, quit | stdin from a FIFO the wrapper keeps open; on Esc the bridge writes `save-image <file>` / `save-session <file>` then `quit` |
| Picker, buttons | a native `cp0_lvgl` picker (like `vizsetup`) that execs the emulator; on-screen hardware buttons drawn by the bridge and sent as Ctrl+Shift keys |
| Not possible | OS 4 sound (not in the frontend), the band and on-screen keyboard, `X-Background` (no lifecycle helper) |

- **Pros:** upstream binaries unchanged; GPL boundary simple; desktop-session boards need nothing new (the bridge
  writes the framebuffer as viz1090 does).
- **Cons / risks:** hack-on-hack (mouse interposition, FIFO CLI, scaling in a preload library); SDL2 and SDL2_image
  pull X11/Wayland/Pulse libraries onto Lite images (I); no multitasking; Esc/power clash; fixed, not responsive.
- **Effort:** 3-4 sessions (build 0.5-1, bridge 1-1.5, picker 1, package and tests 0.5-1). **Size:** about 9-10 MB
  installed, a 4 MB `.deb` per architecture (I) plus SDL2 dependencies.

### B. Native `cp0_lvgl` app linking the cores (kind A) - recommended

| Part | Detail |
| --- | --- |
| Build | `build.sh` fetches upstream at the pinned tag into a scratch folder (never committed), compiles the core sources into a static library with the toolkit's compiler and flags, then the app through `_common/build_app.sh`; no SDL, readline or curl |
| Binaries | `<id>` (picker + OS 4 core) and `<id>-arm` (same frontend + OS 5 core): the UAE symbol clash (2.5) makes one binary per core the simple answer; the picker execs the right one |
| Picker | a folder for ROMs (user's own) and sessions: list, new session from a ROM, rename, delete, card images; neutral labels from screen size, colour and CPU, never device IDs |
| Emulator page | canvas (frame to RGB565/XRGB8888, dirty lines, integer scale by hand), emulation thread with the core, frames under a mutex; pen from touch, keys as 3.2, a button row; header Esc saves and returns; save every N minutes and on `close` |
| Launcher contract | `X-Portrait=480x640`, `X-MinSize=640x480`, `X-Background=freeze` (the cores catch up after a pause: OS 5 skips lag over 3 slices, OS 4 resets its clock past 500 ms, S) |
| Audio | OS 4 beeper: a square wave from `onPwmChange`; OS 5 PCM queue; the toolkit has no audio output today: ALSA (runtime in the sysroot, headers to add) or SDL audio, a new dependency |
| Tests without ROMs | unit tests (frame conversion, scaling, pen mapping, key map, labels), headless scenes with a fake core (test pattern) at every size; board tests with the owner's ROM |

- **Pros:** our widgets, header, status strip, tokens, portrait and landscape layouts, multitasking, sessions with
  real names, OS 4 sound, a path to the band; arm64 and amd64 from the same toolkit; no SDL on the boards.
- **Cons / risks:** more code of ours (frontend about 1,500-2,500 lines, I); GPL binary; two binaries; core API is
  internal upstream (pin a tag; re-check at each move); emulation thread vs LVGL thread; OS 5 speed on small boards.
- **Effort:** P1 OS 4 3-3.5 sessions; P2 OS 5 + audio 1.5-2; P3 band/keyboard text, install, network 1.
  **Size:** about 5.2 MB per binary (toolkit 3.3-3.6 MB as Serial Console, + core 1.8-2.0 MB), 10-11 MB both, a
  4.5 MB `.deb` per architecture (I).

## 5. Branding

**What the checks do (S).** The apps repo `tools/branding/check-branding.sh` greps tracked and untracked text files
(`git grep -I`: binaries skipped, submodules not entered) for the stock-device brand words only, with
`branding-allow.txt`. The launcher's `tools/dev/check-branding.sh` adds a second check: the alphabet's trademark
(pattern `g[r]affiti`, case-insensitive) in every line **and file name** outside `docs/dev/**`, with no allow-list.
**The apps copy is behind** (no trademark check), although both say "keep the two in step". Neither checks the OS name
or the company name.

**How upstream would trip them (M, tracked files under `src/`).** The trademark in 88 lines of 32 files (trap and
struct names), which fails the launcher-style check; the OS name in 1,931 lines of 347 files and in 117 file paths
(`emulator/<os name>/...`, `Em<OS name>OS.cpp`); the company word, upper case, in 337 lines, all but 2 inside
technical identifiers (memory permission flags). Upstream device IDs (`--device-id` values, the "using device" log
line) and the skins (10 of 24 button SVGs) carry the OS name or a logo. The stripped binaries hold the OS name in
87 strings (`cloudpilot-emu`) and 5 (`cp-uarm`), the trademark in 1 and 2 (trap-name tables): the scripts skip
binaries and `.deb` files, but these strings ship.

**Options.** (1) Never commit upstream source: fetch the pinned tag at build time into a scratch folder (viz1090
style); a submodule is also invisible to the check but puts the names in the repo's tree. (2) Our code never names a
device ID and never passes `--device-id` (auto-detection works); labels from screen, colour, CPU. (3) Neutral name,
id, `.desktop` `Name`, `app.json`, README ("an emulator for classic 2000s handhelds, OS 4 and OS 5"); candidates in
question 1. "Pilot" was itself an early device name of the same line: legal but not neutral. (4) README and Store
text never link ROM sources (upstream's README does) and say the user brings their own; accept any file name
(`.rom`, `.bin`, `.gz`), our own session extension, `.prc`/`.pdb`/`.pqa` to install. (5) Screenshots only of our
pages and of a fake-core test pattern: a booted ROM shows the OS name on screen. (6) Bring the apps copy of the check
in step with the launcher's, and give the app a local `tests/check_names.sh` (like the Strokes one) that greps the
app folder and the package's file list and text files for the three words (case-insensitive; the company word as an
upper-case word). A repo-wide check for the OS name would also hit the common noun (part of the hand) in the
launcher's touch-rejection code, so keep it per app. (7) Binary strings: accept, or try a build switch that drops
the trap-name tables (unverified).

## 6. Recommendation and phases

**B**, OS 4 first, OS 5 gated by a measured speed:

| Phase | Content | Cost |
| --- | --- | --- |
| P0 | run the static probes 3 s each (`armbench-arm64` on the Pi 4 B and the deck, `armbench-x86o3` on the tablet); optional: a headless harness that boots the owner's ROM and prints emulated seconds per second (OS 4) and MIPS (OS 5) | 0.25 session + 10 min owner |
| P1 | app `<id>` 0.1.0 (draft): fetch-and-build of the OS 4 core, picker, emulator page, pen, keys, button row, sessions, freeze, beeper if cheap, portrait and landscape layouts, scenes, unit tests, `check_names.sh`, README with credits and GPL source offer, arm64 + amd64 | 3-3.5 sessions |
| P2 | `<id>-arm` with the OS 5 core on the boards P0 allows, PCM audio, SD card, pv screen sizes per board (480x720 on 480x800) | 1.5-2 sessions |
| P3 | Strokes band / on-screen keyboard text for OS 4, install from a folder, native network | about 1 session |

Board test (owner's ROM): Pi 4 B upright, tablet, deck; boot, writing area, buttons, Esc saves and resumes, `free -m`.

## 7. Open questions for the owner

1. **Name and id:** "Retro PDA", "Pocket Emulator", "Handheld Emulator" or "Pilot Emulator" (see 5 (3))?
2. **Scope:** OS 4 first and OS 5 only after the P0 numbers, and OS 5 hidden on the deck and the Pi 3A+?
3. **Landscape layout:** on 640x480 / 800x480, drop the top header for a side column so a 160x160 device shows at 2x,
   or keep the header and show it at 1x (or 1.9x)?
4. **Text input:** v1 with the emulated writing area only, the Strokes band / on-screen keyboard for OS 4 later (P3)?
5. **ROMs and licence:** a visible folder (for example `~/ROMs`) that the user fills by USB or scp, and a GPL-3.0
   package with the upstream source tarball on the release: agreed?

Reproduce (WSL, no sudo): `-dev` packages by `apt-get download` + `dpkg -x` into a private root, `src/Makefile.local`,
`make bin`; cores: a tree copy with `CXX_NATIVE=aarch64-linux-gnu-g++`, `make -k`; probe linked to the OS 5 core.
