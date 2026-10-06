# MeshZero

Version 0.1.0, package `meshzero`. Part of [cyberdeck-zero-apps](../../README.md).

**Status: work in progress, unpublished (draft).** `app.json` has `"draft": true`, so `tools/make_registry.py` neither builds nor lists it. Build a local package with `python tools/make_registry.py --only meshzero --out <folder>`.

A touch messenger and client for [MeshCore](https://github.com/meshcore-dev/MeshCore). The LoRa radio is a separate **companion
radio board** plugged into the deck's USB port (running the MeshCore *companion radio, USB* firmware); the deck is its
screen and keyboard. The app speaks the MeshCore companion protocol over USB serial.

## Features

* **Status**: connection state (no board, port busy, permission denied, wrong firmware, connected), the board
  model and firmware, node name, radio parameters, battery, counts. Buttons send an advert (flood or zero-hop).
* **Chats**: channels (**+ Add hashtag channel**: type `#name`, the key is the first 16 bytes of SHA-256 of the name as MeshCore
  does; a channel the app added can be removed with **Remove** or Del in its chat) and direct conversations with unread markers; **Chat** view with the history, delivery status of
  your messages (sending, sent, delivered, no ack, failed) and a **Message** button.
* **Contacts**: the nodes the radio knows (name, type, last SNR, last heard); Enter opens the chat, or the detail of
  a repeater or sensor.
* **Settings**: node name, frequency, bandwidth, spreading factor, coding rate, TX power; edit, **Save to radio**,
  Undo, and **Preset EU 868** (sets only the frequency to 869.525 MHz; the EU 868 band is 863-870 MHz, step 0.025 MHz). If channel slot 0 is empty the Public channel can be added.
* History and contact cache are kept in `~/.local/share/meshzero` (JSON lines, bounded: 200 messages per
  conversation, 1500 in all).
* Text and numbers are typed with the Bluetooth keyboard only (there is no on-screen keyboard). With no keyboard awake an editor says "Keyboard needed: wake the Bluetooth keyboard"; everything else works by touch.

## Install and requirements

Install from **Settings > Apps** like the other apps. The user must be in the `dialout` group (the Raspberry Pi OS
default user is). The board is found by itself (`/dev/ttyACM*`, `/dev/ttyUSB*`, by USB id); to force a port put its
path in `~/.local/share/meshzero/port` or start the app with `MESHZERO_PORT=/dev/...`.

## Keys

| Key | Action |
| --- | --- |
| Tab | Next tab (Status, Chats, Contacts, Settings) |
| Up / Down | Move in a list, scroll a chat |
| Left / Right | Settings: change the value of the selected row (elsewhere: switch tab) |
| Enter | Open / edit; in a chat, write a message; in the editor, accept / send |
| Del | In the chat of a channel added by the app: Remove (press twice) |
| A / D | Advert flood / zero-hop |
| U | Contacts: refresh the list from the radio |
| M | Chat: write a message |
| S / V | Settings: save / undo |
| Esc (short) | Back, or cancel the editor; at Status / Chats / Contacts / Settings it only shows "Hold Esc 3 s to exit" |
| Esc (hold 3 s) | Leave the app (done by the launcher) |

Touch: tabs, rows, the header buttons (Back, Message, Send, Remove), the Advert buttons and the `-` `+` steppers of the radio settings; together with the launcher's per-app gestures (Settings > Touch: drag = Up/Down, tap = Enter). Typing needs the keyboard (digits, `.`, `-`, Backspace, Enter to accept, Esc to cancel).

## Testing without a board

`tools/meshcore_sim.py` is a simulated companion radio over a pseudo terminal (Python 3 only). See the header of the
script for the options and the commands. In short, on the deck:

```
python3 meshcore_sim.py --echo --chatter 30 &       # creates /tmp/ttyMC0
echo /tmp/ttyMC0 > ~/.local/share/meshzero/port     # point the app at it (delete the file for the real board)
```

Unit tests (protocol, model, store, client, serial transport against the simulator):
`wsl -e sh apps/meshzero/src/tests/run_tests.sh`.

## Credits

MeshCore protocol and firmware: MeshCore (MIT). Wire format reference: meshcore_py (MIT). The UX is inspired by MeshCore clients such as wadamesh (GPL-3.0) and Meshy (GPL-3.0-or-later): ideas only, no code copied.

Data migration: the 0.1.x test builds kept their history in `~/.local/share/meshcore`; at startup that folder is renamed to `~/.local/share/meshzero` once, if the new one does not exist yet.

Not in v1: private channels with a 16-byte key, contact add/remove, maps, Bluetooth LE.

## Board clock

At connect the app sets up the clock (Status shows "Clock: deck NTP / set by you / board GPS / not set"): deck clock NTP-synced and
no GPS on the board -> the deck time is written to the board; deck offline and no GPS -> you type the start date and time
(prefilled with the deck's, Enter writes it, Esc leaves the board clock alone); GPS enabled -> the board clock is read and never
written. **Settings > Sync clock now** runs it again. The GPS state is read from the board's custom variable `gps`
(the companion protocol has no GPS field); `--gps`, `--board-clock-offset`, `--refuse-set-time` in `tools/meshcore_sim.py` play every case.
