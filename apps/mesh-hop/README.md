# Mesh Hop

Version 0.8.1, package `mesh-hop`. Part of [cyberdeck-zero-apps](../../README.md).

**Status: early release.** 0.2.2 and 0.3.0 were tested on the deck by the owner with real boards (Seeed XIAO nRF52840 and a
spare XIAO S3 WIO, MeshCore firmware v1.15.0, a board swap included). 0.4.0 is the responsive, touch-complete version; its board
tests are in [`tests/BOARD-TESTS.md`](tests/BOARD-TESTS.md). The GPS switch of the Position box is not tested on a real board yet.
0.5.0 adds Bluetooth LE and Wi-Fi (TCP) nodes: tested with a simulated BlueZ, a simulated board and the simulator over TCP, **not
yet on a real board** (section N of the board tests). 0.6.0 adds the **CLI** page (the upstream meshcore-cli inside Mesh Hop):
tested with the simulator only, **not yet on a real node** (section 10 of the board tests). 0.7.0 regroups Home (a **Tools** page)
and adds the **Repeater page** (log in to your repeaters and rooms from Contacts): tested with the simulator and the bundled
meshcore-cli only, **not yet on a real node** (section 11 of the board tests). 0.8.1 adds the page's **read-only view** (Status,
Neighbours, Owner, each on a screen of its own): tested with the simulator; its first layout (0.8.0, one list, never published) was
tried by the owner on the deck (section 12 of the board tests). Several MeshCore
features are not there yet (see [Known limits](#known-limits)).

A full-screen client for [MeshCore](https://github.com/meshcore-dev/MeshCore) LoRa mesh networks. The radio is a separate
**companion radio board** (a *node*) reached over the deck's USB port, over **Bluetooth LE** or over **Wi-Fi** (MeshCore's
companion firmware for USB, BLE or Wi-Fi); the deck is its screen and keyboard. Everything works **by touch or by keyboard**, on any screen from **640x480** up. Mesh Hop replaces the 320x170 UI of
[MeshZero](../meshzero/README.md) and keeps its core: the serial transport, the protocol, the client, the model and the history
store (`src/core/`, with unit tests).

## What changed in 0.8.1

* **Repeater page: Status, Neighbours and Owner** (report 036, second part), as a **hub and three screens**. The page itself is
  the hub: its bar is **Log in** / **Log out** and **More**; under ACCESS (as before) a section **READ** has three rows with a
  chevron, each with a one-line summary at the right: **Status** ("4.12 V · up 3 d 4 h"), **Neighbours** ("12 heard · newest 12 s
  ago"), **Owner** ("Olivier · fw 1.12.0"), or "not read yet", "needs login", "no answer", "firmware too old", "waiting… up to 12 s".
  A row (a tap on it selected, or Enter) opens that section's **screen**: the header "Hilltop repeater > Status", the line under it
  "read 14:02 · tap Refresh to read again" (or "waiting… up to 12 s", or what went wrong), **one button, Refresh** (**Cancel** while
  a read runs; `R` or Ctrl+R), and the section's rows:
  * **Status** (guest or admin): battery, uptime, noise floor, last RSSI / SNR, packets received / sent, flood / direct received
    and sent, airtime TX / RX, duplicates, RX errors (newer repeaters), TX queue; a room: its posts stored / pushed.
  * **Neighbours** (guest or admin): the repeaters it hears at zero hop, newest first, by their name in your Contacts (else `<` and
    their key prefix; "(ambiguous 2)" when two contacts start the same), with SNR and when; **More neighbours** reads the next
    page while some remain ("14 of 22 shown").
  * **Owner**: logged in, the owner's name, the repeater's **firmware version** and its owner info; not logged in, **Read owner
    (direct route)** asks without a login, only when the route is direct (the repeater answers that 4 times in 3 minutes, for
    everyone together); on a flood route "Log in to read".
* **What is read when**: the **status once by itself** right after a successful login (guest or admin); a screen never read is
  read **by itself once** when you open it (logged in); after that, **Refresh** on its screen. The hub's **More > Refresh all** (or
  `R` on the hub) reads Status, then Neighbours, then Owner, one after the other (their summaries show it; a section without an
  answer is marked and the next one still goes; a lost login, a lost link or the CLI stop the chain; "Stop reading" there, or
  leaving the page, cancels). No automatic retry, no confirmation box. Esc on a screen goes back to the hub; what was read stays
  while the page is open; leaving it, Log out and a new login clear it.
* `R` on the Repeater page was Reset path in 0.7.2: Reset path stays in **More**, and Ctrl+R in the login box. More's order:
  Details, Open in CLI, Reset path, **Refresh all**, Forget password (last).
* When the node learns a new route to a contact (a login or a request that went by flood brought one back), Mesh Hop reads that
  contact again: the route line and Read owner (direct route) follow at once.
* A login to a repeater or room older than v1.9.0 now shows **Admin** for an admin (0.7 showed Guest); when an old node's answer
  carries no role at all the page says "Logged in" instead of guessing.
* 0.8.0 was never published: it had the three sections in the page's one list, with a Refresh on the bar that read the section of
  the selected row; after his test on the deck the owner chose the hub and a screen per section.

## What changed in 0.7.2

* **Repeater page**: the login box now shows the **route** under its title ("Route: direct, 2 hops", or "Route: flood: the login
  goes to the whole mesh"), since the box covers the page's own line. **Reset path** forgets the node's stored route to that
  repeater (MeshCore's reset path: the next message goes by flood): a button at the right of "Remember on this deck" in the login
  box when a route is known (Ctrl+R), and in the page's More (or `R`, until 0.8.1), before or after logging in. No confirmation box; the box or the status line
  says "Path reset: the next message goes by flood" and the route lines change to flood.
* **More** of the Repeater page, new order: Details, Open in CLI, Reset path, Forget password (only when one is saved) last.
* **CLI**: the **To…** button, its picker and Ctrl+T are gone: pick a repeater in Contacts (a tap on it, More > **Open in CLI**), or
  type `to <name>`; the entry line has the room. Opened from Tools, the status line says so until you send a line. The automatic
  `to <its key>` of Open in CLI shows as "to Hilltop repeater <909192" instead of the 64 hex digits (it still sends the full key).
* (0.7.1's fixes are below: no deck reset when a USB node is chosen, no stale USB node name.)

## What changed in 0.7.1

* On the Pi Zero 2 W the whole deck reset when a USB node was chosen with 0.6.0 and 0.7.0 (0.5.3 connected fine). The only new
  work at that moment was the CLI's background warm-up (`meshcore-cli -v`, Python compiling its files for 10-20 s), started as
  soon as a node connected. It now runs only when the CLI page is opened, at the lowest CPU and disk priority, and the first
  session waits for it. `MESHHOP_NO_WARMUP=1` skips it. (Why the deck reset under that load is not known yet.)
* **Choose a node**: a USB row no longer shows the name of the board that was on that port before. The name learned on a port is
  kept with the USB identity of the device that answered (USB id and serial number) and shown only while that device is plugged
  there; another board shows its USB name until it has answered once.

## What changed in 0.7.0

* **Home**: Chats, Contacts, **Tools**, Settings, Node. **Tools** holds Nearby, the Packet log, Statistics and the CLI: the CLI row
  of Home and the Statistics and Packet log rows of Settings moved there (the pages themselves did not change). Tab / Shift+Tab go
  Chats, Contacts, Tools, Settings in a loop.
* **Contacts**: the bar is **Search**, Filter, Sort, More. Search (or `/`) finds a part of the name in any case, or `<` and the start
  of the key (`<9a3f`), as the official app does. A tap on the selected row now opens it **by its type**: a repeater or a room its
  **Repeater page**, a chat contact its conversation, a sensor its details. A 3 s hold on a row (or `I`) opens its **Options**:
  Manage, Message, Details, Groups, Delete. More keeps Nearby (and `N`).
* **Repeater page** (report 036, first part): log in to a repeater or room with its password (masked; **Remember on this deck**
  keeps it in the CLI's own password file, so the CLI's `login` asks nothing either) or as a **guest** (a blank password); the page
  shows the role since when, the repeater's clock against the deck's, **Log out**, and **More**: **Open in CLI** (meshcore-cli
  already on that repeater, admin commands work with no `login` typed), Forget password, Details. Status, neighbours and owner come
  in the next version.
* **CLI** (removed in 0.7.2): **To…** (beside Help, or Ctrl+T) picks a repeater or room from the contacts with the same Search and types `to <its full
  key>`: two contacts of the same name, a name with spaces or one that looks like a key can no longer pick the wrong node. Its Esc
  goes back to Tools (or to the Repeater page it was opened from).
* **Settings**: a first section **MY CONTACT**: the node's name (a link to RADIO > Node name) and its public key (Enter shows the
  whole 64 hex digits).

## What changed in 0.6.0

* **CLI** (Home > CLI): the upstream **meshcore-cli 1.6.6** runs inside Mesh Hop on the node in use, for example to log in to
  your repeaters and administer them; Chats keep their messages meanwhile. A Help page lists the common commands and the
  dangerous ones. See [CLI](#cli).
* The package bundles meshcore-cli and its Python libraries in `/opt/mesh-hop/cli` and now depends on `python3 (>= 3.10)`,
  `python3-requests` and `python3-serial`.

## What changed in 0.5.3

* Rebuilt against launcher v0.8.5 (`d0cb456`): battery in the header, Wi-Fi interface lookup (not only `wlan0`), the
  launcher's status feed. No other change.

## What changed in 0.5.2

* Rebuilt on launcher v0.8.1 (`2b1a070`), no behaviour change intended. The toolkit changed under it: upstream
  decoupling, time zone and Settings work, the x86 port; nothing the app uses was removed.

## What changed in 0.5.1

* **Multitasking** (`X-Background=run`; APP-CONTRACT section 13 of the launcher; [guidelines](../../docs/APP-GUIDELINES.md#multitasking-hidden-and-shown-again-x-background)): under a launcher with multitasking on, Super, Alt+Tab or three
  fingers held leave Mesh Hop open in the background **still receiving**: the link (USB, Bluetooth or Wi-Fi) and the
  history go on, nothing is drawn and no page is rebuilt, and a message that arrives counts as unread even in the
  conversation left open; its card in the switcher brings it back with the new messages and counts. An older
  launcher, or multitasking off: as before.

## What changed in 0.5.0

* **Choose a node**: the app opens on one list of every way to reach a node: the **USB** boards plugged in, the **Bluetooth LE**
  nodes a short scan hears (with their signal as bars), and the nodes **added by IP address** (MeshCore's Wi-Fi interface, TCP
  port 5000). **Nothing connects by itself**: the node chosen last is highlighted, a tap on it (or Enter) connects, then Home.
  Home has a fourth row, **Node** (the node in use and how it is reached), that opens the page again.
* **Bluetooth LE**: pairing with the node's PIN (a box asks it when the node asks), the deck's Bluetooth keyboard keeps working
  (Mesh Hop never takes the launcher's pairing role), the link comes back by itself when the node does. The first scan asks
  first (Bluetooth shares the radio with the keyboard and Wi-Fi); scans run only while the page is open.
* **Wi-Fi (TCP)**: add a node by its IP address (and port when not 5000); never a hang: a node that does not answer is reported
  after 5 s.
* The package recommends `bluez` (without it, USB and IP work and the page says Bluetooth is not available).

## What changed in 0.4.1

* Built on the launcher's newer `cp0_lvgl` (commit `f35c2d6`): its buttons use the shared 7 mm minimum tap target, as Mesh
  Hop's own controls already did. The header Esc keeps its 9 mm size (it is not on the 7 mm target).
* **Pointer navigation**: with Settings > System > Pointer navigation on (off by default), a trackpoint, trackball or mouse moves
  through Mesh Hop like the arrow keys, a click is Enter and a held click is Ctrl+O (a chat's options).

## What changed in 0.4.0

* **Any screen from 640x480 up** (tested in the simulator at 640x480, 800x480, 720x720, 1024x600 and 1280x720): every page is laid
  out from the launcher's layout tokens; 0.3.0 was a fixed 640x480 picture.
* **Home list** (Chats, Contacts, Settings) instead of the tab row, and the launcher's **page header** with its **Esc** button on
  every page; the Esc of Home asks *Quit Mesh Hop?*, so a touch-only board can leave the app.
* **On-screen keyboard** when no keyboard is connected (the launcher's shared rule). 0.3.0 typed with a physical keyboard only.
* **Chats**: one pane at 640x480 (the list, then the conversation as its own page), two panes where both fit; a *Search* / *Add
  channel* bar; **hold a chat** (or Ctrl+O) for its options; a compose view while the on-screen keyboard is up.
* **Contacts**: a list with a **Filter / Sort / More** bar instead of the table and its filter chips; *More* holds *Send advert*
  (flood or zero-hop), *Nearby* and *Manage contacts*.
* The launcher's dialogs for every box, menu, question and editor; rows show what can be tapped (chevron, grey information,
  action dot).
* Built on the shared app platform (`apps/_common`) and the launcher's `cp0_lvgl`, with the shared headless harness and scenes.

## Screenshots

Made with the simulator (`mesh-hop-script`, a simulated board with invented names and test data), **not on the deck's panel**.

| | |
| --- | --- |
| ![Home](../../docs/screenshots/mesh-hop-home.png) Home: Chats (unread total), Contacts, Settings | ![Chats](../../docs/screenshots/mesh-hop-chats.png) Chats at 640x480: the Search / Add channel bar, channels and direct chats with their unread counts |
| ![Conversation](../../docs/screenshots/mesh-hop-conversation.png) 640x480 by touch: a conversation's own page, the messages, the entry and Send | ![Compose with the on-screen keyboard](../../docs/screenshots/mesh-hop-compose.png) Writing without a keyboard: the compose view above the on-screen keyboard |
| ![Options of a chat](../../docs/screenshots/mesh-hop-options.png) Hold a chat (or Ctrl+O): its options (Search messages, Add channel, Mute, Delete, Details) | ![Contacts](../../docs/screenshots/mesh-hop-contacts.png) Contacts: the Filter / Sort / More bar and the list |
| ![Contact details](../../docs/screenshots/mesh-hop-details.png) A contact's details: Message, Groups, Delete, then name, type, groups, key, route | ![Settings](../../docs/screenshots/mesh-hop-settings.png) Settings: the BOARD section with Position and Board GPS |
| ![Statistics](../../docs/screenshots/mesh-hop-statistics.png) Statistics of the board | ![Chats at 800x480](../../docs/screenshots/mesh-hop-chats-800.png) 800x480: Chats in two panes, the conversation follows the selected chat |
| ![Contacts at 800x480](../../docs/screenshots/mesh-hop-contacts-800.png) 800x480: Contacts, with the SNR on the rows | |

**On real boards** (0.4.0 build, 2026-10-18, Home list with a connected radio board; nothing personal on these pages):

| | | |
| --- | --- | --- |
| ![Deck, 640x480](../../docs/screenshots/mesh-hop-home-deck.png) Deck (Pi Zero 2 W), 640x480 | ![Pi 3A+, 800x480](../../docs/screenshots/mesh-hop-home-3a.png) Pi 3A+ with the Waveshare 4.3" DSI, 800x480, touch only | ![HackBerry, 720x720](../../docs/screenshots/mesh-hop-home-hackberry.png) HackBerry CM5, 720x720, with its hint line |

## Screens and sizes

* The screen must be at least **640x480** (`X-MinSize=640x480`; a smaller one shows a notice and a way out).
* **One pane or two**: Chats shows the list and the conversation side by side only when both fit at the text sizes (800x480,
  1024x600 and 1280x720 in the simulator); at 640x480 and on a square 720x720 it is one pane: the list, then the conversation as its
  own page. Contacts shows the SNR on the wider screens.
* Rows and buttons are 7 mm high (the launcher's row token); rows where a tap only selects are 5 mm.

## Using it

**Choose a node.** The app opens on **Choose a node** (the header's title): one list with, for each node, an icon (USB, Bluetooth,
Wi-Fi), its name and a second line (`ttyACM0 · T1000-E`, `BLE · paired`, `192.168.1.20:5000`). A USB row shows the node name
learned on that port only while the same USB device (USB id and serial number) is plugged there; another board on the port shows
its USB name (`USB JTAG/serial debug unit`) until it has answered once. Bluetooth nodes heard by the scan
show their signal as bars (4 from -60 dBm, 3 from -70, 2 from -85, 1 above -100: the launcher's rule). Then a Bluetooth row
(Scanning..., Rescan, or a grey note when Bluetooth is off or missing) and **Add a node by IP address**. A tap selects a row, a tap
on the selected row connects (Enter too); the row says Connecting..., Pairing..., Type the PIN, Connected, or Failed with the
reason and "tap to retry"; once connected the app goes to Home. Nothing connects by itself: the node chosen last is only
highlighted. **O** (or Ctrl+O, or a 3 s hold on the row): Forget this node (a paired Bluetooth node: the deck forgets the
pairing), Remove IP node, Info. **S** scans, **A** adds by IP. The header's Esc asks **Quit Mesh Hop?** here; opened from
Home > Node it goes back to Home.

* **Bluetooth**: the first time, a box asks before the first scan (Bluetooth shares the radio with the keyboard and Wi-Fi: a
  Bluetooth keyboard may lag briefly during a scan): **Scan** or **Not now** (a row starts it later); asked once. A scan lasts
  10 s, runs only while the page is open, stops on a connect. A node not paired yet asks for its PIN: the 6 digits on its screen,
  or 123456 when it has none (the on-screen keyboard's number pad without a keyboard).
* **Add a node by IP address**: the address or name of a MeshCore node on Wi-Fi, with `:port` when not 5000, then an optional
  name; wrong input is refused in the box. The nodes added are always listed.

**Home and the header.** After a connection the app shows **Home**: **Chats** (the unread total in blue, or "no new"), **Contacts** (the number of
contacts), **Tools** (Nearby, Packet log, Statistics, CLI; the nodes waiting in Nearby in amber), **Settings** (the board) and
**Node** (the node in use and how; it opens Choose a node). Every page has the standard header: the title (with the sub-page where it fits, e.g.
"Mesh Hop > Tools > Nearby"), the **Esc** button and the launcher's status strip. The line under the header says the board (model
and state) or, for a few seconds, a notice. The key hints are a line at the bottom, shown only while a keyboard is connected.
The header's Esc goes back one step; from the root of a page it goes to Home; at Home it asks **Quit Mesh Hop?** (Cancel / Quit).

**Chats.** The channels and direct conversations (under CHANNELS and DIRECT), each on two lines: the name, then the last message and
its time; at the right the **unread count** in blue ("muted" for a muted chat). A tap selects a row, a tap on the selected row opens
it. The bar above the list has **Search** and **Add channel**. **Hold a chat** (a finger 3 s on its row, a bar shows the hold
filling; Ctrl+O with a keyboard) for its menu, titled "Options for <name>": **Search messages**, **Add channel**, **Mute / Unmute**,
**Delete conversation** (direct) or **Delete messages** (channel), and **Details** for a direct chat with a known contact. By touch
the line under the title says "Hold a chat: options".

* **A conversation**: the sender and time of each message (yours in green, with its status), the text in the large size, the entry
  and **Send** at the bottom. The line under the title says the nodes heard in the last hour, "muted", or the bytes left while you
  write. At 640x480 the conversation is its own page (its header's Esc goes back to the list); with two panes it follows the
  selected row.
* **Compose**: a tap on the entry brings the on-screen keyboard when no keyboard is connected, and the page becomes the compose view:
  the header, the last message, the entry (and Send where it fits; the keyboard's Enter sends too), the keyboard. After a Send, or
  Esc, the keyboard goes.
* **Message status**: a channel message ends at **sent** when the board accepts it (a channel has no acknowledgement; "sent
  (unconfirmed)" if the board does not answer within 5 s); a direct message goes sending, sent, delivered or no ack.
* **Heard back by N repeaters**: when your own channel message comes back from the mesh (the board's radio log), "sent" becomes "heard
  back by N repeaters"; N grows for 60 s, then it is saved with the message. Read it as a hint: the app cannot decrypt, it matches the
  channel hash and the packet size, and **N counts distinct routes**. It needs the channel key read from the board at connect.
* **Search messages** (the bar, the menu, or Ctrl+F): words (in the text and the sender's name), narrowed by **chat** (Ctrl+K),
  **dates** (Ctrl+D: last 24 h, 7, 30 or 90 days, or a typed day or range like `2026-09-01 2026-09-30`) and **direction** (Ctrl+R).
  Results newest first; a tap (or Enter) opens the conversation at that message, framed in blue. Without a keyboard only the words fit
  above the on-screen keyboard at 640x480; its Enter puts it away and the filters and results come back.
* **Add channel**: a hashtag channel (`#name`), a private channel with a key typed as 32 hex characters, or one with a random key
  (shown as text to share).
* **Delete** asks Yes / No with the number of messages and removes them from memory and from the history file; a deleted direct
  conversation leaves the list (the contact stays), a channel stays with its history cleared.

**Contacts.** A row per contact on two lines: the name, then the type, when it was heard and the hops (and the SNR on the wider
screens); the distance at the right when both positions are known. The line under the title says what is shown. The bar:

* **Search** (or `/`): a field under the bar; it filters as you type: a part of the name (any case, spaces count), or `<` followed
  by the first hex digits of the key (`<9a3f`: the nodes whose key starts so). The line under the title adds `"hill" in the name` or
  `key 9a3f...`. Esc (the header's Esc by touch) clears it and shows every contact again; without a keyboard the field brings the
  on-screen keyboard, whose Enter puts it away (the rows come back).
* **Filter**: the type, the time since heard, the group (a pick list each).
* **Sort**: name, heard, type, SNR, hops, distance; choosing the field in use again reverses it. The order is kept.
* **More**: **Send advert** (an arrow choice: **Flood** or **Zero-hop**, starting on the one used last), **Nearby** (with the
  number of nodes waiting) and **Manage contacts**.

A tap selects a row, a tap on the selected row (or Enter) opens it **by its type**: a **repeater** or a **room** its [Repeater
page](#repeaters-and-rooms), a **chat** contact its conversation in Chats (made when there is none yet), anything else (a sensor) its
**details**. **Hold a row 3 s** (a bar fills; `I` with a keyboard) for "Options for <name>": **Manage** (a repeater or room: its
Repeater page), **Message** (a chat contact), **Details**, **Groups**, **Delete** (asks first). The **details**: name, type, groups,
the whole public key, the route (flood, direct, or the hops with their hash size), last advert, when it was heard, position, distance,
SNR; **Message** (a chat node), **Groups** and **Delete** on its bar.
**Manage contacts** puts a check box on every row: **Select...** (all shown, none, not heard for 7 or 30 days, never heard,
invert), **Delete n** (asks Yes / No, then deletes **one by one on the board** with a progress box and Stop), **Group...** (add to a
group, take out, new group), **Done**. **Groups** are kept on the deck, per board; the board knows nothing of them.
The list is virtual (only the rows in view are widgets), so 160 or more contacts scroll as quickly as a few.

**Tools.** Two-line rows with a chevron (a tap selects, a tap on the selected row opens):

* **Nearby** (the nodes waiting, in amber): the nodes heard, newest first, pending ones first, with type, SNR, hops, age and **Add** /
  **Ignore**; **Scan** asks the nodes in direct radio range to answer (zero-hop discover, 8 s); **Ignored** shows the ignored nodes
  and restores them. Also Contacts > More > Nearby (and `N`).
* **Packet log** (capturing or not, and the count): the radio packets the board hears: time, route, type, hops, SNR, RSSI, size, the
  channel for a group text; the selected row shows its path and hex bytes; capture is **off at every start**, Start / Stop / Clear,
  the last 500 packets, in memory only.
* **Statistics**: battery, uptime, errors, noise floor, RSSI, SNR, airtime, packet counters; Refresh, automatic refresh every 5 s
  ("Firmware too old for this feature" on a board without it).
* **CLI**: see [CLI](#cli).

**Settings.** A list in sections; a tap selects, a tap on the selected row changes or opens it.

* **MY CONTACT**: **Name** (the node's name; it is changed in RADIO > Node name: Enter takes you there) and **Public key** (grouped,
  `a0a1 a2a3 … bebf`; Enter shows the whole 64 hex digits).
* **BOARD**: connection, firmware, battery, clock (information); **Position** (opens the Position box: the **Board GPS** switch
  when the firmware has a `gps` variable, and the **latitude** and **longitude** in decimal degrees; OK sends SET_ADVERT_LATLON and
  reads it back, **Clear** sets "no position"; setting a position sends no advert); **Board GPS**. (Statistics and the Packet log
  are on [Tools](#using-it).)
* **RADIO**: node name, **Preset** (26 built in from `api.meshcore.nz`, dated 2026-10-07; refreshed in the background at launch when
  the deck is online, once more 60 s later if that failed, the last good copy kept), frequency, bandwidth, spreading factor, coding
  rate, TX power (an arrow choice: the arrows or Left / Right, OK, Cancel), **Path hash size** (1, 2 or 3 bytes; firmware level 10
  and up), **Repeat** (only on the frequencies the board allows; switching it on asks first).
* **DIRECT MESSAGES**: **Message tries** and **Forget the route** (before which try). **SCHEDULED ADVERT**: off (default), every 1, 3, 6 or 12 h, flood or
  zero-hop (the app sends it while it runs). **CONTACTS**: **Add nodes by itself** (manual add mode off: new nodes wait in Nearby)
  and **Still added by itself** (the auto-add types). **CLOCK**: **Sync clock now** (deck NTP, board GPS or a typed start time).
* **CHANNELS**: each channel (not the fixed Public one) with Mute, Key (shown as text) and Remove; Add channel.
* **HISTORY**: Delete all messages, Delete messages older than 7, 30 or 90 days, **Forget this board's data**, **Other boards' data**.
* **BOARD ACTIONS**: **Reboot the board** and **Factory reset the board** (see below). **RADIO CHANGES**: **Undo changes** and
  **Save to radio**, each asks Yes / No.

A feature the board's firmware lacks (custom variables, channel commands, path hash, statistics, auto-add filter, discover, factory
reset) shows "Firmware too old for this feature" instead of its row.

## Repeaters and rooms

**The Repeater page** (a repeater or room tapped in Contacts, or its Options > Manage) is a hub (0.8.1): the header says its name,
the line under it "Repeater · direct, 2 hops · heard 3 min ago" (or "Room", "flood" when no route is known). The bar: **Log in**
(**Log out** once logged in) and **More**. The rows: ACCESS, "Not logged in", or "Admin since 14:02" ("Guest", and for a room
"Read-only" or "Read-write"; "Logged in" when an old node does not say), with "password saved" when one is; logged in, **Clock at
login**: the repeater's clock and its gap to the deck's (amber, with a warning, when it is more than a minute ahead: it may then
drop the commands sent from here), and the note of Log out. Then **READ**: **Status**, and for a repeater **Neighbours** and
**Owner**, rows with a chevron and a one-line summary at the right ("4.12 V · up 3 d 4 h", "12 heard · newest 12 s ago", "Olivier ·
fw 1.12.0"; "not read yet", "needs login", "no answer" in amber, "firmware too old", "direct route only", "waiting… up to 12 s").
A first tap selects a row, a tap on it selected (or Up / Down, then Enter) opens its **screen**. At 640x480, logged in, the READ
rows start at the bottom of the screen: scroll (or Down) to them.

**A section's screen**: the header "Hilltop repeater > Status" (on a narrow screen only "Status": the shared header drops the
first parts that do not fit), the line under it "read 14:02 · tap Refresh to read again", "waiting… up to 12 s" or what went wrong,
**one button, Refresh** (**Cancel** while a read runs; `R` or Ctrl+R), and the rows. A screen never read is read **by itself once**
when it opens and you are logged in (the status after a login already was: it shows at once); after a failure it waits for
Refresh. Esc (or the header's) goes back to the hub, whose summary follows; a read still running goes on there.

* **Logging in**: not logged in, the login box opens by itself: under its title the route ("Route: direct, 2 hops" or "Route:
  flood: the login goes to the whole mesh"); the password (masked, 15 bytes at most; the saved one is already in
  it, masked: Log in is enough), **Remember on this deck** (on) with **Reset path** at its right when a route is known, **Cancel**, **Guest** (a blank password: the firmware's default
  guest password is empty, so most repeaters give guest), **Log in**. Enter logs in, Esc cancels, Tab goes to Remember (Space switches
  it), Ctrl+R resets the path. Without a keyboard the on-screen keyboard comes only when you tap the password. While it waits the box says **Waiting… up to N s**; Cancel (Esc) stops waiting. A wrong
  password gets **no answer** from the firmware, so it ends as "No answer. Wrong password, or <name> is out of reach." (nothing is
  retried by itself); the node's errors say "the node is busy", "not in the node's contacts" or "firmware too old". On success the
  box closes and the line under the title says the role (and "password saved").
* **Log out** forgets the session on this deck; the repeater itself keeps the deck in its list (an admin until `setperm`, a guest
  until it reboots): the page says so.
* **Status** (guest or admin; read **once by itself** after each successful login): Battery (V), Uptime (d h m), Noise floor,
  Last RSSI / SNR, Packets received / sent, Received flood / direct, Sent flood / direct, Airtime TX / RX (h m), Duplicates flood /
  direct, RX errors (repeater v1.12.0 and newer), TX queue; a room has Airtime TX and Posts stored / pushed. Not logged in: "Log in
  to read".
* **Neighbours** (repeaters; guest or admin): one row per repeater it hears at zero hop, newest first: its name in your Contacts,
  else `<` and the first 4 bytes of its key (`<e0e1e2e3`), "(ambiguous 2)" when two of your contacts start with those bytes; at the
  right "SNR 7.0 · 12 s ago". The repeater sends 14 per page: **More neighbours** ("14 of 22 shown", an action row: a tap on it
  selected, or Enter) reads the next one (a later page may take up to 25 s; after a failure it asks that page again). Some older
  repeaters answer only an admin: a guest then gets "no answer".
* **Owner** (repeaters): logged in, Name, **Firmware** (its version, v1.12.0 and newer: older ones say "firmware too old") and the
  owner info (`set owner.info` on the repeater). Not logged in, **Read owner (direct route)** (an action row; Refresh does the
  same) asks without a login, but only when the route is direct (zero hop or a known path): the repeater ignores that request by
  flood and answers it at most 4 times in 3 minutes for everyone together, which the screen says after a request without an
  answer. On a flood route the row says "Log in to read" (or a direct route: a login or message by flood usually brings one back,
  and the row follows).
* **Refresh all** (the hub's More, or `R` on the hub): Status, then Neighbours (the first page), then Owner, one after the other
  (one request at a time, like the node); the hub's summaries show where it is. A section without an answer, too old or unreadable
  is marked and the next one still goes; not logged in any more, the CLI, a lost link or a busy node stop the chain (the sections
  not read yet keep what they showed). While it runs, More says **Stop reading** (and `R` stops it). Not logged in, it reads the
  owner on a direct route and nothing else ("Log in first").
* **When something fails** the screen says why (the line under its header, and the whole text as its first row: "No answer. <name>
  may be out of reach, or its session ended (a reboot): log in again.", "Log in first", "Firmware too old for this", "Unreadable
  answer", "The CLI holds the node"...), the hub's summary in amber ("no answer"...), and the status line for a few seconds;
  **Refresh** asks again. Nothing is retried by itself and nothing asks for a confirmation.
* What was read stays on the page while it is open (nothing is saved): leaving the page, **Log out** and a new login clear it.
* **More**: **Details**: the contact's details. **Open in CLI** opens the CLI and types `to <its full key>` at meshcore-cli's first
  prompt (the output shows "to <name> <its first 6 key digits>"): logged in as admin on the page, admin commands work there at once
  (the repeater knows this node, not the program), and the CLI's Esc comes back to the page. **Reset path**: the node
  forgets its stored route to the repeater (RESET_PATH, as the CLI's `rp`), so the next message or login goes by flood and finds a
  new route; no confirmation, the status line says "Path reset: the next message goes by flood". **Refresh all** (0.8.1, above).
  **Forget password** (only when one is saved, last) deletes it, as the CLI's `fp`.
* Leaving the page stops a login or a read that still waits. The passwords are meshcore-cli's own files (see [Files](#files-the-app-keeps)):
  one store for the page and the CLI's `login`.

## CLI

**What it is.** Tools > **CLI** (0.6.0: Home > CLI) runs the upstream command line client [meshcore-cli](https://github.com/meshcore-dev/meshcore-cli)
**1.6.6** (bundled in the package) on the node Mesh Hop is connected to. meshcore-cli does not open the node itself: it talks to
a local **relay** inside Mesh Hop, which keeps the link (USB, Bluetooth or Wi-Fi). So a message that arrives during a session
shows in the console **and** is kept in Chats as usual; the replies of a repeater stay in the console and never go to Chats.

**Opening it.** Tools > CLI needs a **connected node** (otherwise the row says "not connected" and a tap says to connect first,
Home > Node); a Repeater page's More > **Open in CLI** opens it too, already on that repeater. The page shows meshcore-cli's output
and an entry line at the bottom with **Help**; opened from Tools, the status line says how to pick a repeater ("Contacts > a repeater >
More > Open in CLI, or type: to <name>") until you send a line; the first start takes a few seconds while Python loads. The first time
in a run, the page first runs `meshcore-cli -v` in the background at the lowest CPU and disk priority (Python compiles its files
once, into `~/.cache/mesh-hop`; 10-20 s the very first time on a Pi Zero 2 W) and says *Starting meshcore-cli...* meanwhile.
Mesh Hop 0.6.0 and 0.7.0 ran that step as soon as a node connected; on the Pi Zero 2 W the deck reset at that moment, so it now
waits for the CLI page. `MESHHOP_NO_WARMUP=1` in Mesh Hop's environment skips it (diagnostics).

**Keys.**

| Key | Action |
| --- | --- |
| Enter | send the line |
| Up / Down | the previous / next line typed (history of this session) |
| PgUp / PgDn | scroll the output (or drag it) |
| Ctrl+C | an interrupt: at its prompt meshcore-cli **ends**; leave the page and open CLI again to restart it |
| Ctrl+H | the Help page |
| Esc | leave the CLI: the session ends and Mesh Hop takes the node back; Tools (or the Repeater page it came from) is back |

By touch: the **Help** button opens the same page (a tap on a command puts it into the entry), a tap on the entry brings the on-screen keyboard when
no keyboard is connected, its Enter sends.

**While a session runs**, Mesh Hop's own sends and timers (messages to send and their retries, the scheduled advert, the clock sync, a Nearby scan) **wait**;
they go on after you leave the CLI.

**Repeaters and passwords.** A Repeater page's More > **Open in CLI** (or `to <name>` typed) picks a repeater or room, then a plain
`login` asks its password (typed as `*`) and keeps it **for that repeater**, so the next `login` asks nothing. Open in CLI types the
full key: `to <name>` looks the name up and can take another node when two have the same name or a name looks like a key. Easier: log in on the [Repeater page](#repeaters-and-rooms)
(Contacts, a tap on the repeater): its Remember writes the same file, and its login counts here too (Help says so). The passwords are saved in
`~/.local/share/mesh-hop/cli/.config/meshcore/<repeater key>.pass` (mode 0600); `fp` forgets the one of the repeater picked (a
wrong one, for example). Typing `login <password>` on one line works but **shows the password** on the screen: use a plain
`login`. Mesh Hop keeps no command history file.

**Good to know.**

* `get acl` runs as `req_acl` over LoRa (meshcore-cli turns it into the request), so it works on a remote repeater.
* Some repeater commands only work over a **cable**, not over LoRa: `get prv.key`, `erase`, `stats-*`, `set freq` (Help, "Cable
  only").
* **No confirmation** for dangerous commands: `set prv.key` (replaces the repeater's identity at its next reboot, no way back),
  `poweroff`, `reboot`, `clkreboot`, `start ota`, `set radio` and `tempradio` (can move the repeater off your frequency),
  `password`, `setperm`. A remote command has no delivery acknowledgement: never retry one blindly. See the warnings on the Help
  page ("Careful: no confirmation").
* `meshcore-cli -h` (Help, "All commands") lists every command and its shortcut.
* Needs **python3 >= 3.10** (the package depends on it, with `python3-requests` and `python3-serial`); the other Python
  libraries are bundled in `/opt/mesh-hop/cli/py`.

**After removing the package** (`sudo apt remove mesh-hop`), the CLI's saved passwords stay in `~/.local/share/mesh-hop/cli` and
its compiled Python files in `~/.cache/mesh-hop`. Delete them with `rm -rf ~/.local/share/mesh-hop/cli ~/.cache/mesh-hop` (or the
whole `~/.local/share/mesh-hop` to remove the messages too).

## Typing: keyboard or on-screen keyboard

* With a **keyboard** (Bluetooth or USB), typing goes to the field at once: a letter on the Chats list or a conversation starts the
  message.
* With **no keyboard connected**, a tap on a field (the message entry, the search words, an editor, a field of the Position box,
  the Search of Contacts, the password of the login box: masked on the keyboard's echo line too) brings the launcher's
  **on-screen keyboard**; the field stays above it. It goes after a Send, Enter or Esc, or within about a second
  when a keyboard connects. The launcher's shared rule decides (`input.state`: no keyboard, or a Bluetooth keyboard asleep for 10
  minutes; Settings > System > On-screen keyboard; `APPLAUNCH_OSK`); the app never shows it by its own test and has no button to
  summon it.
* Everything is reachable by touch: rows, Home, bar cells, arrows and buttons; drag scrolls lists and conversations. There is no
  swipe gesture. The only touch actions that need a hold are a chat's options and a contact's Options (a 3 s hold on its row; a
  contact's Details are also its Repeater page's More); Search and Add channel are also on the bar.

## Keys

A short **Esc** on the keyboard is Back and never quits: it closes the newest layer first (box, editor, the on-screen keyboard,
search, statistics, packet log, details, the Repeater page, Nearby, Manage contacts, the Search of Contacts, a conversation's
page), from the root of a page it goes to Home (the CLI goes back to Tools, or to the Repeater page it was opened from),
and at Home it says "Hold Esc 3 s to exit". **Holding Esc 3 s** ends the app (the launcher does that).

| Key | Action |
| --- | --- |
| Up / Down, Enter or Right | Home: choose a page, open it |
| Tab / Shift+Tab | on a page: the next / previous page (Chats, Contacts, Tools, Settings), in a loop |
| Up / Down, PgUp / PgDn, Home / End | move in a list (Chats, Contacts, Tools, Settings, Nearby, details, results, packet log); scroll a conversation; Repeater page: Up / Down select the READ rows (the hub) or a screen's rows (above the first: none), PgUp / PgDn scroll, Home: no row selected, the top |
| Enter or Right, or a letter | Chats: write in the selected conversation (one pane: its page opens with the entry) |
| Enter | entry: send; Contacts: open the contact by its type (a repeater or room: its Repeater page; a chat: its conversation; a sensor: its details; Manage contacts: mark); Tools: open; Settings: change / do; Nearby: add; details: open the chat; Repeater page: the hub: open the selected section's screen (also Right; no row selected and not logged in: log in); a screen: the selected action row (More neighbours, Read owner) |
| Left / Right | Settings: step the value of the row; entry: move the cursor; arrow choice: previous / next value |
| Ctrl+O | Chats: the menu of the selected or open conversation (the touch way is a 3 s hold on its row) |
| Ctrl+M | Chats: mute / unmute the conversation |
| Ctrl+F | Chats: search the messages; then Ctrl+K chat, Ctrl+D dates, Ctrl+R direction |
| `S` `R` `T` `H` | Contacts: next sort, reverse, type filter, heard filter |
| `/` | Contacts: Search (then type; Up / Down move in the rows, Enter opens, Esc clears) |
| `G` `N` `X` `I` `U` | Contacts: group filter, Nearby, Manage contacts, the Options of the selected contact (Manage, Message, Details, Groups, Delete), ask the board for its contacts again |
| `L` `M` `R` | Repeater page (the hub): log in or log out, More (Details, Open in CLI, Reset path, Refresh all, Forget password), Refresh all (0.8.1; stops a read that runs; it was Reset path in 0.7.2) |
| `R`, Ctrl+R | a Repeater page's screen (Status, Neighbours, Owner): read it again (Cancel while a read runs); Esc: back to the hub. Ctrl+R on the hub: as `R` |
| Tab, Space, Ctrl+R | the login box: go to Remember on this deck, switch it, Reset path (Enter logs in, Esc cancels or stops the wait) |
| `A` `D` | Contacts: send a flood / zero-hop advert at once (Settings: `A` sends a flood advert) |
| Space or Enter, `A` `N` `I` `M` `D` or Del, `G` | Manage contacts: mark, all, none, invert, the Select menu, delete, group |
| `I` `S` `V` | Nearby: ignore (or restore), scan, show / hide the ignored |
| `G`, `D` or Del | contact details: groups, delete |
| `R` or Enter, `A` | Statistics: refresh, switch the automatic refresh |
| `S` or Space, `C` | Packet log: start / stop the capture, clear it |
| `S` `V` | Settings: save to radio, undo changes (each asks Yes / No) |
| `M` `K` Del | Settings, a channel row: mute, show the key, remove |
| `Y` `N` | Yes / No boxes |

Letters typed into a field are text, not shortcuts. The keyboard layout is US by default, AZERTY when `MESHHOP_KEYMAP=fr` or
`XKBLAYOUT="fr"` is in `/etc/default/keyboard`.

## Install and requirements

* A **MeshCore companion radio board** with a *companion radio* firmware: the USB build plugged into the deck's USB port, the BLE
  build (paired with its PIN), or a Wi-Fi build on the deck's network (its IP address). Path hash size needs firmware level 10 and
  up, repeat level 9 and up; statistics, discover and factory reset need their commands.
* A screen of at least 640x480 and a launcher with **full-screen app support** (`X-Fullscreen=true` in the `.desktop` file).
* A keyboard is optional: without one the launcher's on-screen keyboard is used.
* For USB, the user must be in the `dialout` group (the Raspberry Pi OS default user is). For Bluetooth, BlueZ (`bluez`,
  recommended by the package; any user may use it through D-Bus).
* `curl` (for the preset list; without it the app keeps the saved or built-in list), `libfreetype6`, `libinput10`,
  `libxkbcommon0` (the package depends on them). For the CLI: `python3 (>= 3.10)`, `python3-requests`, `python3-serial`
  (depends too).
* **x86-64 PCs:** an amd64 package, `mesh-hop_0.7.0_amd64.deb`, is in the amd64 registry
  ([amd64](../../README.md#amd64-x86-64-pcs)) for a PC running the launcher. It has had no board test on a PC yet.

**Install from the deck:** Settings > Apps > Sources > + Add a GitHub source `OSRdesign/cyberdeck-zero-apps` (if not added yet),
then Apps > Mesh Hop > Enter (it asks for the sudo password). If an older version is installed, the deck offers the new one as an
update in the same menu.

**Or install a local `.deb`** (a build of your own). On a PC with this repository:

```
python tools/make_registry.py --only mesh-hop --out <a folder outside packages/>
```

Copy the `mesh-hop_0.7.0_arm64.deb` it writes to the deck and run `sudo apt install ./mesh-hop_0.7.0_arm64.deb` there. Remove it
with `sudo apt remove mesh-hop`.

The USB boards are listed by themselves (`/dev/ttyACM*`, `/dev/ttyUSB*`, with their USB names); a path in
`~/.local/share/mesh-hop/port` is listed too. `MESHHOP_PORT=/dev/...` is a choice made already: the app connects to that port at
once and opens on Home (the simulator, the scripted runs). The app reads the launcher's fonts in
`/usr/share/APPLaunch/share/font`.

## Reboot and factory reset: read this

Settings > **Reboot the board** restarts it and erases nothing. Settings > **Factory reset the board** **erases the board's keys
(its identity), contacts and channels**: it asks two Yes / No questions and the Yes of the second waits 3 s. The firmware formats its
file system (up to a minute on an ESP32 board) and restarts with a new identity; the app waits up to 60 s and judges by the key the
board reports ("Board reset", or "not reset" for the same key). To the mesh it is then a new node. There is no power-off command in
the companion protocol. Settings > History > **Forget this board's data**, **Other boards' data** and the *Delete* rows remove the
deck's saved copy of messages, groups and flags; each asks first.

## Files the app keeps

All in `~/.local/share/mesh-hop`. Plugging another board shows that board's own conversations and contacts (the app knows a board by
its public key); plugging the first back restores them. The log holds no message text.

| Path | What |
| --- | --- |
| `boards/<first 12 hex digits of the board key>/` | per board: `messages.jsonl` (history: 200 messages per conversation, 1500 in all), `read.txt`, `contacts.jsonl` and `channels.jsonl` (caches), `groups.jsonl` (and `groups.bak`), `prefs.txt` (mute flags, ignored nodes, advert schedule) |
| `prefs.txt` | global: the direct message retry settings |
| `presets.jsonl` | the last good copy of the radio preset list |
| `nodes.txt` | the nodes added by IP address, the node chosen last, the answer to the Bluetooth scan question, the node names learned (0.5.0; a USB name with the identity of the device that answered, `name usb:/dev/ttyACM0 <name> 303a:1001/<serial>`, so another board on that port does not inherit it) |
| `port` | optional: a serial port to list on the start page |
| `cli/` | the CLI's home: `.config/meshcore/<repeater key>.pass`, the passwords kept by the CLI's `login` (0.6.0) and by the Repeater page's Remember on this deck (0.7.0): one file per repeater or room, mode 0600, for both |
| `mesh-hop.log` | debug log, size bounded (`MESHHOP_DEBUG=1` logs every frame) |
| `*.pre-boards.bak` | the files of 0.2.1 and before, kept after they moved into the first board's folder |

## Known limits

* No **Map** yet. The **CLI** (0.6.0) is the upstream meshcore-cli in a line console, not a full terminal. Bluetooth LE and Wi-Fi (TCP) are new in 0.5.0 and not yet tested with a real node.
* The **Repeater page** reads (0.8.1) the status, the neighbours and the owner; the admin parameters, telemetry and the access
  list are not there (the CLI has them); no Trace / Ping on Tools yet, no favourites, no QR code of My contact. A wrong password,
  an ended session and an out-of-reach repeater look the same (the firmware answers none of them): the page says so. Its session is
  the board's: a new connection to the board starts without it (the repeater still lists the deck). What the page read is not kept
  once it is left.
* Pairing is done by Mesh Hop itself (its own BlueZ agent); a node paired in another program works too. Forgetting a node is
  done on the start page (or `bluetoothctl remove`).
* A **channel message has no delivery acknowledgement** in MeshCore: it ends at "sent", never "delivered". "Heard back by N
  repeaters" is an approximation (distinct routes, matched by channel hash and size).
* **Emoji and other characters the launcher's fonts lack** show as empty boxes (in messages and names from other clients).
* **Keyboard layout**: the app translates the keys itself, US by default or its own FR (AZERTY) table; any other layout types as US.
* **Key repeat**: holding a key repeats arrows and typed characters; Enter, Esc and the letter shortcuts act once per press.
* The packet log is not saved or exported and holds 500 packets. The scheduled advert runs only while the app runs and the board is
  connected. The deck keeps 200 messages per conversation and 1500 per board.
* The screenshots on this page come from the simulator, not from the panel.

## Credits and licences

Mesh Hop is MIT (see the `copyright` file of the package).

* **MeshCore** firmware and protocol: Scott Powell / rippleradios.com, MIT, <https://github.com/meshcore-dev/MeshCore>.
* **meshcore_py** (Florent de Lamotte, MIT, <https://github.com/meshcore-dev/meshcore_py>) was the reference of the wire
  format; the protocol code was written from the protocol documentation and from it. The radio presets come from the public list
  of `api.meshcore.nz`.
* **MeshCore Open** (MIT), **meshcore-gui** (MIT), **wadamesh** (GPL-3.0) and **Meshy** (GPL-3.0-or-later) were looked at as
  feature references only. **No code was copied from the GPL projects.**
* **LVGL** (MIT), **FreeType** (FreeType License or GPL-2, the system `libfreetype6`) and the launcher's `cp0_lvgl` runtime of the
  CardputerZero launcher by M5Stack (MIT). The launcher's fonts are read at run time (Montserrat, Font Awesome Free, DejaVu Sans:
  SIL OFL 1.1, CC BY 4.0 for the icons, Bitstream Vera / DejaVu licence); they are not in the package.

* **meshcore-cli** 1.6.6 (MIT) and its libraries, bundled for the CLI page unmodified (pinned with their sha256 in
  `cli/requirements.txt`): **meshcore** 2.3.17 (MIT), **pycayennelpp** 2.4.0 (MIT), **pyserial-asyncio-fast** 0.16
  (BSD-3-Clause), **pycryptodome** 4.1.0 (BSD-2-Clause and Unlicense, partly MIT), **prompt_toolkit** 3.0.53 (BSD-3-Clause),
  **wcwidth** 0.9.3 (MIT). Their licence texts are in `/usr/share/doc/mesh-hop/LICENSE.*`, the details in the `copyright` file.

## For developers

```
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh cross        # aarch64, into root/usr/share/APPLaunch/bin/mesh-hop
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh host         # PC: shared harness + scenes, mesh-hop-script, unit tests
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh all --strict # both, warnings as errors
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh flows [WxH]  # the 23 simulator flows (tests/flows), about 17 minutes
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh flows sizes  # shell, pages and chats at the five sizes and at the HackBerry's density
```

Mesh Hop is a native full-screen app on the shared app platform (`apps/_common`, see `apps/_common/build_app.sh -h`): it links the
launcher's `cp0_lvgl` (pinned by `apps/_common/LAUNCHER_REF`) for the display, touch, keyboards, status strip, the page header, the
dialogs and the on-screen keyboard (with the shared rule of `apps/_common/osk_policy`), and lays every page out from the layout
service's tokens (`mesh_app.cpp` `layout_body()`, `layout_chats()`).

* **Simulator**: `tools/meshcore_sim.py` simulates a board on a pseudo-terminal (`--contacts N`, `--manual-add`, `--new-nodes N`,
  `--fw-level N`, `--old-firmware`, `--no-gps-var`, `--heard-back N`, `--identity A|B`, `--reset-format-delay SEC` and more: see its
  header; stdin commands `newnode`, `reboot`, `swap`); `--tcp PORT` serves a TCP client instead (a node on Wi-Fi). Its remote
  repeaters and rooms answer logins like the firmware (report 036): an empty guest password by default (`--guest-password`,
  `--admin-password`, or `$MESHHOP_SIM_GUEST_PASSWORD` / `$MESHHOP_SIM_ADMIN_PASSWORD`), no answer to a wrong password, one
  pending login at a time, `--remote-delay SEC`, `--remote-drop N|all` (stdin `drop N|all|0`), stdin `remote-reboot [all]`, and
  `--remote-cases` for a duplicate name, a hex-looking name and names with spaces. They also answer the read-only requests (slice
  S2): status, neighbours (`--neighbours N`, paged like the firmware) and owner info (`--owner-info`, `--remote-fw`) to a logged-in
  board, the anonymous owner request only on a direct route and 4 per 180 s, and `--old-repeater` (no owner info, no anonymous
  answers, neighbours for admins only); 0.8.1: `--learn-path` (an answer to a login or request sent by flood brings a route back:
  the contact is 1 hop away and the board pushes PATH_UPDATED, 0x81). Its names (Alice, Bob, Zed...) are invented. `MESHHOP_DEMO=<scenario>` plays the start page with pretend USB ports and Bluetooth
  (`src/core/node_demo.hpp`).
* **Flows**: the scripts in `tests/flows/` drive `mesh-hop-script` (`--script f --shot-dir d [--size WxH] [--ppmm N]`: the shared
  headless harness at any size and density). Commands: `key`, `type`, `tap "label"`, `tapfield n`, `tapkey "label"` (a key of the
  on-screen keyboard), `hold`, `keyboard on|off`, `shot`, `dump`, `bench`, `sim <line>`, `@panes1` / `@panes2`, `fill`, `check` (the
  shared layout checks and the size rules of every control; any finding fails) and `expect`. The first line of each script names
  its simulator options; `run_flows.sh` runs all of them. The seed scripts write test histories: `seed_chats.py` (`chats_run.sh`),
  `seed_history.py` (`history_run.sh`, `p2_run.sh`); `presets_run.sh` puts a local web server in place of `api.meshcore.nz`.
  0.7.0: `repeater_run.sh` (`repeater.script` with a keyboard, `repeater-touch.script` by touch only; the simulator's
  `--remote-cases --contacts 6`) runs the Repeater page, the Search of Contacts, Tools and MY CONTACT at 640x480 and 800x480;
  `cli_run.sh` (with `MESHHOP_MESHCLI`, the mccli of `build.sh cli`) adds `cli-repeater.script`: an admin login on the page, More >
  Open in CLI, `ver` with no `login` typed, then (0.7.2) Open in CLI from the pages of two of the same name, a name with spaces and a
  hex-looking name (each sends its own full key). 0.7.2: `repeater.script` and `repeater-touch.script` also show the route in the
  login box and Reset path from the box (Ctrl+R, a tap) and from More. 0.8.1: `repeater_s2_run.sh` runs the read-only view (the hub and
  its three screens) at 640x480 and 800x480: `repeater-s2.script` (a keyboard: the status by itself after a login, the hub's
  summaries, each screen with its Refresh and Cancel, Neighbours and Owner reading themselves once, More neighbours, Refresh all
  from the hub and its stop, More, the owner without a login, a flood contact whose guest login brings a route back, the answers
  lost on the air; `--neighbours 24 --owner-info ... --learn-path`), `repeater-s2-old.script` (`--old-repeater`) and
  `repeater-s2-touch.script` (touch only).
* **Scenes** of the shared harness (`tests/scenes/`) and the **unit tests** (`tests/unit/`) run with `build.sh host`.
* **Package**: `python3 tools/make_registry.py --only mesh-hop --out <a folder outside packages/>` (arm64), then the
  same with `--arch amd64`; add `--owner OSRdesign --repo cyberdeck-zero-apps` when the checkout has no `origin` remote. Run it with
  Windows Python from the checkout so the files get modes 644 / 755.
* **Building the amd64 package** (the binary in `root.amd64/`, for PCs; `make_registry.py` lays it over `root/`): rebuild it with
  every release, the arm64 build does not touch it. It is a native build in the launcher's amd64 tree
  (`LAUNCHER_TREE_AMD64`, default `~/zero7/<launcher folder>-amd64`, made by the launcher's `tools/dev/wsl-build.sh --arch amd64`)
  with the -dev packages of libinput, xkbcommon, libudev, dbus-1, gio-2.0 and freetype2. Without `sudo apt install` on the build
  box, unpack those -dev .debs and their libraries into a private root (`dpkg-deb -x <deb> <root>`, here
  `~/zero7/xb-amd64-devroot/root`) and point the build at it:

  ```
  R=$HOME/zero7/xb-amd64-devroot/root; L=$R/usr/lib/x86_64-linux-gnu; PC=$HOME/cdk-build/mesh-hop/amd64-pc
  mkdir -p $PC; for f in $L/pkgconfig/*.pc; do        # the .pc files with their prefix moved into the root
      sed "s|^prefix=/usr|prefix=$R/usr|; s|^libdir=/usr/lib/x86_64-linux-gnu|libdir=$L|; s|^libdir=\${prefix}/lib/x86_64-linux-gnu|libdir=$L|" "$f" > $PC/$(basename "$f"); done
  export PKG_CONFIG_PATH=$PC PKG_CONFIG_SYSTEM_LIBRARY_PATH=$L
  export CONFIG_TOOLCHAIN_FLAGS="-isystem $R/usr/include -isystem $R/usr/include/x86_64-linux-gnu -L$L -Wl,-rpath-link,$L"
  export LAUNCHER_SRC=<the launcher checkout at the pinned commit> CDK_BUILD=$HOME/cdk-build/mesh-hop-amd64   # a fresh build folder
  sh apps/mesh-hop/build.sh cross --arch amd64 --sync
  ```

  It prints the binary's size and NEEDED libraries (libinput, xkbcommon, freetype, libstdc++, libc...) and writes
  `root.amd64/usr/share/doc/mesh-hop/build-info`. Use a build folder (`CDK_BUILD`, and `LVBUILD` for the host LVGL objects) of its
  own: one shared between builds once mixed objects and the binary crashed. Check `file root.amd64/usr/share/APPLaunch/bin/mesh-hop`
  (x86-64) and that build-info names the new version before packaging.

| Path | What |
| --- | --- |
| `src/core/` | transport, protocol, client, model, store, clock policy, presets (`radio_presets.inc` is the dated data file), preset feed, channel keys, firmware features, contact groups, message search, log, the radio log parser and echo tracker, the packet log, the remote login queue of the client (log in / out of a repeater or room, one request at a time, the session table: report 036) and the password store shared with meshcore-cli (`passwords.*`: `<data>/cli/.config/meshcore/<key>.pass`); the read-only requests on that queue (report 036 slice S2: `request_status`, `request_neighbours`, `request_owner`: BINARY_REQ 50 and ANON_REQ 57, the push 0x8C matched by its tag; the parsers of the status, the neighbour pages and the owner info in `protocol.*`; `neighbour_name()` in `model.*`; 0.8.1: PATH_UPDATED reads the contact back, the role of an old node's login, the 25 s floor of a later neighbours page) (no LVGL; unit tested: `tests/unit/test_remote.inc`, `test_remote_s2.inc`, `sim_remote_check.py`) |
| `src/core/ble/` | the Bluetooth LE link to a BLE companion board: BlueZ over sd-bus loaded at run time, the link state machine, an `ITransport`, a fake for the tests; see [`docs/BLE.md`](docs/BLE.md) |
| `src/core/tcp_transport.*`, `nodes.*`, `node_demo.*` | the TCP transport (and the "no node chosen" one), what the deck remembers about the nodes (`nodes.txt`), the start page's demo worlds; see [`docs/CONNECT.md`](docs/CONNECT.md) |
| `src/ui/ui_nodes.*` | the rows of the start page "Choose a node" (no LVGL; unit tested) |
| `tools/ble_probe.cpp`, `tests/ble/` | the BLE probe to run on the deck (`tools/build_ble_probe.sh`) and its test against a mock BlueZ on a private D-Bus (`tests/ble/run_mock.sh`) |
| `src/ui/ui_logic.*`, `ui_phase2.cpp`, `ui_pages.*` | keys, editor filters, formatting, chat and contact rows, filters and sort, selection, groups, details, Nearby, search, statistics, popups, the Settings layout, the Back rule (no LVGL; unit tested) |
| `src/ui/key_bridge.*` | the platform's keys to the app's key events, with the US / FR tables (no LVGL; unit tested) |
| `src/ui/stage.*`, `session.hpp`, `app_spec.cpp` | the keyboard bridge, the tick timer, the app's spec for the shared runners |
| `src/ui/ui_repeater.*` | 0.7.0: Home's rows and the page loop, the Tools rows, what a tap on a contact opens and its Options, MY CONTACT's key text, the Repeater page's rows, More's order and the login box's texts (errors, the wait, the route line, Reset path's answers), the CLI's `to` line; 0.8.1: the read-only view (the hub's rows and summaries, each screen's rows and line, their states and error texts, every value's format, `RepeaterReads`: what the page read, the one-at-a-time queue of Refresh, the automatic first read of a screen) (no LVGL; unit tested: `tests/unit/test_repeater_ui.inc`) |
| `src/ui/mesh_app.*` | the shell (Home, header, status line, hint line, dialogs), the pages (0.7.0: Tools, the Search of Contacts), Chats in one or two panes, the text fields and the on-screen keyboard, the virtual row list |
| `src/ui/repeater_page.cpp` | 0.7.0: the Repeater page of a repeater or room and its login box (report 036 slice S1); 0.8.1: its read-only view, the hub and a screen per section, Refresh / Cancel, Refresh all (slice S2) |
| `src/ui/cli_page.*`, `cli_logic.*` | the CLI page (meshcore-cli on a pty through the relay, the line console, Help; 0.7.0: Open in CLI; 0.7.2: no To… picker); its logic unit tested (`tests/unit/test_cli_ui.inc`) |
| `src/main.cpp` | `cdk::run` (apps/_common: display, touch, keyboards, signals, the "needs 640x480" notice) |
| `tests/flows/`, `tests/scenes/`, `tests/unit/` | the simulator flows and `mesh-hop-script`, the shared harness's scenes, the unit tests |
