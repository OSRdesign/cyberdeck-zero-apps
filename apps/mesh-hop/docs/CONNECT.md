# Mesh Hop: choosing a node (USB, Bluetooth LE, IP)

Mesh Hop 0.5.0 opens on **Choose a node**: one list of every way to reach a MeshCore companion node. Nothing connects by
itself; the node chosen last is highlighted. This page covers the TCP (Wi-Fi) link and the start page; the Bluetooth LE link
is in [`BLE.md`](BLE.md).

Markers: **[V]** verified in the source named next to it, **[I]** inferred (the reason is given), **[T]** shown by this
repository's tests.

## 1. The companion over TCP (MeshCore Wi-Fi / Ethernet interface)

Sources: MeshCore `main` 81dbae5 (2026-10-15), meshcore_py v2.3.17 (105f407), meshcore-cli v1.6.6 (dfcc74e).

| Fact | | Source |
| --- | --- | --- |
| A companion build with `ENABLE_WIFI_INTERFACE` (ESP32, RP2040) runs a TCP server on **port 5000** (`TCP_PORT`, a build flag); an Ethernet build on `ETHERNET_TCP_PORT` 5000 | V | `examples/companion_radio/main.cpp` l.39-47, l.250; `src/helpers/ethernet/SerialEthernetInterface.h` l.5-6 |
| **The framing is the USB one**: the board sends `'>'` + u16 LE length + payload ("use same header as serial interface so client can delimit frames"); it takes `'<'` + u16 LE length + payload. Frames longer than `MAX_FRAME_SIZE` (176) and frames of another type byte are skipped | V | `src/helpers/wifi/SerialWifiInterface.cpp` `checkRecvFrame` |
| **One client at a time**: a new connection closes the old one (the newest wins) | V | `SerialWifiInterface::checkRecvFrame` (`server.available()`, `client.stop()`) |
| No password, no pairing: anyone on the network can connect | V | `SerialWifiInterface.cpp` (no authentication step) |
| No keep-alive or heartbeat in the firmware; meshcore_py treats 5 commands without an answer as a dead link | V | `SerialWifiInterface.cpp`; meshcore_py `tcp_cx.py` `TCP_DISCONNECT_THRESHOLD` |
| The Wi-Fi network comes from build flags `WIFI_SSID` / `WIFI_PWD`, or the board's prefs (`set wifi.ssid` / `set wifi.pwd`, then a reboot); no mDNS: the user must find the board's address (the router's list) | V | `main.cpp` l.40-44, l.126-130; `MyMesh.cpp` l.2133-2150; no mDNS code in `src/` or `examples/companion_radio` |
| Some builds have BLE and Wi-Fi together (e.g. `Xiao_S3_WIO_companion_radio_ble`); the multi-interface writes every frame to all enabled interfaces | V | `variants/xiao_s3_wio/platformio.ini` l.153-163; `src/helpers/MultiSerialInterface.h` `writeFrame` |
| meshcore-cli connects with `-t host` (`-p port`, default 5000); meshcore_py uses the same `'<'` framing and scans for `'>'`, skipping junk | V | meshcore-cli README l.69-70; `tcp_cx.py` `send`, `handle_rx` |

**So TCP needed no new framing**: `TcpTransport` hands the bytes to the Client as the serial port does, and the Client's
`FrameParser` / `encode_frame` work unchanged [T: `test_nodes.inc`, a loopback server and the scripted companion].

### TcpTransport (`src/core/tcp_transport.*`)

* Non-blocking: `open()` starts a connect (a numeric address at once; a host name is looked up on a short-lived thread, its
  answer dropped when it comes too late) and returns `NotFound` until the connection is up, `Error` with the reason when it
  fails ("connection refused (no MeshCore Wi-Fi interface on this port?)", "network unreachable (is the deck on that
  network?)", "no answer in 5 s", "unknown host"). `poll()` (the app's timer) moves it on. No call blocks [T].
* A connect gives up after 5 s; the Client tries again (3 s later), the start page keeps showing the last reason
  (`last_failure()`) meanwhile.
* TCP_NODELAY; keep-alive after 10 s idle, every 5 s, 3 probes: a Wi-Fi link that died while idle is noticed in about 25 s
  [I: kernel keep-alive, not measured on the deck]. A closed connection is `read() == -1` ("the node closed the connection").
* Not tested with a real Wi-Fi companion yet: the simulator has a TCP mode (`tools/meshcore_sim.py --tcp 5000`).

## 2. The start page "Choose a node"

What the owner decided (2026-10-19): one list for every way; transport icon, the node's name, a second line; Bluetooth signal
as the launcher's bars; the node chosen last highlighted but **never connected by itself**; the Bluetooth scan only while the
page is shown, after a first-use warning; a PIN box; IP nodes added by hand and always listed; the header untouched.

### Rows (`src/ui/ui_nodes.*`, LVGL-free, unit tested)

| Row | Marker | First line | Second line |
| --- | --- | --- | --- |
| a USB board (`/dev/ttyACM*`, `/dev/ttyUSB*`, the port file) | dot | its node name once it answered, else the USB product name | `ttyACM0 · T1000-E` |
| a Bluetooth node (Nordic UART service or a `MeshCore-` name; paired first, then the strongest) | dot; signal bars while the scan hears it | its node name, else the advertised name without `MeshCore-` | `BLE · paired` / `BLE · not paired yet (PIN)` |
| a node added by IP address | dot | its name, else the address | `192.168.1.20:5000` |
| Bluetooth | dot (blue while scanning) | Scanning for Bluetooth nodes... / Rescan for Bluetooth nodes / Scan for Bluetooth nodes | how many were heard, or the keyboard warning |
| Bluetooth missing or off | none (grey, never selected) | Bluetooth is not available / Bluetooth is off | the reason |
| Add a node by IP address (last) | chevron | | A MeshCore node on Wi-Fi (TCP port 5000) |

The node being connected shows its state on its second line: `· Connecting...`, `· Pairing...`, `· Type the PIN` (amber),
`· Connected` (green), `Failed: <reason> · tap to retry` (red); the line under the title repeats it. Row affordance: a first tap
selects, a tap on the selected row acts (a node connects, a failed one retries, the connected one goes back to Home); keys:
Up / Down, Enter, `O` or Ctrl+O (or a 3 s hold) the options (Forget this node for a paired Bluetooth node, Remove IP node,
Info), `S` scan, `A` add by IP, `I` info. The header's Esc at the start asks **Quit Mesh Hop?** (the root, as Home); opened
from Home > Node it goes back to Home.

**Signal bars**: the count of `meshzero::signal_bars(rssi)`, the launcher's rule (`cp0_ui_status_signal_percent`: -100 dBm is
0 %, -50 dBm is 100 %, linear; `cp0_statusbar.c` lights bar *i* from 1, 30, 60, 80 %): **4 bars from -60 dBm, 3 from -70, 2
from -85, 1 above -100**. They are drawn as the toolkit's Wi-Fi rows draw them (`network_glyphs`: four rounded bars, 8 / 12 /
3 px for a 28 px block, heights 9, 15, 21, 28, scaled to the row's text size; green `0x33CC33`, grey `0x4D4D4D`); the toolkit
does not export that helper, so the app has the same geometry (`bar_geo` in `mesh_app.cpp`). BlueZ drops the RSSI when the scan
stops: a node not heard has no bars.

### Bluetooth on the page

* The Bluetooth backend starts when the page first shows (D-Bus only: the paired nodes are listed from BlueZ, no radio use).
* **First-use warning** before the first scan ever: "Bluetooth shares the radio with the keyboard and Wi-Fi: a Bluetooth
  keyboard may lag briefly during a scan", **Scan** / **Not now** (the safe answer is the default). The answer is kept
  (`nodes.txt`): asked once. Not now leaves USB and IP as they are and a row "Scan for Bluetooth nodes"; a scan started from it
  counts as Scan.
* After Scan: one 10 s LE scan per visit of the page (Rescan for another); it stops on a connect and when the page goes.
* Pairing: a tap on a node not paired yet pairs it through Mesh Hop's own BlueZ agent (never the default one); when BlueZ asks,
  the PIN box comes ("the 6 digits on its screen, or 123456 when it has no screen"; the on-screen keyboard's number pad without
  a keyboard). Cancel or Esc cancels the pairing. A wrong PIN ends in "Failed: Pairing failed (wrong PIN?) · tap to retry".

### What is kept (`~/.local/share/mesh-hop/nodes.txt`, `src/core/nodes.*`)

The nodes added by IP address (address, port, name), the node chosen last (`last`), the answer to the scan question
(`ble_scan yes|no`) and the node names learned once a node answered (`name usb:/dev/ttyACM0 OSR wio`; a USB name also records
the identity of the device that answered, `vid:pid/serial` from sysfs, and is shown only while that device is on the port: a line
without it, from an older version, is not trusted for a USB device with known ids). One file for the deck,
next to the per-board folders; the per-board data stays keyed by the board's public key, so a board reached over USB one day
and Bluetooth the next keeps its chats.

### Without touching the page

* `MESHHOP_PORT=/dev/...` (the simulator, the scripted runs, a udev alias) is a choice already made: the app connects to that
  port at once and opens on Home, as before 0.5.0.
* `MESHHOP_DEMO=<scenario>` plays the page with pretend USB ports and Bluetooth (`src/core/node_demo.*`: empty, usb, ble,
  first, pin, fail, ip, off, nobt) for the screenshots; nothing of the system is touched.

## 3. Tests

* `tests/unit/test_nodes.inc` (in `test_core`): `TcpTransport` against a loopback server (framing both ways in any chunking,
  refused, a host that never answers, a closed connection and a new one, a host name), the Client's whole handshake over TCP
  against the scripted companion, the node book; `tests/unit/test_nodes_ui.inc` (in `test_ui`): the rows, bars, states,
  captions and options.
* `tests/scenes/nodes-*.scene` (the shared harness, `build.sh host`): the page at the five sizes for every scenario of
  `MESHHOP_DEMO`.
* `tests/flows/nodes.script` (`nodes_run.sh`, in `run_flows.sh`): the simulator over TCP: Add a node by IP address, connect,
  Home, Home > Node, the 3 s hold, Esc back to Home.
