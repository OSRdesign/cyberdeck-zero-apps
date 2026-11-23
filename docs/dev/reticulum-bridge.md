# Reticulum bridge (service core, phase P1a part 1)

The background service of the Reticulum apps (launcher report 057, architecture A, decisions D1-D20). One Python
process is the RNS **shared instance**, runs an **LXMF router** with the deck's own identity, and offers our native apps
a **JSON-lines Unix socket**. It is our code (MIT, written with AI assistance) on top of the public Python API of RNS and
LXMF; it is not a Reticulum implementation. The package bundles the two pinned upstream wheels unmodified (section 9). Source:
[`apps/reticulum/bridge/`](../../apps/reticulum/bridge/). Status: **packaged as a draft** (`apps/reticulum`, `draft: true`, section 9), **not published**; tested on a PC only
(sections 7 and 9). The board tests and the status app (P1b) are open (section 8).

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
`RETICULUM_BRIDGE_CONFIG`, `RETICULUM_BRIDGE_DATA`, `RETICULUM_BRIDGE_RUN`. Needs Python 3.10 or later, `rns` 1.6.1 and
`lxmf` 1.3.1 ([`requirements.txt`](../../apps/reticulum/bridge/requirements.txt), pinned with sha256), and the
distribution's `python3-cryptography` and `python3-serial`.

## 2. Files

| Path | What | Mode |
|---|---|---|
| Reticulum config folder: `--config`, else the first of RNS's search order that holds a `config` (`/etc/reticulum`, `~/.config/reticulum`, `~/.reticulum`), else `~/.reticulum` | `config`: written once when none exists (below), then changed only by the interface verbs; `config.bak`: the copy before the last change; `storage/`: RNS's own (transport identity, tables) | folder 0700, a new `config` 0600, an existing one keeps its mode |
| `--data`, default `$XDG_DATA_HOME/reticulum` (`~/.local/share/reticulum`) | `identity` (the deck's LXMF identity, separate from NomadNet's), `lxmf/` (the router's store), `display_name` | 0700 / 0600 (the bridge runs with umask 077) |
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
`lxmf_address`, `ifaces_up` (count), `ifaces_up_names` (`;`-separated), `started` (epoch). Trust it only while `pid` is
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
that sent `announces.subscribe`; a client must skip lines that have `event` and no `id`.

Error codes: `bad_request` (not JSON, no verb; `id` null), `unknown_verb`, `invalid` (with `field`), `not_found`,
`exists`, `not_supported`, `incomplete` (with `missing`), `busy` (with `port`, `holders`), `permission`,
`port_error`, `attach_failed`, `config_error`, `no_display_name`, `no_transmit`, `lxmf_off`, `confirm`, `identity`,
`timeout`, `internal`.

| Verb | Params | Result |
|---|---|---|
| `hello` | - | `bridge`, `protocol` (1), `rns`, `lxmf`, `verbs` |
| `status` | - | `pid`, `state`, `bridge`, `protocol`, `rns`, `lxmf`, `mode`, `transport` (false), `config_dir`, `identity_hash`, `lxmf_address`, `display_name`, `interfaces_total`, `interfaces_up` (names), `uptime` |
| `ifaces` | - | `interfaces`: every `[[section]]` of the config, hand-written ones too: `name`, `type`, `enabled`, `outgoing` (upstream default true when absent), `online`, `stats` (from `get_interface_stats()`: `status`, `rxb`, `txb`, `atxb` / `ptxb` (announce / path request bytes sent), `arxb`, `prxb`, `bitrate`, `clients`, `rssi`, `snr`, ... when RNS reports them), `options` (secrets left out), `secrets_set`, `subsections`, `supported`, `missing` (required options not set), `lock` (`held`, `not held`, or null when the port is not a device), `error` (why it is not up) |
| `iface.types` | - | `types`: `[{type, supported, port_lock, options, reason?}]`, `common` (options of every type), `switches` (`enabled` and `outgoing` of a new interface: both false) |
| `iface.add` | `name`, `type`, `options` | the new interface (as in `ifaces`), written with `enabled = no`, `outgoing = False` |
| `iface.edit` | `name`, `options` (`null` or `""` removes an option; absent = unchanged) | the interface, `reloaded` (bool, or null when it is off) |
| `iface.remove` | `name` | `removed` |
| `iface.enable` | `name`, `enabled` (default true) | the interface. On: refuses `incomplete`; takes the port lock (`busy`); writes `enabled = yes`, then `attach_interface()`; on failure writes `enabled = no` again (`attach_failed`). Off: writes `enabled = no`, `detach_interface()`, lets the lock go |
| `iface.outgoing` | `name`, `outgoing` (bool): **the transmit switch** | the interface, `reloaded` (`reload_interface()` when it is on) |
| `announces.subscribe` | `on` (default true) | `subscribed`, `recent` (the last 64 heard announces) |
| `paths` | `max_hops` (optional) | `count`, `paths` (up to 256 rows of `get_path_table()`, bytes as hex) |
| `announce` | `display_name` (optional; stored for later) | `announced`, `lxmf_address`, `display_name`, `via` (interfaces that may transmit). Refuses `no_display_name` until a name is set, `no_transmit` while no interface is on, online and allowed to transmit |
| `identity.info` | - | `identity_hash`, `lxmf_address`, `display_name`, `identity_file` |
| `identity.export` | `confirm: true` | `identity_hash`, `format` (`base32`), `key` (the 64-byte private key, `Identity.get_private_key()`; base32 of RFC 4648). Logged without the key |
| `service.stop` | - | `stopping`; the bridge answers, then exits cleanly |

Events: `announce` (`time`, `destination`, `identity`, `kind` (`lxmf`, `lxmf_propagation`, `nomadnet_node` or `other`,
from the destination hash of the announced identity), `name` (LXMF display name, NomadNet node name), `hops`,
`interface`), `ifaces_changed` (after every `iface.*` change), `stopping`.

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
  directly on such an interface was not sent either). The bridge's `announce` refuses in that state anyway.
- **A TCP interface sends its link set-up as soon as it is switched on, also with `outgoing = False`:** 236-240 bytes
  on connect with RNS 1.6.1 (afterwards each side's statistics name the other's implementation, version and transport
  id). That is part of switching an interface on; on an `RNodeInterface` report 057 8.3 measured 0 bytes in 30
  minutes with RNS 1.6.0 (not repeated with 1.6.1: no radio on the PC).
- The LXMF router with the deck's identity sends nothing by itself: 0 announce and 0 path request bytes in 10 s with
  `outgoing = True` before any `announce` request (and none in the listen-only phase).
- `announce` is the only verb that sends; it refuses while no interface may transmit, and the peer receives it, with the
  display name, once one may. On loopback the other node's announce handler saw it 0.5 to 18 s after the request (7
  runs; the bridge's announce byte counter moved at once, no CPU went to stamps; the cause of the spread was not
  investigated), so the tests wait up to 60 s.
- **Not covered by these rules:** once an interface may transmit, RNS answers what others ask of our own destinations
  (path requests, link requests, delivery proofs of an incoming LXMF message); such answers need the other side to know
  the deck's address (from an announce the user made, or the address shared by hand). report 057 8.1 showed that
  `outgoing = False` does not stop a packet to a known path, so the bridge must refuse send requests on listen-only
  interfaces when LXMF sending comes (P2). Not tested here.
- Stamps: the bridge calls `RNS.Reticulum(configdir=...)` with the constructor's defaults (`enable_stamping=False`);
  the configuration's `[stamps]` section is left to the user. RNS 1.6.2 is to enforce stamps (report 3.6): decide the
  bridge's stamp setting with that release.

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
destination's `display_name` attribute and calls `LXMRouter.announce()`. `lxmf.send`, the message store and the
propagation-node verbs are phase P2. `--no-lxmf` runs without the router (no identity, no LXMF address).

## 7. Tests (PC)

```sh
sh apps/reticulum/bridge/tests/run_tests.sh             # Linux or WSL, 30 s to 2 minutes
VENV=~/my-venv sh apps/reticulum/bridge/tests/run_tests.sh -k Integration
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
| `test_integration.py` | the bridge (RNS 1.6.1 + LXMF 1.3.1) and a plain RNS node (`peer.py`) over loopback TCP: section 4's measurements, the busy port through the daemon, identity export, hand edits kept, a second bridge refused (exit 3), stop by `service.stop` and by SIGTERM, and the first-run text started by RNS |

Measured on the PC (WSL2 Ubuntu 24.04, Python 3.12.3, 12 cores; indicative only, not Zero 2 W numbers): first-run
configuration, every interface off, 60 s idle: with LXMF VmRSS 41.4 MB (VmHWM 43.4 MB), idle CPU 0.12 %, 12 threads,
socket answering 0.41 s after start; with `--no-lxmf` 40.2 MB, 0.17 %, 11 threads, 0.30 s.

## 8. Not done yet

- **Publishing the `reticulum` package:** it is built (section 9) but `app.json` still says `"draft": true`. Publishing
  means the board tests below, then removing `draft`, putting the tested debs in `packages/`, `make_registry.py
  --no-build`, and the README rows. Not done in part 2.
- The status app (P1b), the board tests (memory and CPU on the Zero 2 W against the D2 budget, the unit's limits on a
  real user manager, an RNode on USB, the port lock against Mesh Hop), AutoInterface between two machines, BLE RNodes,
  and the P2 verbs (`lxmf.send`, `lxmf.messages`, propagation nodes).
- Stamps: left at the default (off) in the first configuration; decide in P2 (section 4).

## 9. Package (P1a part 2)

Source: [`apps/reticulum/`](../../apps/reticulum/): `app.json` (`reticulum` 0.1.0, **draft**), `root/` (the package tree),
`DEBIAN/` (maintainer scripts), `tools/vendor.py`, `build.sh`, `tests/package_test.sh`. One tree serves arm64 and amd64
(pure Python, no `root.amd64/`, no ELF file).

**Layout of the package**

| Path | What |
|---|---|
| `/opt/reticulum/bridge/reticulum_bridge/` | our bridge (a copy of `apps/reticulum/bridge/reticulum_bridge/`: `vendor.py` writes it, `build.sh check` compares it) |
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
3, the unit counts that as success). The "Keep running" switch of the status app (P1b) will call `systemctl --user
enable reticulum` / `disable`; stopping is `service.stop` or `systemctl --user stop reticulum`. Without a user manager
(`XDG_RUNTIME_DIR` unset, as over a bare ssh login) the bridge refuses to start (exit 2, section 1).

**Without a tile.** The package has no `.desktop` file and no icon. The launcher builds the home grid from the
`.desktop` files, so there is no tile. The Store backend (read in the launcher source, not run) installs with `dpkg`,
takes the installed state from `dpkg`, and its post-install step returns when the package has no `.desktop`
(`rewrite_desktop` in `appstore_native.cpp`), so Settings > Apps should list, install and remove it like any app. Not
verified on a board.

**Build and test.** WSL for the wheels and the tests, Windows Python for the packages (`build_deb.py` takes the file mode
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
then `bridge/tests` unchanged against the installed `rns` and `lxmf` (46 tests). **Known flake:** the integration tests'
`rnstatus -j` helper sometimes returns an empty output (seen in about one call of six to twelve, also with the PC venv
and the source tree, so it is upstream's `rnstatus` and not the package), which fails `test_02` or `test_04` of
`test_integration` in a minority of runs: run again.

**First configuration.** Unchanged and checked on the installed copy: one `[[Default Interface]]` (`AutoInterface`) with
`enabled = no` and `outgoing = False`, `share_instance = Yes`, `enable_transport = No`, log level 3. Stamping stays at the
default (off); decided in P2.
