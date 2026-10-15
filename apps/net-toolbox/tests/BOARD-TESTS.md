# Net Toolbox 0.2.0: board tests

For the Controller and the user (controller-led testing: the Controller deploys and reads logs, the user does the
physical steps). `deck.py` is `C:\CLAUDE\zero7\launcher\docs\dev\deck\deck.py` (password in `PIPW`; `DECK_HOST=<ip>`
points it at another board). The app's process name is `net-toolbox` (`pgrep -x net-toolbox`; never `pkill -f`).

Expected pictures: the contact sheets of `build.sh host` in
`\\wsl.localhost\Ubuntu\home\osrde\cdk-build\net-toolbox-nt2\host\out\sheets\` (`home.png`, `home-offline.png`,
`interfaces-deck.png`, `interfaces-pi3.png`, `interfaces-pi5.png`, `interfaces-offline.png`, `diag-healthy.png`,
`diag-notes.png`, `diag-offline.png`, `diag-dnsfail.png`, `diag-portal.png`, `diag-running.png`, `ping-mixed.png`,
`ping-lost.png`, `ping-failed.png`, `ping-unavailable.png`, `ping-target.png`, `ping-host.png`, `dns-ok.png`,
`dns-nx.png`, `dns-timeout.png`, `dns-compare.png`, `dns-mx.png`, `dns-ptr.png`, `dns-choices.png`, `dns-name.png`,
`port-open.png`, `port-closed.png`, `port-noanswer.png`, `port-unreachable.png`, `port-multi.png`, `port-web.png`,
`port-nx.png`, `port-choices.png`, `touch.png`, `nav.png`, `notice.png`; 0.2: `trace-healthy.png`, `trace-silent.png`,
`trace-ratelimit.png`, `trace-loss.png`, `trace-unreached.png`, `trace-unreach.png`, `trace-starting.png`,
`trace-resolving.png`, `trace-nx.png`, `trace-lan.png`, `trace-target.png`, `trace-touch.png`, `http-ok.png`,
`http-redirect.png`, `http-moved.png`, `http-404.png`, `http-500.png`, `http-head.png`, `http-plain.png`,
`http-e-resolve.png`, `http-e-refused.png`, `http-e-timeout.png`, `http-cert-expired.png`, `http-cert-self.png`,
`http-cert-mismatch.png`, `http-cert5.png`, `http-cert20.png`, `http-ignore.png`, `http-running.png`,
`http-settings.png`, `http-touch.png`, `pubip-both.png`, `pubip-v4.png`, `pubip-cgnat.png`, `pubip-offline.png`,
`pubip-running.png`, `pubip-whois.png`, `whois-domain.png`, `whois-arin.png`, `whois-ripe.png`,
`whois-notfound.png`, `whois-long.png`, `whois-offline.png`, `whois-running.png`, `whois-query.png`). The scenes use recorded boards and scripted
answers: the numbers on the boards differ, the pictures should not.

Wanted: the deck (Pi Zero 2 W) with its Bluetooth keyboard; the Pi 3A+ (touch only, 5 GHz Wi-Fi); a router you can
switch off or unplug from the internet; a second PC with `nc -l 2222` (or any host with a closed and a firewalled port);
a captive-portal network if one is at hand (a hotel or a phone hotspot sign-in). Write down every value that differs
from the reference command by more than the tolerance given.

## 0. Deploy

```
python tools/make_registry.py --only net-toolbox --out C:/CLAUDE/zero7/pkg-out
python deck.py put C:/CLAUDE/zero7/pkg-out/net-toolbox_0.2.0_arm64.deb /tmp/net-toolbox.deb
python deck.py sudo 'dpkg -i /tmp/net-toolbox.deb'
python deck.py run '/usr/share/APPLaunch/bin/net-toolbox --version; cat /proc/sys/net/ipv4/ping_group_range; command -v iw'
```

`--version`: `Net Toolbox (net-toolbox) 0.2.0`, the launcher commit, `cp0_lvgl pinned`. `ping_group_range` should read
`0 2147483647` (else B5 first). Logs: `python deck.py run 'journalctl --user -n 80 --no-pager | grep -E "cdk|net-toolbox"'`
(expect one line `[net-toolbox] the real network, interfaces from the system, screen 640x480`, nothing else).

## B1. Deck (640x480, Bluetooth keyboard): Home and Interfaces

1. The home tile reads **Net Toolbox** under a rose pentagon with two arrows. Open it: the title, a green line `wlan0:
   <address>` (the address of `ip -4 addr show wlan0`), then Diagnose my connection, Ping, Traceroute, DNS
   lookup, Port check, HTTP check, Public IP, Whois, Interfaces (nine rows: the list scrolls, End goes to the last). Keys: Down / Up move the highlight, Enter opens, Esc / Left go back.
2. Switch the deck's Wi-Fi off in Settings. Within 2 s the Home line turns
   amber **No network**; switch it on: the green line returns.
3. **Interfaces** (Enter on it): `WLAN0` State Connected, IPv4 (with /prefix), IPv6, MAC (`ip link show wlan0`), Network
   (`iw dev wlan0 link`: SSID), Signal `-NN dBm, Excellent|Good|Fair|Weak` (within 5 dBm of `iw`, words by -50 / -60 /
   -70), Bit rate; `LO`; then NETWORK: Gateway (`ip route | grep default`), DNS server 1 `(asked first)` and the others in
   the order of `cat /etc/resolv.conf`. One row scrolls per Down press; the list refreshes every 2 s without jumping
   (watch the signal change when you cover the antenna). Esc comes back to Home with the highlight on Interfaces.
4. Touch only (Pi 3A+): one tap on a Home row selects it, a second tap opens it; the header Esc goes back; a drag
   scrolls the lists.

## B2. Deck: Diagnosis

1. Home, Diagnose my connection: the ladder starts at once; each rung turns from a hollow circle to a spinning amber arc
   to a green disc with a check, one after another (a second or two in all); `Everything works`.
2. The details: Link up `wlan0`, Address = the board's, Gateway `<gateway>, N ms` (against `ping -c 3 <gateway>`), DNS
   `via <resolver>, N ms` (the resolver is the first in `resolv.conf`, which on the deck is not the gateway), Internet
   `1.1.1.1, N ms`, Captive portal `none (204)`.
3. **Run again** (Enter, or one tap on it) restarts it; **Save log** writes `~/net-logs/diagnosis-<date>-<time>.txt`
   (`ls ~/net-logs; cat` it: each rung with its state, the resolver, the explanation); the line over the ladder says
   `Saved ~/net-logs/...`.
4. Failures, one at a time (the picture of each is the matching contact sheet):
   - Wi-Fi off: **Link up failed**, the other five rungs show a dash and `skipped`.
   - Router up, internet cable out: Gateway passes, DNS or Internet fails with the words of the core.
   - In `/etc/resolv.conf` (copy it first, `sudo cp`) put `nameserver 192.0.2.1` first: **DNS failed** names it, or passes
     through the next server (the asked-first note); put the file back.
   - On a captive-portal network: **Captive portal failed**, the message says a sign-in page is suspected.
5. Esc in the middle of a run goes to Home at once; `pgrep -x net-toolbox` shows one process, `ss -tnp | grep net-toolbox`
   nothing left open after 1 s (no work in the background).

## B3. Deck: Ping

1. Home, Ping: `Press Start to ping Gateway <address>`, an empty graph and strip. Start: a green line, `now N ms`, the
   strip fills from the right, a cell per packet; stats `sent N lost 0 (0%) min avg max jitter` against `ping -c 20
   <gateway>` (tolerance: avg +-3 ms, the loss exactly). Stop: the line freezes, the band says `Stopped`.
2. Unplug the router's Wi-Fi (or walk away): lost packets show as red ticks on the graph's baseline, a gap in the line and
   tall red cells in the strip; the loss % rises; a slow packet (move the deck near a microwave) is an amber dot and a
   medium amber cell. Back in range the line returns.
3. **Target** (Enter, or a tap): Gateway, 1.1.1.1 Cloudflare, 8.8.8.8 Google, the saved ones, Type a host... with a check on
   the current. Pick 1.1.1.1: Start pings it (the band says `Pinging 1.1.1.1`).
4. Type a host...: with the Bluetooth keyboard the field takes the keys at once and **no on-screen keyboard** shows;
   type `example.com`, Enter: the target is `example.com` (the band shows its address after Start). Disconnect the
   keyboard (or use the Pi 3A+): the on-screen keyboard appears under the field, the field stays visible above it.
   A bad name (`a b`) says why under the field and stays.
5. **Save target** with a typed host: `Saved target example.com`; leave and open Ping again: it is in the Target list
   (`cat ~/.config/cyberdeckzero/net-toolbox.conf` shows `target=example.com`). **Save log** writes
   `~/net-logs/ping-<date>-<time>.txt` with the last 120 packets.
6. `nonexistent.invalid`: the band says `... does not exist (NXDOMAIN)` in red, nothing runs.
7. `sudo sysctl -w net.ipv4.ping_group_range='1 0'` (restore with `0 2147483647`): the page shows the notice **Ping is not
   available here** with the reason and the fix, Start only repeats it; no crash.

## B4. Deck: DNS lookup

1. Look up `example.com` A through the system resolver: the band `OK (NOERROR), N ms, via <first resolver>`; the answer
   row `93.184... A <ttl> s` (against `getent ahostsv4 example.com`; the address may differ, the name must resolve).
2. Type: A, AAAA, MX, TXT, NS, PTR (the Roller with a check). MX and NS show their targets; PTR with an address typed as
   the name (`1.1.1.1`) shows `one.one.one.one`. `www.<a site with a CNAME>`: the CNAME row first, then the address.
3. Resolver: System default `<address>`, Cloudflare, Google, Quad9, Type an address...; an invalid address is refused with
   the reason under the field. The band names the resolver used.
4. A name that does not exist (`nx-12345.example.com`): amber `No such name (NXDOMAIN)`, the list says `No records`.
5. Compare resolvers: a row per resolver (system, 1.1.1.1, 8.8.8.8, 9.9.9.9) with its time and answer count, filled in as
   they answer; the band names the fastest. Block one resolver on the router (or type `192.0.2.1`): its row says `no
   answer` in red after the timeout.
6. Save log: `~/net-logs/dns-<date>-<time>.txt`. The values (name, type, resolver) are kept: quit, open again, the
   settings file has `dns_name=`, `dns_type=`, `dns_resolver=`.

## B5. Deck: Port check

1. `example.com`, Web: `Open` with a green disc and check, `connected in N ms`; then a port of another PC with nothing
   listening (`Port` > Custom port... `2223`): `Closed` with a red cross (that PC refuses: it is there); a port that a
   firewall drops (the PC's `sudo iptables -A INPUT -p tcp --dport 2224 -j DROP`, remove it after): `No answer` with an
   amber dash after about 3 s; a host that does not exist: `Not checked`, red.
2. Presets: Web (80, 443), Mail (25, 465, 587, 993), SSH 22, DNS 53, Remote desktop 3389 ...: with several ports the word is
   `Open` when all agree, else `Mixed` and the detail lists each port (`25 No answer   465 Open 21 ms`).
3. Nothing is sent on an open port (`nc -l 2222` on the other PC prints nothing when the app connects).
4. Save log: `~/net-logs/port-<date>-<time>.txt`; the host and the port are kept.

## B6. Pi 3A+ (touch only) and the other sizes

1. Every action by tap: one tap selects a row, a second tap acts (Start, Stop, Run, Look up, Save log ...); the header Esc
   goes back; the Choice pages (Roller) take a tap on a row and a drag.
2. The on-screen keyboard appears for Type a host..., the DNS name, the typed resolver and Custom port; the field is above
   it; a tap on the key row types; Enter on its key submits.
3. Everything readable on the 640x480 deck and the 800x480 screen: no text cut except a long value with dots, the graph
   and the strip aligned (a cell under its point), the ladder's marks and words complete.
4. On a screen under 640x480 the app shows the notice `needs 640x480` and ends on any key.

## B7. Resources (deck)

1. Leave Ping running for 10 minutes: `ps -o rss,pcpu -p $(pgrep -x net-toolbox)` stays under 25 MB and a few % CPU; the
   graph keeps moving without lag. Press Esc: the process ends within a second and no `ping` or helper process is left.
2. Leave Home open for 5 minutes: CPU near 0 (the 2 s refresh reads a few small files).
3. Run Diagnosis 20 times (Run again): memory does not grow (compare `rss` before and after).

## B8. Deck: Traceroute (0.2)

`which traceroute tracepath mtr` finds nothing on the boards: the app does its own. Compare with `ping -c 3` and, on the
PC, `traceroute` / `mtr -rwc 20` to the same host.

1. Home, **Traceroute**: the status line `Press Start to trace Gateway <address>`, three large cells (Start, Target, Save
   log) and under them the empty chain with a short hint. Tab, Left / Right move between the cells; Down goes into the
   chain; Esc and Left (from the first cell) go back to Home.
2. **To 1.1.1.1**: Target > 1.1.1.1 Cloudflare, Start. Hops appear one after another within about a second, then the
   numbers move once a second without the rows jumping. The status line reads `to 1.1.1.1, round N, M hops`. Hop 1 is the
   gateway (`ip route | grep default`, a few ms); the nodes are filled green; the last hop (1.1.1.1, its name
   `one.one.one.one` when known) has a double ring and the word `reached`. At least 7 hops are in view at once on the
   deck without scrolling the page; Up / Down move the highlight and scroll the chain.
3. Enter (or a tap on the selected hop): its details open in place (addresses seen, last / avg / best / worst, jitter,
   sent / received, loss); Enter again closes them. Stop (Enter on the first cell): the line says `Reached in N hops, X ms`
   (the destination's average, within 5 ms of `ping -c 20`).
4. **To a LAN host** (Target > Type a host... > the address of a PC on the Wi-Fi): one or two hops, `Reached in 1 hop, N ms`.
5. **To a name** (`example.com`): the status line names the address too; hop names come from reverse DNS (a hop with no
   name shows its address in the larger text). A name that does not exist (`nx-12345.example.com`): a red line and the
   reason, nothing runs.
6. **Silent and limited hops**: a hop that never answers shows a hollow dotted circle and `no reply` (never red); the path
   goes on beyond it. A router that answers late or only some of the time and is followed by clean hops shows a grey ring
   and `probably rate limiting`, not red. Real loss (unplug the far end, or trace a host behind a lossy link) shows red
   circles with an X and a loss tag such as `30 %`.
7. **Cancel in the middle** (Stop, or Esc while it runs): the page leaves at once; `pgrep -x net-toolbox` shows one
   process; `ss -u -p | grep net-toolbox` shows nothing left open after 1 s.
8. A destination that does not answer ICMP at all (a host that drops everything): once stopped, the line says `Did not
   reach the destination: no answer after hop N`.
9. **Save log**: `~/net-logs/traceroute-<date>-<time>.txt` with one line per hop; the line at the top says where.
10. Pi 3A+ (touch only): tap Start / Stop / Target / Save log (one tap each), a tap on a hop selects it, a second tap opens
    its details; a drag scrolls the chain.

## B9. Deck: HTTP check (0.2)

Compare with `curl -sS -o /dev/null -w '%{json}' <url>` on the PC.

1. Home, HTTP check, Run on `https://example.com`: the badge `200 OK` (green check), the waterfall with DNS, Connect, TLS,
   Waiting and Transfer named under their segments and the total in ms (within 20 % of curl's `time_total`), the
   certificate badge `expires in N days` (green from 30 days; `openssl s_client -connect example.com:443 </dev/null |
   openssl x509 -noout -enddate` gives the date), then Subject, Issuer, Valid until, Remote IP, HTTP version, Size,
   Redirects. Run shows `Checking...`; Stop during a slow request ends curl (`pgrep curl` shows none afterwards).
2. **Method > HEAD**: Size reads `none (HEAD asks for the headers only)`.
3. **An HTTP-only site** (`http://neverssl.com`): `no certificate` (plain HTTP, nothing is encrypted), no TLS segment.
4. `http://example.com` redirects: Redirects 1 and a Final URL row.
5. **Certificates** (with the Ignore switch Off): `https://expired.badssl.com` gives a red badge `Certificate expired` and
   the certificate block `expired N days ago`; `https://self-signed.badssl.com` gives `Self-signed certificate`;
   `https://wrong.host.badssl.com` gives `Certificate name mismatch`. Each says the reason in plain words under the badge.
   With **Ignore certificate errors** On (a warning note shows under the row) the same address answers `200 OK` and the
   certificate block still says what is wrong; switch it Off again.
6. A name that does not exist, a closed port (`https://192.168.x.x:1` is refused, `https://example.com:81` times out): the
   red badge with the plain-words error; the waterfall shows only the segments that happened.
7. Offline (Wi-Fi off): `Name not found` or `Timed out`, no crash, Run works again once the network is back.
8. Save log: `~/net-logs/http-<date>-<time>.txt`. The settings file has `http_url=` and `http_insecure=` (the method is
   not kept).

## B10. Deck: Public IP and Whois (0.2)

1. Home, **Public IP**: it asks by itself (`Asking Cloudflare for the IPv4 address...`), then Public IPv4 (compare with
   `curl -4 https://www.cloudflare.com/cdn-cgi/trace`), Public IPv6 (`none` in the neutral colour on a network without
   IPv6; an address when there is one), Country, Cloudflare data centre, Local address (`ip -4 addr show wlan0`, with
   `private` or `carrier-grade NAT`). On a mobile hotspot behind carrier-grade NAT the amber note appears.
2. **Run again** asks again. **Whois of this address** opens Whois with the address and runs it at once: Net name,
   Organisation, Country, Range, AS number, Abuse contact, then the raw answer. Esc goes back to Public IP.
3. Home, **Whois**, Query `example.com` (or a domain of your own), Run: Registrar, Created, Updated, Expires with the days
   left in words (`2027-01-06 (expires in 83 days)`), Status, Name servers; empty fields are not shown; long values wrap
   and are never cut; the raw lines under the heading Raw answer (cut with dots there only). Compare with `whois` on the PC.
4. An IP address (`8.8.8.8`, then `193.0.6.139`): the ARIN and the RIPE styles both fill the summary. A domain that does not
   exist: `No match for ...` in amber. A bad query (`a b`): the reason under the field.
5. Wi-Fi off: Public IP says `Not reachable: no connection to Cloudflare` in red; Whois says `No whois server answered`
   and the reason as a row. Nothing hangs; Esc leaves at once.
6. Save log on each: `~/net-logs/publicip-...txt`, `whois-...txt`.
7. `ss -tnp | grep net-toolbox` shows nothing left open after leaving a page; `pgrep curl` shows none.

## B11. Pi 3A+ with the Wi-Fi dongle (0.3): the network chooser

The board: wlan0 on the home network (default route), a USB Wi-Fi dongle (Realtek RTL8822BU) as wlan1 on a phone hotspot.
Do not change the connections for this test; join the hotspot first with Settings > Wi-Fi of the launcher or
`nmcli device wifi connect ... ifname wlan1`.

1. Home: the first row is **Network**, value `Automatic (wlan0)`; the page starts on Diagnose my connection; the band says
   `wlan0: 192.168.68.xxx`.
2. **Interfaces**: both `WLAN0 (DEFAULT ROUTE)` and `WLAN1` show State, IPv4, IPv6, MAC, **Network** (the SSID), Signal and
   **Bit rate**; wlan1's has the hotspot's name. Compare with `iw dev wlan1 link`. If wlan1 shows no Network and no Bit rate
   row, its driver may not speak nl80211: note what `iw dev wlan1 link` prints.
3. Network row, Enter: Automatic, wlan0, wlan1 with their addresses, Automatic checked. Pick **wlan1**: the Home value reads
   `wlan1`, the band `wlan1: 172.20.10.x`. Leave and start the app again: wlan1 is still chosen (`~/.config/cyberdeckzero/
   net-toolbox.conf` has `interface=wlan1`).
4. **Public IP**: the address is the hotspot's carrier address (compare `curl --interface wlan1 https://www.cloudflare.com/cdn-cgi/trace`
   and the same with `--interface wlan0`: two different `ip=` lines); its status line ends `(via wlan1)`. Back on Automatic
   it shows the home line's address again.
5. **Ping** (target Gateway = 172.20.10.1, then 1.1.1.1): answers, `(via wlan1)`. While it runs, on a PC ping the board's wlan0
   address: nothing changes. **Traceroute** to 1.1.1.1: the first hop is 172.20.10.1, not the home router.
6. **DNS lookup**: the Resolver row reads `wlan1's DNS 172.20.10.1` (`nmcli -g IP4.DNS,IP6.DNS device show wlan1` shows the
   server); Look up works; Compare lists it first.
7. **Port check** to a host on the home LAN (the router 192.168.68.1 port 80): No answer or Unreachable over wlan1 (it is
   another network), Open on Automatic. **HTTP check** `https://example.com`: 200 over wlan1.
8. **Diagnosis** on wlan1: all six rungs green; the Link rung says `wlan1`, the gateway 172.20.10.1, DNS `via 172.20.10.1`.
   Switch the hotspot off on the phone: Link up fails with `wlan1 is not connected to a network ...` (or the gateway
   silent), never a hang.
9. Unplug the dongle with wlan1 chosen: within 2 s Home says `wlan1 not found: using Automatic (wlan0: ...)` in amber, the
   Network row `Automatic (wlan0)`; every tool works on wlan0 and the status lines have no `(via ...)`. Plug it in again
   and rejoin: wlan1 is used again by itself.
10. `ss -tnp | grep net-toolbox` and `pgrep curl`: nothing left after leaving a page.

## B12. Ethernet adapter on a hub (0.3)

A USB Ethernet adapter on the board's hub (name `enx<mac>` or `eth0`), cable to the router; Wi-Fi also connected.

1. Network row: the adapter is listed (`enx00e04c680a12  192.168.1.xx`); the long name may be cut with dots on the
   smallest screen. Interfaces: its heading with State Connected, Speed (`1 Gbit/s` or `100 Mbit/s`), no Network row.
2. Choose it: Diagnosis, Ping, DNS lookup, Public IP run with `(via enx...)`; the rung Link up shows the name and the speed.
3. Pull the cable out: within 2 s the Network list says `no link` for it; Diagnosis on it fails at Link up with
   `... has no link: the cable is unplugged ...`; on Automatic the diagnosis still passes over Wi-Fi.
4. Plug the cable in again: it works at once. Unplug the adapter from the hub with it chosen: the `not found` note, Automatic.
5. Wake-on-LAN (when its page exists): the magic packet leaves by the wired interface (`sudo tcpdump -ni enx... udp port 9`
   on the board).

## B13. Speed test (0.3): the deck and the Pi 3A+

Home, **Speed test** (row 8). The note `Uses up to about 150 MB each way. Start it yourself; on a phone hotspot that is mobile
data.` is under the cells before the first press; nothing is sent until Start. Do this on Wi-Fi at home, not on a hotspot,
unless the step says so.

1. Start. The gauge reads **Latency** (the needle stays at 0), then **Download**: the needle swings up and settles in about
   a fifth of a second per update, the big value and the chart follow, the thin bar counts `1 s of 8 s` to `8 s of 8 s`.
   Then **Upload** the same. The dial's scale steps up (0-10, 0-25, 0-50, 0-100 ...) with the highest value and never goes
   back down during the run; the numbers fade and come back with the new scale. Nothing flickers (the rest of the page does
   not redraw), the keys and taps still answer while it runs.
2. Expected speeds measured by the core worker on the same Wi-Fi: the **deck (Zero 2 W)** 22 to 35 Mbit/s down and 30 to 36
   up; the **Pi 3A+** about 100 or more down and up. A deck far under 15 Mbit/s or a 3A+ under 60 is worth a note (the
   Wi-Fi, the position, `iw dev wlan0 link`).
3. The result card replaces the gauge: Download and Upload as large values with `Mbit/s`, Latency (idle) and Jitter, Latency
   while downloading and while uploading with the difference (`52 ms (+34 ms)`), the grade badge with its letter and its plain
   line, `Cloudflare, <colo>`. On a home router with SQM a grade A or B is normal; a grade F shows heavy buffering (try the
   same run while another device is downloading). Save log writes `~/net-logs/speedtest-<date>-<time>.txt`.
4. **Stop** in the middle of the download and of the upload: the page says `Stopped` within about a second, shows the card of
   what was measured, `pgrep curl` shows nothing a second later, and Start runs again.
5. Esc / Left during a run: back to Home at once, `pgrep curl` shows nothing, no traffic afterwards (`ss -tnp`).
6. Wi-Fi off, then Start: `No connection`, the plain words in red under the cells (no hang); Wi-Fi on again, Start works.
7. With a second adapter chosen under Network (B11), the status line ends `(via wlan1)` and `curl` uses that interface
   (`ss -tnp | grep curl` while it runs shows the wlan1 address).
8. **Mobile data**: on a phone hotspot one run moves up to about 50 MB each way on a 25 Mbit/s link and never more than 150 MB
   each way. Only run it when the data plan allows it; the note says so before you press.
9. At 640x480 the Start cell, the note and the whole dial are on screen without scrolling; the numbers 0 to 10 (or to 1000)
   do not touch each other or the big value. The same on an 800x480 or 1024x600 screen.
10. Memory: `ps -o rss= -C net-toolbox` during a run is under 25 MB; the CPU of the app (not curl) stays low (`top`).

## B14. Wake-on-LAN (0.3)

Needs one machine that can be woken (a PC with Wake-on-LAN switched on in its BIOS and network card settings, on Ethernet)
and a second one that is already on.

1. Home, **Wake-on-LAN**: `No machine saved yet`, the note about Wake-on-LAN. **Add a machine**: Name, then MAC (`ip link`
   on the target, or its label), then an address to watch (the target's IP, optional). A bad MAC (`zz:12`) is refused with
   `Not a MAC address: type 12 hex digits, like aa:bb:cc:dd:ee:ff` under the field; `aa:bb:cc:dd:ee:ff`, `AA-BB-...` and
   `aabbcc...` are accepted and shown as `aa:bb:cc:dd:ee:ff`. A name already saved is refused. The machine appears with `Not
   woken yet` and its MAC and address; leave and start the app again: it is still there
   (`~/.config/cyberdeckzero/net-toolbox.conf` has a `wol=` line).
2. Open the machine: the ring says `Not sent yet`. **Wake now** on the machine that is **on**: `Packet sent on wlan0` (the
   interfaces that are up), then within a second `Already awake` (green check): it answered the first ping. The strip is
   green from the first cell.
3. **Wake now** on the machine that is **off** (shut down, not suspended): the ring spins with `Waking... N s`, the strip
   fills grey one cell a second; when it boots: `Awake after N s` (green check), the strip turns green from that second. On
   the target `sudo tcpdump -ni <if> udp port 9` (or the router's log) shows the packet.
4. The machine does not wake (Wake-on-LAN off in its BIOS, or it is on Wi-Fi): after 120 s the ring says `No answer yet`
   (amber dash), the strip is all grey, the status line says `No answer from <address> after 120 s`. **Stop** during the
   watch: `Watch stopped`, nothing more is sent (`tcpdump` shows no more pings).
5. A machine without an address to watch: Wake now says `Sent` (arrow disc) and the strip is replaced by `Add an address to
   watch to see when it wakes up`. **Watch after waking** Off: the same with an address.
6. With a chosen network (B11/B12) the packet leaves by that interface only (`sudo tcpdump -ni wlan1 udp port 9`), and the
   status line ends `(via wlan1)`; on Automatic by every interface that is up (`Packet sent on wlan0, wlan1`).
7. Edit each of Name, MAC address, Watch address (the rows of the machine page): the change is kept; renaming keeps the last
   status. **Delete**: the row asks `Press again to delete`; any other action disarms it; the second press removes it. Add
   machines until 20 are saved: `list full`, and Add says `You can save up to 20 machines: delete one first.`
8. Esc from the page while it watches: back to the list at once, no more pings (`tcpdump`).
9. Save log on a machine page: `~/net-logs/wol-<date>-<time>.txt`.

## B15. Bluetooth radar (0.3): the Pi 3A+ only

**Never start a BLE scan on the deck during tests unless the owner decides**: the deck's Bluetooth keyboard shares the radio.
Run this on the Pi 3A+ (bluez installed, `hci0` powered).

1. Home, **Bluetooth radar**: the first-use note beside the radar, the permanent line `Wi-Fi and Bluetooth share one radio: a
   Bluetooth keyboard may stutter while scanning.`, the radar with its three labelled rings, no sweep, nothing scanning
   (`bluetoothctl show` says `Discovering: no`).
2. With a phone advertising (a Bluetooth settings page open on it, or a BLE beacon app): Start. The status line counts
   `Scanning, N devices, S s left` down from 60; the sweep turns once in about 3 s with a faint trailing wedge; blips appear
   and pulse when the sweep passes them; the phone is in the near or medium ring; walking away with it moves the blip
   outwards to the far ring and, once it is out of range for 20 s, it fades and leaves the list. The list shows the
   strongest first with `-54 dBm, near`; the highlight follows the keys and the radar rings that device and names it.
3. Enter on a device (or a tap, then a second tap): the details open in place: Name, Address, Address type, Vendor, Signal,
   Smoothed signal, TX power, Last seen, Heard. Esc closes them, the next Esc leaves the page.
4. Stop (or the 60 s are up): the sweep stops, the radar keeps its blips still, the line says `Stopped, N devices seen` or
   `Finished, N devices seen`; `bluetoothctl show` says `Discovering: no` within a second.
5. Esc / Left in the middle of a scan, and leaving the app by Esc at Home: `Discovering: no` within a second and `pgrep
   bluetoothctl` shows nothing.
6. With a Bluetooth keyboard paired and typing in another window (or `evtest` on it): note whether it stutters while the scan
   runs (the page says it may); it must be fine again as soon as the scan stops.
7. Bluetooth off (`sudo rfkill block bluetooth`): Start says `Bluetooth is blocked by rfkill` with `Turn Bluetooth on in
   Settings > Bluetooth`; `bluetoothctl power off`: `Bluetooth is switched off`; unblock and power on again, Start works.
8. Memory and CPU: `ps -o rss= -C net-toolbox` before and during a scan with 20 or more devices around: the page adds under
   3 MB; the CPU stays modest (the sweep redraws a 250 px circle ten times a second only while scanning; `top` shows well
   under 10 % of one core on the 3A+).
9. Save log after a scan: `~/net-logs/ble-<date>-<time>.txt`.
