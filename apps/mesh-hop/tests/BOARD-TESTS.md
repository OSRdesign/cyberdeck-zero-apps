# Mesh Hop 0.4.0 / 0.5.0: board tests

For the Controller and the owner (controller-led testing: the Controller deploys and reads logs, the owner does the
physical steps and ticks the boxes). Commands run from Windows: `deck.py` is
`C:\CLAUDE\zero7\launcher\docs\dev\deck\deck.py` (password in `PIPW`; `DECK_HOST=<ip>` points it at another board).
The app's process name is `mesh-hop` (`pgrep -x mesh-hop`; never `pkill -f`).

0.5.0 (the start page "Choose a node", Bluetooth LE and IP nodes): section **N**; the app now opens on that page (D1 starts
there). Boards: **deck** (Pi Zero 2 W, 640x480, Bluetooth keyboard), **Pi 3A+** (800x480 touch, no keyboard: the on-screen
keyboard), **HackBerry** (CM5, 720x720 square touch, built-in USB keyboard). Radio: a MeshCore companion radio board on USB
(board A: Seeed XIAO nRF52840; board B: XIAO S3 WIO, MeshCore v1.15.0), and for R1-R3 a second MeshCore node in radio range
(a phone with the MeshCore app, or the other board on another host) and, for heard-back, a repeater in range.

Expected pictures: the flow shots of `build.sh flows sizes` in `\\wsl.localhost\Ubuntu\home\osrde\cdk-build\mesh-hop\flows-sizes\`
(or the `CDK_BUILD` folder used): `shell-<size>/h*.png` (Home, header, dialogs), `pages-<size>/p*.png` (Contacts, details,
Nearby, Settings, Statistics, packet log), `chats-<size>/c*.png` (Chats, conversation, compose, search, editors), for
640x480 (deck), 800x480 (Pi 3A+) and 720x720-ppmm670 (HackBerry); the README screenshots for a first look.

## 0. Build and deploy (each board)

```
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh all --strict
python tools/make_registry.py --only mesh-hop --out C:/CLAUDE/zero7/pkg-out
set DECK_HOST=<board ip>        (deck: leave it unset)
python deck.py put C:/CLAUDE/zero7/pkg-out/mesh-hop_0.5.0_arm64.deb /tmp/mesh-hop.deb
python deck.py sudo 'dpkg -i /tmp/mesh-hop.deb || apt-get -f install -y'
python deck.py run '/usr/share/APPLaunch/bin/mesh-hop --version; id -nG; ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null'
```

`--version`: `Mesh Hop (mesh-hop) 0.4.0`, the launcher commit `9912361...` with `cp0_lvgl pinned`, and "needs a screen of
at least 640x480". `id -nG` lists `dialout`; the board shows
as a `/dev/ttyACM*` or `/dev/ttyUSB*` port. Logs while testing:

```
python deck.py run 'journalctl --user -n 80 --no-pager | grep -E "cdk|mesh-hop"'
python deck.py run 'tail -n 40 ~/.local/share/mesh-hop/mesh-hop.log'
```

## M. Memory (each board): how to read it

With the app open, after Chats, a conversation, Contacts scrolled to the end and Settings were each shown once:

```
python deck.py run 'p=$(pgrep -x mesh-hop); grep -E "VmRSS|VmHWM" /proc/$p/status; top -b -n 2 -d 3 -p $p | tail -1; free -m'
```

VmRSS is the memory in use now, VmHWM the peak since the start. **Both must be under 25 MB** (25600 kB); the second `top` line's
%CPU at idle (no typing, no scrolling) a few percent at most. Write the numbers in the table at the end. Repeat after the on-screen
keyboard has been shown (Pi 3A+) and after a search.

| Board | VmRSS kB | VmHWM kB | idle %CPU | Pass | Fail |
| --- | --- | --- | --- | --- | --- |
| deck | | | | [ ] | [ ] |
| Pi 3A+ | | | | [ ] | [ ] |
| HackBerry | | | | [ ] | [ ] |

## D1. Deck (640x480, BT keyboard): Home and Quit

1. Open Mesh Hop from the launcher (0.5.0: first the start page, section N1; choose the USB board there). Expect Home (compare
   `shell-640x480/h01-home-keyboard.png`): header "Mesh Hop" with the Esc
   button and the status strip, the board line under it, the rows **Chats** (unread total in blue, or "no new"), **Contacts**
   (the count) and **Settings**, each with a chevron; the key hint line at the bottom.
2. Up / Down move the highlight, Enter opens the page; on a page Tab / Shift+Tab go Chats > Contacts > Settings in a loop.
3. A short Esc on the keyboard on a page: back to Home. At Home: the notice "Hold Esc 3 s to exit", the app stays.
4. Tap the header's Esc at Home: the dialog **Quit Mesh Hop?** with Cancel and Quit. Cancel: nothing. Again, Quit: the app ends,
   the launcher is back. Open it again. Hold Esc 3 s on the keyboard: the app ends.

[ ] pass  [ ] fail  Notes:

## D2. Deck: Chats list, hold, options, Search, Add channel

1. Chats: one pane (`chats-640x480/c01-chats.png`): the bar **Search** / **Add channel**, CHANNELS and DIRECT, two lines per
   chat, the unread count in blue at the right.
2. Up / Down select; Enter opens the conversation as its own page (the header's title is its name), Esc back to the list.
3. Tap a row: it is selected; tap it again: it opens. Back to the list.
4. Hold a finger 3 s on a channel row: a bar fills, then the menu "Options for # Public": Search messages, Add channel,
   Mute / Unmute, Delete messages. On a direct chat (a known contact): Mute / Unmute this contact, Delete conversation, Details.
   Ctrl+O on the selected row opens the same menu. A hold released before 3 s opens nothing.
5. **Mute** a chat: "muted" at the right, Home's total drops; Unmute (also Ctrl+M).
6. **Search** (the bar, or Ctrl+F): type a word you know is in a message; the results newest first; Ctrl+K / Ctrl+D / Ctrl+R narrow by
   chat, dates, direction; Enter on a result opens the conversation at that message, framed in blue. Esc closes the search.
7. **Add channel** (the bar): the menu hashtag / private key / random key. Make `#p5test`: it appears under CHANNELS. Remove it later
   in Settings > CHANNELS (Remove asks Yes / No).
8. **Delete messages** of `#p5test` (empty) or of a test chat: Yes / No with the count; No keeps them.

[ ] pass  [ ] fail  Notes:

## D3. Deck: conversation and compose with the keyboard

1. Open a conversation: messages with sender and time, yours green with a status; the line under the title says the nodes heard
   in the last hour; the entry and **Send** at the bottom. Up / Down / PgUp / PgDn scroll, never past the first or last message.
2. Type a letter on the list or on the page: the entry takes it. Typing shows the bytes left in the line under the title. Enter
   sends (see R1 for the radio). Esc leaves the entry, Esc again goes back to the list.
3. Accents: with `XKBLAYOUT="fr"` (or `MESHHOP_KEYMAP=fr`) the AZERTY keys type the right letters (é è à ç), with US the US ones.

[ ] pass  [ ] fail  Notes:

## D4. Deck: Contacts

1. Contacts (`pages-640x480/p01-contacts.png`): the bar **Filter**, **Sort**, **More**; two lines per row (name; type, heard,
   hops), the distance at the right when both positions are known.
2. **Filter**: Type, Heard, Group, each a pick list; the line under the title says what is shown. `T` and `H` cycle them.
3. **Sort**: name, heard, type, SNR, hops, distance; the field in use again reverses. `S`, `R`. Restart the app: the order is kept.
4. **More > Send advert**: the arrow choice Flood / Zero-hop (starts on the one used last); OK sends (the log has the advert).
   **More > Nearby**: the nodes heard with Add / Ignore, Scan (8 s), Ignored. **More > Manage contacts**: check boxes, Select...,
   Delete n (asks first, one by one with a progress box and Stop: test on a contact you do not need, or say No), Group..., Done.
5. Tap a row, tap again: the **details** (key, route, last advert, position, distance, SNR), Message / Groups / Delete on its bar;
   Esc (or the header's Esc) closes them. With 100+ contacts, Down held through the list stays quick.

[ ] pass  [ ] fail  Notes:

## D5. Deck: Settings, Statistics, Packet log

1. Settings (`pages-640x480/p13-settings.png`): BOARD (connection, firmware, battery, clock as grey information), Position,
   Board GPS (only with a GPS board), Statistics, Packet log; RADIO; DIRECT MESSAGES; SCHEDULED ADVERT; CONTACTS; CLOCK;
   CHANNELS; HISTORY; BOARD ACTIONS; RADIO CHANGES. Every row has a chevron, a dot or no marker (information).
2. **Position**: the box with latitude and longitude; type `48.85` / `2.35` (',' works too), OK: the row shows it after the
   read-back; a wrong value (`95`) is refused in the box. **Clear**: "not set". A GPS board: the **Board GPS** switch in the box and
   on its row; on, the board's fix replaces the typed position (note whether it does: never tested on a real board).
3. **Radio settings**: the Preset box lists the presets (and says which list); Bandwidth, Spreading factor, Coding rate, TX power
   in the arrow choice (Left / Right or the arrows, OK, Cancel); the changed rows are marked. **Undo changes** puts them back (asks
   Yes / No). Do not **Save to radio** unless you mean to change the board's radio.
4. **History**: Delete messages older than 90 days (asks first); Forget this board's data / Other boards' data show their size
   and ask first (say No).
5. **Statistics**: battery, uptime, noise floor, RSSI, SNR, airtime, packet counters; refreshes every 5 s; `R` refreshes, `A`
   switches the automatic refresh. Esc back.
6. **Packet log**: capture off at the start; Start (or `S`): rows appear as the board hears packets; Up / Down select, the path and
   hex bytes below; Clear (`C`); Esc back. Restart the app: it is off and empty again.

[ ] pass  [ ] fail  Notes:

## R. Real radio checks (deck, board A; repeat R1 on board B if at hand)

1. **R1 Send**: in **Public** (or a test channel the other node has), send `p5 test <time>`. The status goes to **sent** within 5 s
   (or "sent (unconfirmed)" if the board does not answer); the other node receives it. A direct message to the other node: sending,
   sent, then **delivered** (or "no ack" when it is out of range). Its answer arrives in the direct chat with an unread count.
2. **R2 Heard back**: with a repeater in range, a channel message turns into **heard back by N repeaters** within a few seconds,
   N may grow for 60 s, then stays after a restart of the app.
3. **R3 Advert**: More > Send advert > Flood: the other node lists or updates this deck's node; Zero-hop: only nodes in direct
   range hear it. The packet log (capture on) shows the advert packets of other nodes; Nearby lists the nodes heard.
4. **R4 Unplug / plug**: pull the board's USB cable: the board line says it is gone, Chats stays readable. Plug it back: connected
   again by itself, the same chats.

[ ] pass  [ ] fail  Notes:

## N. 0.5.0: Choose a node (deck; N7 also on the Pi 3A+ by touch)

Nodes: board A on USB; **MeshCore-OSR T1000** (EC:EB:45:39:28:2D, Seeed T1000-E, BLE companion firmware, no screen: PIN
**123456** unless one was set) and **MeshCore-OSR wio** (D7:C5:07:5B:55:EB, BLE companion: its PIN is on its screen if it has
one, else 123456); both were heard by the deck on 2026-10-19. No phone may be connected to a BLE node (it takes one central at a
time). The renders to compare: `scratchpad\mhble\<size>\nodes-*.png` and the sheets in `scratchpad\mhble\sheets`.
Logs: `tail -n 60 ~/.local/share/mesh-hop/mesh-hop.log` (lines `node:`, `ble:`, `bluez:`).

1. **N1 Start page, no auto-connect (USB)**: plug board A, open the app: the page **Choose a node**, the USB row
   (`ttyACM0 · ...`), highlighted when it was the node chosen last; nothing connects (the board line does not say Connected).
   Tap it twice (or Enter): Connecting..., then Home. Quit and open again: the page again, the row highlighted, not connected.
2. **N2 USB list**: on the page, unplug board A: its row goes within about a second; plug it: it comes back.
3. **N3 First-use warning**: with no `~/.local/share/mesh-hop/nodes.txt` (rename it first), open the app: the box
   **Scan for Bluetooth nodes?** (the keyboard and Wi-Fi warning), **Not now** focused. Not now: a row "Scan for Bluetooth
   nodes", USB still usable. Quit, open: not asked again. Tap the row twice: it scans (10 s, "Scanning for Bluetooth nodes...",
   then "Rescan").
4. **N4 Scan**: the T1000 and the wio appear with bars (move one away: fewer bars on a Rescan); Meshtastic nodes are not listed.
   **Type on the M4 during a scan**: note any lag (seconds, how often).
5. **N5 Pair the T1000**: tap it twice: Pairing..., the PIN box ("Pair with OSR T1000", 123456 as the hint); type `111111`, OK:
   "Failed: Pairing failed (wrong PIN?) · tap to retry". Tap it again, type `123456`: Connecting..., then Home, the board line
   connected; Home's **Node** row says its name and "Bluetooth". The M4 keeps typing. `bluetoothctl info EC:EB:45:39:28:2D`
   (another terminal) shows Paired: yes. The log shows `bluez: RequestPasskey` (Mesh Hop's agent asked, not the launcher's).
6. **N6 Reconnect**: on Home, switch the T1000 off for 20 s, then on: "Board disconnected", then connected again by itself
   (note the time). Quit and open the app: the T1000 highlighted, a tap connects without a PIN.
7. **N7 IP node**: with a MeshCore Wi-Fi node, or the simulator on a PC on the same network
   (`python3 tools/meshcore_sim.py --tcp 5000 --tcp-host 0.0.0.0`): **Add a node by IP address**, type `192.168.1` (refused:
   "Not an IP address..."), then `<the address>` (and `:port` if not 5000), a name: the row appears and stays after a restart.
   Tap it twice: connected, Home. Add `192.0.2.1`: tap twice: "Failed: no answer in 5 s · tap to retry", the app never hangs.
8. **N8 Options**: on the T1000 row: **O** (or Ctrl+O, or a 3 s hold by touch): Forget this node, Info. Info: the address,
   "Paired". Forget: asks; Forget: the row says not paired (or goes when out of range); `bluetoothctl devices` no longer lists it.
   On an IP node: Remove IP node (asks), Info.
9. **N9 From Home**: Home > **Node** (the fourth row, a chevron) opens the page with the node in use connected (green); its Esc
   goes back to Home; choosing another node there switches (the old one disconnects) and goes to Home.
10. **N10 Bluetooth off / missing**: launcher Settings > Bluetooth off, open the page: a grey "Bluetooth is off" row, no error
   box, USB still works. Bluetooth on again: reopen the page, the scan works.

[ ] pass  [ ] fail  Notes:

## P. Pi 3A+ (800x480, touch only, on-screen keyboard)

1. The tile shows; open by touch. Home as `shell-800x480/h02-home.png`; no key hint line. Every row and button reacts where it
   is tapped (corners included: tap the header's Esc, the last row of a list, the right end of a bar).
2. **Chats in two panes** (`chats-800x480/c01-chats.png`): the list at the left (about a third), the conversation at the right
   following the selected row; the bar across both. The header's Esc goes to Home.
3. Hold a chat 3 s: its options (as D2.4). Search and Add channel from the bar; the search words bring the on-screen keyboard.
4. **Send by touch**: tap the entry: the on-screen keyboard; the compose view (header, last message, entry, keyboard) as
   `chats-800x480/c07-compose-osk.png`; type, the keyboard's Enter (or Send) sends; the keyboard goes. Its Esc closes it without
   sending. No way to summon the keyboard by hand exists (correct).
5. Contacts: Filter / Sort / More / Manage contacts / a contact's details, all by touch (as D4); the SNR shows on the rows.
6. Settings: the Position box by touch, its fields bring the on-screen keyboard and the box stays above it; an arrow choice
   (Bandwidth) by its arrows; Statistics and Packet log open and close by the header's Esc.
7. Home: the header's Esc asks **Quit Mesh Hop?**, Quit ends the app. Connect a Bluetooth keyboard while Chats is open: the
   on-screen keyboard goes within about a second, the hint line appears.
8. Memory (M).

[ ] pass  [ ] fail  Notes:

## H. HackBerry (720x720, built-in keyboard)

1. Open from the launcher (stop the desktop first if the launcher needs it). Home as `shell-720x720-ppmm670/h01-home-keyboard.png`;
   the text sizes read well at the board's density.
2. **Chats is one pane** on the square screen (the list, then the conversation's page), as at 640x480.
3. Typing with the built-in keyboard: a message, a search, an editor. The on-screen keyboard does not show while the built-in
   keyboard is there (unless the board's keyboard setting forces it; note which).
4. Touch: rows, bars, the header's Esc, a 3 s hold on a chat; taps land where the finger is (rotation / touch accuracy: the four
   corners and the centre).
5. Contacts, Settings (Position, a radio arrow choice), Statistics, Packet log as on the deck; the Quit dialog at Home.
6. With a radio board plugged in: R1 Send on this board.
7. Memory (M).

[ ] pass  [ ] fail  Notes:

## Summary

| Section | deck | Pi 3A+ | HackBerry |
| --- | --- | --- | --- |
| Home, Quit (D1 / P1, P7 / H1, H5) | [ ] | [ ] | [ ] |
| Chats list, hold, options, Search, Add channel (D2 / P2, P3 / H2) | [ ] | [ ] | [ ] |
| Conversation, send (D3, R1 / P4 / H3, H6) | [ ] | [ ] | [ ] |
| Contacts (D4 / P5 / H5) | [ ] | [ ] | [ ] |
| Settings, Statistics, Packet log (D5 / P6 / H5) | [ ] | [ ] | [ ] |
| Touch accuracy, rotation (P1 / H4) | n/a | [ ] | [ ] |
| Memory under 25 MB (M) | [ ] | [ ] | [ ] |
| Radio: send, heard back, advert, replug (R) | [ ] | | |
| 0.5.0 Choose a node: USB, first scan, BLE pair / reconnect / forget, IP (N1-N10) | [ ] | [ ] (N7 by touch) | |

## Unverified until a board run

| # | Assumption |
| --- | --- |
| U-1 | The Board GPS switch of the Position box and the GPS fix replacing a typed position work on a real GPS board |
| U-2 | VmRSS stays under 25 MB on the Zero 2 W with 160+ contacts and a full history (the host runner is an indication only) |
| U-3 | The 3 s hold is reliable on the Pi 3A+ and HackBerry touch panels (no early release from a small finger move) |
| U-4 | The on-screen keyboard goes within about a second when a Bluetooth keyboard connects or wakes |
| U-5 | The HackBerry's built-in keyboard counts as a keyboard for the shared on-screen keyboard rule |
| U-6 | 0.5.0: BlueZ 5.82 on the deck asks Mesh Hop's agent (not the launcher's default one) for the PIN of a pairing Mesh Hop starts, and a real MeshCore BLE node answers DEVICE_QUERY / APP_START over the Nordic UART service (shown with a simulated BlueZ only) |
| U-7 | 0.5.0: a 10 s LE scan and a BLE node link do not make the M4 (a Bluetooth Classic keyboard) lag enough to matter; Wi-Fi keeps working |
| U-8 | 0.5.0: a real MeshCore Wi-Fi companion talks over TCP as the simulator does (same framing, port 5000) |
| U-9 | 0.5.0: the bar thresholds (4 bars from -60 dBm ... 1 above -100) read sensibly for nodes at 1, 5 and 20 m |
