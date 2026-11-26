# Reticulum bridge (service core P1a, contacts and messages P2)

The background service of the Reticulum apps (launcher report 057, architecture A, decisions D1-D20). One Python
process is the RNS **shared instance**, runs an **LXMF router** with the deck's own identity, and offers our native apps
a **JSON-lines Unix socket**. It is our code (MIT, written with AI assistance) on top of the public Python API of RNS and
LXMF; it is not a Reticulum implementation. The package bundles the two pinned upstream wheels unmodified (section 9). Source:
[`apps/reticulum/bridge/`](../../apps/reticulum/bridge/). Status: **packaged as a draft** (`apps/reticulum`, `draft: true`, section 9), **not published**; tested on a PC only
(sections 7 and 9). The board tests are open (section 8). Since P1b the package also holds the app that talks to it: one package, `reticulum`, with the tile, the app binary and this service (section 9; the app's [README](../../apps/reticulum/README.md)). **Package 0.3.1 (bridge 0.2.1) answers the board QA of 0.3.0: Wi-Fi / LAN has no listen-only state, the announcement stamp is made in the background, and a cancelled message says why in the list too (sections 4 and 10).** **Phase P2 (service side, bridge 0.2.0, package 0.3.0-bridge): contacts, the heard list and direct messages, with the message store and the new-message alerts (section 10). The app screens for them (Messages, conversations, Contacts, Heard recently, My address, Announce myself, the alerts row in Settings) are in package 0.3.0 (section 11).**

## 1. Running it

```sh
cd apps/reticulum/bridge
python3 -m reticulum_bridge [--config DIR] [--data DIR] [--run DIR] [--no-lxmf]
python3 -m reticulum_bridge.client status                         # one request, prints the response line
python3 -m reticulum_bridge.client announces.subscribe --events 60 # and the event lines for 60 s
```

It runs in the foreground and logs to stderr (`[reticulum] ...`, one line per event, never a key or passphrase); RNS
logs to stdout at the configuration's `loglevel`. Exit status: 0 clean stop (SIGTERM, SIGINT, SIGHUP or
`service.stop`), 1 could not start (reason on stderr), 2 usage (for example `XDG_RUNTIME_DIR` unset: there is no `/tmp`
fallback), 3 a bridge already runs for this run folder. Environment equivalents of the options:
`RETICULUM_BRIDGE_CONFIG`, `RETICULUM_BRIDGE_DATA`, `RETICULUM_BRIDGE_RUN`. `RETICULUM_BRIDGE_LAUNCHER_SOCK` (P2: replaces the path of the launcher's `launcher.sock`; for tests, section 10). Needs Python 3.10 or later, `rns` 1.6.1 and
`lxmf` 1.3.1 ([`requirements.txt`](../../apps/reticulum/bridge/requirements.txt), pinned with sha256), and the
distribution's `python3-cryptography` and `python3-serial`.

## 2. Files

| Path | What | Mode |
|---|---|---|
| Reticulum config folder: `--config`, else the first of RNS's search order that holds a `config` (`/etc/reticulum`, `~/.config/reticulum`, `~/.reticulum`), else `~/.reticulum` | `config`: written once when none exists (below), then changed only by the interface verbs; `config.bak`: the copy before the last change; `storage/`: RNS's own (transport identity, tables) | folder 0700, a new `config` 0600, an existing one keeps its mode |
| `--data`, default `$XDG_DATA_HOME/reticulum` (`~/.local/share/reticulum`) | `identity` (the deck's LXMF identity, separate from NomadNet's), `lxmf/` (the router's store), `display_name`, and since P2 `messages.db` (contacts, messages, heard list; section 10) with its `-wal` / `-shm` files, and `settings.json` | 0700 / 0600 (the bridge runs with umask 077) |
| `--run`, default `$XDG_RUNTIME_DIR/reticulum` (tmpfs) | `bridge.sock` (the socket), `bridge.state` (below), `bridge.lock` (one bridge per user; stays) | folder 0700, files 0600 |

**First configuration** (only when the folder has no `config`; an existing file is never overwritten, `O_EXCL`):
`share_instance = Yes`, `enable_transport = No`, `loglevel = 3`, and one interface, upstream's default
`[[Default Interface]]` (`AutoInterface`), present with `enabled = no` and `outgoing = False`. RNS would otherwise write
its own first file with that interface switched on.

**Changes** (`iface.*`): the file is read again before each change, so hand edits made while the bridge runs are kept;
only the lines of the one `[[section]]` change (other sections, comments and unknown keys stay byte for byte); the
result is parsed with RNS's own ConfigObj and compared with the intended values before anything is written; then
`config.bak`, a temporary file, fsync and rename. A section with multi-line (`'''`) values is refused ("edit by hand").

**`bridge.state`** (key=value, rewritten atomically only when a value changes; checked every 15 s and after each
change; removed at a clean exit): `pid`, `state=up`, `bridge`, `protocol`, `rns`, `lxmf` (version or `off`), `mode`
(`master`, or `client` when another program already runs the shared instance), `config_dir`, `identity`,
`lxmf_address`, `unread` (messages not marked read), `ifaces_up` (count), `ifaces_up_names` (`;`-separated), `started` (epoch). Trust it only while `pid` is
alive; no file means down. A stale socket (a bridge that was killed) is removed by the next bridge once it holds the
lock.

## 3. Protocol 1

One JSON object per line, UTF-8, at most 64 KiB per line, at most 8 clients.

```
request   {"id": 7, "verb": "iface.enable", "params": {"name": "Home LAN"}, "timeout": 15}
response  {"id": 7, "ok": true, "result": {...}}
          {"id": 7, "ok": false, "error": {"code": "busy", "message": "...", ...}}
event     {"event": "announce", "data": {...}}                    (no id)
```

`id` is any string, number or null and is echoed; `params` defaults to `{}`; `timeout` (seconds, 0.5-120, default 15)
bounds the wait: when it passes the bridge answers `timeout` (the work may still finish; its late answer is dropped).
Requests run on two worker threads, so responses can arrive out of order: match them by `id`. Events go only to clients
that subscribed: `announces.subscribe` for the announce stream (`announce`, `heard`), `messages.subscribe` for the
message stream (`message`, `message_state`, `unread`, `conversation_deleted`, `contacts_changed`); a client must skip lines
that have `event` and no `id`.

Error codes: `bad_request` (not JSON, no verb; `id` null), `unknown_verb`, `invalid` (with `field`), `not_found`,
`exists`, `not_supported`, `incomplete` (with `missing`), `busy` (with `port`, `holders`), `permission`,
`port_error`, `attach_failed`, `config_error`, `no_display_name`, `no_transmit` (**no connection is allowed to send**: the app
maps it to "No connection is allowed to send"), `lxmf_off`, `confirm` (a request needs the user's agreement: `identity.export` without `confirm`, `iface.enable` of a kind with no listen-only state without `allow_sending`), `identity`, `timeout`, `internal`.

| Verb | Params | Result |
|---|---|---|
| `hello` | - | `bridge`, `protocol` (1), `rns`, `lxmf`, `verbs` |
| `status` | - | `pid`, `state`, `bridge`, `protocol`, `rns`, `lxmf`, `mode`, `transport` (false), `config_dir`, `identity_hash`, `lxmf_address`, `display_name`, `interfaces_total`, `interfaces_up` (names), `uptime` |
| `ifaces` | - | `interfaces`: every `[[section]]` of the config, hand-written ones too: `name`, `type`, `enabled`, `outgoing` (upstream default true when absent), `listen_only` (what listen only means for this kind: `silent`, `handshake` or `none`, section 4), `online`, `stats` (from `get_interface_stats()`: `status`, `rxb`, `txb`, `atxb` / `ptxb` (announce / path request bytes sent), `arxb`, `prxb`, `bitrate`, `clients`, `rssi`, `snr`, ... when RNS reports them), `options` (secrets left out), `secrets_set`, `subsections`, `supported`, `missing` (required options not set), `lock` (`held`, `not held`, or null when the port is not a device), `error` (why it is not up) |
| `iface.types` | - | `types`: `[{type, supported, port_lock, listen_only, options, reason?}]`, `common` (options of every type), `switches` (`enabled` and `outgoing` of a new interface: both false) |
| `iface.add` | `name`, `type`, `options` | the new interface (as in `ifaces`), written with `enabled = no`, `outgoing = False` |
| `iface.edit` | `name`, `options` (`null` or `""` removes an option; absent = unchanged) | the interface, `reloaded` (bool, or null when it is off) |
| `iface.remove` | `name` | `removed` |
| `iface.enable` | `name`, `enabled` (default true), `allow_sending` (bool) | the interface. On: refuses `incomplete`; **for a kind with `listen_only` = `none` (Wi-Fi / LAN) refuses `confirm` unless `allow_sending` is true, and then writes `outgoing = True` with `enabled = yes`: such a kind is on (sending) or off, nothing else**; takes the port lock (`busy`); writes `enabled = yes`, then `attach_interface()`; on failure writes `enabled = no` again (`attach_failed`). Off: writes `enabled = no`, `detach_interface()`, lets the lock go |
| `iface.outgoing` | `name`, `outgoing` (bool): **the transmit switch** | the interface, `reloaded` (`reload_interface()` when it is on). `outgoing: false` for a kind with `listen_only` = `none` is refused (`invalid`, field `outgoing`): switch it off with `iface.enable` instead |
| `notices.take` | - | `notices`: things the user is to be told once, then forgotten (kept in `<data>/notices.json` until taken): `{kind: "listen_only_removed", name, type, time}` = a connection that an older version left on and listen only, of a kind that cannot be, was switched off at start (section 4) |
| `announces.subscribe` | `on` (default true) | `subscribed`, `recent` (the last 64 heard announces). Also switches on the `heard` events (section 10) |
| `paths` | `max_hops` (optional) | `count`, `paths` (up to 256 rows of `get_path_table()`, bytes as hex) |
| `announce` | `display_name` (optional; stored for later) | `announced`, `lxmf_address`, `display_name`, `via` (interfaces that may transmit). Refuses `no_display_name` until a name is set, `no_transmit` while no interface is on, online and allowed to transmit |
| `identity.info` | - | `identity_hash`, `lxmf_address`, `display_name`, `identity_file` |
| `identity.name` | `name` (optional) | `display_name`. Without `name` it reads the stored display name; with a string (1-64 characters, no control characters) it stores it (`display_name` file, 0600, atomic) and sets the LXMF destination's name; `""` or `null` clears it. It never transmits: only `announce` sends the name. Added for the app's Settings > Name (P1b) |
| `identity.export` | `confirm: true` | `identity_hash`, `format` (`base32`), `key` (the 64-byte private key, `Identity.get_private_key()`; base32 of RFC 4648). Logged without the key |
| `service.stop` | - | `stopping`; the bridge answers, then exits cleanly |

Events: `announce` (`time`, `destination`, `identity`, `kind` (`lxmf`, `lxmf_propagation`, `nomadnet_node` or `other`,
from the destination hash of the announced identity), `name` (LXMF display name, NomadNet node name), `hops`,
`interface` (the configured interface's name; for a connection spawned by a TCP server interface that is the server's
name)), `ifaces_changed` (after every `iface.*` change), `stopping`. The P2 verbs and events are in section 10.

**Option lists.** Each option: `name`, `kind` (`int`, `float`, `bool`, `str`, `list`), and where the RNS manual or report
057 4.2 states them `min`, `max`, `choices`, `unit`, `default` (RNS's value when the option is absent; null = none),
`required` (RNS cannot bring the interface up without it), `secret`, `device` (a port path that gets the lock). Values
are accepted as JSON numbers, booleans and strings ("868000000", "yes"); `null` or `""` means empty. `enabled` and
`outgoing` are not options: only `iface.enable` and `iface.outgoing` change them. Types: `AutoInterface`,
`BackboneInterface`, `TCPServerInterface`, `TCPClientInterface`, `UDPInterface`, `I2PInterface`, `RNodeInterface`,
`SerialInterface`, `PipeInterface`, `KISSInterface`, `AX25KISSInterface`; `RNodeMultiInterface` is listed as not
supported (its sub-interfaces are nested sections: by hand); the loader's undocumented `WeaveInterface` is left out;
custom interface modules show in `ifaces` but cannot be added. The RNode radio fields (`frequency`, `bandwidth`,
`txpower`, `spreadingfactor`, `codingrate`) have no default and no example value (D10); the airtime limits are optional
and empty by default. Common options: `mode`, `network_name`, `passphrase` (secret), `ifac_size`, `announce_cap`,
`bitrate`, `gravity`, `bootstrap_only`; the discovery options are not offered (hand edits are kept).

## 4. Transmit rules (D8) and what was measured

- Interfaces written by the bridge start with `enabled = no` and `outgoing = False`; the first configuration has every
  interface off. Measured: with the first configuration nothing is attached and `rnstatus` reports zero tx bytes; an
  added interface stays off and the peer sees no connection.
- `outgoing = False` (listen only): announces and path requests are not sent (measured: 0 announce and 0 path request
  bytes while the peer's announce was received; in a separate PC run outside the suite, an announce asked of RNS
  directly on such an interface was not sent either). The bridge's `announce` refuses in that state anyway. **But RNS checks
  `outgoing` in one place only** (`Transport._outbound`, `interface.OUT`, when it broadcasts a packet): whatever an
  interface sends by itself ignores it. QA of 0.3.0 measured it (F1): a Wi-Fi / LAN connection that was on and listen only
  still multicast 6 datagrams in 10 s, against 7 when sending and 0 when off. Audit of the bundled RNS 1.6.1 source, kind by
  kind (`listen_only` in `iface.types` / `ifaces` is this column, `options.LISTEN_ONLY`):

  | Kind | `outgoing = False` is honoured? | What still goes out (source) | `listen_only` |
  |---|---|---|---|
  | `AutoInterface` (Wi-Fi / LAN) | **no** | a multicast discovery token every 1.6 s and a unicast one to each peer heard (`AutoInterface.py` `announce_handler`, `peer_announce`, `reverse_announce`: `sendto`, no `OUT` test); measured 6-7 datagrams per 10 s | `none` |
  | `TCPClientInterface` | partly | the connection's own hello (a tunnel synthesis packet, 236-240 bytes) at every connect and reconnect (`TCPInterface.py` `connect` -> `Transport.synthesize_tunnel` -> `early_synthesis`, no `OUT` test), TCP keep-alives; data, announces and path requests are honoured | `handshake` |
  | `TCPServerInterface` | partly | the same hello to each client that connects (a spawned `TCPClientInterface`) | `handshake` |
  | `BackboneInterface` | partly | the same (`BackboneInterface.py` `early_synthesis`) | `handshake` |
  | `I2PInterface` | partly | the hello, a 2-byte keep-alive (`I2PInterface.py`) and the I2P router's tunnel set-up | `handshake` |
  | `UDPInterface` | yes | nothing by itself (`process_outgoing` is reached only from `Transport`) | `silent` |
  | `RNodeInterface` | yes | nothing on air: the start-up commands go to the modem; the ID beacon (`id_callsign` + `id_interval`) is timed from the first transmission (`first_tx` is set by `process_outgoing`), so it never starts while nothing was sent, and the bridge reloads the interface when `outgoing` changes, which resets it. Measured 0 bytes in 30 minutes (report 057 8.3, RNS 1.6.0) and `txb 0` on the owner's listen-only RNode during the 0.3.0 QA | `silent` |
  | `KISSInterface` | yes | nothing by itself (same beacon rule as the RNode) | `silent` |
  | `AX25KISSInterface`, `SerialInterface`, `PipeInterface` | yes | nothing by itself (no beacon in 1.6.1) | `silent` |
  | `RNodeMultiInterface` (by hand only) | yes, per sub-interface (`outgoing` of each sub-section, `RNodeMultiInterface.py`) | nothing by itself | `silent` |
  | a custom interface module | not known | not promised | `none` |

  **Rule (owner decision 2026-11-25):** a kind whose listen only is not silent must not show "listen only" to the user as if it
  were. `AutoInterface` (`none`) has no listen-only state: it is on, and may send, or off. The bridge refuses `iface.outgoing
  false` for it, refuses `iface.enable` without `allow_sending: true` (the app asks the user first), and **at every start (and
  every 15 s tick, except as a client of someone else's shared instance, whose interfaces are its owner's) switches off a connection of such a kind that is stored as on with `outgoing = False`** (a 0.3.0 state or a
  hand edit): the file gets `enabled = no` (also `interface_enabled`), the interface is detached or never attached, a
  `listen_only_removed` notice is kept for `notices.take`, and the app says so once. This happens before RNS reads the file:
  nothing transmits during the upgrade (tested). The `handshake` kinds (TCP, Backbone, I2P) keep a listen-only choice because
  data, names and announcements are held back, but the app says on their page that the link still says hello when it comes up;
  making them `none` too is one line in `options.LISTEN_ONLY` (the bridge tests then need another loopback kind for their
  listen-only measurements: UDP). "Off" is silent for every kind: the interface is detached and its sockets are closed
  (measured for Wi-Fi / LAN: 0 datagrams).
- A packet to a destination whose path is known, and traffic on a link, is not stopped by `outgoing = False` either
  (report 057 8.1): see the route check below.
- The LXMF router with the deck's identity sends nothing by itself: 0 announce and 0 path request bytes in 10 s with
  `outgoing = True` before any `announce` request (and none in the listen-only phase).
- `announce` is the only verb that sends; it refuses while no interface may transmit, and the peer receives it, with the
  display name, once one may. On loopback the other node's announce handler saw it 0.5 to 18 s after the request (7
  runs; the bridge's announce byte counter moved at once, no CPU went to stamps; the cause of the spread was not
  investigated), so the tests wait up to 60 s.
- **Not covered by these rules:** once an interface may transmit, RNS answers what others ask of our own destinations
  (path requests, link requests, delivery proofs of an incoming LXMF message); such answers need the other side to know
  the deck's address (from an announce the user made, or the address shared by hand). report 057 8.1 showed that
  `outgoing = False` does not stop a packet to a known path, so since P2 the bridge refuses `messages.send` and
  `messages.retry` itself (`no_transmit`) when no interface may transmit and when the known path leads over one that may not
  (section 10); the first is tested against real LXMF, the second (two interfaces, one of them listen-only) only with fakes.
- **Stamps (RNS 1.6.1 proof of work), QA of 0.3.0 F2.** The earlier text said "stamps off". That was wrong: the constructor's
  `enable_stamping=False` only means "do not force them on". `Transport.STAMP_DESTINATIONS`, `STAMP_PRS` and `STAMP_LRS` are
  `True` in the class, and the configuration's `[stamps]` section can only turn them on (`Reticulum.py`: `if value == True`),
  never off. What they do: `STAMP_DESTINATIONS` puts a proof of work of cost 25 (`DESTINATION_VALUE`) on the first announcement
  of each destination (`Destination.announce` waits for `_await_stamp`, then keeps the result on disk in RNS's storage, so it
  is paid once per identity); `STAMP_PRS` stamps each path request (cost 15.8); `STAMP_LRS` stamps a link request when the
  destination announced a link cost (12.5). Measured on the PC (`taskset`): the path request stamp 0.1 s, the destination stamp
  24 / 34 / 43 s on one core and 3 / 10 s on four; on a Pi 3A+ the 0.3.0 QA saw about two minutes of four workers and the 0.3.1 QA measured **272 s** the first time (palmpi, four workers at about 50 % CPU each; the second warm-up took 9.9 s, so the cost is random; about 25 s to 45 s on a Pi 4B; a message sent 31 s in failed after 53.6 s, after the stamp was ready delivery took 3 s), the first answer
  to a stranger's path request waited for it and the stranger's first message failed. **Decision: keep the defaults.** RNS 1.6.1
  accepts unstamped announcements and path requests only because enforcement is off by default (`STAMPS_ENFORCED`:
  `validate_announce` drops an unstamped announcement and `Transport` drops an unstamped path request when it is on), and the
  source calls this a "transition period" with "TODO: remove" comments; this document's earlier note said 1.6.2 is to
  enforce. A deck with stamps off would work with today's non-enforcing nodes and silently become unreachable for every node
  that enforces. Not checked: how Sideband and NomadNet behave (neither is installed here); only the RNS source was read.
  **What the bridge does instead (`Bridge.warm_stamp`):** once per run, as soon as a connection is on and allowed to send (at
  start, or when the user allows it), the destination's stamp is made in a background thread at the service's own low
  priority (`Nice=10`, `CPUQuota=200%`); with one on disk it is instant. The cost moves from "the first stranger who asks"
  to "the first time the user allows sending", once per identity. The log says `announcement stamp ready in N s`.
  Not measured on a board after this change.

## 5. Serial port lock (report 057 4.4)

For an `RNodeInterface`, `SerialInterface`, `KISSInterface`, `AX25KISSInterface` (and a hand-written
`RNodeMultiInterface`) whose `port` is a device path (not `ble://` or `tcp://`), the bridge, before RNS opens the port
(at start for interfaces that are on, and on `iface.enable` or a port change):

1. refuses a port another process has open (`/proc/<pid>/fd`), naming it: `busy`, "/dev/ttyACM0 is used by mesh-hop.
   Close that program first." with `holders` (pid, name);
2. opens the device and takes `flock(LOCK_EX | LOCK_NB)` (Mesh Hop's lock: refused when held), and keeps it while the
   interface is on, so the other serial apps see the port as busy; no `TIOCEXCL` (it would also keep RNS's own open
   out). A missing device is `not_found`, no access is `permission` with the `dialout` fix.

The bridge names itself `reticulum` (`/proc/<pid>/comm`), so other apps can say "used by reticulum". When a port is busy
at start, the bridge detaches that interface right after RNS started and reports the reason in `ifaces`; RNS has
already tried to open it by then (Mesh Hop sets `TIOCEXCL`, so that open fails; another holder without it would not be
protected: a known limit). In `client` mode (another program runs the shared instance) the bridge holds no locks.

## 6. Identity and LXMF

The identity file is created at the first start (`RNS.Identity`, written with `to_file`, mode 0600) and never
replaced; an unreadable file stops LXMF (logged), it is not overwritten. The router (`LXMF.LXMRouter`, storage in
`lxmf/`) registers that identity as its delivery identity with the stored display name; `announce` sets the
destination's `display_name` attribute and calls `LXMRouter.announce()`. The contacts, the message store and the messaging verbs are phase P2 (section 10); there are no
propagation-node verbs (P2 sends direct messages only). `--no-lxmf` runs without the router (no identity, no LXMF address).

## 7. Tests (PC)

```sh
sh apps/reticulum/bridge/tests/run_tests.sh             # Linux or WSL, about 6 minutes (P2 added three of them)
VENV=~/my-venv sh apps/reticulum/bridge/tests/run_tests.sh -k Integration     # or -k Messaging, -k Store
python3 apps/reticulum/bridge/tests/measure.py --seconds 60   # memory and CPU of an idle bridge
```

The script makes a venv (`~/.cache/reticulum-bridge-venv`, `--system-site-packages`) with the hash-checked wheels on
first use, then runs `unittest`. Everything runs in temporary folders, on 127.0.0.1, with its own `instance_name`;
nothing touches `~/.reticulum` or the real `XDG_RUNTIME_DIR`.

| File | Covers |
|---|---|
| `test_options.py` | the documented types only; RNode ranges of report 4.2, no defaults or examples on the radio fields; number, list, text and name validation; switches refused as options; required fields; the ID beacon pair |
| `test_rnsconfig.py` | the first-run file (content, 0600) and never-overwrite; RNS's search order; add, edit, remove touching one section only (comments, other sections, sub-sections kept); multi-line values refused; backup and atomic write keeping the mode |
| `test_portlock.py` | a pseudo terminal held by another process is refused; flock between two owners; missing device; the holder's name from a fake `/proc` ("used by mesh-hop") |
| `test_server.py` | socket and folder modes; one bridge per run folder; replies, errors, bad lines; request timeout with the second worker answering; events only to subscribers; stop answers first; stale socket |
| `test_name.py` | `identity.name`: get, set (trimmed, 0600), clear, refusals (empty, control characters, over 64 characters), the destination follows; no RNS needed |
| `test_store.py` | (P2, no RNS) the SQLite store: modes 0600, contacts, history paging and the text budget, conversations, unread and mark-read, states only move forward, `sending` at start becomes `failed`/`interrupted`, retention (per conversation and total, also after a reopen; the documented caps), batching (a new message is on disk at once, read marks and heard announces wait for `flush`), the heard list kept and capped, delete, reopen, a damaged file moved aside, Unicode |
| `test_messenger.py` | (P2, no RNS) strict address, name and text validation, the route rule (a known path over a listen-only interface is refused), the settings file (default off, 0600, bad values), the launcher alert against a fake `launcher.sock` (the two requests of the contract, no message text, one per 2 s, silent without a launcher, 254-byte limit, socket path rules), and incoming messages on a fake router (alerts off sends no request, on sends one; own address and empty messages ignored; events; unread) |
| `test_messaging.py` | (P2) **two real bridges** (RNS 1.6.1 + LXMF 1.3.1, `bridgekit.py`) linked by a loopback TCP interface, 15 ordered tests, about 160 s: verbs and limits (287 / 319 from the library), invalid addresses on every verb, refusals with no connection and with a listen-only one (message and announce, nothing stored, 0 announce and 0 path request bytes), the heard list from the other bridge's announce, add from heard, contacts, send and `delivered`, unread, mark-read and `bridge.state`, alerts off / on against a fake launcher, a 2000-byte message, history paging, a reply from a stranger, an offline contact (`failed` after 5 tries, no resend when it is back and has announced, manual retry delivers), taking the permission back mid-send, delete a conversation and remove a contact (with and without the conversation; the other side's copy untouched), restart persistence (contacts, messages, settings, heard list; a killed bridge: `failed`/`interrupted`, no resend), file modes, no message text in the logs, 500 messages and memory |
| `test_listenonly.py` | (0.3.1) a real multicast listener on the machine's link-local address and the real bridge: a Wi-Fi / LAN connection that is off sends **0** datagrams; listen only is refused (`iface.outgoing false` -> `invalid`) and switching on without `allow_sending` is refused (`confirm`); on it sends (the positive control); off again 0; a stored "on, listen only" connection (`enabled` and the hand-written `interface_enabled`) is switched off **before RNS reads the file** (0 datagrams during the start), the file says `enabled = no`, `notices.take` returns it once; the kinds' `listen_only` values. About 50 s; skipped without an IPv6 link-local interface |
| `test_integration.py` | the bridge (RNS 1.6.1 + LXMF 1.3.1) and a plain RNS node (`peer.py`) over loopback TCP: section 4's measurements, `identity.name` through the socket, the busy port through the daemon, identity export, hand edits kept, a second bridge refused (exit 3), stop by `service.stop` and by SIGTERM, and the first-run text started by RNS |

Measured on the PC (WSL2 Ubuntu 24.04, Python 3.12.3, 12 cores; indicative only, not Zero 2 W numbers): first-run
configuration, every interface off, 60 s idle: with LXMF VmRSS 41.4 MB (VmHWM 43.4 MB), idle CPU 0.12 %, 12 threads,
socket answering 0.41 s after start; with `--no-lxmf` 40.2 MB, 0.17 %, 11 threads, 0.30 s. **P2, 500 messages** (`test_messaging`
test 15, two bridges, 500 sent in 1.4 s and all delivered 3.4 s later, 509 messages stored on the receiving side): VmRSS 40.8 ->
41.9 MB on the sender and 42.7 -> 43.7 MB on the receiver (VmHWM 42.8 and 44.8 MB, budget 60 MB), `messages.db` 82-98 KB. Over 9 runs the peak after the burst was 43-46 MB; **once the receiver peaked at 60.1 MB** (a run of the package test on a busy machine; not explained, not seen again), so the test's bound is 64 MB and the idle budget of 60 MB is not proven for a burst of 500 on a loaded machine.
Delivery on loopback: under a second when both sides are connected; a message sent right after a connection was reloaded
(`iface.outgoing` reloads a TCP server and its client reconnects after about 5 s as a new interface) was delivered after 6-21 s
in 15 of 16 explored runs and failed after LXMF's tries in one (the tests wait for the link first, and a failed message is
retried by hand); an offline contact fails after 44-46 s.

## 8. Not done yet

- **Publishing the `reticulum` package:** it is built (section 9) but `app.json` still says `"draft": true`. Publishing
  means the board tests below, then removing `draft`, putting the tested debs in `packages/`, `make_registry.py
  --no-build`, and the README rows. Not done in part 2.
- The board tests of the service and of the app (`apps/reticulum/tests/BOARD-TESTS.md`: start time, memory and CPU on the
  Zero 2 W against the D2 budget, the unit's limits on a real user manager, an RNode on USB, the port lock against Mesh
  Hop, Settings > Apps listing the one package), AutoInterface between two machines, BLE RNodes, and the P2 service on a board
  (memory with stored messages on the Zero 2 W, the SD writes, a real radio, the real launcher's toast and beep). The app of
  phase P1b (Connections, Status, Settings) is built and tested on the PC with a fake bridge and against the real one; the P2
  screens (Contacts, Messages) are built since package 0.3.0 (section 11) and tested on the PC with the fake bridge (scenes) and against two real bridges; not on a board.
- Propagation nodes (`pn.*`) and NomadNet pages (`page.get`) of the report's protocol sketch: not built, not planned in P2.
- Stamps: the RNS defaults stay (on, section 4) and the announcement stamp is made in the background once sending is allowed; the board cost of that burst on a Zero 2 W and a Pi 3A+ is not measured after the change.

## 9. Package (P1a part 2)

Source: [`apps/reticulum/`](../../apps/reticulum/): `app.json` (`reticulum` 0.3.2, **draft**; 0.3.1 = the answers to the board QA; 0.3.0-bridge was the service with phase P2 and the app of 0.2.1; 0.3.0 adds the P2 screens, section 11), `root/` (the package tree),
`DEBIAN/` (maintainer scripts), `tools/vendor.py`, `build.sh`, `tests/package_test.sh`. One tree serves arm64 and amd64
(pure Python, no `root.amd64/`, no ELF file).

**Layout of the package**

| Path | What |
|---|---|
| `/opt/reticulum/bridge/reticulum_bridge/` | our bridge, 0.2.0 with P2 (a copy of `apps/reticulum/bridge/reticulum_bridge/`: `vendor.py` writes it, `build.sh check` compares it) |
| `/opt/reticulum/py/` | `rns` 1.6.1 and `lxmf` 1.3.1 unpacked **unmodified** from the wheels pinned with sha256 in `bridge/requirements.txt` (both `py3-none-any`; nothing architecture specific is vendored; `cryptography` and `pyserial` are the distribution's packages) |
| `/usr/lib/systemd/user/reticulum.service` | the user unit (below) |
| `/usr/share/doc/reticulum/` | `copyright` (Debian format), `LICENSE.rns`, `LICENSE.lxmf`: the Reticulum License text, taken from the notice at the head of `RNS/__init__.py` and `LXMF/Utilities/lxmd.py` of the wheels (they carry no licence file of their own) |

`depends: python3 (>= 3.10), python3-cryptography, python3-serial` (plus the `libc6 (>= 2.38)` that `build_deb.py` adds to
every package); `recommends: python3-bleak`; permissions `network`, `additional_hardware`, `background_service`; licence
`MIT AND BSD-3-Clause AND LicenseRef-Reticulum` (BSD-3-Clause for `RNS/vendor/configobj.py` and `validate.py`; the other
bundled third-party files are MIT; all are listed in `copyright`). The bridge is not put on `PATH`: the apps start it
through the unit.

**Maintainer scripts** (`DEBIAN/`): `postinst configure` compiles the bytecode of `/opt/reticulum/py` and `bridge`
(`python3 -m compileall -q -j 0`, errors ignored; under a second on the PC, not measured on a board); `prerm
remove|upgrade|deconfigure` stops `reticulum.service` of every logged-in user (`runuser` into `/run/user/<uid>`, best
effort) and deletes every `__pycache__` under `/opt/reticulum`; `postrm remove|purge` deletes stray bytecode and the
empty folders. The user's data (`~/.reticulum`, `~/.local/share/reticulum`) is never touched. Nothing starts or enables
the unit.

**The unit** (`reticulum.service`, `systemctl --user`): `Type=simple`, `ExecStart=/usr/bin/python3 -m reticulum_bridge`,
`PYTHONPATH=/opt/reticulum/bridge:/opt/reticulum/py`, `XDG_RUNTIME_DIR=%t`, `MALLOC_ARENA_MAX=2`, `Restart=on-failure`,
`RestartSec=3`, at most five starts a minute, `SuccessExitStatus=3` (a bridge already runs: success, no restart),
`RestartPreventExitStatus=2` (usage error); a clean exit 0 (`service.stop`, SIGTERM) stays stopped.

| Setting | Value | Where it comes from |
|---|---|---|
| `Nice`, `IOSchedulingClass`, `CPUQuota` | 10, idle, 200% | report 057 3.3 (mitigations to try) |
| `MALLOC_ARENA_MAX` | 2 | report 3.3 (the manual's value) |
| `MemoryHigh`, `MemoryMax` | 96M, 128M | **chosen for this package**, not a report figure: D2 budget 60 MB idle, measured 38-45 MB (8.1-8.3); the soft limit leaves room for traffic, the hard limit is about twice the budget; to be checked on the Zero 2 W |
| `TasksMax` | 64 | **chosen**: 10-12 threads measured; room for two request workers and the stamp worker processes (one per core) |
| `NoNewPrivileges` | yes | hardening that costs nothing here |

Whether a user manager enforces `Memory*` and `CPUQuota` depends on its cgroup delegation: not verified on a board.
`[Install] WantedBy=default.target` exists only so that `systemctl --user enable reticulum` works later.

**How apps start it (D6).** The unit is **not** enabled. An app that finds no `$XDG_RUNTIME_DIR/reticulum/bridge.sock`
(or a `bridge.state` whose `pid` is dead) runs `systemctl --user start reticulum`, polls for the socket (0.6 s on the PC;
the Zero 2 W is untested) and sends `hello`. Every app may do this, also when another already did (a second bridge exits
3, the unit counts that as success). The "Keep running" switch of the app (P1b, Settings) calls `systemctl --user
enable reticulum` / `disable`; stopping is `service.stop` or `systemctl --user stop reticulum`. Without a user manager
(`XDG_RUNTIME_DIR` unset, as over a bare ssh login) the bridge refuses to start (exit 2, section 1).

**The tile (P1b).** The package began as a service without a tile (0.1.0); the owner folded the app into it (decision of
2026-11-23: Settings > Apps lists only registry entries, and a tile-less service package is not a good listing). 0.2.1 is one
package, `reticulum`: the app binary `/usr/share/APPLaunch/bin/reticulum` (an ELF file per architecture: `root/` holds the
arm64 binary, `root.amd64/` the x86-64 one), `reticulum.desktop` (`X-Fullscreen`, `X-Portrait=480x640`,
`X-Background=freeze`), the tile icon, and everything above. The app starts the service when it opens
(`systemctl --user start reticulum`, then it waits for the socket), polls `status` and `ifaces` every 3 seconds while a page is
shown, and stops the service when it leaves unless Keep running is on (`systemctl --user enable` / `disable`). Source:
`apps/reticulum/src/`; README, tests and board protocol next to it.

**Build and test.** (The app: `sh apps/reticulum/build.sh cross|host|all`, see its README.) WSL for the wheels and the tests, Windows Python for the packages (`build_deb.py` takes the file mode
from the exec bit, and every file on `/mnt/c` has one; `build.sh package` refuses to run there):

```sh
sh apps/reticulum/build.sh vendor     # wheels into ~/cdk-build/reticulum/wheels (hashes checked); rewrites root/opt/reticulum/py, LICENSE.*, the bridge copy
sh apps/reticulum/build.sh check      # the committed root/ equals what vendor writes
python tools/make_registry.py --only reticulum --arch arm64 --out C:/somewhere/outside/packages    # and --arch amd64; SOURCE_DATE_EPOCH=1795392000 gives the same bytes
sh apps/reticulum/tests/package_test.sh /mnt/c/somewhere/reticulum_0.1.0_amd64.deb                  # in WSL
```

`package_test.sh` installs with `dpkg-deb -x` into a scratch root (nothing is installed on the machine) and checks the
contents and modes, `systemd-analyze verify --user`, a bridge started from that read-only layout (hello, status, a second
bridge exits 3, `service.stop` exits 0, nothing written under the tree), the maintainer scripts with `/opt` remapped, and
then `bridge/tests` unchanged against the installed `rns` and `lxmf` (105 tests since 0.3.1, about 4 minutes more). **Known flake:** the integration tests'
`rnstatus -j` helper sometimes returns an empty output (seen in about one call of six to twelve, also with the PC venv
and the source tree, so it is upstream's `rnstatus` and not the package; it also happened in the second package test run of 0.3.1 with
nothing else running, not only under load). Since 0.3.1 the helper asks again up to four times (1.5 s apart) and fails only when it stays
empty four times; before that it failed `test_02` or `test_04` of `test_integration` in a minority of runs. A second flake of the harness, seen once in four package runs: a bridge killed with SIGKILL (`test_13`) left RNS's forked proof-of-work workers holding its listening socket, and the client of the restarted bridge was reset by the dead one's socket; the test bridges now run in their own process group and a kill takes the group (under systemd the unit's control group does the same).

**First configuration.** Unchanged and checked on the installed copy: one `[[Default Interface]]` (`AutoInterface`) with
`enabled = no` and `outgoing = False`, `share_instance = Yes`, `enable_transport = No`, log level 3. Stamping stays at the
RNS defaults (on); see section 4.

## 10. Contacts and messages (phase P2, service side)

Owner decisions for P2 (2026-11-24), as built. Source: `bridge/reticulum_bridge/messenger.py` (verbs, rules), `store.py`
(the database), `alerts.py` (the launcher request). The app screens are section 11.

**Rules**

- **Direct messages only.** LXMF's `DIRECT` method (a link to the contact), no propagation node, stamps stay off. A
  message up to `single_packet_bytes` (287 bytes of text) fits one packet; a longer one is still sent (a link transfer,
  slower on radio) up to `max_text_bytes` (8192, the bridge's own sanity cap: one request line is at most 64 KiB).
- **Delivery states** of a message you sent: `sending`, `sent` (only when a relay took it: direct delivery never passes
  through it, so today it does not occur), `delivered` (the contact's LXMF stack got it; not "read"), `failed` (with
  `detail`). Received messages have state `received`. After LXMF's bounded tries (5 tries, 10 s apart: 44 s measured on
  loopback, link set-up included) a message ends `failed` / `undelivered`. **Retry is manual** (`messages.retry`); nothing
  is ever sent again by itself: not when the contact is heard again, not after a restart (what was `sending` at the start is
  marked `failed` / `interrupted`). `detail` of a failed message: `undelivered` (LXMF gave up), `no_path` (the contact's key
  could not be found: the bridge asked for the path five times), `no_transmit` (the connection that may send was switched off
  or set listen-only while the message was on its way: it is cancelled), `rejected`, `cancelled`, `interrupted`, `internal`.
- **A contact not heard yet** (a typed address): the bridge asks the network for the path itself (`sending` meanwhile), then
  hands the message to LXMF. A contact whose known path runs over an interface that is gone (a TCP client of ours
  reconnected: its connection is a new interface) is treated the same: the old path is dropped and asked for again.
  **Known library behaviour, not fixed:** a message sent to a contact whose bridge has just restarted (or whose connection was
  reloaded) can take 12-60 s on loopback and sometimes ends `failed` / `undelivered` after LXMF's five tries; the manual retry
  then delivers in about a second (measured in 9 restart runs: 7 delivered after 12-48 s, 2 failed after 49 and 61 s, and both retries delivered in 1 s). Closing the
  router's old link when the contact announced again did not change that, so it is not done.
- **Nothing transmits unless the user acts and a connection allows it.** `messages.send`, `messages.retry` and `announce`
  are refused with `no_transmit` unless an interface is enabled, online and `outgoing`, and, for a send, unless the way to
  the contact (when known) avoids an interface that may not transmit (report 057 8.1: `outgoing = False` does not stop a
  packet to a known path, so the bridge checks; when this program is a client of someone else's shared instance the owner of
  the interfaces decides). A refused request stores nothing. Taking the permission back (`iface.outgoing` false,
  `iface.enable` false) while a message is on its way cancels it (`failed` / `no_transmit`).
- **Local only.** Delete a conversation, remove a contact, rename and mark-read never send anything to the other side.
- **Contact** = address (exactly 32 hex digits; surrounding blanks dropped, upper case turned to lower, nothing else accepted:
  no `lxmf@`, no separators) + an optional name (1-64 characters, no control characters). A peer without a name is a contact
  too (the app shows "Anonymous" and the short address). Your own address is refused. A message from or to an address that
  is not a contact is allowed: it is a conversation with `contact: false`.
- **Heard list**: LXMF delivery announces seen (not our own): name = the announced display name (kept from an earlier
  announce when a later one has none), last heard time, hops, interface, `contact`. At most 500 entries, oldest dropped; kept
  in the database (written at most every 15 s), so it survives a restart.
- **Announce** stays manual (`announce`, the name set with `identity.name`); never automatic, none at start. Note: once an
  interface may transmit, RNS itself answers a path request for our address with an announce (section 4, "Not covered"):
  a contact who sends to a typed address of ours, or whom we send to, makes the other bridge answer with its announce.
- **Unread**: per conversation and in total (`status.unread`; also `unread=N` in `bridge.state`, for anything that wants to
  show "2 new" without a socket: tmpfs, rewritten when it changes).
- **Alerts**: setting `alerts` (default `false`; `settings.set`; file `settings.json` in the data folder, 0600). When it is
  on and a message arrives, the bridge asks the launcher for a toast and a beep (below). They fire only while the bridge
  runs (the app's Keep running switch governs that).
- **No message text in the logs** (they name ids, addresses and sizes) and none in the toast.

**Storage.** `<data>/messages.db` (SQLite through the stdlib `sqlite3`, WAL journal, `synchronous=NORMAL`), mode 0600 (its
`-wal` and `-shm` files too), folder 0700. Tables `contacts`, `messages`, `heard`. A new message, a contact change and a
final state (`delivered`, `failed`) are committed at once (WAL: no fsync per commit); read marks, heard announces and `sent`
are batched and committed every 15 s and at exit, so the SD card sees a few small writes per message, not one per event.
**Retention** (checked at each insert, oldest first): 2000 messages per conversation, 20000 in total, 500 heard entries; delete
a conversation to free more. A database SQLite cannot open is moved to `messages.db.corrupt-<time>` and a new one starts
(logged). Measured: 500 messages are about 80-100 KB on disk.

**Alerts and the launcher.** The contract has what is needed (`docs/APP-CONTRACT.md` section 14): the bridge sends two
requests to `$XDG_RUNTIME_DIR/applaunch/launcher.sock`, each on its own connection, one line, one reply:
`toast New message from <name>` (the contact's or heard name, else `Anonymous <first 8 hex digits>`; control characters
removed, 60 characters at most; the launcher shows it when it is in front) and `beep notify`. It uses no other request,
never sends the text, sends at most one alert per 2 s, drops the alert silently when there is no launcher or it answers
nothing (an older launcher, none running; the failure is remembered for 10 s), and runs on a thread of its own with a 0.5 s
timeout. All of it is in `alerts.py`; the environment variable `RETICULUM_BRIDGE_LAUNCHER_SOCK` replaces the socket path
(the tests use it with a fake launcher). The unit sets `XDG_RUNTIME_DIR=%t`, so the real path is found. Not verified: the real
launcher on a board (the PC tests use a fake socket that answers like the contract's examples).

**Verbs.** JSON as in section 3. `address` is the contact's LXMF address everywhere (a stranger's conversation too). Without
LXMF (`--no-lxmf`, or the store failed to open) every one answers `lxmf_off`.

| Verb | Params | Result |
|---|---|---|
| `contacts.list` | - | `contacts`: `[{address, name, added, unread, last_heard, heard_name}]`, named ones first |
| `contacts.add` | `address`, `name` (optional) | the contact. No `name`: the heard name when the address was heard, else none. `exists` when already there |
| `contacts.rename` | `address`, `name` (`""` or null clears it) | the contact |
| `contacts.remove` | `address`, `keep_conversation` (default **true**) | `removed`, `conversation_kept`, `messages_deleted`. With `false` the messages go too |
| `heard.list` | `limit` (1-500, default 200) | `count` (all kept), `heard`: `[{address, name, time, hops, interface, contact}]`, newest first |
| `messages.conversations` | - | `conversations`: `[{address, name, contact, count, unread, preview (80 characters, one line), last_time, last_dir, last_state, last_detail, last_id}]` (`last_detail`: why the last message failed, as in the message, so a list can say the same as the conversation), newest first, 200 at most; `unread_total` |
| `messages.history` | `address`, `limit` (1-200, default 50), `before` (a message id) | `address`, `name`, `contact`, `messages` (oldest first), `has_more`. The newest `limit` messages older than `before`; it also stops early at about 200 KB of text (then `has_more` is true) |
| `messages.send` | `address`, `text` | the message `{id, address, dir, time, text, state: "sending", detail, unread, bytes}`. Refusals: `invalid` (address, empty text, control characters, over 8192 bytes: `limit`), `no_transmit`, `lxmf_off` |
| `messages.retry` | `id` | the message, `sending` again (same id, same place in the history). `invalid` unless it is a failed message you sent; `no_transmit`; `not_found` |
| `messages.delete_conversation` | `address` | `address`, `messages_deleted`. Cancels what is still on its way to that address. The contact stays |
| `messages.mark_read` | `address` | `address`, `marked`, `unread_total` |
| `messages.limits` | - | `single_packet_bytes` 287, `link_packet_bytes` 319 (`LXMessage.ENCRYPTED_PACKET_MAX_CONTENT` and `LINK_PACKET_MAX_CONTENT` of the installed LXMF 1.3.1, read from the library at run time), `max_text_bytes`, `overhead_bytes` 112, `per_conversation`, `total`, `delivery_attempts` 5, `note`. The app counts the bytes of the UTF-8 text against `single_packet_bytes` |
| `messages.subscribe` | `on` (default true) | `subscribed`, `unread_total`. Switches on the message events below |
| `settings.get` | - | `{"alerts": false}` |
| `settings.set` | the keys to change: `alerts` (bool) | all settings. `invalid` for an unknown key or a non-boolean |

`status` has two more fields: `unread` (total) and `alerts`. `hello.verbs` lists the new verbs.

**Events** (lines with `event` and no `id`):

| Event | Sent to | Data |
|---|---|---|
| `heard` | `announces.subscribe` | `{address, name, time, hops, interface, contact}`, for every LXMF delivery announce heard (a repeat of the same address included) |
| `message` | `messages.subscribe` | the message (as in `messages.send`) plus `name`, `contact`, `unread_total`: a new message of either direction (yours too, so a second client sees it) |
| `message_state` | `messages.subscribe` | `{id, address, state, detail}`: `sent`, `delivered`, `failed`, or `sending` again after a retry |
| `unread` | `messages.subscribe` | `{address, unread: 0, unread_total}` after `messages.mark_read` marked something |
| `conversation_deleted` | `messages.subscribe` | `{address, unread_total}` |
| `contacts_changed` | `messages.subscribe` | `{address, action}`, `action` is `added`, `renamed` or `removed` |

**Examples** (addresses shortened here; real ones have 32 digits):

```
> {"id":1,"verb":"messages.limits"}
< {"id":1,"ok":true,"result":{"single_packet_bytes":287,"link_packet_bytes":319,"max_text_bytes":8192,"overhead_bytes":112,"per_conversation":2000,"total":20000,"delivery_attempts":5,"note":"..."}}
> {"id":2,"verb":"contacts.add","params":{"address":"BDDCFB7C...AB423"}}
< {"id":2,"ok":true,"result":{"address":"bddcfb7c...ab423","name":"Bob","added":1795560102.1,"unread":0}}
> {"id":3,"verb":"messages.send","params":{"address":"bddcfb7c...ab423","text":"Hello Bob"}}
< {"id":3,"ok":true,"result":{"id":1,"address":"bddcfb7c...ab423","dir":"out","time":1795560102.364,"text":"Hello Bob","state":"sending","detail":null,"unread":false,"bytes":9}}
< {"event":"message","data":{"id":1,"address":"bddcfb7c...ab423","dir":"out","time":1795560102.364,"text":"Hello Bob","state":"sending","detail":null,"unread":false,"bytes":9,"name":"Bob","contact":true,"unread_total":0}}
< {"event":"message_state","data":{"id":1,"address":"bddcfb7c...ab423","state":"delivered","detail":null}}
> {"id":4,"verb":"messages.send","params":{"address":"bddcfb7c...ab423","text":"Hello"}}        (no connection may send)
< {"id":4,"ok":false,"error":{"code":"no_transmit","message":"no interface that is up may transmit: switch one on and allow it to transmit first (iface.outgoing)"}}
> {"id":5,"verb":"messages.send","params":{"address":"nothex","text":"Hello"}}
< {"id":5,"ok":false,"error":{"code":"invalid","message":"address: 32 hexadecimal digits (0-9, a-f) expected","field":"address"}}
> {"id":6,"verb":"messages.conversations"}
< {"id":6,"ok":true,"result":{"conversations":[{"address":"8f694d83...","name":null,"contact":false,"count":1,"unread":1,"preview":"hello","last_time":1795560108.388,"last_dir":"in","last_state":"received","last_id":1}],"unread_total":1}}
> {"id":7,"verb":"messages.history","params":{"address":"8f694d83...","limit":2,"before":40}}
< {"id":7,"ok":true,"result":{"address":"8f694d83...","name":null,"contact":false,"messages":[{"id":38,"dir":"in","state":"received","unread":true,"...":"..."}],"has_more":true}}
> {"id":8,"verb":"settings.set","params":{"alerts":true}}
< {"id":8,"ok":true,"result":{"alerts":true}}
```

## 11. The app's contacts and messages screens (phase P2, package 0.3.0)

Source: `apps/reticulum/src/` (`core/chat.*` the plain-language model, `ui/rich_list.*`, `ui/ret_chat.cpp`, `ui/ret_convo.cpp`);
README and screenshots next to it. What the app does with the verbs of section 10, and where it chose:

- **Requests that can transmit** are five, each in one function and reached only from a button the user pressed: `iface.outgoing`
  with true (the confirmed Allow sending), `iface.enable` with `allow_sending` (the confirmed Switch on of a kind with no listen-only state), `messages.send` (Send or Enter in the field), `messages.retry` (a Retry button, or
  More > Send the failed messages again) and `announce` (the primary button of the Announce myself question). All carry
  `user_action` in the app's link, which refuses them otherwise (unit test `test_transmit_sources` reads the sources). No timer,
  poll, event or start-up code asks for any of them.
- **A kind with no listen-only state (Wi-Fi / LAN)**: its page has no *Allow sending* row, only *Switch on*, with the line "This kind of connection is on or off. While it is on, it may send."; *Switch on* asks first ("Switch on Wi-Fi LAN?": it makes this deck known to others on it and may send), and the confirmation is `iface.enable` with `allow_sending: true` carrying `user_action`, the fifth request that can transmit. At start the app asks `notices.take` and tells once, in a dialog, about a connection the service switched off. A kind with a *handshake* shows its listen-only page with the line that the link still says hello. Home's hint under the rows is "Nothing transmits until you switch a connection on." while all are off, "A connection is on and may send. Messages and your name go out only when you send them." when one may send, and "Connections are on and only listen. Messages and your name are not sent." otherwise.
- **Questions** (Delete conversation, Remove contact, Announce myself, Stop the service, Remove connection, Allow sending, Switch on Wi-Fi LAN): the focus starts on **Cancel**, so an Enter alone cancels; Right moves to the main button, Left / Up back (the arrows stop at the ends: Left from Cancel stays on Cancel), Tab and Shift-Tab go round and never leave the question, a letter does nothing, Esc cancels; both buttons show an amber focus ring; touch is unchanged. This departs from the toolkit dialog (Enter = the main button, buttons not focusable), which the guideline table still describes for plain notices (one button: Enter = OK).
- **`no_transmit`** is shown as *No connection is allowed to send*. A message that failed for that reason says so in the Messages list too, through the same function as in the conversation (`ret::failure_text`; the list row, which is about 25 characters wide, uses the short form `ret::failure_brief`: *Failed, sending was off*). The app also knows from `ifaces` (polled every 3 s, and asked when
  a conversation opens) whether any connection is on, online and allowed to send, and shows the blocked state before asking; when
  the answer says `no_transmit` anyway it shows it then. A blocked conversation keeps the typed text (the draft is kept while the
  page is left, in memory only).
- **Events** (`message`, `message_state`, `unread`, `conversation_deleted`, `contacts_changed`, `heard`) update the open page in
  place; the app subscribes with `messages.subscribe` and `announces.subscribe` after each (re)connect and then asks for the
  contacts, the conversations, the heard list and the limits again, because events may have been missed. `status.unread` is
  watched every 3 s as a second source. The open conversation is marked read when it opens and for each message that arrives
  while it is open.
- **Limits:** the counter uses `messages.limits` `single_packet_bytes` (asked once per run; 287 until it answers) and counts UTF-8
  bytes; the field stops at `max_text_bytes`. Past one packet Send stays enabled.
- **Local only:** Delete conversation (`messages.delete_conversation`), Remove contact (`contacts.remove` with
  `keep_conversation` false, as the confirmation says) and Rename send nothing to the other side.
- **The heard list** of the Contacts pages is `heard.list` (LXMF delivery announcements); Home's *heard N this hour* and the Status page
  still use the announcement stream of phase P1 (every kind of announcement).
- **Alerts:** Settings > New message alerts is `settings.set` `alerts`; the app never shows the toast itself (the service asks
  the launcher).
