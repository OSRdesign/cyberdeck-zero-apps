# Mesh Hop

Version 0.1.1 (roadmap phase 1), package `mesh-hop`. Part of [cyberdeck-zero-apps](../../README.md).

**Status: work in progress, unpublished (draft).** `app.json` has `"draft": true`, so `tools/make_registry.py` neither builds
nor lists it. Build a local package with `python tools/make_registry.py --only mesh-hop --out <folder>`.

A full-screen (640x480) client for [MeshCore](https://github.com/meshcore-dev/MeshCore) LoRa mesh networks. The radio is a
separate **companion radio board** plugged into the deck's USB port (MeshCore *companion radio, USB* firmware); the deck is
its screen and keyboard. Mesh Hop replaces the 320x170 UI of MeshZero with a UI made for messaging and keeps everything below
it: the serial transport, the protocol, the client, the model and the history store (`src/main/core/`, a copy of the
MeshZero core with unit tests). `apps/meshzero` stays as it is until Mesh Hop is accepted.

## What it does (0.1.1)

* **Chats**: two panes. Left: the channels and the direct conversations, each with the **number of unread messages** (a gold
  pill; nothing for none, "muted" for a muted channel); the total of unread messages is on the **Chats tab title**. Right: the
  conversation with the time of each message and the status of yours, a message entry and a **Send** button. A channel
  message ends at **sent** when the board accepts it (a channel has no acknowledgement, so never "delivered"; if the board
  does not answer within 5 s it shows "sent (unconfirmed)"); a direct message goes sending, sent, delivered or no ack.
  **Mute** (header button, or Ctrl+M) silences a channel: no pill, not counted in the tab total.
  **+ Add channel** opens a menu: a hashtag channel (`#name`), a private channel with a key typed as 32 hex characters, or a
  private channel with a random key (shown as text to share).
* **Contacts**: table of name, type, last heard, SNR, hops and distance (when both positions are known). Tap a column title
  or press `S` to sort (`R` reverses); a "Loading..." box shows while a long list is re-sorted or filtered, the chosen order
  is kept, and the row you tap is the row that opens. **Filter chips** by node type (All, Chat, Repeater, Room, Sensor) and by
  time since last heard (any time, 1 h, 24 h, 7 d); keys `T` and `H` cycle them. The list is virtual: only the rows in view
  are widgets, so Up/Down on 161 contacts is as quick as a touch scroll. Enter opens the chat of a chat node, or the detail
  of a repeater, room or sensor.
* **Settings**: board status (connection, model, firmware level, battery, clock) and, when the firmware lists the `gps`
  custom variable, a **Board GPS** switch; node name; **Preset** (the 26 presets of `api.meshcore.nz`, list dated 2026-10-07,
  see `src/main/core/radio_presets.inc`), frequency, **bandwidth, spreading factor, coding rate and TX power in a choice box**
  (the value in the middle, a large arrow on each side: touch the arrows, or Left / Right, Enter accepts, Esc cancels);
  **direct message retries** (number of tries, and when the stored route is forgotten and the message floods: default 3 tries,
  forget before try 3); clock sync; the channels (the fixed Public channel is not listed): Mute, Key (shows the key as text)
  and Remove (asks Yes / No) per channel; **Undo changes** and **Save to radio** are the last rows and each asks Yes / No.
  The board clock follows the deck policy of MeshZero (deck NTP, board GPS, or a typed start time).
* **Firmware check**: a feature the board's firmware lacks (custom variables, channel commands) is switched off with the
  notice "Firmware too old for this feature"; a board without GPS shows no GPS row.
* **Map** and **Terminal**: placeholders ("coming in phase 2/3").
* Footer: hints for the current screen on the left, the board (model and state) on the right. The top bar is the launcher's
  shared bar (clock, Wi-Fi, Bluetooth), the five tabs sit in its left part.
* History, the contact cache and the preferences (muted channels, retry settings) are kept in `~/.local/share/mesh-hop`
  (JSON lines and text, bounded: 200 messages per conversation, 1500 in all). The old `~/.local/share/meshzero` is not
  touched or imported. `mesh-hop.log` in the same folder is the debug log (size bounded, no message text): it records each
  channel send and what the board answered (`MESHHOP_DEBUG=1` logs every frame).

## Input rules

* Text is typed with the **Bluetooth keyboard only**. There is no on-screen keyboard. With no keyboard awake an editor says
  "Keyboard needed: wake the Bluetooth keyboard" and the entry box shows the same hint.
* Short **Esc is always Back** and never quits (it shows "Hold Esc 3 s to exit" on a top-level screen); **holding Esc for
  3 s** ends the app: the launcher does that.
* Everything is reachable by touch: tap rows, tabs, chips, arrows and buttons, drag to scroll lists and conversations.
  There is no swipe gesture.

| Key | Action |
| --- | --- |
| Tab / Shift+Tab | next / previous tab, in a loop (there are no other tab keys) |
| Up / Down | Chats: select a conversation (list) or scroll (entry); Contacts, Settings: move |
| Enter, or typing | Chats list: write; entry: send; Contacts: open; Settings: change / do |
| Left / Right | Settings: step the value of the row; entry: move the cursor; choice box: previous / next value |
| Esc | Back (box to nothing, entry to list, close detail or editor) |
| Ctrl+M | Chats: mute / unmute the channel (plain letters start a message); Settings, channel row: `M` |
| `S` `R` `T` `H` `A` `D` `U` | Contacts: sort, reverse, type filter, heard filter, advert, zero-hop advert, refresh |
| `S` `V` | Settings: save, undo (each asks Yes / No) |
| `K`, Del | Settings, channel row: show the key, remove the channel |
| `Y` `N` | Yes / No boxes |

The keyboard layout is US by default, AZERTY when `MESHHOP_KEYMAP=fr` or `XKBLAYOUT="fr"` is in `/etc/default/keyboard`.

## Install and requirements

Install from **Settings > Apps** like the other apps (a launcher with full-screen app support, `X-Fullscreen=true`, is needed).
The user must be in the `dialout` group (the Raspberry Pi OS default user is). The board is found by itself
(`/dev/ttyACM*`, `/dev/ttyUSB*`, by USB id); to force a port put its path in `~/.local/share/mesh-hop/port` or start the app
with `MESHHOP_PORT=/dev/...`. Fonts: the app reads the fonts the launcher installs in `/usr/share/APPLaunch/share/font`
(Montserrat, Font Awesome, DejaVu Sans) and falls back to its built-in Montserrat without accents.

## Build and test

```
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build/build.sh          # aarch64, into root/usr/share/APPLaunch/bin
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build/build.sh host     # x86-64 PC build, headless mode (~/mesh-hop-host/mesh-hop)
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/src/tests/run_tests.sh  # unit tests (core, simulator, UI logic)
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/src/tests/ui_smoke.sh   # scripted run + screenshots against the simulator
```

Both builds use the launcher's SDK (LVGL, toolchain) through a scratch project under `projects/` of the launcher tree that is
kept out of git; nothing of this app is added to the launcher repository. `tools/meshcore_sim.py` simulates a board
(options for the 0.1.1 cases: `--contacts N`, `--chan-reply ok|msgsent|odd|none`, `--old-firmware`, `--no-gps-var`).
The scripts in `src/tests/` (`smoke.script`, `bigcontacts.script`, `chan-silent.script`, `firmware.script`, `offline.script`)
drive the headless mode (`mesh-hop --headless --script f --shot-dir d`: renders into memory, takes keys and taps from a script,
writes PNG screenshots, `bench <key> <n>` times key presses); it also runs on the deck, over ssh, without touching the panel.

## Layout of the sources

| Path | What |
| --- | --- |
| `src/main/core/` | transport, protocol, client, model, store, clock policy, presets (`radio_presets.inc` is the dated data file), channel keys, firmware features, log (no LVGL; unit tested) |
| `src/main/ui/ui_logic.*` | keys, editor filters, formatting, chat and contact rows, filters and sort state, popup logic, Settings layout, Back rule (no LVGL; unit tested) |
| `src/main/ui/platform.*` | framebuffer, touch and keyboard (evdev), headless mode |
| `src/main/ui/app.*` | the screens, the virtual row list, the popups |
| `src/main/ui/cp0_statusbar.[ch]` | the launcher's shared top bar, copied from `cyberdeck-zero-launcher/ext_components/cp0_lvgl` (keep in sync) |
| `src/main/src/main.cpp` | start-up, signals, the script runner of the headless mode |
