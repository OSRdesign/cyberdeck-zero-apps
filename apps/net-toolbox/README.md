# Net Toolbox

Version 0.1.0 (draft, not published), package `net-toolbox`. Part of [cyberdeck-zero-apps](../../README.md).

Net Toolbox gathers the tools you reach for when a network misbehaves: a step-by-step **diagnosis** ("why is my
internet broken?") in plain words, **ping** with loss and jitter, a **DNS lookup** with a resolver you choose, a TCP
**port check** and the **interfaces** of the board. It is a full-screen touch app made for the deck's small screen
(Pi Zero 2 W, 640x480) and meant to work the same on the Pi 3A+, the Pi 5 and the CM5. It does not repeat
[LAN Scan](../lanscan/README.md) (devices and their ports) or [Wi-Fi Survey](../wifi-survey/README.md) (channels).
Nothing leaves the board until you start a test.

Kind: a native `cp0_lvgl` app (kind A of the [guidelines](../../docs/APP-GUIDELINES.md)), because it has its own
pages, live results and must be complete by touch and by keys; the usual command-line tools (`dig`, `traceroute`)
are not installed on the boards, and their output does not fit the small screen.

The plan and the measured facts behind it are in the launcher's report 027 (`docs/dev/reports/027-net-toolbox.md`).

**State of this version:** the core and the pages (Home, Interfaces, Diagnosis, Ping, DNS lookup, Port check) are
written; the unit tests and the headless scenes pass; the board tests (`tests/BOARD-TESTS.md`) are still to be run on
the deck and the Pi 3A+.

## Pages and keys

Every page is a column under the shared header (title, the on-screen Esc, the status strip), laid out by the launcher's
layout service. **Up / Down** move the highlight, **Enter** (or a tap on the highlighted row) acts, **Esc / Left** (or the
header's Esc) go one level up and stop the tool in use; at Home they end the app. A tap on a row selects it, a second tap
on the selected row acts (the launcher's CompactList rule); every action is a row, so every action is reachable by tap
and by keys.

| Page | What it shows | Actions (rows) |
|---|---|---|
| **Home** | a line with the active interface and its address (or **No network**, amber), refreshed every 2 s | Diagnose my connection, Ping, DNS lookup, Port check, Interfaces |
| **Interfaces** | read-only, refreshed every 2 s: a heading per interface with State, IPv4, IPv6, MAC, Speed (Wi-Fi: Network, Signal `-58 dBm, Good`, Bit rate), then Network: Gateway and the DNS servers in `resolv.conf` order, the first marked | one row scrolls per key press |
| **Diagnosis** | starts by itself: the **health ladder**, six rungs (Link up, Address, Gateway, DNS, Internet, Captive portal), each a mark and a word: hollow circle *waiting*, spinning amber arc *checking...*, green disc with a check (the detail: `3 ms`, the address, the resolver), red disc with a cross, a dash *skipped*; under it the first failure in plain words or **Everything works** with the core's notes | Run again, Save log |
| **Ping** | the latency **line graph** of the last 120 packets (time on x, ms on y with an automatic scale, the **jitter band** (average plus and minus the jitter) shaded behind the line, a lost packet a red tick on the baseline and a gap, a slow one an amber dot), the **packet strip** (one cell per packet: short green *ok*, medium amber *slow*, tall red *lost*, small grey *waiting*) with its legend, and the statistics (sent, lost and loss %, min / avg / max, jitter). When ICMP sockets are not allowed a notice says why and how to fix it | Start / Stop, Target (Roller: Gateway, 1.1.1.1, 8.8.8.8, the saved targets, Type a host...), Save log, Save target |
| **DNS lookup** | the status line (response code in words, time, resolver used) and the answer rows (data, record type and TTL; the CNAME chain); Compare resolvers lists the system's, 1.1.1.1, 8.8.8.8 and 9.9.9.9 with their time and answer count, one after the other | Look up, Compare resolvers, Name (typed), Type (A, AAAA, MX, TXT, NS, PTR), Resolver (System default, Cloudflare, Google, Quad9, Type an address...), Save log |
| **Port check** | a large mark and word (**Open** green check, **Closed** red cross, **No answer** amber dash, **Unreachable** red mark, **Mixed**), the host with its address and what it means with the connect time | Run, Host (typed), Port (Roller of the presets Web 80 443, Mail, SSH, DNS, Remote desktop ... or Custom port...), Save log |

Typed values (a host, a name, a resolver address, a port number) open a text field page: a physical keyboard types at
once; with none connected the launcher's on-screen keyboard shows (the rule of `input.state`, never summoned by hand);
Enter keeps the value, a bad one says why under the field, Esc goes back. The choices (target, type, resolver, port)
are Roller pages with a check on the current value. **Save log** writes the page's result (see *Files the app keeps*)
and the line at the top says where. Ping has **Stop**, not Pause: a stopped ping keeps its graph and numbers on screen.

Leaving a page, and leaving the app, stops its worker thread: nothing runs in the background (a cancel ends a probe
within 50 ms). The page changes were checked for leaks (`tests/scenes/leak.scene`).

## Screenshots

The contact sheets of the headless scenes (`build.sh host`, folder in *Build and test*) show every page at 640x480,
800x480, 720x720, 1024x600 and 1280x720. Photos from the deck and the Pi 3A+ are to be added after the board tests.

## Tools in 0.1

* **Diagnosis**: six steps in order, stopping at the first failure (the steps after it are *skipped*):
  * *Link up*: an interface other than `lo` is up with a link (the default route's first);
  * *Address*: it has an IPv4 address that is not self-assigned (169.254.x.x);
  * *Gateway*: the default gateway answers ping (three tries). A router that ignores ping but answers on TCP 53, 80
    or 443 (even with a refusal) passes too, with a note;
  * *DNS*: `example.com` resolves through the resolver in use: the system's servers in the order of
    `/etc/resolv.conf` (the next one is asked when one is silent, as the C library does), or the one you chose. The
    result names the resolver that answered (on the deck `resolv.conf` lists 192.168.1.1 first while the gateway is
    192.168.68.1). When no server answers, one ping to 1.1.1.1 tells "DNS down" from "internet down";
  * *Internet*: 1.1.1.1, then 8.8.8.8 answer ping; when ping is not allowed or is blocked, a TCP connection to their
    port 443 counts (with a note);
  * *Captive portal*: a plain HTTP request for `http://connectivitycheck.gstatic.com/generate_204`; the answer 204
    means no portal, anything else means the network shows its own page (a sign-in page is suspected).

  Each step has a state (pending, running, pass, fail, skipped) and a short detail (*192.168.68.1, 3 ms*); the failed
  one comes with an explanation and what to do (*The gateway answers but DNS does not (192.168.1.1 timed out). Try
  another resolver, for example 1.1.1.1, in DNS lookup.*).
* **Ping**: to a name or an address (a name is resolved by the app's own DNS client, IPv4 first; an address is used
  as typed). Interval, timeout and count (0 = until stopped). Each packet is *ok*, *slow* (longer than 100 ms and
  than 3 times the average of the replies before it) or *lost* (no reply within the timeout; a send error counts as
  lost); the last 120 packets are kept for the strip and the graph. Sent, received, loss (lost / (received + lost):
  a packet still waiting is not lost yet), min / avg / max and the jitter (the mean difference between successive
  round trips). No root: an unprivileged ICMP socket (IPv4 and IPv6).
* **DNS lookup**: A, AAAA, MX, TXT, NS, PTR (from an address: type `1.1.1.1`) and CNAME, through the system's
  resolver (the first of `resolv.conf`), a typed address, or 1.1.1.1 (Cloudflare), 8.8.8.8 (Google), 9.9.9.9
  (Quad9). The answer rows (name, type, TTL, data), the CNAME chain, the time, the resolver and its response code
  in words (*NXDOMAIN, no such name*). A compare mode asks several resolvers one after the other and times each.
* **Port check**: one port or a preset (Web 80 443, Mail 25 465 587 993, SSH 22, DNS 53, Remote desktop 3389, VNC
  5900, File sharing 445, MQTT 1883, Printer 631 9100, HTTP alt 8080 8443) of a host: *open*, *closed* (the host
  refused: it is there, nothing listens), *filtered* (no answer within the timeout), *unreachable* (the network says
  so), with the connect time. Nothing is sent on an open port: the connection is closed at once.
* **Interfaces**: each adapter with its state, IPv4 and IPv6 addresses, MAC, MTU, speed (wired), the Wi-Fi signal
  (`/proc/net/wireless`) and, from `iw`, the network name and bit rate; the default gateway, the DNS servers and the
  search domains.
* **Save**: every result can be written as text to `~/net-logs/<tool>-<YYYYmmdd-HHMMSS>.txt`.

## How it works

* Each running tool has **one worker thread** (256 kB stack) that waits with `poll()` in slices of at most 50 ms, so
  a cancel (Esc, a new start) ends it within that time; no thread pool, no detached threads. The thread publishes
  immutable snapshots under a mutex; the screen looks at a sequence number on a timer and never waits for the
  network (as System Monitor's sampler).
* **Ping** uses `socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)` (and ICMPv6), which Linux allows to the groups in
  `net.ipv4.ping_group_range` (Raspberry Pi OS: everyone). The app checks at start whether the socket opens and,
  when not, says why and how to fix it.
* **DNS** is the app's own client (UDP with EDNS0, the query sent again after each try's timeout, TCP when the
  answer is truncated, once more without EDNS for an old server), not `getaddrinfo`, which cannot be cancelled,
  hides which server answered and cannot be pointed at another resolver. The parser bounds every read: compression
  pointers must point backwards (at most 32), names are at most 255 bytes, record counts must fit the packet, and a
  record whose data does not match its type is kept as *(malformed, N bytes)*. Names for ping and port check look in
  `/etc/hosts` first (`localhost`, the board's own name), then ask A, then AAAA.
* **Port check** and the captive-portal request are non-blocking TCP connections with a timeout; the HTTP request is
  hand-written (one `GET` with `Connection: close`; only the status line and the headers are read).
* **Interfaces**: `/sys/class/net/<if>/` (flags, operstate, carrier, address, MTU, speed), `getifaddrs` for the
  addresses, `/proc/net/route`, `/etc/resolv.conf`, `/proc/net/wireless`; `/usr/sbin/iw dev <if> link` (full path:
  `/usr/sbin` is not in every PATH) through the shared `async_run` with a 1.5 s timeout, optional.

## Requirements

* A screen of at least **640x480** (`X-MinSize=640x480`; under it the app says so and closes on any key) and a
  CyberdeckZero Launcher with full-screen app support (contract 1).
* Ping needs unprivileged ICMP sockets (`net.ipv4.ping_group_range` covering the user's group; the default on
  Raspberry Pi OS). Without them ping says so with the fix, and the diagnosis uses TCP connections instead.
* `iw` (package `iw`) for the Wi-Fi network name and bit rate; optional.
* No new package dependency (`depends` = the template's `libfreetype6, libinput10, libxkbcommon0`), no root.
* Network: the tests talk to the hosts you name; the diagnosis contacts your gateway, your DNS servers, 1.1.1.1
  (Cloudflare), 8.8.8.8 (Google) and `connectivitycheck.gstatic.com` (Google), only when you start it.

## Install

From **Settings > Apps** once it is published. By hand:

```sh
python3 tools/make_registry.py --only net-toolbox --out <a folder outside packages/>
sudo dpkg -i net-toolbox_0.1.0_arm64.deb      # remove: sudo dpkg -r net-toolbox
```

## Files the app keeps

| Path | What |
|---|---|
| `${XDG_CONFIG_HOME:-~/.config}/cyberdeckzero/net-toolbox.conf` | saved ping targets and the last values of each tool (`key=value`); written through a temporary file and a rename. Delete it to reset. `NET_TOOLBOX_CONFIG` names another file |
| `~/net-logs/<tool>-<YYYYmmdd-HHMMSS>.txt` | a result you saved (only when you ask). `NET_TOOLBOX_LOG_DIR` names another folder |

Logs go to stderr with the tag `[net-toolbox]` (the launcher's journal).

## Known limits

* **Not yet run on a board**: the tools are unit-tested on fixtures and on loopback and the pages on headless scenes;
  the board protocol (`tests/BOARD-TESTS.md`) is still to be run.
* Ping has Start and Stop, no Pause: Start begins a new series.
* IPv4 first: the gateway is the IPv4 default route; an IPv6-only network is not diagnosed yet.
* Names ending in `.local` (mDNS) are not resolved; use the address.
* The real ICMP path was tested on the build PC only inside a network namespace that allows ping (the PC itself
  does not); on the boards it is still to be run.

## Credits and licences

<!-- branding-allow-begin: credit of the cp0_lvgl runtime linked into the binary (MIT) -->
MIT, 2026 OSRdesign. The binary contains the launcher's `cp0_lvgl` runtime (M5Stack Technology CO LTD, MIT) and
LVGL (LVGL Kft, MIT); see `root/usr/share/doc/net-toolbox/copyright`. The fonts are the launcher's, read at run
time; none is shipped.
<!-- branding-allow-end -->
At run time the app reads the Linux kernel's `/proc` and `/sys` interfaces and, when installed, runs `iw` (ISC); it
is not shipped or modified. The ping, DNS (RFC 1035, EDNS0 of RFC 6891), TCP and HTTP clients are the app's own code.
The captive-portal check uses Google's `generate_204` address; the internet check uses Cloudflare's 1.1.1.1 and
Google's 8.8.8.8.

## Build and test

In WSL (the launcher build tree `~/zero7/launcher` with its SDK; see `../_common/build_app.sh -h`):

```sh
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/net-toolbox/build.sh host           # tests + scenes
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/net-toolbox/build.sh all --strict   # + aarch64 binary
python3 tools/make_registry.py --only net-toolbox --out <a folder outside packages/>      # a local .deb
```

* `src/core/`: no LVGL. `interfaces` (the readers and parsers), `source` (the system, or a fixture folder),
  `netaddr` (addresses, reverse names, host names), `worker` (the tool thread, the cancel token, the snapshot
  channel), `sock` (non-blocking socket steps), `dns` (the wire format), `dns_client` (queries, compare,
  `resolve_host`, the lookup session), `ping` (statistics, the packet ring, the ICMP socket, the engine, the
  session), `portcheck`, `http`, `diagnosis` (the steps, the probe interface, the session), `netlog` (saved results
  and their text), `settings`, `text`.
* `src/ui/`: `toolbox_app` + `tool_pages` (the frame: navigation, the band, the Choice and Typed pages, saving; the five
  tool pages), `view_model` (every text, tone and number a page shows, from the core's snapshots: no LVGL, unit-tested),
  `charts` (the health ladder, the ping graph and strip, the result badge: drawn with LVGL's draw API), `backend`
  (the runners the pages use: the core's sessions, or the scripted ones of the scenes), `style` (palette and helpers),
  `wifi_link` (`iw` through `async_run`).
* `tests/`: unit tests (`test-net-toolbox`, about 500 checks): the readers on `tests/fixtures/<board>/` (the deck,
  the deck with Wi-Fi not associated, a Pi 3A+ on 5 GHz with IPv6, a Pi 5 on Ethernet with Wi-Fi off); DNS packets
  built and parsed, crafted bad packets and a 40 000-case mutation fuzz; the DNS client against a fake server on
  loopback (UDP, TCP fallback, retries, silence, a closed port, FORMERR, compare, cancel); the ping statistics and
  engine on a scripted socket; port check and HTTP against loopback listeners (open, closed, filtered through a
  full accept queue); the diagnosis on a fake network, one test per failure point; the log files, the settings, the
  worker. The real ping of 127.0.0.1 and ::1 runs when the host allows ICMP sockets and is skipped with the reason
  otherwise. Headless scenes in `tests/scenes` (about 40, each at 640x480, 800x480, 720x720, 1024x600 and 1280x720, with the layout
  checks of `_common/headless`): Home, Interfaces on the four recorded boards, the Diagnosis in six states (healthy, a gateway
  that ignores ping, no link, DNS down, a captive portal, in progress), Ping (ok / slow / lost packets, everything lost,
  an unknown name, no ICMP socket, the target Roller, a typed host with the on-screen keyboard), DNS lookup (answer,
  NXDOMAIN, timeout, compare, MX, PTR, the choices, the typed name), Port check (open, closed, no answer, unreachable,
  a preset with several ports, an unknown host, the choices), `nav` (Up / Down, Esc and Left at every level, Esc at Home
  quits), `touch` (every action by tap), `save-empty`, `leak` and the notice under 640x480. The scenes run on a
  scripted backend (`NET_TOOLBOX_DEMO`, see `src/ui/backend.hpp`) and the recorded boards of `tests/fixtures`
  (`NET_TOOLBOX_FIXTURE`): no network, the same picture every time. `tests/test_ui_model.cpp` checks `view_model`.

## Changes

* 0.1.0 (draft): first version: interfaces, diagnosis with the health ladder, ping with the latency graph, jitter band
  and packet strip, DNS lookup and compare, port check, saved results and settings; the pages for touch and keys, the unit
  tests and the headless scenes. Board tests still to run.
