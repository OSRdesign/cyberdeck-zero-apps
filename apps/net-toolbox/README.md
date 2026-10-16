# Net Toolbox

Version 0.4.1, package `net-toolbox`. Part of
[cyberdeck-zero-apps](../../README.md).

Net Toolbox gathers the tools you reach for when a network misbehaves: a step-by-step **diagnosis** ("why is my
internet broken?") in plain words, **ping** with loss and jitter, a **DNS lookup** with a resolver you choose, a TCP
**port check**, the **interfaces** of the board and, from 0.2, a **traceroute** drawn as a chain of hops, an **HTTP check**
with a timing waterfall and the certificate's days left, your **public IP** and **whois**. It is a full-screen touch app made for the deck's small screen
(Pi Zero 2 W, 640x480) and meant to work the same on the Pi 3A+, the Pi 5 and the CM5. From 0.4 it also **replaces
[LAN Scan](../lanscan/README.md) and [Wi-Fi Survey](../wifi-survey/README.md)**: **Local devices** (the devices on your network
and the open ports of one of them), **Wi-Fi networks** (the networks around, their channels and the least crowded one) and a
**Wi-Fi walk test** (the signal against time while you walk with the board). LAN Scan and Wi-Fi Survey remain installable
for now. Nothing leaves the board until you start a test (the Wi-Fi list is read by NetworkManager's own scan as soon as its
page opens, as the diagnosis and the public address start when you open them).

Kind: a native `cp0_lvgl` app (kind A of the [guidelines](../../docs/APP-GUIDELINES.md)), because it has its own
pages, live results and must be complete by touch and by keys; the usual command-line tools (`dig`, `traceroute`)
are not installed on the boards, and their output does not fit the small screen.

The plan and the measured facts behind it are in the launcher's report 027 (`docs/dev/reports/027-net-toolbox.md`).

**State of this version (0.2.0):** the core and the pages of all nine tools (Home, Diagnosis, Ping, Traceroute,
DNS lookup, Port check, HTTP check, Public IP, Whois, Interfaces) are written; the unit tests and the headless scenes
pass; the pages ran on the deck (Zero 2 W); the board tests (`tests/BOARD-TESTS.md`) are still to be run on the Pi 3A+. The core of the four new
tools is described in [0.2.0: the new tools' core](#020-the-new-tools-core).

**0.3.0:** three more tools with their pages: a **speed test** (a needle gauge, the live
throughput chart, download, upload, latency and the latency under load graded A to F for bufferbloat), **Wake-on-LAN**
with saved machines and a wake watch, and a **Bluetooth radar** (the devices around as blips on three rings: near, medium,
far). The core is described in [0.3.0: the new tools' core](#030-the-new-tools-core), the pages in
[The 0.3 pages](#the-03-pages). Also 0.3: the **interface chooser** for boards with several ways out (a second Wi-Fi
adapter, Ethernet): see [The network chooser](#the-network-chooser-03). The speed test and the Bluetooth radar
have run on the deck (Zero 2 W) and the Pi 3A+; Wake-on-LAN and the interface chooser with a second adapter are still to be
tried on a board (`tests/BOARD-TESTS.md`, B11 to B15).

**0.4.0 (draft):** three more tools with their pages, folding in LAN Scan and Wi-Fi Survey. **Local devices** discovers the
devices on your network (their address, MAC, vendor and name), marks this device and the router, and scans the open TCP ports
of one device you pick (its page also pings it, checks a port, saves it for Wake-on-LAN). **Wi-Fi networks** lists the networks
around (signal bars, channel, band, security), a Channels view with the least crowded channel, and a page per network that
follows it by its BSSID. The **Wi-Fi walk test** draws the signal against time with a weak-zone line and ends with a summary
card. The cores are described in [Local devices (0.4): the core](#local-devices-04-the-core) and [Wi-Fi networks and walk
test (0.4): the cores](#wi-fi-networks-and-walk-test-04-the-cores), the pages in [The 0.4 pages](#the-04-pages). The pages ran
in the headless scenes only; the board steps B16 to B19 (`tests/BOARD-TESTS.md`) are still to be run. Local devices scans only
the chosen interface's own private network (never a public address), without root.

## Screenshots

Shots 01 to 12 are real captures of the app running on the boards: the deck (Pi Zero 2 W, 640x480) and a Pi 3A+ with
the Waveshare 4.3" DSI panel (800x480), on a home network. Shots 13 and 14 are rendered by the app's headless harness
(`build.sh host`) with fixture data at other screen sizes, **not photographs of a panel**; the contact sheets of the headless scenes
(folder in *Build and test*) show every page at 640x480, 800x480, 720x720, 1024x600 and 1280x720.

| | |
|---|---|
| ![Home](screenshots/01-home-deck.png) 640x480, deck. Home: the active interface and its address on top, then the tools | ![Diagnosis](screenshots/02-diagnosis-deck.png) 640x480, deck. Diagnosis: the six rungs all green, *Everything works* |
| ![Ping](screenshots/03-ping-deck.png) 640x480, deck. Ping 1.1.1.1: the latency graph with one slow spike (amber dot), the packet strip and the statistics | ![Traceroute](screenshots/04-traceroute-deck.png) 640x480, deck. Traceroute to 8.8.8.8: the chain of hops, two silent ones (*no reply*), a bar per hop |
| ![HTTP check](screenshots/05-http-check-deck.png) 640x480, deck. HTTP check of example.com: *200 OK*, the timing waterfall (DNS, Connect, TLS, Waiting, Transfer) and the certificate's days left | ![DNS lookup](screenshots/06-dns-lookup-deck.png) 640x480, deck. DNS lookup: the answer for github.com A, its TTL and the resolver used |
| ![Compare resolvers](screenshots/07-dns-compare-deck.png) 640x480, deck. Compare resolvers: the system's, Cloudflare, Google and Quad9 with their times | ![Port check](screenshots/08-port-check-deck.png) 640x480, deck. Port check of the router's web ports: *Open*, with each port's connect time |
| ![Interfaces](screenshots/09-interfaces-deck.png) 640x480, deck. Interfaces: wlan0 with its state, addresses, MAC, network and signal | ![Diagnosis on the Pi 3A+](screenshots/10-diagnosis-pi3a.png) 800x480, Pi 3A+. The same diagnosis on the wider screen |
| ![Ping on the Pi 3A+](screenshots/11-ping-pi3a.png) 800x480, Pi 3A+. Ping 8.8.8.8 with the jitter band shaded behind the line | ![Traceroute on the Pi 3A+](screenshots/12-traceroute-pi3a.png) 800x480, Pi 3A+. Traceroute scrolled to the end: hop 10, *dns.google*, reached |
| ![Traceroute render](screenshots/13-traceroute-720x720.png) 720x720 render, HackBerry size (square). The whole chain of nine hops fits on the square screen | ![HTTP check render](screenshots/14-http-check-1280x720.png) 1280x720 render, larger screen. The HTTP check with the certificate's subject, issuer and expiry |

The screens above come from several screen sizes: the app adapts to the screen it runs on.

## Pages and keys

Every page is a column under the shared header (title, the on-screen Esc, the status strip), laid out by the launcher's
layout service. **Up / Down** move the highlight, **Enter** (or a tap on the highlighted row) acts, **Esc / Left** (or the
header's Esc) go one level up and stop the tool in use; at Home they end the app. A tap on a row selects it, a second tap
on the selected row acts (the launcher's CompactList rule); every action is a row, so every action is reachable by tap
and by keys.

| Page | What it shows | Actions (rows) |
|---|---|---|
| **Home** | a line with the effective interface and its address (or **No network**, amber; or the `wlan1 not found: using Automatic` note), refreshed every 2 s | **Network** (value `Automatic (wlan0)` or `wlan1`: opens the choice page), Diagnose my connection (the page starts on this row), Ping, Traceroute, DNS lookup, Port check, HTTP check, Speed test, Public IP, Whois, Wake-on-LAN, Bluetooth radar, **Local devices**, **Wi-Fi networks**, **Wi-Fi walk test**, Interfaces (sixteen rows: the list scrolls) |
| **Interfaces** | read-only, refreshed every 2 s: a heading per interface with State, IPv4, IPv6, MAC, Speed (Wi-Fi: Network, Signal `-58 dBm, Good`, Bit rate), then Network: Gateway and the DNS servers in `resolv.conf` order, the first marked | one row scrolls per key press |
| **Diagnosis** | starts by itself: the **health ladder**, six rungs (Link up, Address, Gateway, DNS, Internet, Captive portal), each a mark and a word: hollow circle *waiting*, spinning amber arc *checking...*, green disc with a check (the detail: `3 ms`, the address, the resolver), red disc with a cross, a dash *skipped*; under it the first failure in plain words or **Everything works** with the core's notes | Run again, Save log |
| **Ping** | the latency **line graph** of the last 120 packets (time on x, ms on y with an automatic scale, the **jitter band** (average plus and minus the jitter) shaded behind the line, a lost packet a red tick on the baseline and a gap, a slow one an amber dot), the **packet strip** (one cell per packet: short green *ok*, medium amber *slow*, tall red *lost*, small grey *waiting*) with its legend, and the statistics (sent, lost and loss %, min / avg / max, jitter). When ICMP sockets are not allowed a notice says why and how to fix it | Start / Stop, Target (Roller: Gateway, 1.1.1.1, 8.8.8.8, the saved targets, Type a host...), Save log, Save target |
| **DNS lookup** | the status line (response code in words, time, resolver used) and the answer rows (data, record type and TTL; the CNAME chain); Compare resolvers lists the system's, 1.1.1.1, 8.8.8.8 and 9.9.9.9 with their time and answer count, one after the other | Look up, Compare resolvers, Name (typed), Type (A, AAAA, MX, TXT, NS, PTR), Resolver (System default, Cloudflare, Google, Quad9, Type an address...), Save log |
| **Port check** | a large mark and word (**Open** green check, **Closed** red cross, **No answer** amber dash, **Unreachable** red mark, **Mixed**), the host with its address and what it means with the connect time | Run, Host (typed), Port (Roller of the presets Web 80 443, Mail, SSH, DNS, Remote desktop ... or Custom port...), Save log |
| **Traceroute** | the status line (`to example.com 93.184.215.14, round 14, 9 hops`; stopped: the verdict, `Reached in 9 hops, 43 ms` or `Did not reach the destination: no answer after hop 6`) and **the chain**: a line down the left with one node per hop, then the hop number, the name (reverse name when known, else the address; the address smaller beside it), a latency **bar** (one linear scale: the largest average on screen; a stub for a tiny one; the best-to-worst range as a lighter extension), the average in ms and a loss tag (`30 %`). The node is a shape and a colour: filled **green** circle ok; **amber** circle with a notch (1 to 20 % loss, or an average over 150 ms); **red** circle with an X (more than 20 % loss that later hops share: real loss); hollow **dotted** circle and the words `no reply` (a silent hop is normal, never red); **grey ring** and `probably rate limiting` (loss at that hop only, the later ones are clean); the destination gets a double ring and the word `reached`. Hops appear as the first round fills them and the numbers move once a second without the layout jumping. Enter or a tap on the selected hop opens its details in place (all addresses seen, last / avg / best / worst, jitter, sent / received, loss, a plain reading of the node) | a row of three large cells above the chain: Start / Stop, Target (Gateway, 1.1.1.1, 8.8.8.8, the last host typed, Type a host...), Save log; Tab, Left / Right move between the cells, Down goes into the chain, Up from the first hop comes back |
| **HTTP check** | Run, then right under it: the status **badge** (`200 OK` green check, `301 moved` amber arrow, `404 Not Found` / `500 ...` red cross, an error in plain words with the same red cross), the final URL when it differs, the **waterfall** (one bar split into DNS, Connect, TLS, Waiting, Transfer on one scale, each segment in its own tone and named below with its milliseconds, a segment above 0 ms at least 2 px wide, the total), the **certificate badge** (`expires in 83 days` green from 30 days, amber under 30, red under 7 or `expired 12 days ago`) with Subject, Issuer, Valid until, then Remote IP, HTTP version, Size, Redirects | Run / Stop (curl is ended), URL (typed), Method (GET, HEAD), Ignore certificate errors (On / Off; On shows a warning), Save log |
| **Public IP** | asks by itself on entry: Public IPv4, Public IPv6 (`none` is normal and neutral), Country, Cloudflare data centre, Local address; behind carrier-grade NAT (a local address in 100.64.0.0/10) a note says `Your provider shares one public address between customers (carrier-grade NAT): you cannot receive inbound connections` | Run again, Whois of this address (opens Whois with it and runs it), Save log |
| **Whois** | a summary of the answer (domain: Registrar, Created, Updated, Expires with the days left in words, Status, Name servers; address: Net name, Organisation, Country, Range, AS number, Abuse contact; an empty field is not shown, a value is never cut: it wraps) and under the heading Raw answer the raw lines as compact rows (cut with dots, the first 200 lines; Save log has them all) | Query (typed), Run / Stop, Save log (placed under Run so that it stays in reach) |

The three 0.3 pages are described under [The 0.3 pages](#the-03-pages), the three 0.4 pages under [The 0.4 pages](#the-04-pages).

Typed values (a host, a name, a resolver address, a port number) open a text field page: a physical keyboard types at
once; with none connected the launcher's on-screen keyboard shows (the rule of `input.state`, never summoned by hand);
Enter keeps the value, a bad one says why under the field, Esc goes back. The choices (target, type, resolver, port)
are Roller pages with a check on the current value. **Save log** writes the page's result (see *Files the app keeps*)
and the line at the top says where. Ping has **Stop**, not Pause: a stopped ping keeps its graph and numbers on screen.
The Traceroute, HTTP check, Public IP and Whois pages keep their rows in a list of their own (rows of any height: wrapped
values, paragraphs, the chain's hops, the badges) with the same keys and the same tap rule; Tab goes from the cells of
Traceroute to the chain and back.

Leaving a page, and leaving the app, stops its worker thread and ends its curl (Stop does the same): nothing runs in the
background (a cancel ends a probe within 50 ms). The page changes were checked for leaks (`tests/scenes/leak.scene`).

## What each tool sends, and to whom

| Tool | Sends | To |
|---|---|---|
| Traceroute | ICMP echo (or UDP to ports 33434 and up) with growing TTLs, one probe per hop per second; PTR questions for the names of the hops | the host you chose (Gateway, 1.1.1.1, 8.8.8.8, or the host you typed), the routers on the way, your DNS server |
| HTTP check | one `GET` or `HEAD` request through `curl` (never a shell); a TLS handshake read by `openssl s_client` only when curl refused the certificate | the address you typed, nothing else |
| Public IP | two `curl` requests (IPv4, IPv6) to `https://www.cloudflare.com/cdn-cgi/trace` | Cloudflare |
| Whois | the query over TCP 43 | `whois.iana.org`, then the registry or regional registry it names (ARIN, RIPE NCC, APNIC, LACNIC, AFRINIC, a TLD registry) and a registrar server when the registry names one, at most three referrals |
| Speed test (0.3) | HTTPS requests through `curl`: 11 small ones (latency), then about 8 s of download and 8 s of upload, **at most 150 MB each way** per run (on a 25 Mbit/s link about 25 MB each way); pings beside it, one every 0.2 s | `speed.cloudflare.com` (Cloudflare); the pings to 1.1.1.1 (Cloudflare) or to your gateway |
| Wake-on-LAN (0.3) | one 102-byte UDP datagram per destination (4 on one interface: 255.255.255.255 and the subnet's broadcast, ports 9 and 7); then, when the machine has a host to watch, one ping a second for up to 120 s | your local network (broadcast), or the broadcast address you saved; the machine's address |
| BLE scan (0.3) | nothing: the adapter listens (an active LE scan sends scan requests to the devices it hears, as any phone does) | the Bluetooth devices around |
| Local devices (0.4) | an ICMP echo and a few one-packet TCP connect probes (ports 80, 443, 22, 445, 62078, 8080) to each address of your subnet, to make the kernel learn its MAC; then a name query per device (mDNS on UDP 5353, NetBIOS on UDP 137, reverse DNS to your resolver); a port scan sends TCP connects to one device you pick | the addresses of your own private subnet (the chosen interface's /24 at most); the device you pick for the port scan |
| Wi-Fi networks (0.4) | nothing of its own: it asks NetworkManager for its scan results (`nmcli device wifi list`, every 5 s; the adapter's probe requests are NetworkManager's own scan, as any phone's) | the networks around; it never connects |
| Wi-Fi walk test (0.4) | nothing: it reads `/proc/net/wireless` and runs `iw dev <if> link` once a second | the board's own adapter |

Everything starts when you press Run or Start, except the diagnosis (0.1) and the public address, which asks when you
open its page.

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

## 0.2.0: the new tools' core

The core (`src/core/`, no LVGL) of four more tools; their pages are described above (the screen code is in `src/ui/`:
`rich_list.*` the list of rows of any height and the action cells, `chain.*` the hop rows, the badge, the waterfall and
the certificate block, `flow_runner.*` the curl tools on the shared `async_run`, `view_model_02.cpp` the words). Same rules as 0.1: one worker thread per
running tool (256 kB stack), every wait a `poll()` slice of at most 50 ms (a stop ends a tool within that time),
immutable snapshots under a mutex that the screen polls, nothing on the network until you start a tool.

* **Traceroute / MTR** (`traceroute`): the app's own, no root, no `traceroute` or `mtr` program (neither is installed).
  One socket for the whole run: an unprivileged ICMP echo socket (`SOCK_DGRAM`, `IPPROTO_ICMP`, allowed by
  `net.ipv4.ping_group_range` as for ping) and, when that socket is not allowed, UDP datagrams to ports 33434 and up
  (the snapshot says which, and why). Each probe leaves with its own TTL (`IP_TTL`, `IPV6_UNICAST_HOPS`); the answers
  come back on the socket's error queue (`IP_RECVERR` / `IPV6_RECVERR`, `MSG_ERRQUEUE`): *time exceeded* names the
  router of that hop, an echo reply (ICMP) or *port unreachable* from the destination (UDP) ends the path, an
  *unreachable* from a router ends it there with its words (*host unreachable*, *communication prohibited*, ...).
  It runs in **rounds** like mtr: a round probes every hop up to the destination (every hop up to the maximum while
  the destination is not known), a few ms apart, all in flight together and waited for in one `poll()` loop, so a
  round lasts about one probe timeout. Per hop and across the rounds: sent, received, lost (no answer within the
  timeout), loss %, last, average, best, worst, jitter (as ping: the mean difference of successive round trips), the
  addresses that answered (most frequent first, four kept; *several addresses* when the path is balanced) and their
  reverse names. A hop that never answers is a **silent hop**, not an error. **Rate limiting**: a hop with loss when
  every later hop that answers has none is flagged `loss_only_here` (*likely ICMP rate limiting, not a real problem*).
  The list stops at the destination; when it never answers, every hop up to the maximum is probed and a summary
  stops at the last hop that answered. Settings: maximum hops (30), round interval (1 s), rounds (0 = until
  stopped), probe timeout (1.5 s). The destination: an address, `/etc/hosts`, or the app's DNS client (IPv4 first,
  IPv6 when the name has only AAAA records or for an IPv6 address). **Reverse names** use the DNS client's packet code
  in a non-blocking form on the same thread: one UDP socket to the resolver, a PTR query when an address first
  answers, at most 4 waiting, 1.5 s each (one try), at most 64 addresses (the cache): the probing never waits for a
  name.
* **HTTP check** (`httpcheck`): the system's `curl` (8.14.1 on the boards), as an argv, never through a shell:
  `curl -sS -v -o /dev/null -g --proto =http,https --proto-redir =http,https -m 15 --connect-timeout 8
  --max-redirs 5 [-L] [-I] [-k] -w '%{json}\n' --url <address>`. `-k` (ignore certificate errors) only when you turn
  it on; `-I` asks with HEAD instead of GET; `--proto` keeps curl, and every redirect, to http and https; `-g` turns
  off curl's `{}` / `[]` URL patterns; `-v` only serves to tell a refused connection from no route (curl 8.14 says
  *Could not connect to server* for both). The address: a host, `host:port`, `http://...` or `https://...` (https when
  no scheme is typed); refused: blanks, control characters, quotes, `;`, `` ` ``, `$`, `\`, `<`, `>`, a leading `-`,
  another scheme, a user name or password, a bad host or port, more than 2048 characters. curl's `%{json}` report
  gives the status (with its reason: *404 Not Found*), the final address and the redirect count, the HTTP version, the
  server's address, the size, and the timings, shown as stages: DNS (namelookup), connect (connect - namelookup), TLS
  (appconnect - connect), wait / first byte (starttransfer - pretransfer), transfer (total - starttransfer); with
  redirects curl adds the requests together. **The certificate**: for a connection curl verified, its report carries
  the chain (`certs`: subject, issuer, start and expire dates of the server's certificate). When curl refuses the
  certificate (exit 60: expired, self-signed, unknown issuer, not yet valid) its report has none; the app then runs
  `openssl s_client -connect <host>:<port> -servername <host>` (stdin empty, so it ends after the handshake; no
  `-servername` for an address) and reads the server's certificate from its *Certificate chain* lines (`s:`, `i:`,
  `v:NotBefore: ...; NotAfter: ...`), without judging it. Checked on the deck on 2026-10-15 (curl 8.14.1 with
  OpenSSL 3.5.7; also OpenSSL 3.0.13 on the build PC): curl alone gives the dates of a good certificate but not of a
  refused one, and `openssl s_client` alone gives them without a pipe to `openssl x509`. The days left use the board's
  clock (a wrong clock shows as *not yet valid* or *expired*). Errors in plain words from curl's exit code and
  message: the name cannot be found, refused, no route, timed out (no answer to the connection, or too slow),
  certificate expired / not yet valid / name mismatch / self-signed / untrusted issuer, TLS failed, too many
  redirects, empty reply, connection cut, not HTTP, curl not installed. With `-k` the page is shown and a note says
  what was ignored. The core builds the commands and reads their output; who runs them is injectable (`cmdrun`): the
  pages will use the shared `async_run` (as `iw`), the sessions and the tests the core's own runner (posix_spawn,
  stdout and stderr in one pipe, stdin `/dev/null`, `LC_ALL=C`, its own process group: a timeout or a stop ends the
  whole group, SIGTERM then SIGKILL after 0.5 s).
* **Public address** (`publicip`): `curl -4 -sS -m 8 --proto =https https://www.cloudflare.com/cdn-cgi/trace`, then
  the same with `-6`. Cloudflare answers `key=value` lines: the address (`ip`), the country (`loc`, two letters) and
  the Cloudflare site that answered (`colo`, an airport code). An IPv6 request that fails is reported as *no IPv6*,
  the usual case at home, not as an error. `ip_class()` tells public, private, carrier-grade NAT (100.64.0.0/10),
  loopback, link-local or reserved; `nat_kind()` compares the board's address with the public one: the same (the
  board has the public address), behind a router (NAT), or behind carrier-grade NAT (the board's address is in
  100.64.0.0/10: the provider shares one public address).
* **Whois** (`whois`): the app's own TCP port 43 client. The query (a domain, an IPv4 or an IPv6 address) goes to
  `whois.iana.org`, whose `refer:` (or `whois:`) line names the registry: the TLD's server for a domain, the regional
  registry for an address (ARIN, RIPE NCC, APNIC, LACNIC, AFRINIC); that answer may name another server
  (`Registrar WHOIS Server:`, ARIN's `ReferralServer:`), asked in turn: at most 3 referrals, never the same server
  twice for the same query, 64 kB per answer, 10 s in all. `whois.arin.net` gets `n + <address>`, the others the
  query as typed; a registry that finds no `www.example.com` is asked for `example.com` (at most twice). The result
  keeps the raw answers (control characters dropped, bytes that are not UTF-8 shown as `?`, 300 bytes per line, 400
  lines in all) and a summary of the most specific answer (IANA's own only when no other came): for a domain the
  registrar, created, updated, expires, status codes and name servers; for an address the network name, holder,
  country, range, AS number and abuse contact. A field the answers do not give stays empty.
* **Saved results** (`netlog`): `format_trace`, `format_httpcheck`, `format_publicip`, `format_whois`, same text style
  as 0.1. **Settings**: `trace_target`, `http_url`, `http_insecure` (0 by default), `whois_query` in the same file;
  a 0.1 file reads with their defaults.

Limits of the 0.2 core: the real ICMP trace, routers that limit their ICMP answers, and the whois servers were
exercised from the build PC (WSL, UDP probes through its NAT; whois over the internet) but not yet on a board; curl's
`certs` of a redirect chain describe the last server only; a hop that answers from more than four addresses keeps
the others as a count; reverse names come from the resolver in `resolv.conf` (or the one chosen), over UDP only.

## 0.3.0: the new tools' core

The core (`src/core/`, no LVGL) of three more tools; their pages are not written yet. Same rules as before: one worker
thread per running tool (256 kB stack), every wait a `poll()` slice of at most 50 ms, immutable snapshots under a mutex
that the screen polls, nothing leaves the board until you start a tool. The methods were chosen on the deck and the Pi
3A+ on 2026-10-15 (the measures are below).

* **The stream runner** (`cmdrun`: `StreamRunner`, `DirectStreamRunner`): the speed test and the BLE scan run a program
  whose output is a stream, which the shared `async_run` cannot carry (it hands lines to the screen's thread). The
  runner (posix_spawn, no shell, its own process group, `LC_ALL=C`, as the 0.2 runner) counts and drops curl's body on
  stdout (it is never kept: the memory stays flat), reads stderr (or stdout and stderr together) as lines, and feeds
  stdin with zero bytes (an upload) or text (bluetoothctl's commands). The tool sees each line and a tick after each
  wake-up and can send more text, close stdin or stop the program. A stop, a cancel or the timeout first writes a
  farewell to stdin (bluetoothctl: `scan off`, `quit`) and waits its grace, then SIGTERM, then SIGKILL, to the whole
  group. Writing to a program that is gone gives EPIPE, never SIGPIPE.
* **Speed test** (`speedtest`): Cloudflare's open speed endpoints, no account, through `curl` (8.14.1 on the boards):
  * *Latency*: one curl, 11 requests of `https://speed.cloudflare.com/__down?bytes=0` on one kept-alive connection. The
    first sets the connection up (its DNS, TCP and TLS time is shown as *connection setup*), the 10 others give round
    trips: first byte minus request sent, less the server's own time from its `Server-Timing` header (Cloudflare's
    worker takes 20 to 35 ms). Median and jitter (as ping). Beside it, 10 pings of the ping target (the idle latency).
  * *Download*: one curl asks for `__down?bytes=N` several times on the same connection (50 MB each; the server refuses
    100 000 000 bytes and more), the sizes adding up to the cap (150 MB): the body is counted and dropped. It stops 8 s
    after the first byte, or when every byte came (*capped*).
  * *Upload*: one curl, `-X POST -T -` (chunked) with `Expect:` off: the runner writes zero bytes on its stdin, at most
    the cap; 8 s after curl took the first byte, stdin is closed, curl ends the body and reads the answer, whose
    `cf-meta-upload-bytes` header says what the server received. If the answer does not come within 6 s, curl is
    stopped and the speed is what it sent (with a note).
  * *Figures*: a sample every 0.5 s (the live figure and the chart, Mbit/s = bytes x 8 / seconds / 10^6); the result
    leaves out the first second (TCP's slow start): (bytes at the end - bytes at 1 s) / (time between); a phase shorter
    than 2 s is taken whole. An upload is measured as the bytes handed to curl at both ends, so what waits in the
    buffers drops out of the difference.
  * *Latency under load and bufferbloat*: the ping module pings the target (1.1.1.1, or your gateway, or none:
    setting `speed_ping`) every 0.2 s from the first byte of each direction to its end. The increase is the loaded
    median minus the idle median: **A** under 30 ms, **B** under 60 ms, **C** under 200 ms, **D** under 400 ms, **F**
    from 400 ms (the steps of the Waveform bufferbloat test, without its A+); every ping lost under load is an F with
    that said. The grade shown is the worse direction. Without ping (not allowed, no target) the speeds are measured,
    the grade is not.
  * *Server*: the Cloudflare site that answered (an airport code, `colo`).
  * *Errors in plain words*: no curl, no DNS, no connection, TLS failed, the certificate refused (*is the board's clock
    right?*), HTTP 403 / 429 (*too many tests: wait a minute*) / 5xx, the connection broke (measured over what came when
    it lasted at least 1 s), too slow. curl's own message is kept for the log.
  * *Why this way*: on the deck one connection gave 22 to 27 Mbit/s down and two at once 24 Mbit/s together; on the Pi
    3A+ (5 GHz) one gave 110 to 113 Mbit/s and two 107: one connection fills the link. The CPUs have no AES
    instructions (openssl speed: AES-256-GCM 22.6 MB/s on the deck, 30 MB/s on the 3A+; ChaCha20-Poly1305 162 and 233
    MB/s), and curl at full speed took 21 % of a core with AES-256-GCM against 11 % with ChaCha20 on the deck, so
    ChaCha20-Poly1305 is asked first (`--tls13-ciphers`, AES kept as a fallback); HTTP/1.1 (`--http1.1`). Upload: 32
    to 36 Mbit/s on the deck, about 106 Mbit/s on the 3A+; curl's own memory while uploading from stdin stayed at 15
    MB (a separate process), the app's does not grow.
* **Wake-on-LAN** (`wol`): the magic packet (6 bytes 0xFF, then the MAC address 16 times: 102 bytes) in a UDP datagram
  with `SO_BROADCAST`, no root. MAC addresses as `aa:bb:cc:dd:ee:ff`, `aa-bb-cc-dd-ee-ff` or `aabbccddeeff`; refused
  with words: other forms, a character that is not hex, the all-zero, the broadcast and multicast addresses. Where it
  goes: on each interface that is up with an IPv4 address (or the one chosen, setting `wol_iface`), to
  255.255.255.255 and to the subnet's directed broadcast (192.168.71.255 for 192.168.68.121/22), on port 9 and also
  7 (or the machine's own port); the socket is bound to the interface's address, which makes Linux send the broadcast
  out of that interface. A machine with a saved broadcast address (another subnet's) gets its packets there instead,
  routed by the system. The result: packets sent and errors, each in words. **Wake watch** (setting `wol_watch`):
  when the machine has a host to watch, it is pinged once a second for up to 120 s: *answers after 23 s*, *no answer
  yet (41 s)*, *no answer after 120 s*; an answer to the very first ping means it was probably awake already. Up to
  20 saved machines (name, MAC, host to watch, broadcast address, port).
  **Wake-on-LAN works only when the machine is set up for it** (in its BIOS or UEFI and in its network card's
  settings), usually over **wired Ethernet**; machines on **Wi-Fi rarely wake up**. A machine that is fully off (not
  asleep) may need its power supply on (some lose Wake-on-LAN after a power cut).
* **BLE scan** (`ble`), the data of the radar: BlueZ's `bluetoothctl` (5.82 on the boards), run by the stream runner
  with `menu scan`, `duplicate-data on`, `transport le`, `back`, `scan on` written on its stdin. With duplicate data on,
  BlueZ reports every advertisement it hears: one `[CHG] Device <addr> RSSI: 0xffffffb8 (-72)` line each time (busy
  devices several a second, others about once a second or less, as often as they advertise). On the Pi 3A+, 8 s gave
  about 200 RSSI lines from about 20 devices. Each new device gets one `info <addr>`, whose answer gives its address
  type (public or random). Why not another way: a plain `bluetoothctl --timeout 10 scan on` gave no RSSI line at all;
  a discovery started with `busctl call` ends with the call (BlueZ ties a discovery to the D-Bus client that started
  it: `Discovering: no` one second later); a D-Bus client inside the app (sd-bus) would need libsystemd in the shared
  build. The table: address, address type, name (or an alias that is not just the address), RSSI raw and smoothed
  (exponential, 0.3), the time of the last reading, TX power, the company of the manufacturer data (a short table of
  common Bluetooth SIG ids: Apple, Microsoft, Samsung, Google, Espressif, Nordic ...; else *company 0x1234*; the app
  ships no OUI table and random addresses have none), first and last seen. A device unheard for 20 s leaves the live
  list; *seen* counts every device heard in the run. **Distance**: near from -60 dBm, medium from -80 dBm, far below,
  from the smoothed RSSI only: a rough guess, not metres. Before scanning, plain words instead of a failure: no
  adapter, Bluetooth blocked by rfkill (soft or hard), BlueZ missing (no `bluetoothctl`), bluetoothd not running,
  the adapter powered off. A scan lasts 60 s at most (setting `ble_seconds`, 5 to 60); run it again to go on. **It
  always turns discovery off**: at the end of the time (`scan off`, `quit`), on Stop, when the page or the app goes,
  when BlueZ refuses, on the runner's timeout (the farewell, then SIGTERM and SIGKILL); BlueZ also ends the discovery
  of a client that goes away (on the Pi 3A+, `Discovering: no` 0.5 s after a SIGKILL of bluetoothctl in the middle of
  a scan). **Wi-Fi and Bluetooth share one radio chip**: a Bluetooth keyboard or mouse may hiccup while the scan runs
  (the snapshot carries that warning for the page).
* **Saved results** (`netlog`): `format_speedtest`, `format_wol`, `format_ble`, same text style. **Settings**: `wol=`
  lines (the saved machines), `wol_iface`, `wol_watch`, `speed_ping`, `ble_seconds`; a 0.2 file reads with their
  defaults and 0.2 ignores them.

Limits of the 0.3 core: the speed test measures one connection to the nearest Cloudflare site, not your line's
contract; on a link faster than about 150 Mbit/s the 150 MB cap ends a direction before 8 s (the Pi 5 on gigabit:
about 1.2 s; the result is then the whole transfer); the speed itself costs data (up to 300 MB a run). Phones and
watches change their random Bluetooth address every few minutes (privacy), so one phone can show as several devices
over a long session; devices that do not advertise (a connected keyboard) are not seen. bluetoothctl's output format
is BlueZ 5.82's (5.6x lines such as `RSSI: -72` are read too). Measured on the boards with the shell (curl,
bluetoothctl); the app's own sessions were exercised on the build PC (the real speed test against Cloudflare, a fake
bluetoothctl) but not yet on a board.

## Local devices (0.4): the core

The core (`src/core/`, no LVGL) of a LAN scanner, folding in what [LAN Scan](../lanscan/README.md) did, written in Net
Toolbox's style: one worker thread per running tool (256 kB stack), every wait a `poll()` slice of at most 50 ms (a
stop ends it within that time), immutable snapshots under a mutex that the screen polls, nothing on the network until
you start it, no root. Its pages ([The 0.4 pages](#the-04-pages)) are made from the snapshots below.

* **The network** (`subnet`): the subnet to scan is the chosen interface's own IPv4 address and mask (the 0.3
  interface chooser, `scope.hpp`; `scan_range_of(NetState, iface)`). `ScanRange` is the first and last usable host of
  that range in host byte order. The range is **capped at a /24** (254 hosts): a wider mask (a /16, a /22) is narrowed
  to the /24 around the address; a /25 to /30 keeps its own hosts; **/31 and /32** have none and refuse. The scan is
  **refused with plain words** (`ScanRange::problem`, shown as a `Failed` state like the other tools) when the address
  is not private (RFC 1918: 10/8, 172.16/12, 192.168/16, or link-local 169.254/16) — a public address is never swept —
  or when the interface has **no IPv4 address** ("No network").
* **Discovery** (`devices`, `DevicesSession`, `run_devices`): the subnet is swept with unprivileged **ICMP echo** (the
  same datagram socket as ping, no root) and a few **non-blocking TCP connect probes** (ports 80, 443, 22, 445, 62078,
  8080), which makes the kernel **ARP-resolve** every live host; `/proc/net/arp` then gives the MAC of the hosts that
  are there (**complete entries only**, flag `0x2`; incomplete entries, other interfaces and the all-zero MAC are
  dropped — `arp_neighbours`). The ICMP round trip is recorded when a host answers a ping. Bounded: the probe sockets
  are capped (about 64 TCP in flight), every wait is a poll slice, and the whole run has a total budget (a /24 in about
  10 s on the Zero 2 W). Everything is bound to the chosen interface (`SO_BINDTODEVICE`). This device and the default
  gateway (`ip route` / `/proc/net/route` as the other tools) are marked. Devices **appear in the snapshot as they are
  found** (a live list). **Rescan** keeps the devices already known and refreshes them.
* **Names** (`lannames`), best first, per device: **mDNS** (a reverse PTR query to the host over UDP 5353 with the QU
  bit, the DNS wire format of `dns.hpp`), **NetBIOS** (an NBSTAT node-status request over UDP 137), then **reverse
  DNS** (the resolver, reusing `rdns.hpp`). `best_name()` picks the first non-empty, and a trailing `.local` / `.lan`
  and the like are dropped. All of it is bounded and cancellable and never blocks the discovery.
* **Vendor** (`oui`): the IEEE OUI registry ships with the app as `net-toolbox-oui.tsv`
  (`root/usr/share/APPLaunch/share/`, built by `tools/make_oui.py`); `lookup_vendor()` maps a MAC's first three bytes
  to a company. A MAC whose first byte has the **locally-administered bit** (`0x02`) set — the random addresses phones
  and watches use for privacy — is shown as a **private (random) address** with no vendor guess.
* **Port scan of one device** (`devices`, `PortScanSession`, `run_port_scan`): a TCP **connect** scan of one private
  address on the chosen interface, two modes: the **common ports** (a curated list with service names, `local_common_ports`)
  or **1-1024**. Non-blocking, bounded concurrency (~64), a per-connect timeout of about 700 ms for the LAN, progress
  counters (scanned / total / open) and the open list with its service name; cancellable. Only ever the one device you
  pick: **the engine itself** refuses (state Failed, *Only devices on your own network can be scanned.*) any target that
  is not a private or link-local IPv4 host inside the scanned subnet — a public address, 0.0.0.0, loopback, multicast,
  255.255.255.255, the subnet's own network and broadcast addresses, another subnet (`scan_target_problem`) — before
  any socket is made.
* **Where the probes go**: the `Prober` and `NameLookup` base classes hold the guard, so every implementation obeys
  it: a poke or a name question reaches only an address inside the scan range, never this device, and nothing at all
  before the range is set (fail closed). The mDNS and NetBIOS questions go unicast to that host only; no multicast is
  sent. The reverse-DNS question goes to your resolver, as in the other tools.
* **The snapshot API** (for the pages worker). `DevicesSnapshot`: `state` (Idle, Scanning, Done, Cancelled, Failed),
  `error`, `interface`, `subnet` (`192.168.68.0/24`), `gateway`, `range` (the `ScanRange` scanned, to hand to the port
  scan), the counts `total` / `probed` / `found`, a `naming` flag, and `vector<Device>` **sorted by IP numeric**, each `Device` with `ip`, `ip_order`, `mac`, `vendor`,
  `vendor_private`, `name`, `name_source` (mDNS / NetBIOS / reverse DNS), `has_rtt` + `rtt_ms`, `is_gateway`,
  `is_self`. `PortScanSnapshot`: `state`, `error`, `address`, `full_range`, the counts `total` / `scanned` /
  `open_count`, and `vector<OpenPort>{port, service, connect_ms}` sorted by port. The sessions follow the house
  pattern: `start(...)`, `stop()`, `running()`, `seq()`, `latest()`. `DevicesSession::start(net, iface, resolver,
  rescan)` picks the subnet, reads the MAC and gateway and runs the scan; a rescan keeps the known devices.
  `PortScanSession::start(address, full_range, range)` needs the `DevicesSnapshot`'s `range` (without one it refuses
  with *No network*) and refuses any address outside it. The sockets follow the chosen network through
  `set_active_interface` as every other tool. The probing is injectable (`Prober`, `NeighbourReader`, `NameLookup`) so the tests drive the engine
  on loopback fakes.
* **Saved results** (`netlog`): `format_devices` and `format_portscan`, same text style as the other tools. No new
  settings key is needed.

Limits of the 0.4 core: the discovery and the port scan were unit-tested on fixtures and on loopback fakes and loopback
listeners; the real ICMP / ARP sweep and the mDNS / NetBIOS answers were not yet run on a board (`tests/BOARD-TESTS.md`).
A host that answers neither ICMP, a TCP SYN nor ARP (fully firewalled, or off) is not seen. mDNS names end in `.local`
and are shown without that suffix; a device that changes its random MAC between scans shows as a new device.

## The 0.3 pages

Same frame as the others (status line, large cells or rows, Esc / Left back, a tap selects and a second tap acts), same
rule: nothing starts until you press Start, Wake now or Start on the radar, and leaving the page (Esc, Left, the header's
Esc) or the app stops the tool and its worker thread or child process first. The speed test and Wake-on-LAN pages say `(via
wlan1)` in the status line when a network other than Automatic is chosen; the radar does not use the network and says nothing.

| Page | What it shows | Actions |
|---|---|---|
| **Speed test** | the cells **Start / Stop** and **Save log**, then a note that is always there, before and after a run: `Uses up to about 150 MB each way. Start it yourself; on a phone hotspot that is mobile data.` (the number is the core's byte cap). Then **the gauge**: a 220 degree dial with ticks and six numbers, an arc in the accent blue filled up to the value, a tapered needle with a hub that eases to each new value in about 180 ms, the phase (Latency, Download, Upload) and the big value with its unit under the hub. The scale steps 10, 25, 50, 100, 250, 500, 1000 Mbit/s with the highest value seen and never comes back down during a run; the numbers cross-fade when it steps. Beside it (at 640x480, 800x480, 1024x600, 1280x720) or under it (720x720) the **throughput chart** of the phase in progress on the same scale, and a thin **progress bar** with `4 s of 8 s`. When the run ends the gauge gives way to the **result card**: Download and Upload as large values with the unit, Latency (idle) and Jitter, Latency while downloading and while uploading with the difference (`138 ms (+120 ms)`), the **bufferbloat grade** as a badge (a disc for A and B, a triangle for C, a square for D and F, with the letter) and one plain line (`A: latency stays flat under load` ... `F: latency explodes under load: heavy buffering in the router`), the server (`Cloudflare, CDG`), and the notes (no ping target, a direction that broke). Errors in plain words (no connection, the TLS connection failed, Cloudflare refused the request). Stopping ends the test within about a second and keeps what was measured | Start / Stop, Save log |
| **Wake-on-LAN** | a list: **Add a machine**, then each saved machine (name, MAC and the address to watch, and its last wake in this run: `Sent 14:02`, `Awake after 22 s`, `No answer yet`, `Not sent`, `Not woken yet`), and the note `The target must have Wake-on-LAN switched on; it usually works over Ethernet, rarely over Wi-Fi.` At most 20 machines (the row says `list full`) | Add a machine (typed pages: Name, MAC address, Watch address, the address optional), a machine opens its page |
| **A machine** | the **wake watch**: a ring with a shape and a word (hollow ring `Not sent yet`, arrow disc `Sent`, spinning arc `Waking... 37 s`, green check `Awake after 22 s`, amber dash `No answer yet`, red cross `Not sent`), the line under it (`Packet sent on wlan0, wlan1`), and a strip of 120 one-second cells (grey while waiting, green from the first answer, dark for the seconds to come; only when the machine has an address to watch) | Wake now / Stop (sends through the chosen network: every interface that is up, or the one chosen; then pings the address once a second for up to 120 s, cancellable), Name, MAC address, Watch address (each opens its typed page: a bad MAC or a name already saved is refused in plain words under the field; `aa:bb:cc:dd:ee:ff`, `aa-bb-...` and `aabbcc...` are accepted), Watch after waking (On / Off), Save log, **Delete** (the row asks `Press again to delete`; any other action disarms it) |
| **Bluetooth radar** | the status line (`Scanning, 14 devices, 37 s left`, `Stopped, 23 devices seen`, `Finished, 29 devices seen`, or the reason: `Bluetooth is switched off`, `Bluetooth is blocked by rfkill`, `No Bluetooth adapter`, `Bluetooth is not installed`, with the one-line fix such as `Turn Bluetooth on in Settings > Bluetooth`), the cells Start / Stop and Save log, the permanent line `Wi-Fi and Bluetooth share one radio: a Bluetooth keyboard may stutter while scanning.`, then **the radar** and the list of devices side by side (stacked on a tall screen like 720x720). The radar: three rings (near, medium, far: signal strength, not distance), a crosshair, a sweep line that turns once in 3 s with a faint trailing wedge (only while scanning; stopped, the radar is still), the devices as blips (the angle is a stable hash of the address, so a device keeps its place; the ring comes from the smoothed signal; the blip pulses when the sweep passes it and fades with the time since it was heard, and is gone after 20 s), the highlighted device ringed and named. The list: name or address, smoothed dBm and the class word (`-54 dBm, near`), strongest first, the highlight following the list | Start / Stop (scans at most 60 s, stops by itself), Save log, Enter or a second tap on a device opens its **details** in place (name, address and type, vendor, signal raw and smoothed, TX power, last seen, times heard); Esc or Enter closes them, the next Esc leaves the page |

What was chosen at 640x480 (the deck): the speed test puts the dial (radius about 150 px) beside the chart, the Start
cell and the note above them without scrolling; the radar is a 250 px circle with the list (four rows) at its right. At
720x720 the dial sits over a wide chart and the radar (300 px) over the list. The BLE list is a `RichList` (the same look
and keys as CompactList's compact density, with rows of two lines and a way up to the cells), the highlight is polled every
100 ms.

What the speed test sends, to whom and how much is in the table above (Cloudflare's `speed.cloudflare.com`, one HTTPS
connection at a time, at most 150 MB each way: on a 25 Mbit/s line about 25 MB each way, and on a phone hotspot that is
mobile data). The latency under load pings 1.1.1.1 (`speed_ping` in the settings: a host, `gateway` or `off`).

## The 0.4 pages

Same frame as the others (a status line, large cells or rows, Esc / Left back, a tap selects and a second tap acts), and the
same rule: leaving a page (Esc, Left, the header's Esc) or the app stops its scan and its worker thread first. The pages say
`(via wlan1)` in the status line of Local devices when a network other than Automatic is chosen; the Wi-Fi pages name the
interface they read (`on wlan0`). Nothing starts until you press **Scan** (Local devices), **Start** (the walk test) or one of the
port scan's rows; the Wi-Fi list starts when its page opens (it only asks NetworkManager for its own scan). The Local devices
scan and the port scan bind their sockets to the chosen interface (`scope.effective()`), as every other tool does.

| Page | What it shows | Actions |
|---|---|---|
| **Local devices** | the status line (`Press Scan to look for devices on your network`; `Scanning 192.168.68.0/24: 112 of 254, 9 found`; `Done: 14 devices on 192.168.68.0/24`; `Stopped: ...`; the core's refusal in plain words: `No network` with `Connect to a network first, or choose another one under Network.`), the cells **Scan / Rescan / Stop** and **Save log**, the hint `Scans only your own network. Use it on networks you own or are allowed to test.` before the first scan, then **a row per device, found as it is found and sorted by address**: line 1 the name (the address when there is none) with the address at the right, line 2 the vendor (`Private (random) address` for a random MAC, `Vendor unknown`), the MAC and the round trip (`2 ms`); the router carries a diamond and the word `router`, this device a disc and `this device` (a shape and a word). The highlight follows the device, not the row, while the list grows | Scan / Rescan (keeps the devices already known) / Stop, Save log, Enter or a tap on a device |
| **A device** | IP address, Name (and where it came from: mDNS, NetBIOS, reverse DNS), MAC address, Vendor (`Private (random) address: the vendor cannot be known`), Round trip, Role (`Router: the gateway of this network`, `This device`, `Another device`); the status line `192.168.68.1, router`; under the actions the **open ports as the scan finds them**: `Port 22  ssh  open` (open ports only), with the status `Scanning 24 of 81: 2 open`, `Done: 4 open ports among 1024`, `No open port found among the common ones`, or `Only devices on your own network can be scanned.` | **Ping this device** (opens Ping with the address as its target and starts it), **Check a port** (opens Port check with the address as Host, not started), **Scan common ports** (81 usual LAN services), **Scan ports 1-1024**, **Stop the port scan** while it runs (Esc stops it too), **Save for Wake-on-LAN** (only for a device with a MAC: adds a saved machine with the name, the MAC and the address to watch; a name already saved gets ` 2`, a MAC already saved says `Already saved as <name>`, twenty machines say `You can save up to 20 machines: delete one first.`), Save log (the devices and the port scan) |
| **Wi-Fi networks** | the status line (`11 networks on wlan0, scanned 3 s ago`; `Scanning wlan0`; `Wi-Fi is switched off`; `No Wi-Fi adapter on end0: choose wlan0 under Network`; `nmcli is missing: the scan needs NetworkManager` with the package to install), the cells **View** (Networks or Channels), **Rescan** and **Save log**. **Networks view**: a row per network sorted by signal: the name (`(hidden)` for a hidden one), the signal bars (1 to 4 bars for Weak, Fair, Good, Excellent) and `-48 dBm`, a check and `connected` on the one in use, and `ch 36, 5 GHz, WPA2` under it (amber for Open and WEP). **Channels view**: the sentence `Least crowded: channel 11 (2.4 GHz), channel 44 (5 GHz)`, the bars of the 2.4 GHz channels 1 to 13 (the height counts the neighbouring channels too, the number over a bar the networks on that channel) and of the 5 GHz channels seen plus the usual ones, the channel numbers under the bars, the busiest named (`Busiest: channel 6, 2 networks`); the two bands side by side on a wide screen (800 px and over) or one over the other; a board with no 5 GHz says so (`No 5 GHz network seen: this adapter may be 2.4 GHz only`); one small note says channels 149 to 165 are not allowed in every country | View, Rescan (a fresh scan now), Save log, Enter or a tap on a network |
| **A network** | followed by its BSSID across the scans: Status (`Connected`, `In range`, `Gone: not in the last scan`, the last values stay), Network name, BSSID, Vendor (from the OUI table; a virtual or random BSSID says it has none), Signal (`-48 dBm, Excellent`), Channel, Frequency, Band, Bit rate, Security with a one-line meaning under it (`Open: anyone near can read your traffic.`, `WPA2: encrypted, a password is needed.`) | Scan now, Save log |
| **Wi-Fi walk test** | the status line (`Press Start, then walk around`, `Walking: 01:23, weakest -78 dBm`, `... in a weak spot ...`, `Stopped: 01:23, weakest -78 dBm`), the cells **Start / Stop**, **Reset**, **Save log**, the caption `Walk slowly and keep the deck in your hand; stop at the dead spot to read the numbers.`, then **the live block**: the big reading (`-64 dBm`) with its word (Excellent, Good, Fair, Weak) and a shape (check, exclamation mark, cross), the line `Below -75 dBm: the weak zone` when it is, the network and the interface, the elapsed time; and **the chart of the last 2.5 minutes** (time on x, dBm on y; the weak threshold as a dashed amber line; the readings under it drawn amber and thicker **and** marked with small squares; a red tick at the bottom for a second without link; a tick with a diamond at the top for each access point change) with its legend. The row **Weak below -75 dBm**. When it is stopped the chart shows the whole walk and a **summary card** follows: Duration, Readings, Weakest (and when), Average, Strongest (and when), Time in the weak zone (`12 s (14 %)`), Dips under the line, Without link, Access point changes with the time and the new BSSID of each (`not known: iw is missing` without iw) | Start / Stop, Reset (forgets the walk), Save log, Weak below (Enter cycles -70, -75, -80 dBm; kept in the settings, applied at the next Start) |

What was chosen at 640x480 (the deck): the Local devices rows are two lines of the list's text size (four devices in view),
the tag sits on the second line left of the round trip so that a long name keeps its room; the Channels bars are drawn with
the count of networks over each bar and the channel numbers under it (every second number when the 5 GHz bars are narrow, the
least crowded one always); the walk page puts the reading at the left and the chart at the right (the reading over the chart on
a tall screen such as 720x720), with the caption, the legend and the `Weak below` row all in view. The long lists
are rows of the same `RichList` as the 0.3 pages (a tap selects, a second tap acts; the keys move the highlight; every action
is a row or a cell, so touch alone does it all).

Wi-Fi vendors and device vendors come from `net-toolbox-oui.tsv`, read once on first use (about 1 MB in memory), the same table
for both. Saved texts: `devices-...txt` (the devices, and on a device's page the port scan too), `wifi-...txt`,
`wifi-network-...txt`, `walktest-...txt`. The walk's threshold is the only value of these pages kept in the settings
(`walk_weak_dbm`); `wifi_rescan_ms` sets the list's interval.

## The network chooser (0.3)

A board can have several ways out: its Wi-Fi, a second Wi-Fi adapter (a USB dongle on a phone hotspot), an Ethernet
adapter on a hub. Before 0.3 every tool followed the system's default route. Home's first row, **Network**, picks the
interface the tools use; the choice page lists **Automatic (wlan0)** (the system's route, as before; the brackets
name the main interface now) and every interface but the loopback as `name  address`, or `name  not connected` for a
Wi-Fi adapter and `name  no link` for a cable that is out (listed and selectable, not hidden); the one in use is
checked. The choice is the setting `interface=` (absent = Automatic; a file of an older version reads as Automatic).

What the choice does, `scope.hpp` (`NetScope`, resolved against the interfaces every time they are read):

* **Chosen interface found**: the tools go out of it. **Not found** (adapter unplugged): the scope falls back to
  Automatic, Home says `wlan1 not found: using Automatic (wlan0: ...)` in amber, the setting is kept, and the adapter
  plugged in again is used again. Never an error.
* **Sockets**: `SO_BINDTODEVICE` with the interface name, one helper in `sock.hpp` (`bind_to_device`), no root on a
  kernel 5.7 or later (the boards run 6.18; on a Pi 3A+ an ICMP datagram socket and a TCP connect bound to `wlan0` go
  out of `wlan0`, and a name that does not exist fails with `ENODEV`, shown as "network interface not found"). It
  covers the ICMP echo (ping, the speed test's latency, the wake watch), the traceroute probes and their reverse-name
  socket, DNS over UDP and over TCP, the TCP port check, whois and the captive-portal HTTP of the diagnosis. The choice
  is process-wide state (`set_active_interface`), set by the UI, so no tool needs a parameter.
* **curl**: `--interface <name>` is added in the one place that starts a command (`spawn_child`, and the UI's flow
  driver), to `curl` only, with the name checked (`[A-Za-z0-9_.:-]`, 1 to 15 bytes, no leading `-`, not an address); the
  argv stays an array. It covers the HTTP check, the public address and the speed test.
* **DNS**: the system's resolver may not be reachable over the chosen interface (resolv.conf holds the servers of the
  main network). With a chosen interface the "System default" resolver is that interface's own: first the servers
  NetworkManager knows (`nmcli -g IP4.DNS,IP6.DNS device show <if>`, through the command runner, read again after 20 s),
  else the interface's gateway, else (nothing known) the system's list. The resolver label says which: `wlan1's DNS
  172.20.10.1`, `wlan1's gateway 172.20.10.1`, `System default (not wlan1's) 192.168.1.1`. A resolver you type or pick
  (Cloudflare ...) is used as it is, over the chosen interface.
* **Gateway**: the default route through that interface (`/proc/net/route` has one per interface; the metric picks the
  system's), used by the diagnosis and by the "Gateway" targets of ping, traceroute and the rest.
* **Diagnosis**: the six steps check that interface: Link up (a Wi-Fi adapter that is not connected, a cable that is out,
  an adapter that is gone), its address, its gateway, DNS through its servers, the internet and the portal through
  bound sockets; the explanations name it ("wlan1 is not connected ...", "The gateway of wlan1 (172.20.10.1) does not
  answer ..."). Automatic is the 0.2 behaviour, word for word.
* **Wake-on-LAN** is per interface by nature: each magic packet is planned for one interface and now also bound to it
  (`SO_BINDTODEVICE`; an older kernel's refusal is ignored, the source address still steers it).
* **Pages**: the status line of every tool that sends traffic ends with `(via wlan1)` when a network is chosen (the
  words of the page give way to it on a narrow screen, cut at a space with `...`); the Interfaces page marks the heading
  `WLAN1 (IN USE)` and the main one `(DEFAULT ROUTE)` (only when there is more than one interface); the Wi-Fi
  details (Network, Signal, Bit rate) come for **every** connected wireless interface, asked from `iw` one at a time
  (all of them when the page opens, then one per 2 s refresh in turn, 1.5 s timeout, never two at once); a Wi-Fi
  interface that is not connected says so and has none of those rows.
* **The next pages** (speed gauge, Wake-on-LAN, BLE radar) ask `ToolboxApp::scope()` (`iface`, `effective()`,
  `note()`) and `dns_servers()`; the sockets and curl follow by themselves.

Limits: `openssl s_client` (the certificate read when curl refuses a certificate) has no interface option and is not
bound; curl's own name lookup uses the system resolver (the connection itself goes out of the chosen interface); the
nmcli answer needs NetworkManager (the boards have it); an Ethernet adapter's name is any (`eth0`, `end0`, `enx<mac>`)
and the long ones are cut with dots in the choice list on the smallest screen; Bluetooth is not an interface and is
not affected; a fixture board (`NET_TOOLBOX_FIXTURE`) never binds anything.

## Wi-Fi networks and walk test (0.4): the cores

The core (`src/core/`, no LVGL) of two tools that 0.4 brings in: **Wi-Fi networks** (the networks around, their
channels and the least crowded one; a port of [Wi-Fi Survey](../wifi-survey/README.md)'s scanner, MIT, our own code) and
the **Wi-Fi walk test** (the signal against time while you walk with the board). Their pages are described in
[The 0.4 pages](#the-04-pages). Same rules as the other tools: one worker thread per running tool, every wait a slice of at most 50 ms, immutable snapshots
the screen polls (`latest()`, `seq()`), commands through the shared command runner (`cmdrun`: no shell, its own process
group, `LC_ALL=C`, a timeout, ended by Stop), no root.

**What they read and send.** Neither connects to anything or sends traffic of its own:

* *Wi-Fi networks* asks NetworkManager for its scan results:
  `nmcli -t -e yes -f IN-USE,BSSID,SSID,MODE,CHAN,FREQ,RATE,SIGNAL,SECURITY device wifi list ifname <if> --rescan <m>`.
  The scan itself is NetworkManager's: the adapter sends probe requests on each channel, as any phone's scan does. The
  first list of a run asks `--rescan yes` (a fresh scan, which nmcli waits for), the next ones every 5 s (setting
  `wifi_rescan_ms`, 2 to 60 s) ask `--rescan auto` (nmcli scans again only when NetworkManager's list is older than about
  30 s, else reads it at once: 63 ms on the Pi 3A+); a manual rescan asks `yes` again. When NetworkManager refuses a
  fresh scan (one ran a moment ago, the adapter is busy), its last results are read with `--rescan no` and the snapshot
  says so (`cached`). An empty list is checked with `nmcli -t -f WIFI-HW,WIFI radio` (Wi-Fi switched off).
* *The walk test* reads `/proc/net/wireless` once a second (any user, no command; a fresh value at each read: -62, -64,
  -65, -65, -66 dBm on the Pi 3A+ standing still) and runs `/usr/sbin/iw dev <if> link` each second when `iw` is
  installed (1.5 s timeout): its BSSID shows when the board moves to another access point, and its `signal:` line is the
  level when the proc file lacks the interface.

**Wi-Fi networks** (`wifiscan.hpp`): per network the SSID (`(hidden)` for a hidden one; bytes that are not UTF-8 and
control characters shown as `?`), the BSSID (and whether it is locally administered: a virtual or random address, never
looked up), the signal in % and an estimate in dBm, the channel, the band (2.4, 5 or 6 GHz, from the frequency), the
frequency, the bit rate, the security in plain words (Open, Enhanced Open, WEP, WPA, WPA2, WPA3, Enterprise; transition
modes as `WPA/WPA2`, `WPA2/WPA3`; Open and WEP are flagged weak), ad-hoc, and the network the interface is connected to
(`in_use`). Sorted strongest first (equal signals: in use, then by name, hidden last). The vendor comes from an injected
lookup (`VendorLookup`, the pages wire the OUI table of Local devices), asked once per OUI and run. The dBm estimate
inverts NetworkManager's own conversion (`pct = 100 - 100 x |dBm + 40| / 60`, -40 dBm and stronger is 100 %): dBm = -40 -
0.6 x (100 - pct). On the Pi 3A+ nmcli said 57 % for the network `iw` read at -65 dBm: -66 by this formula (Wi-Fi Survey's
`pct / 2 - 100` gave -72, now corrected). **Channels summary**: networks per channel on each band and the least crowded
channel, Wi-Fi Survey's rule kept: on 2.4 GHz a network weighs 5 on its channel and 4, 3, 2, 1 on the channels 1 to 4
away (they overlap); the lowest weight among 1 to 13 wins, ties going to 1, 6 or 11, then the lowest. On 5 GHz the
channels seen plus 36, 40, 44, 48, 149, 153, 157, 161, 165, on 6 GHz the channels seen plus the preferred scanning
channels (5, 21, ... 229): the fewest networks wins (ties: the lowest). A 5 or 6 GHz band where nothing was seen gets no
recommendation (the adapter may lack it: the Pi Zero 2 W is 2.4 GHz only). **The detail** (`WifiFollower`) follows one
network by its BSSID: each new list refreshes it wherever it moved in the list; a list without it marks it *gone* with
its last values; a failed scan changes nothing. **The interface**: the network chooser's (`interface=`;
`wifi_scan_iface()`: for Automatic the main interface when it is Wi-Fi, else the first Wi-Fi one). An interface that is
not a Wi-Fi adapter, or is not there, is a failure in plain words, `No Wi-Fi adapter on wlan1` (the run tries again every
interval: an adapter plugged in again is used again). Other failures in words: nmcli missing (package `network-manager`),
NetworkManager not running, Wi-Fi switched off, no answer in time (the last list stays, with the reason).

**Walk test** (`walktest.hpp`): Start begins a walk, Stop ends it and keeps everything, Reset forgets it. A sample a second
on a fixed schedule (a late one does not make the next ones bunch up). The chart keeps the last 10 minutes (a ring of 600
samples); the summary covers the whole walk: duration, samples, weakest dBm and when (seconds from the start), average
(mean of the dBm readings), strongest and when, the time in the **weak zone** (below -75 dBm, setting `walk_weak_dbm`, -95
to -40; a sample without link counts as weak and is also counted apart) in seconds and in %, the **dips** (a dip starts at
the first weak sample and ends once the signal has been back at or above the threshold for 2 s, so a signal hovering at
the threshold makes one dip, not ten; each with its start, end and lowest level, at most 64 listed, all counted), and the
**access point changes** (roaming: when and from which BSSID to which; unknown without `iw`).

**The API** (for the pages):

| Tool | Session | Snapshot | Pure helpers |
|---|---|---|---|
| Wi-Fi networks | `WifiScanSession`: `start(WifiScanConfig{iface, interval_ms, vendor})`, `stop()` (the list stays), `rescan()`, `latest()`, `seq()`, `running()` | `WifiSnapshot{state (Idle, Scanning, Ready, Failed, Stopped), iface, error, busy, cached, rescan, networks, channels {g24, g5, g6: networks, channels, recommended}, lists, list_at_ms, interval_ms}` | `wifi_scan_iface`, `parse_nmcli_wifi_list`, `summarize_channels`, `WifiFollower`, `band_words`, `security_weak` |
| Walk test | `WalkSession`: `start(WalkConfig{iface, weak_dbm, interval_ms, hysteresis_ms})`, `stop()`, `reset()`, `latest()`, `seq()`, `running()` | `WalkSnapshot{state (Idle, Running, Stopped, Failed), iface, error, weak_dbm, samples (t_ms, linked, dbm, weak), dropped, summary, dips, roams, in_dip, linked, dbm, bssid, ssid, source, iw_missing, elapsed_s}` | `WalkRecorder`, `proc_wireless_dbm`, `read_walk_signal` |

Saved results (`netlog_wifi`): `format_wifi_networks`, `format_wifi_network` (the detail, gone or not), `format_walktest`
(the settings, the summary, the dips, the access point changes and every sample kept), same style as the others.
**Settings**: `wifi_rescan_ms` (5000), `walk_weak_dbm` (-75); a 0.3 file reads with their defaults. From 0.4 `depends`
also has `network-manager` (its `nmcli`, GPL-2.0-or-later, runs as a separate program, not shipped or modified;
installed on Raspberry Pi OS). Tests: `tests/test_wifiscan.cpp`, `tests/test_walktest.cpp`
on nmcli, iw and `/proc/net/wireless` output recorded on the Pi 3A+ (`tests/fixtures/wifi`, sanitized: see its README),
made-up lines for the rare cases, a fake clock and a fake runner, and a fake nmcli program run by the real runner.

Limits of the 0.4 cores: the dBm of a network is NetworkManager's percentage turned back into dBm (whole dB, -40 dBm and
stronger all read as 100 %), and its value is as old as NetworkManager's last scan (up to about 30 s with `auto`); the
walk test's level is the link's (the access point it is connected to), not the other networks'; nmcli does not give a
network's channel width, so a 40 or 80 MHz network counts on its main channel only; the 5 GHz candidates include 149 to
165, which some countries do not allow for Wi-Fi; a hidden network appears without a name; the cores ran on the build PC
(fixtures, fakes) and the commands were run by hand on the Pi 3A+, but the sessions have not run on a board yet.

## Requirements

* A screen of at least **640x480** (`X-MinSize=640x480`; under it the app says so and closes on any key) and a
  CyberdeckZero Launcher with full-screen app support (contract 1).
* Ping needs unprivileged ICMP sockets (`net.ipv4.ping_group_range` covering the user's group; the default on
  Raspberry Pi OS). Without them ping says so with the fix, and the diagnosis uses TCP connections instead.
* `iw` (package `iw`) for the Wi-Fi network name and bit rate; optional.
* `depends` = the template's `libfreetype6, libinput10, libxkbcommon0` and, from 0.2, `curl, ca-certificates` (the
  HTTP check and the public address); no root. The `openssl` program (installed on both boards) reads the dates of a
  certificate curl refused; without it that certificate shows no dates.
* From 0.3: the speed test uses `curl` too (already in `depends`); the BLE scan needs BlueZ's `bluetoothctl` (package
  `bluez`, installed on Raspberry Pi OS) and a running `bluetoothd`; it stays optional (not in `depends`): without it
  the scan says so. Wake-on-LAN needs nothing more (its wake watch uses the ping socket, as ping).
* Network: the tests talk to the hosts you name; the diagnosis contacts your gateway, your DNS servers, 1.1.1.1
  (Cloudflare), 8.8.8.8 (Google) and `connectivitycheck.gstatic.com` (Google), only when you start it. From 0.2: the
  public address asks `www.cloudflare.com` (Cloudflare), whois asks `whois.iana.org` and the servers it names, only
  when you start them. From 0.3: the speed test talks to `speed.cloudflare.com` (Cloudflare) and pings 1.1.1.1 or
  your gateway, only when you start it.

## Install

From **Settings > Apps**. By hand:

```sh
python3 tools/make_registry.py --only net-toolbox --out <a folder outside packages/>
sudo dpkg -i net-toolbox_0.1.0_arm64.deb      # remove: sudo dpkg -r net-toolbox
```

## Files the app keeps

| Path | What |
|---|---|
| `${XDG_CONFIG_HOME:-~/.config}/cyberdeckzero/net-toolbox.conf` | saved ping targets, the saved Wake-on-LAN machines (0.3, at most 20) and the last values of each tool (`key=value`); written through a temporary file and a rename. Delete it to reset. `NET_TOOLBOX_CONFIG` names another file |
| `~/net-logs/<tool>-<YYYYmmdd-HHMMSS>.txt` | a result you saved (only when you ask). `NET_TOOLBOX_LOG_DIR` names another folder |

Logs go to stderr with the tag `[net-toolbox]` (the launcher's journal).

## Known limits

* **Not yet run on a board**: the tools are unit-tested on fixtures and on loopback and the pages on headless scenes;
  the board protocol (`tests/BOARD-TESTS.md`) is still to be run.
* 0.4 (draft): the Local devices, Wi-Fi networks and Wi-Fi walk test pages have only run in the headless scenes; the board steps
  B16 to B19 are still to be run. A host that answers neither ICMP, a TCP SYN nor ARP is not listed; the Wi-Fi list is
  NetworkManager's (its signal is a percentage turned back into dBm, up to about 30 s old); the walk test needs a Wi-Fi link
  and, for the access point changes, `iw`. Leaving a device's page stops its port scan; leaving Local devices or Wi-Fi networks
  stops their scans (the results are not kept).
* 0.3: the network chooser is unit-tested and shown in headless scenes; the dongle (wlan1) and an Ethernet
  adapter on a hub are still to be tried on a board (`tests/BOARD-TESTS.md`, B11 and B12).
* 0.3: the speed test and the Bluetooth radar pages ran on the deck and the Pi 3A+ (speed test about 19 to 115 Mbit/s
  down on the two boards' Wi-Fi, grade B; the radar heard 17 to 23 devices); the Wake-on-LAN page ran in the headless scenes
  only (B14 is still to be run). The last wake status of a machine is kept while the app runs, not in the settings file. A
  BLE scan is never started on the deck during tests without the owner's say (the Bluetooth keyboard shares the radio).
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
Google's 8.8.8.8. From 0.2 the app also runs `curl` (curl licence, MIT-style) and, when installed, `openssl`
(Apache 2.0) as separate programs, not shipped or modified; the traceroute, the whois client (RFC 3912) and the JSON
reader are the app's own code; the public address comes from Cloudflare's `/cdn-cgi/trace` page. From 0.3 the speed
test uses Cloudflare's speed endpoints (`speed.cloudflare.com/__down` and `/__up`, the ones behind
speed.cloudflare.com), the BLE scan runs BlueZ's `bluetoothctl` (GPL-2.0) as a separate program, not shipped or
modified; the Wake-on-LAN sender and the speed and Bluetooth readers are the app's own code. The company names of the
BLE scan come from the Bluetooth SIG's assigned numbers (company identifiers); the bufferbloat grade steps follow
Waveform's bufferbloat test. From 0.4 the Local devices tool ships `net-toolbox-oui.tsv`, a shortened, abridged
derivative of the IEEE Registration Authority's public OUI listing (<https://standards-oui.ieee.org/>; the company
names are shortened for the small screen, no formal licence is given with the listing, no endorsement by the IEEE is
implied); see `root/usr/share/doc/net-toolbox/copyright`. The ICMP / TCP discovery, the mDNS and NetBIOS name queries
and the port scanner are the app's own code.

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
  and their text), `settings`, `text`. From 0.2: `traceroute` (the probe socket, the rounds, the hop statistics, the
  session), `rdns` (reverse names without blocking), `cmdrun` (the command runner interface, the direct runner, the
  step flows), `httpcheck` (the address rules, the curl and openssl command lines and parsers, the flow, the session),
  `minijson` (curl's report), `publicip`, `whois`. From 0.3: `cmdrun` also has the stream runner, `speedtest` (the
  command lines, curl's report, the meter, the grades, the engine with the ping beside it, the session), `wol` (MAC
  addresses, the packet, the plan, the sender, the wake watch, the session), `ble` (bluetoothctl's lines, `show`,
  `/sys`, the device table, the scan, the session). From 0.4: `subnet` (the scan range with the /24 cap and the
  private checks, the `/proc/net/arp` reader), `oui` (the IEEE OUI table parse and the vendor lookup with the
  random-MAC rule), `lannames` (the mDNS and NetBIOS packets, the reverse-name priority, the non-blocking resolver),
  `devices` (the discovery engine, the real prober / ARP reader / name lookup, the sessions) and `devscan` (the
  one-device TCP connect port scan).
* `src/ui/` from 0.4: `tool_pages_04` (the five pages), `view_model_04` (their words, tones and layouts), `widgets_04` (the device
  row, the network row, the channel bars, the walk's reading and chart), `demo_data_04` (the scripted answers), `oui_vendor` (the
  OUI lookup of the Wi-Fi pages, apart because `oui.hpp` and `wifiscan.hpp` both name a `VendorLookup`).
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
* `tests/` from 0.2 (about 1100 checks in all): the traceroute engine on a fake hop network injected at the socket
  layer (latency per hop, loss, a silent hop, a hop answering from two addresses, rate limiting at one hop, real loss
  further on, the destination at hop N, a destination that never answers, a router saying *unreachable*, the ICMP to
  UDP fallback, a long path in one round, cancel, the statistics), the reverse names through the fake DNS server, and a
  real UDP trace of 127.0.0.1 and ::1 (ICMP too when the host allows it); curl's reports recorded on the deck
  (`tests/fixtures/curl`: a healthy HTTPS page, HEAD, a redirect followed and not followed, too many redirects, an
  expired, a self-signed, an untrusted-root and a wrong-name certificate, `-k` on an expired one, a DNS failure, a
  refused connection, a connect timeout, a TLS reset) and `openssl s_client`'s (`tests/fixtures/openssl`), the days
  left on an injected clock (expiring soon, expired, not yet valid), the address rules against about 35 hostile
  inputs, the flow on a fake runner and on fake `curl` / `openssl` programs run by the real runner, the runner itself
  (timeout, cancel, the process group ended, the output cap); the public address on Cloudflare's recorded trace (its
  address replaced by 203.0.113.7) and the address classes; whois answers recorded from IANA, Verisign, MarkMonitor,
  ARIN, RIPE NCC and APNIC (`tests/fixtures/whois`), and the client against fake whois servers on 127.0.0.2 and up
  (referral chains, ARIN's `n +`, the parent-domain retry, a referral loop, the referral limit, an oversized answer,
  garbage bytes, a silent server, a refused connection, cancel); the 0.2 log texts and settings.
* `tests/` from 0.3 (about 1900 checks in all; `stream_fakes.*` holds a scripted stream runner on a fake clock, a
  scripted command runner and a scripted ping): `test_stream.cpp` the stream runner on real `sh` commands (the body
  counted and never kept, zero bytes on stdin whole or ended early, text in and out, the farewell on a cancel, the
  group ended on a stop, SIGKILL after the grace, the timeout, a missing program, no SIGPIPE, long lines);
  `test_speedtest.cpp` curl's report lines recorded on the deck (`tests/fixtures/speedtest`), Server-Timing, the
  command lines (the cap never exceeded, no request the server refuses), the meter (Mbit/s, the ramp left out, short
  phases), the grade table, the error words, the whole test on the scripted runner (phases, live figures, the cap, the
  time target, an unconfirmed upload, DNS / 429 / TLS / no curl / 403 / a broken connection, no ping, every ping lost,
  cancel), the real ping module beside a scripted load (a fake echo socket that answers slower under load), the session
  on fake `curl` programs run by the real runner (the uploaded zero bytes counted by the fake server, Stop ends curl's
  process group); `test_wol.cpp` the MAC parser (about 25 hostile inputs), the packet's bytes, directed broadcasts, the
  packets planned on the recorded boards, the saved form, a fake sender and a real UDP send on loopback, the wake watch
  on a fake echo socket (answers, at once, never, cancel, ping not allowed, a name that cannot be found);
  `test_ble.cpp` bluetoothctl's output recorded on the Pi 3A+ (`tests/fixtures/ble`, sanitized: see its README), the
  line cleaner and parser (every RSSI line of the recording), hostile lines, `show` and `/sys` in plain words, the
  device table on a fake clock (smoothing, expiry, seen, names, the `info` answer, companies, distance, the 256 bound),
  the scan replayed with discovery turned off on every exit path, and a fake bluetoothctl program run by the real
  runner (the time up, Stop, the session destroyed); `test_netlog03.cpp` the 0.3 texts and settings.

* `tests/` from 0.4 (5493 checks in all): `test_ui_model_04.cpp` the pages' words and numbers (the device rows and tags, the
  status lines, the port scan words, the machine saved for Wake-on-LAN, the network rows, the problems and their fixes, the
  detail rows, the channels view and layouts, the walk's reading, window, scale, summary and weak-zone cycle) on the scripted
  snapshots of `demo_data_04.cpp`; the headless scenes (193 in all, each at five sizes) add `devices-*` (idle, scanning, done,
  stopped, empty, no network, refused, a chosen network, a device's page, its port scans running, done, empty and refused,
  Save for Wake-on-LAN, Ping and Check a port from it, touch, Esc), `wifi-*` (the list, a long list, empty, off, no
  NetworkManager, no Wi-Fi adapter, a network's page, an open network, a random BSSID, a network that disappeared, Rescan,
  Channels, a 2.4 GHz-only board, touch) and `walk-*` (idle, walking, the summary, a long walk, no link, no iw, no adapter, the
  weak-zone setting, Reset, touch).

## Changes

* 0.4.1: rows show what can be tapped, by the launcher's row rule: a chevron opens a page or changes a value, read-outs are
  grey information (a status colour is kept), and a row that does something right there (Run, Stop, Save log, Scan) carries
  a dot that is white when idle and blue while it runs; the values line up in one column. Net Toolbox's own two-line rows
  (devices, Wi-Fi networks, Bluetooth devices, saved machines) draw the same markers. Rebuilt on launcher 9912361.
* 0.4.0 (draft): the **pages** of Local devices (the list that grows as devices are found, two-line rows with the router and
  this-device tags, the Device page with ping, port check, a live TCP port scan and Save for Wake-on-LAN), Wi-Fi networks (the
  list with signal bars, the Channels view with the least crowded channel, a page per network followed by its BSSID) and the
  Wi-Fi walk test (the big reading, the 2.5-minute chart with the weak-zone line, the summary card, the weak-zone setting);
  Home has sixteen rows; the OUI table feeds both vendor lookups; the scripted demonstration backend (`devices=`, `ports=`,
  `wifi=`, `walk=`), the unit tests of the new wording (`test_ui_model_04.cpp`) and the headless scenes. The app now replaces
  LAN Scan and Wi-Fi Survey, which remain installable for now. Only headless scenes so far: the board steps B16 to B19 are
  still to be run.
* 0.4.0 (draft): the core of **Local devices**, a LAN scanner folded in from LAN Scan. Discovery
  without root (ICMP echo and TCP connect probes to prime ARP, `/proc/net/arp` for the MAC, the chosen interface's
  own private /24 only), names best-first (mDNS, NetBIOS, reverse DNS), OUI vendor lookup with the shipped
  `net-toolbox-oui.tsv` (a locally-administered MAC shown as a private address), this device and the gateway marked, a
  live list and a rescan that keeps the known devices; a TCP connect port scan of one device (common ports or 1-1024,
  bounded concurrency, progress); the `DevicesSnapshot` / `PortScanSnapshot` API for the pages, saved texts through
  `netlog` and the unit tests (subnet maths, ARP parser, OUI, mDNS / NetBIOS packets, the scan state machine on
  loopback fakes, the port scan on loopback listeners).
* 0.3.0: the **network chooser** (Home row Network, `interface=` setting): sockets bound with SO_BINDTODEVICE, curl
  with --interface, the interface's own gateway and DNS, the diagnosis of a chosen interface, `(via wlan1)` in the status
  lines, Wi-Fi details for every wireless interface on the Interfaces page, Ethernet as a normal interface. Home has ten
  rows and starts on Diagnose my connection.
* 0.3.0: the **pages** of the speed test (needle gauge with a scale that steps up, live throughput chart, progress
  bar, result card with the bufferbloat grade), Wake-on-LAN (the list of saved machines, the typed Add / Edit pages, the
  wake watch with its ring and one-second strip, Delete with a second press) and the Bluetooth radar (rings, sweep, blips,
  the device list and its details); Home has thirteen rows; the scripted demonstration backend (`speed=`, `wol=`, `ble=`) and
  the scenes for every state; the unit tests of the new wording and maths (`test_ui_model_03.cpp`).
* 0.3.0: the core of the speed test (Cloudflare through curl, one connection, ChaCha20
  first, 8 s and at most 150 MB each way, the latency under load and a bufferbloat grade), Wake-on-LAN (saved machines,
  the packets on every interface or one, the wake watch) and the BLE scan (bluetoothctl with duplicate data on, the
  device table, distance classes, discovery always turned off), the stream runner they share, their saved texts and
  settings, the unit tests.
* 0.2.0, the pages: Home has nine rows; **Traceroute** with the chain of hops (node shapes and colours, latency
  bars on one scale, loss tags, details in place), **HTTP check** (status badge, waterfall, certificate badge, rows;
  curl runs on the shared `async_run` and Stop ends it), **Public IP** (asks on entry, the carrier-grade NAT note, Whois
  of this address) and **Whois** (a summary that is never cut, the raw answer as compact rows); the scripted demonstration
  backend and the headless scenes for each state, the unit tests of the new wording (verdicts, days left, loss tags, node
  classes, waterfall scaling), the board steps.

* 0.2.0: the core of traceroute / MTR (in-app, unprivileged, in rounds), the HTTP check (curl, certificate
  dates through curl or openssl), the public address (Cloudflare's trace) and whois (TCP 43 with referrals), their
  saved texts and settings, the unit tests; `depends` gains `curl, ca-certificates`.
* 0.1.0: first version: interfaces, diagnosis with the health ladder, ping with the latency graph, jitter band
  and packet strip, DNS lookup and compare, port check, saved results and settings; the pages for touch and keys, the unit
  tests and the headless scenes. Board tests still to run.
