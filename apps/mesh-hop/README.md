# Mesh Hop

Version 0.3.0 (published), package `mesh-hop`. Part of [cyberdeck-zero-apps](../../README.md).

**Status: early release, published in the registry.** Install it from **Settings > Apps** like the other apps (see
[Install and requirements](#install-and-requirements)). Versions 0.2.2 and 0.3.0 were tested on the deck by the owner with real boards (Seeed XIAO
nRF52840 and a spare XIAO S3 WIO, MeshCore firmware v1.15.0, a board swap included). The GPS switch of the Position box is not tested on a real board yet. Several MeshCore features are not there yet
(see [Known limits](#known-limits)). The screenshots below come from the simulator, not from the panel.

A full-screen client for [MeshCore](https://github.com/meshcore-dev/MeshCore) LoRa mesh networks. The radio is a
separate **companion radio board** plugged into the deck's USB port (MeshCore *companion radio, USB* firmware); the deck is
its screen and keyboard. It needs a 640x480 screen at least. Everything follows the screen (tested in the simulator at 640x480, 800x480,
720x720, 1024x600 and 1280x720): the shell (Home, the page header with the Esc button, the status line, the dialogs) and every page, sized from the
launcher's layout tokens (rows 7 mm, buttons 9 mm, the Settings text size). Mesh Hop replaces the 320x170 UI of [MeshZero](../meshzero/README.md) with a UI made for messaging and keeps everything below
it: the serial transport, the protocol, the client, the model and the history store (`src/main/core/`, a copy of the
MeshZero core with unit tests). `apps/meshzero` stays as it is until Mesh Hop is accepted.

## Screenshots

Made with the simulator's headless mode (`mesh-hop --headless`, a simulated board with invented names and test data), **not on the
deck's panel**. The real screen shows the same 640x480 picture.

| | |
| --- | --- |
| ![Chats](../../docs/screenshots/mesh-hop-chats.png) Chats: channels and direct conversations with unread pills, the conversation, entry and Send | ![Options of a conversation](../../docs/screenshots/mesh-hop-options.png) The options box of a conversation (Ctrl+O, or a 3 s hold on its name) |
| ![Contacts](../../docs/screenshots/mesh-hop-contacts.png) Contacts: sortable table, filter chips, Select, Group, Nearby | ![Contact details](../../docs/screenshots/mesh-hop-details.png) Details of a contact: key, route, last advert, position |
| ![Nearby](../../docs/screenshots/mesh-hop-nearby.png) Nearby: the nodes heard, with Add and Ignore | ![Settings](../../docs/screenshots/mesh-hop-settings.png) Settings: board status, name, radio |
| ![Statistics](../../docs/screenshots/mesh-hop-statistics.png) Statistics of the board | ![History settings](../../docs/screenshots/mesh-hop-history.png) Settings > History and the board actions |

## What it does

* **Chats**: the channels and the direct conversations (under CHANNELS and DIRECT), each on two lines: the name, then the last message
  and its time (the day, MM-DD, before today); at the right the **number of unread messages** in blue (nothing for none, "muted" for a muted conversation) and the
  chevron. The total of unread messages is on Home's Chats row. A tap selects a row, a tap on the selected row opens it. **At 640x480
  Chats is one pane**: the list, then the conversation as its own page (its name is the header's title; the header's Esc goes back to
  the list). **On the wider screens both panes show side by side when both fit at the text sizes** (800x480, 1024x600 and 1280x720 in the
  simulator; a square 720x720 stays one pane: its list would be too narrow): the list takes about 35 % of the width, the conversation
  follows the selected row, a tap on the selected row puts the keys in the entry, and the header's Esc leaves Chats for Home. Over
  the list (and over both panes) one **menu button** (three lines) sits at the right of the line under the title, the list right under
  its row: its menu, titled with the selected conversation ("Options for # Public"), has **Search messages** and **Add channel**, then
  the options of the selected conversation and, for a direct chat with a known contact, **Details**; with no conversation (an empty
  list) only the first two. Select a row first (one tap), then open the menu. A conversation's own page (one pane) has no bar and no
  button: the header, the line under it, the messages, the entry (Ctrl+O opens the same menu there). The conversation shows the sender and time of each message (blue, yours green), the status of
  yours and the text in the large size, the line under the title says the nodes heard in the last hour (or "muted", or the bytes left
  while you write); the message entry and a **Send** button are at the bottom. **Without a keyboard** a tap on the entry brings the
  on-screen keyboard and Chats becomes the **compose view**: the header, the last message in the line under it, the entry (and Send
  where it fits: at 640x480 the keyboard's Enter sends), the keyboard below. After a Send, or Esc, the keyboard goes. A channel
  message ends at **sent** when the board accepts it (a channel has no acknowledgement, so never "delivered"; if the board
  does not answer within 5 s it shows "sent (unconfirmed)"); a direct message goes sending, sent, delivered or no ack.
  **Heard back by N repeaters** (phase 3): the board reports every radio packet it hears (PUSH_LOG_RX_DATA, no command needed). When
  your own channel message comes back from the mesh, "sent" becomes "heard back by N repeaters"; N grows while it keeps coming back
  over new routes for 60 s, then it is final and saved with the message. Read it as a hint, not a count of repeaters: the app cannot
  decrypt, so it matches the channel hash and the size of the packet; **N is the number of distinct routes** (a copy heard straight
  from the sender is not counted; one repeater reached over two routes counts twice, two repeaters with the same short hash once), and
  another person's message of the same size on the same channel in the same minute can be taken for yours. It needs the channel key
  read from the board in this session (done at connect; a firmware without channel commands gets no count). Direct messages keep their
  acks and are not counted.
  **Mute** (Ctrl+M, or the menu: the menu button, Ctrl+O or a 3 s hold on its row) silences a channel or a contact: no unread count, not counted in Home's total, "muted" in the list.
  **Options of a conversation** (in the menu, after Search messages and Add channel: the menu button over the list acts on the
  selected row, Ctrl+O on the selected or open conversation, a finger held 3 s on a row selects it and opens the menu; a bar shows the
  hold filling): a direct chat offers *Mute / Unmute this contact* and *Delete conversation*; a channel offers *Mute / Unmute* and
  *Delete messages*. Deleting asks Yes / No first with the number of messages, removes them from memory and from the history
  file, and a deleted direct conversation leaves the list (the contact stays in Contacts and on the board; a new message
  starts the conversation again; a channel stays, only its history is cleared).
  **Add channel** (in the menu) opens a menu: a hashtag channel (`#name`), a private channel with a key typed as 32 hex characters, or a
  private channel with a random key (shown as text to share). A direct chat has **Details** in the menu (the contact's details).
* **Search messages** (new in 0.2.0; in the menu, or Ctrl+F): type the words (they are matched in the text and
  in the sender's name, upper and lower case alike) and narrow the result by **chat** (Ctrl+K), **dates** (Ctrl+D: any date, last 24 h,
  7, 30 or 90 days, or a typed day or range like `2026-09-01 2026-09-30`) and **direction** (Ctrl+R: all, received, sent). The
  results are listed newest first with the time, the chat, the sender and the text around the match; a tap (or Enter) opens the
  conversation at that message, framed in blue. It searches what the deck keeps (200 messages per conversation, 1500 in all).
  Without a keyboard the on-screen keyboard comes with the search (only the words fit above it at 640x480); its Enter puts it away and
  the filters and the results come back, a tap on the words brings it again.
* **Contacts**: a row per contact on two lines: the name, then the type, when it was heard and the hops (and the SNR on the
  wider screens); the distance at the right (when both positions are known). The line under the title says what is shown
  (how many, which type, heard when). The bar above the list: **Filter** (the type, the time since heard, the group: a pick
  list each; keys `T` and `H` cycle the first two, `G` opens the groups), **Sort** (name, heard, type, SNR, hops, distance;
  the field in use again reverses it; `S` and `R`) and **More**: **Send advert** (flood or zero-hop in an arrow choice that
  starts on the scope used last; the keys `A` and `D` send them at once), **Nearby** (`N`) and **Manage contacts** (`X`);
  `U` asks the board for its contacts again (it sends them by itself when it connects). A "Loading..." box shows while a long list is re-sorted or filtered, and the chosen order is kept. A tap selects a
  row, a tap on the selected row opens its details; Enter opens the chat of a chat node, or the detail of a repeater, room or
  sensor. The list is virtual: only the rows in view are widgets, so Up/Down on 161 contacts is as quick as a touch scroll.
  New in 0.2.0:
  * **Details** (a tap on the selected row, key `I`): name, type, groups, the whole public key, the route (flood, direct,
    or the hops with their hash size and the hashes), last advert, when we heard it, position, distance, SNR; Message,
    Groups and Delete on the bar (the header's Esc closes them).
  * **Manage contacts** (More, key `X`): a check box on every row; a tap or Space / Enter marks, **Select...** offers all shown, none, not heard
    for 7 or 30 days or more, never heard, invert; **Delete n** asks Yes / No with the count (and says what hidden-by-filter marks
    stay), then deletes **one by one on the board** with a progress box and a Stop button. **Group...** adds the marked contacts to
    a group, takes them out, or makes a new group with them.
  * **Groups** (kept on the deck, per board, in `groups.jsonl`, the previous version in `groups.bak`; the board knows nothing of them): the
    **Group** button (key `G`) filters the table by one group, and makes, renames or deletes groups.
  * **Nearby** (key `N`; the button turns blue with the number of waiting nodes): the nodes heard, newest first, pending ones first,
    each with its type, SNR, hops (and hash size), age, and **Add** / **Ignore** (touch or `Enter` / `I`). Sources: the advert pushes
    of the board, the adverts in its radio log (name, type, position, SNR, hops), the pending contacts of the manual add mode,
    and **Scan** (key `S`): a zero-hop discover that asks the nodes in direct radio range to answer (8 s). Ignored nodes are hidden
    (the **Ignored** button shows them and restores them).
* **Settings**: board status (connection, model, firmware level, battery, clock, **position**: only the position, or "not set") and, when the
  firmware lists the `gps` custom variable, a **Board GPS** switch; **Statistics**; node name; **Preset** (the radio presets of
  `api.meshcore.nz`: 26 are built in, dated 2026-10-07, see `src/main/core/radio_presets.inc`; when the deck is online the app
  refreshes the list in the background at launch, once more 60 s later if that failed, keeps the last good copy in
  `~/.local/share/mesh-hop/presets.jsonl` and says in the preset box which list it shows), frequency, **bandwidth, spreading factor,
  coding rate and TX power in a choice box** (the value in the middle, a large arrow on each side: touch the arrows, or Left /
  Right, Enter accepts, Esc cancels).
  * **Position box** (Enter or a tap on the Position row): the position the board holds, the **Board GPS** switch (only when the
    firmware lists the `gps` custom variable; a board without GPS shows none), and the **latitude** and **longitude** typed in decimal
    degrees ('.' or ',' before the decimals; Tab: next field, Enter: OK, Esc: cancel), on a keyboard, or on the on-screen keyboard after a
    tap on the field (the box keeps the field above it; the keyboard's Enter is OK, its Esc Cancel). A value outside
    -90..90 / -180..180, or not a number, is refused in the box and nothing is sent. OK sends SET_ADVERT_LATLON (`0E`, latitude and
    longitude in signed microdegrees, altitude 0) and reads the position back (SELF_INFO); **Clear** sets 0, 0, which the firmware and
    the app read as "no position". Setting a position sends no advert: the board puts it into its own adverts only when its location
    policy shares it (the box says which). With the board GPS on, the GPS fix replaces a typed position.
  New in 0.2.0:
  * **Path hash size**: 1, 2 or 3 bytes per repeater hash in the paths of the packets the board sends (MeshCore "path hash
    mode" 0, 1, 2). A larger hash means fewer collisions between repeaters but leaves less room per message. Read from the board
    (DEVICE_INFO, firmware level 10 and up), set with SET_PATH_HASH_MODE and read back; an older firmware shows the
    "Firmware too old for this feature" notice instead of the row. Repeaters on firmware 1.12 or older only know 1-byte hashes.
  * **Repeat** on or off (the companion's client repeat, firmware level 9 and up): only on the frequencies the board lists
    (GET_ALLOWED_REPEAT_FREQ); otherwise there is no row (it stays while repeat is on, to switch it off); switching it on asks Yes / No.
  * **Scheduled advert**: off (default), every 1, 3, 6 or 12 h, flood or zero-hop. The board has no advert timer of its own: the
    app sends the advert while it runs and the board is connected.
  * **Contacts**: **Add nodes by itself** switches the manual add mode (off: new nodes wait in Nearby until you add them) and
    **Still added by itself** edits the auto-add filter (which types the board still adds in manual mode, and replace-oldest).
  * **Statistics** (firmware with GET_STATS): battery, uptime, errors, queue, noise floor, RSSI, SNR, TX / RX airtime and the
    packet counters; Refresh, and an automatic refresh every 5 s while the screen is open.
  * **Packet log** (phase 3, D8; under Statistics, a panel over Settings like the statistics): the radio packets the board hears, one row
    each: time (hh:mm:ss), route (FLOOD / DIRECT, `T-` with transport codes), payload type, hops, SNR, RSSI, payload size and, for a
    group text, the channel hash with the channel name when one of your channels has that hash (`a|b ?` when two share it). The
    selected row shows its path and its raw bytes in hex below. **Capture is off at every start**: Start / `S` / Space starts and
    stops it, Clear / `C` empties it, Up/Down/PgUp/PgDn/Home/End select (End follows the newest again), Esc or Close goes back. It
    keeps the last 500 packets in memory only; nothing is written to disk (no export yet).
  * **Reboot the board** (Yes / No) and **Factory reset the board** (two Yes / No boxes; the Yes of the second waits 3 s). A factory
    reset erases the board's keys (its identity), contacts and channels. The firmware formats its file system first (up to a minute on
    an ESP32 board), answers OK and restarts with a new identity; the app waits up to 60 s, then judges by the identity the board reports
    when it is back: a new public key shows "Board reset" (an empty board: its own empty chat list, the old identity keeps its data on the
    deck), the same key shows "not reset". The frame is the command byte `0x33` and the word `reset` (the firmware checks it). There
    is no power-off command in the companion protocol, so none is offered.
  * the rest as before: **direct message retries**, clock sync, the channels (the fixed Public channel is not listed): Mute, Key
    (shows the key as text) and Remove (asks Yes / No); a **History** section (*Delete all messages*, *Delete messages older than*
    7, 30 or 90 days); **Undo changes** and **Save to radio** are the last rows and each asks Yes / No.
  The board clock follows the deck policy of MeshZero (deck NTP, board GPS, or a typed start time).
* **Firmware check**: a feature the board's firmware lacks (custom variables, channel commands, path hash mode, statistics,
  auto-add filter, discover, factory reset) is switched off with the notice "Firmware too old for this feature"; repeat then has
  no row at all; a board without GPS shows no GPS row.
* **Map** and **Terminal**: not there yet, so they are not on Home (the packet log lives in Settings, next to the statistics).
* **Home, header, status line, hints**: the app opens on **Home**, a list with a chevron on each row: **Chats** (the unread total in blue, or "no new"),
  **Contacts** (the number of contacts) and **Settings** (the board). There is no tab row. A page has the standard header: the title ("Chats", or
  "Mesh Hop > Chats" where it fits; one more part inside a sub-page such as a contact's details, Nearby, search, statistics, packet log), the **Esc**
  button and the launcher's shared strip (clock, Wi-Fi, Bluetooth). The line under the header says the board (model and state) or, for a few seconds, a
  notice. The key hints of the screen are a line at the bottom, shown only while a keyboard is connected. Popups, questions, menus and editors are the
  launcher's dialogs, and the Position box is one; an editor's field brings the on-screen keyboard when no keyboard is connected (the dialog stays
  above it, its buttons give way to the keyboard's Enter and Esc).
* **Per board (0.2.2).** The app knows the board by its public key (SELF_INFO). The history, read marks, the contact and channel caches,
  muted conversations, groups, ignored nodes and the advert schedule of each board are kept in
  `~/.local/share/mesh-hop/boards/<first 12 hex digits of the key>/` (JSON lines and text, bounded per board: 200 messages per conversation,
  1500 in all). Plugging another board shows that board's own conversations and contacts; plugging the first one back restores them.
  Before a board has answered, Chats is empty ("No board yet"). The retry settings, the presets cache, the port, the log stay global in
  `~/.local/share/mesh-hop`. The files of 0.2.1 and before (one set for "the" board) move into the folder of the first board that connects
  with this version; the originals stay as `*.pre-boards.bak`. The folders of boards that are not connected are kept; Settings > History
  has **Forget this board's data** and **Other boards' data** (forget the saved data of every board that is not connected, with the size).
  The old
  `~/.local/share/meshzero` is not touched or imported. `mesh-hop.log` in the same folder is the debug log (size bounded, no message
  text): it records each channel send and what the board answered, bulk deletes, reboot and factory reset requests, the preset
  refresh (`MESHHOP_DEBUG=1` logs every frame).

## Input rules

* Text is typed with a **keyboard** (Bluetooth or USB) or, while none is connected, with the **on-screen keyboard**: the launcher's shared
  rule (`input.state`: no keyboard, or a Bluetooth keyboard asleep for 10 minutes; Settings > System > On-screen keyboard; `APPLAUNCH_OSK`)
  decides, the app never shows it by its own test. It comes for the field being typed into (a tap on the message entry or on the search
  words, an editor, a field of the Position box) and goes after a Send, Enter or Esc, or within about a second when a keyboard connects.
  The fields are drawn as the launcher's text fields; the app types into them itself (the US or FR layout of the key bridge, the
  editors' rules). With neither keyboard an editor says "Keyboard needed: wake the Bluetooth keyboard".
* A short **Esc on the keyboard is Back** and never quits (from the root of a page it goes to Home, at Home it says "Hold Esc 3 s to exit"); **holding Esc for
  3 s** ends the app: the launcher does that. The **Esc button of the header** (touch) goes Back the same way; from the root of a page it goes
  to Home, and at Home it asks **Quit Mesh Hop?** (Cancel / Quit), so a touch-only board can leave the app. Esc closes the newest layer first: box, editor,
  the on-screen keyboard of the entry or of the search words, search, statistics, packet log, details, Nearby, select mode, a conversation's page
  (one pane; with two panes the header's Esc goes to Home).
* Everything is reachable by touch: tap rows, Home rows, bar cells, arrows and buttons, drag to scroll lists and conversations.
  There is no swipe gesture. Rows and bar cells are at least 7 mm high (the rows of a list where a tap only selects: 5 mm), buttons 9 mm.

| Key | Action |
| --- | --- |
| Up / Down, Enter | Home: choose a page, open it |
| Tab / Shift+Tab | on a page: the next / previous page (Chats, Contacts, Settings), in a loop |
| Up / Down | Chats: select a conversation (list) or scroll (a conversation, the entry); Contacts, Settings, Nearby, results: move |
| Enter, or typing | Chats list: write (one pane: the conversation opens with the entry); entry: send; Contacts: open (select mode: mark); Settings: change / do |
| Left / Right | Settings: step the value of the row; entry: move the cursor; choice box: previous / next value |
| Esc | Back (box to nothing, entry to list, a conversation's page to the list, close detail, Nearby, search, statistics or select mode; the on-screen keyboard first when it is up); the header's Esc button too; from the root of a page it goes to Home, at Home it asks "Quit Mesh Hop?" |
| Ctrl+M | Chats: mute / unmute the conversation (plain letters start a message); Settings, channel row: `M` |
| Ctrl+O | Chats: the menu (Search messages, Add channel, the options of the selected or open conversation, Details); the touch way is the menu button over the list, or a 3 s hold on a row |
| Ctrl+F | Chats: search the messages (then Ctrl+K chat, Ctrl+D dates, Ctrl+R direction, Enter opens the result) |
| `S` `R` `T` `H` `A` `D` `U` | Contacts: sort, reverse, type filter, heard filter, advert, zero-hop advert, refresh |
| `X` `G` `N` `I` | Contacts: manage contacts (marks for delete / groups), group filter, Nearby, details of the selected contact |
| Space / Enter, `A` `N` `I` `M` `D` `G` | Contacts, select mode: mark, all, none, invert, select menu, delete, group |
| Enter, `I`, `S`, `V` | Nearby: add, ignore (or restore), scan, show / hide the ignored |
| `G` `D` | Contact details: groups, delete |
| `R` `A` | Statistics: refresh, switch the automatic refresh |
| `S` or Space, `C`, Up/Down | Packet log: start / stop the capture, clear it, select a row |
| `S` `V` | Settings: save, undo (each asks Yes / No) |
| `K`, Del | Settings, channel row: show the key, remove the channel |
| `Y` `N` | Yes / No boxes |

The keyboard layout is US by default, AZERTY when `MESHHOP_KEYMAP=fr` or `XKBLAYOUT="fr"` is in `/etc/default/keyboard`.

## Install and requirements

**Requirements**

* A **MeshCore companion radio board** with the *companion radio, USB* firmware, plugged into the deck's USB port. Some
  features need a newer firmware (path hash size needs firmware level 10 and up, repeat level 9 and up; statistics, discover and
  factory reset need their commands); without them the row says "Firmware too old for this feature".
* A keyboard (Bluetooth or USB) for comfortable typing; without one the launcher's on-screen keyboard is used (see [Input rules](#input-rules)).
  US layout (QWERTY) by default, AZERTY when `MESHHOP_KEYMAP=fr` or `XKBLAYOUT="fr"` is in `/etc/default/keyboard`.
* A launcher with **full-screen app support** (`X-Fullscreen=true` in the `.desktop` file).
* The user must be in the `dialout` group (the Raspberry Pi OS default user is).
* `curl` (the package depends on it; the radio preset list is downloaded by `/usr/bin/curl`; without it the app keeps the saved
  or built-in list) and `libfreetype6`.

**Install from the deck:** Settings > Apps > Sources > + Add a GitHub source `OSRdesign/cyberdeck-zero-apps` (if not added yet),
then Apps > Mesh Hop > Enter (it asks for the sudo password).

**Or install a local `.deb`** (for a build you made yourself). On a PC with this repository:

```
python tools/make_registry.py --only mesh-hop --out <a folder outside packages/>
```

Copy the `mesh-hop_0.3.0_arm64.deb` it writes to the deck and, on the deck:

```
sudo apt install ./mesh-hop_0.3.0_arm64.deb
```

(`apt` pulls `curl` and `libfreetype6` if missing.) The tile "Mesh Hop" appears in the launcher. Remove it with
`sudo apt remove mesh-hop`.

The board is found by itself (`/dev/ttyACM*`, `/dev/ttyUSB*`, by USB id); to force a port put its path in
`~/.local/share/mesh-hop/port` or start the app with `MESHHOP_PORT=/dev/...`. Fonts: the app reads the fonts the launcher
installs in `/usr/share/APPLaunch/share/font` (Montserrat, Font Awesome, DejaVu Sans) and falls back to its built-in
Montserrat without accents.

## Reboot and factory reset: read this

Settings > **Reboot the board** restarts it and erases nothing. Settings > **Factory reset the board** **erases the board's keys
(its identity), contacts and channels**; it asks two Yes / No questions and the Yes of the second waits 3 s. The board then
has a new identity: to the mesh it is a new node. Do not use it on the board you rely on unless you mean it.
Settings > History > **Forget this board's data**, **Other boards' data** and the *Delete* rows remove the deck's saved copy
of messages, groups and flags; each asks first.

## Files the app keeps

All in `~/.local/share/mesh-hop`. The log holds no message text.

| Path | What |
| --- | --- |
| `boards/<first 12 hex digits of the board key>/` | per board: `messages.jsonl` (history), `read.txt` (read marks), `contacts.jsonl` and `channels.jsonl` (caches), `groups.jsonl` (and `groups.bak`), `prefs.txt` (mute flags, ignored nodes, advert schedule) |
| `prefs.txt` | global: the direct message retry settings |
| `presets.jsonl` | the last good copy of the radio preset list |
| `port` | optional: the serial port to use |
| `mesh-hop.log` | debug log, size bounded |
| `*.pre-boards.bak` | the originals of the files of 0.2.1 and before, kept after they moved into the first board's folder |

## Known limits

* There is no **Map** and no **Terminal** yet: they are not on Home (no map tiles, no command line yet).
* **USB only**: no Bluetooth LE connection to the board yet (planned; the deck's Bluetooth is also used by the keyboard).
* A **channel message has no delivery acknowledgement** in MeshCore: it ends at "sent" (or "sent (unconfirmed)" if the board does
  not answer within 5 s), never "delivered". Direct messages do get "delivered" or "no ack".
* "Heard back by N repeaters" is an approximation (distinct routes, matched by channel hash and size; see Chats above). A count that
  is still moving when the app quits is lost; the final one is saved (an extra `"hb"` key in `messages.jsonl` that older versions ignore).
* The packet log is not saved or exported, and holds 500 packets at most.
* The scheduled advert runs only while the app runs and the board is connected (the board has no advert timer of its own).
* The deck keeps 200 messages per conversation and 1500 per board.
* The screenshots on this page come from the simulator, not from the panel.

## Credits and licences

Mesh Hop is MIT (see the `copyright` file of the package).

* **MeshCore** firmware and protocol: Scott Powell / rippleradios.com, MIT, <https://github.com/meshcore-dev/MeshCore>.
* **meshcore_py** (Florent de Lamotte, MIT, <https://github.com/meshcore-dev/meshcore_py>) was the reference of the wire
  format; the protocol code was written from the protocol documentation and from it. The radio presets come from the public list
  of `api.meshcore.nz`.
* **MeshCore Open** (MIT), **meshcore-gui** (MIT), **wadamesh** (GPL-3.0) and **Meshy** (GPL-3.0-or-later) were looked at as
  feature references only. **No code was copied from the GPL projects.**
* **LVGL** (MIT), **FreeType** (FreeType License or GPL-2, the system `libfreetype6`) and the status bar renderer
  (`cp0_statusbar`) of the CardputerZero launcher by M5Stack (MIT). The launcher's fonts are read at run time (Montserrat, Font Awesome
  Free, DejaVu Sans: SIL OFL 1.1, CC BY 4.0 for the icons, Bitstream Vera / DejaVu licence); they are not in the package.

## Build and test

```
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh cross        # aarch64, into root/usr/share/APPLaunch/bin/mesh-hop
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh host         # PC: shared harness + scenes, mesh-hop-script, unit tests
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh flows [WxH]  # the 20 simulator flows (tests/flows), about 15 minutes
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh flows sizes  # shell, pages and chats scripts at the five sizes and at the HackBerry's density
```

Mesh Hop is a native fullscreen app on the shared app platform (`apps/_common`, see `apps/_common/build_app.sh -h`): it links
the launcher's `cp0_lvgl` (pinned by `apps/_common/LAUNCHER_REF`) for the display (rotation, 16 / 32 bpp), the touch, the
keyboards and the status strip, through a scratch project under `projects/` of the launcher build tree. The shell is built from the
launcher's widgets (`cp0::ui::Page`, `Dialog`, `StatusStrip`, the on-screen keyboard `cp0::ui::osk` with the shared rule of `apps/_common/osk_policy`)
with the layout service's tokens; every page fills the width under the shell, as high as the room left by it, the key hint line and the on-screen keyboard
(`mesh_app.cpp` `layout_body()`, `layout_chats()`). `tools/meshcore_sim.py` simulates a board
(options: `--contacts N`, `--chan-reply ok|msgsent|odd|none`, `--old-firmware`, `--no-gps-var`; 0.2.0: `--fw-level N`, `--manual-add`,
`--new-nodes N`, `--max-contacts N`, `--repeat-ranges`, `--slow-remove SEC`, `--no-autoadd`, `--old-autoadd`, `--old-other-params`,
`--no-discover`, `--discover-reply`, `--no-stats`, `--old-stats`, `--no-factory-reset`, `--strict-reset-word`; 0.2.2: `--identity A|B` (a second
board with its own key, contacts and channels), `--reset-format-delay SEC`, `--reset-no-ok`; stdin commands `newnode`, `reboot`, `swap` (exchange the
board on the cable)). The scripts in `tests/flows/` (`smoke.script`, `bigcontacts.script`, `chan-silent.script`, `firmware.script`, `offline.script`)
drive `mesh-hop-script` (`--script f --shot-dir d [--size WxH] [--ppmm N]`: the shared headless harness's display at any size, keys through the
shared keyboard queue, taps at the 640x480 coordinates of the page bodies moved to where the body is (most taps are by label), PNG screenshots, `bench <key> <n>` times key presses,
`hold <x> <y> <ms> [shot]` or `hold "label" <ms> [shot]` keeps a finger down, `tap "label"`, `tapfield <n>` (the n-th text field shown), `tapicon` (a button without text), `tapkey "label"` (a key of the
on-screen keyboard), `@panes1` / `@panes2` before a command (only with one / two panes of Chats), `keyboard on|off` (also the launcher's `input.state`: off lets the on-screen keyboard show), `fill` (the pages fill the width),
`check` (the shared layout checks, the field above the on-screen keyboard included, and the size rules of every control: any finding fails), `expect text|osk`); `run_flows.sh` runs all of them.
The shared harness's scenes are in `tests/scenes/`, the unit tests in `tests/unit/`. `history_run.sh` seeds a history of every age and runs `history.script`; `presets_run.sh` (a local web
server stands in for `api.meshcore.nz`; `MESHHOP_ONLINE=0|1` forces the online test, `MESHHOP_PRESETS_URL` and `MESHHOP_CURL` replace the address and
the program) now also runs the retry case. 0.2.0 adds `p2_run.sh` (seeded history, the words `@D60@` in a script become the date 60 days
ago) with `p2-contacts.script`, `p2-nearby.script`, `p2-search.script`, `p2-settings.script` and `p2-oldfw.script`; the first line of each
says which simulator options to give. 0.2.2 adds `b-swap.script` (two boards, run with `SIMCTL=1`: the script command `sim <line>` types into the
simulator), `b-noboard.script` (`NOSIM=1`) and `b-reset.script` (`--reset-format-delay 4`). The Position box has `position.script` (a GPS board)
and `position-nogps.script` (`--no-gps-var --share-location`); the simulator keeps the position set by SET_ADVERT_LATLON (`--no-position` starts at 0, 0).
Phase 3: `--heard-back N` makes the simulator send our own channel messages back in its radio log over N routes 1 to 3 s after the send
(`--heard-back-direct` adds a copy with an empty path), and its radio log every 3 s alternates random bytes with well formed packets of other
traffic. `heard-back.script` (`--heard-back 2 --heard-back-direct`) and `packet-log.script` (`--heard-back 2 --new-nodes 2`) cover both features.
P4: `chats_run.sh` seeds a history of short and long messages, none read (`seed_chats.py`; `BIG=N` adds N to the Public channel), and runs
`chats.script`: the list and its menu (a channel, then a direct chat selected), the tap rule, a conversation's page (no bar, no button; Ctrl+O), writing with a keyboard, the compose view with the on-screen keyboard, the search,
an editor and the Position box with it, the 3 s hold; `run_flows.sh sizes` runs it with `shell.script` and `pages.script` at the five sizes.

## Layout of the sources

| Path | What |
| --- | --- |
| `src/core/` | transport, protocol, client, model, store, clock policy, presets (`radio_presets.inc` is the dated data file), preset feed (strict parser, saved copy, background curl, download schedule), channel keys, firmware features, contact groups, message archive search, log, the radio log parser and echo tracker (`logdata.*`), the packet log ring (`packet_log.*`) (no LVGL; unit tested) |
| `src/ui/ui_logic.*`, `ui_phase2.cpp` | keys, editor filters, formatting, chat and contact rows, filters and sort state, selection, groups, details, Nearby rows, search rows, statistics view, popup logic, Settings layout, Back rule (no LVGL; unit tested) |
| `src/ui/key_bridge.*` | the shared platform's keys (cp0_lvgl key_item) to the app's key events (no LVGL; unit tested) |
| `src/ui/stage.*`, `session.hpp`, `app_spec.cpp` | the keyboard bridge (the platform's keys to the app's), the tick timer, the app's spec for the shared runners (the 640 px stage of P1 is gone) |
| `src/ui/mesh_app.*` | the shell (Home, header, status line, hint line, dialogs), the screens, Chats in one or two panes, the text fields and the on-screen keyboard, the virtual row list |
| `src/main.cpp` | `cdk::run` (apps/_common: display, touch, keyboards, signals, the "needs 640x480" notice) |
| `tests/flows/`, `tests/scenes/`, `tests/unit/` | the simulator flows and `mesh-hop-script`, the shared harness's scenes, the unit tests |
