# Reticulum: board tests (draft 0.3.0)

For the Controller and the owner (controller-led testing: the Controller deploys and reads logs, the owner does the physical
steps). Nothing here was run on a board when this was written: the PC runs are listed in [README.md](README.md). Commands run
from Windows: `board.py` is `C:\CLAUDE\zero7\launcher\tools\boards\board.py`, `deck.py` is
`C:\CLAUDE\zero7\launcher\docs\dev\deck\deck.py` (password in `PIPW`). Remote commands are in single quotes. Nothing uses
`pkill -f`. The app and the service **both** have the process name `reticulum` (the bridge renames itself), so `pgrep -x reticulum`
matches two. Tell them apart by the command line: the app is `pgrep -af '^/usr/share/APPLaunch/bin/reticulum'`, the service is
`pgrep -af 'python3 -m reticulum_bridge'` (or its main pid: `systemctl --user show -p MainPID --value reticulum`). **Installing the package, enabling the unit and any radio settings are decisions of the owner**:
the radio numbers below are never suggested here, the owner picks values that are legal where the board is.

Expected pictures: the contact sheets `\\wsl.localhost\Ubuntu\home\osrde\cdk-build\reticulum\host\out\sheets\` and
`...\host\out-bridge\sheets\` (`walk-mixed.png`, `radio.png`, `add.png`, `settings.png`, `touch.png`, ...) and the numbered
pictures in [`../screenshots/`](../screenshots/), all **rendered with the fake bridge, not from a board**.

## 0. Build, deploy, clean up

```
wsl -e sh /mnt/c/CLAUDE/zero7/apps-reticulum-p1a/apps/reticulum/build.sh all         # binaries (arm64 into root/)
python tools/make_registry.py --only reticulum --arch arm64 --out C:/CLAUDE/zero7/pkg-out-reticulum-p2b
python deck.py put C:/CLAUDE/zero7/pkg-out-reticulum-p2b/reticulum_0.3.0_arm64.deb /tmp/reticulum.deb
python deck.py sudo 'apt-get install -y /tmp/reticulum.deb'      # also pulls python3-cryptography and python3-serial
python deck.py run '/usr/share/APPLaunch/bin/reticulum --version'
python deck.py run 'systemctl --user is-active reticulum; systemctl --user is-enabled reticulum'
```

Expect `--version`: `Reticulum (reticulum) 0.3.0`, the launcher commit, `cp0_lvgl pinned`; the unit is neither active nor
enabled right after the install; the tile "Reticulum" appears within a few seconds. Logs of the app: `journalctl --user -u
cyberdeckzero.service -n 80 --no-pager | grep reticulum`; of the service: `journalctl --user -u reticulum -n 80 --no-pager`.
Clean up: `python deck.py sudo 'dpkg -r reticulum'` (the user's `~/.reticulum` and `~/.local/share/reticulum` stay; remove them
by hand if the test deck should forget its identity).

## B1. Start: how long, and what the screen says (Pi Zero 2 W)

1. Nothing running (`systemctl --user is-active reticulum` says `inactive`). Open Reticulum. Stopwatch from the tap.
   Expect: Home with the amber line `service: starting...` and a row `Starting the service` with a seconds counter, then
   `service: running  OK` and the three rows. **Write down the seconds** (the PC needs 0.6 s; the Zero 2 W is untested: the
   limit in the app is 60 s).
2. Log: `[reticulum] service not running: starting it`, then `[reticulum] service answers`.
3. The deck must not reset or stutter during the start (Mesh Hop's lesson, guidelines section 14: no heavy work at start;
   the bytecode was compiled by the package's postinst). If it does, say what ran in the 30 s before.
4. Second open while the service already runs: no `starting` step, Home is up at once.

## B2. Memory and CPU

With the service up and every connection off, on Home: `python deck.py run 'ps -o pid,rss,%cpu,args -C reticulum'` (two lines: the app's args are `/usr/share/APPLaunch/bin/reticulum`, the service's `python3 -m reticulum_bridge`) and
`systemctl --user show reticulum -p MemoryCurrent -p MemoryHigh -p MemoryMax`, `free -m`. Expect: the app under 25 MB
(guidelines), the service about 40-45 MB (PC figure, budget 60 MB), idle CPU of both at a few percent. Open every page once and
repeat. **Whether the user manager enforces `MemoryHigh`/`MemoryMax`/`CPUQuota` is part of this test** (`MemoryMax=134217728`
in the `show` output only says it is configured).

## B3. Leaving stops the service (Keep running off)

1. Short Esc on Home (BT keyboard): the app ends, the home grid is back. Within 5 s `systemctl --user is-active reticulum`
   says `inactive`. Log: `[reticulum] leaving: stopping the service (Keep running is off)`.
2. The header's Esc button on Home (touch): the same. Hold Esc 3 s inside a text page: the app ends (its own handling or the
   watchdog: `[cdk] ... signal 15, ended cleanly`), the service stops, no `reticulum` process is left (`pgrep -ax reticulum` prints nothing).
3. Open the app, then the home key (the app is hidden, `X-Background=freeze`): the service keeps running (`is-active` says
   `active`); bring the app back from the switcher: the page is as it was, polling resumes.

## B4. Keep running

1. Settings, tap Keep running: value `working...` then `on`. `systemctl --user is-enabled reticulum` says `enabled`.
2. Leave the app: the service stays active (log: `the service keeps running`). Open the app again: Home is up at once and Settings
   says `on`. Switch it off: `is-enabled` says `disabled`; leaving then stops the service again.
3. Reboot with it enabled and nothing else: after login `systemctl --user is-active reticulum` says `active` (the unit's
   `WantedBy=default.target`; a user session must exist, so this needs a login or `loginctl enable-linger`, which this test does
   not do).

## B5. Connections: Wi-Fi / LAN and Internet

1. Connections lists `Wi-Fi LAN` (the first configuration's connection) as `off`. Add a connection, Internet: the form shows
   Name (`Internet`), Server address and Server port as `not set` in amber; fill them with a server the owner trusts (or none: leave
   them and Save). Save: the page of the new connection, `connection: off`, **Allow sending: no**.
2. `python deck.py run 'cat ~/.reticulum/config | tail -12'`: the section is written with `enabled = no` and `outgoing = False`.
3. Switch on: `connection: on, listen only` within a few seconds (or `on, not connected` when the server cannot be reached; a
   refusal is a notice in plain words). Status shows the same word. Switch off: `off`. The page says that listen only still
   opens the link (a short hello goes to the server: about 240 bytes, no message, no name).
4. **Wi-Fi LAN has no listen only (0.3.1).** Its page has no *Allow sending* row, only *Switch on* and the line "This kind of
   connection is on or off. While it is on, it may send." *Switch on* asks "Switch on Wi-Fi LAN?" with the focus on **Cancel**:
   Enter alone, or Left then Enter, or Esc cancels (`connection: off`, nothing sent); Right then Enter (or a tap on the main
   button) switches it on: `connection: on, sending`. Measure what leaves the deck from a second machine on the same network, 10 s
   each, with a multicast listener (the tool the QA used was `mcl.py`; any listener on the group and port of the connection's
   Group name and Discovery port does): connection off = 0 datagrams, on = a few (a discovery packet about every 1.6 s).
   With a second machine running Reticulum on the same network: Status, Heard recently shows it after its announcement.
4b. **An older listen-only Wi-Fi LAN.** Put the 0.3.0 state in the file (Wi-Fi LAN `enabled = yes`, `outgoing = False` in
   `~/.reticulum/config`), stop the service, open the app: a dialog says Wi-Fi LAN was switched off (once; open the app again:
   no dialog), Connections says `off`, the file says `enabled = no`, and the listener saw **0 datagrams** during the start.
5. Nothing is sent by merely opening pages: with every connection off, `python deck.py run 'PYTHONPATH=/opt/reticulum/bridge:/opt/reticulum/py
   python3 -m reticulum_bridge.client ifaces'` shows `"online": false` everywhere.

## B6. A LoRa radio (RNode), with the owner's own settings

1. Plug the RNode into the OTG port (the owner is in the `dialout` group). Add a connection, LoRa radio, Save. On its page
   every radio field reads `not set` in amber under `RADIO: YOU CHOOSE` / `CHECK YOUR LOCAL RULES`. Switch on says what is still to
   set.
2. Port: the list shows the by-id name and `ttyACM0` (not both for the same device). Choose it.
3. The owner enters the radio fields (their own values). A value out of range is refused in plain words (try a frequency of 5).
4. Switch on: `connection: on, listen only`. Status shows the radio with `Channel busy` and `Noise` when the RNode reports them.
5. Open Mesh Hop on the same port first, then switch the radio on: the notice names the program (`... is used by mesh-hop. Close
   that program first.`). Close Mesh Hop, try again: it works.
6. **Allow sending**: the question appears with the focus on Cancel and names the owner's responsibility; Enter alone, Left then
   Enter, or Esc says no and nothing changes (`outgoing` stays false in `ifaces`). Say yes (Right, then Enter) only if the owner
   chooses to transmit: the state word becomes `on, sending`. A radio on but listen only stays at `txb 0` in `rnstatus`.
7. Unplug the RNode while it is on: the state turns to a problem or `not connected` within a few polls; the app does not crash.

## B7. Failures are plain

1. `python deck.py run 'systemctl --user mask reticulum'`, open the app: `service: not running`, the reason from systemctl, `Try
   again`. Unmask, tap Try again: it starts.
2. With the app open and the service up, `python deck.py run 'systemctl --user kill -s KILL reticulum'`: within a few seconds
   Home says `service: not running` / `The service stopped answering.` (systemd restarts it after 3 s: Try again then works).
3. Over `ssh` without a user session: `python deck.py run '/usr/share/APPLaunch/bin/reticulum'` (no `XDG_RUNTIME_DIR`): the app
   starts and says so in plain words; it does not crash (a bare `ssh` has no display, so the harness-less start may stop at the
   display step: the log line tells).
4. Settings, Stop the service: the question, then `service: stopped`, `Start the service` brings it back.

## B8. Input and the on-screen keyboard (deck, touch only for the second half)

1. With the Bluetooth keyboard: every page by keys (Up/Down, Enter/Right, Esc/Left, Home/End); typing in the Name page needs no
   on-screen keyboard.
2. Without a keyboard (asleep or removed): a text page (Name) shows the on-screen keyboard by itself and the field stays visible
   above it; `Settings > On-screen keyboard` says `shows when you type`. Wake the keyboard: the on-screen keyboard goes within about
   a second.
3. Touch only: a tap selects a row, a tap on the selected row does it; the header Esc goes back; nothing needs a long press.
   Judge the touch accuracy on the 2.8" panel: the rows are 7 mm.

## B9. Install and removal through Settings > Apps (after the package is published)

The package is a draft, so Settings > Apps does not list it yet. After publishing: it is listed under Network with its icon and
screenshots, installs (sudo password) with its two Python libraries, shows the tile; removal stops the service first (`prerm`) and
leaves `~/.reticulum`. **This flow, the tile-less case that the earlier service-only package assumed, is replaced by this one:
check that Available lists the app and Installed shows it with the version.**

## B10. Other boards

- Pi 3A+ (touch only), HackBerry (720x720, keyboard rule forced), Pi 5 (800x480): open every page, one connection add, one switch
  on and off; a screenshot of Home and of a radio page.
- The FZ-M1 tablet (amd64 package, 1280x800, portrait with the 800x1280 PDA mode): install the amd64 package, the main path by
  touch and keys.
- A 480x320 panel (Pi 3A+ with its SPI panel): `on, listen only` shortens the connection name (`LoRa ra...`): judge whether that
  is acceptable.

## P2. Contacts and messages (0.3.0)

The pictures to compare with: [`../screenshots/`](../screenshots/) 45 on (rendered with the fake bridge). The owner's short list is
`C:\CLAUDE\zero7\board-tests-current\reticulum-P2-OWNER-TESTS.md`. Two decks, or one deck and a PC with the bridge, are needed
for the real exchange; a second deck needs a connection to the first (the same Wi-Fi with AutoInterface, or a TCP server and
client, or two radios the owner configures: no radio numbers here).

### M1. Nothing is sent by looking

1. Open Messages, Contacts, one contact, Add a contact (do not save), Heard recently, My address, the conversation of a contact
   (do not type). With a connection on and **sending allowed**: `python deck.py run 'journalctl --user -u reticulum -n 60 --no-pager'`
   and the interface statistics (`rnstatus` of the bridge's environment, or Status in the app): no transmit counter moves because of
   the screens (a path request for an address you typed is the bridge asking the network, only after you press Send).
2. Log of the app: no `message handed to the service`, no `sent by the user`.

### M2. Add a contact, send, delivered

1. Add a contact with the other deck's address (My address on that deck, typed in groups of four; spaces are fine; a short one is
   refused with the digit count). Save: the contact is in the list, *not heard yet* or *heard N min ago*.
2. Send a message: *Sending...* then *Delivered HH:MM* (the time the delivery was confirmed, a few seconds after Send). On the other deck: a new message, the
   Messages row with a count, the toast and beep when New message alerts is on there.
3. Reply from the other deck while the conversation is open: it appears at once; the count never shows for the open conversation.
4. **Write down the seconds** from Send to *Delivered* on the real link (Wi-Fi, TCP, radio).

### M3. Offline contact, Retry

Switch the other deck's service off (or its connection). Send: *Sending...*, after LXMF's tries (about a minute) *Failed,
contact may be offline* and a **Retry** button. Switch the other side on, wait a little. Nothing is sent again by itself (check the
other deck). Press Retry: *Sending...* then *Delivered HH:MM*, and HH:MM is the time you pressed Retry (a few minutes after the first Send), not the first send time.

### M4. Nothing may send

With every connection off, or a radio on but **Allow sending: no**: Send is grey; the conversation says *No connection is allowed to
send* with a row that opens Connections; the typed text is still there when you come back; My address > Announce myself shows the same
and *Nothing was sent*. Allow sending on one connection: the note goes within 3 s, Send turns amber, Announce myself asks first and
names the connection. A message that was cancelled because sending was switched off says *Failed, sending was off* in the
Messages list and *Failed, no connection is allowed to send* in its conversation (the same reason, a shorter form for the list row).

### M5. Announce myself

Set a name (My address > Name). Announce myself: the question names your name and the connection and opens with the focus on
**Cancel**: Enter alone, Left then Enter and Esc send nothing (QA of 0.3.0: Left then Enter had announced); Right then Enter
announces. Announce makes
the other deck list you under Heard recently within a minute or so (LoRa: the airtime is the owner's). It never happens by itself:
leave the app open for ten minutes with sending allowed and check the other deck.

### M6. Delete and remove, only on this deck

Delete a conversation: the question names the contact and opens on Cancel (Enter alone keeps it); Right then Enter deletes;
afterwards the contact is still there, and the other deck still has its copy.
Remove contact: the contact and the messages go (Right then Enter); the other deck is unchanged. Rename a contact: the page title
changes at once and reads "Contacts > <Name>" (one name, also when the contact page was opened from a conversation's More). Stop the service: the same rules (Enter alone cancels).

### M7. Alerts, Keep running, memory

1. Settings > New message alerts on, Keep running on, leave the app, send from the other deck: the toast reads *New message from
   <name>* (never the text) and the beep sounds (launcher 0.10.x with the contract of section 14); with the app closed and Keep
   running off nothing arrives until the app is opened again. **Whether the real launcher shows the toast is untested.**
2. Memory with a few hundred stored messages on the Zero 2 W: `ps -o pid,rss,args -C reticulum` and `python3 -m reticulum_bridge`
   (budget 60 MB for the service), the app under 25 MB with the conversation open; `messages.db` size in `~/.local/share/reticulum`.

### M8. Small screens and the keyboard

On the 480x320 deck and in portrait: a conversation with the on-screen keyboard shows the compose row and the latest message on one
line; Esc closes the keyboard and the history is back. A long message (past 287 bytes): the amber counter, and the warning *Long
messages are slow over radio* where the room allows (at 480x320 with the keyboard up the warning takes the line of the latest message: the history is hidden until the keyboard closes, never a cut sliver). With a physical keyboard the field is ready the moment a conversation opens.

### M9. The first message to a never-heard address (the announcement stamp), on the slowest board

RNS makes a proof of work once per identity before the first announcement. Measured: about 25 s to 45 s on a Pi 4B, **272 s** on
a Pi 3A+ the first time (palmpi, 0.3.1 QA; the second warm-up took 9.9 s: the cost is random). Since 0.3.1 the service makes it in
the background as soon as a connection is on and allowed to send. In plain words: the first time, the deck does a heavy sum to
prove it is a real sender, and a Pi 3A+ is slow at it. A message sent before it is done can fail (palmpi: a message sent 31 s in
failed after 53.6 s; once the stamp was ready, delivery took 3 s); the **first-ever message sent too early may fail and Retry
then works**.
1. On the Pi 3A+ (fresh identity: `rm ~/.local/share/reticulum/identity` with the service stopped, and `rm -r ~/.reticulum/storage`
   if the owner agrees to a new identity): switch Wi-Fi LAN on (the question, Right, Enter). `top` shows four busy workers for up to
   5 minutes (the log says `announcement stamp ready in N s`; wait up to 5 minutes on a Pi 3A+, about 25 to 45 s on a 4B); the app stays usable (Nice 10, CPUQuota 200%).
2. After that, from the other deck: add this deck's address by typing it, Send the first message. It should reach *Delivered*
   within a few seconds, not fail after 50 s. Write down the seconds and the temperature.
3. Send the same first message **during** the warm-up (right after switching on): it may fail and need a Retry, which then works; write that down.
