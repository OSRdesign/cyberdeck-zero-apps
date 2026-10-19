# Mesh Hop over Bluetooth LE

Stage A of "BLE first" (2026-10-19): the link to a MeshCore **BLE companion** board, behind the app's existing transport
interface, with its tests and a probe for the deck. Stage B (0.5.0, the same day): the start page **Choose a node** uses it
(see [`CONNECT.md`](CONNECT.md), which also covers TCP); section 5.6 below says what was built of the proposal. Not tried on a
real board yet: section 7 is the protocol for the first test (and `tests/BOARD-TESTS.md` N1-N10 for the app).

Markers used below: **[V]** verified in the source named next to it, **[D]** read on the deck (zero7.local, 2026-10-19,
read-only), **[I]** inferred (not verified; the reason is given).

Sources read: MeshCore `main` 81dbae5 (2026-10-15), meshcore_py v2.3.17 (105f407), meshcore-cli v1.6.6 (dfcc74e),
Adafruit_nRF52_Arduino `master` (Bluefruit52Lib, the nRF52 BLE library MeshCore builds on), BlueZ tag 5.82 (the deck's
version).

## 1. The MeshCore companion over BLE

### 1.1 Service, characteristics, name

| Fact | | Source |
| --- | --- | --- |
| Nordic UART service `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | V | `src/helpers/esp32/SerialBLEInterface.cpp` l.7; rp2040 `NUS_UUID(0x01)`; nrf52 uses Bluefruit's `BLEUart` (the same service); `docs/companion_protocol.md` "BLE Connection"; meshcore_py `ble_cx.py` l.26 |
| RX `6e400002-...`: app to board, **write**. esp32: write (with response) only; nrf52 Bluefruit: write and write-without-response | V | esp32 l.49 `PROPERTY_WRITE`; Bluefruit `BLEUart.cpp` `_rxd.setProperties(CHR_PROPS_WRITE \| CHR_PROPS_WRITE_WO_RESP)` |
| TX `6e400003-...`: board to app, **notify** (esp32 also read) | V | esp32 l.45; Bluefruit `_txd.setProperties(CHR_PROPS_NOTIFY)` |
| The advertisement carries the NUS UUID; nrf52 puts the name in the scan response | V | esp32 l.53 `addServiceUUID`; nrf52 l.197 `Advertising.addService(bleuart)`, `ScanResponse.addName()` |
| The name is `MeshCore-` + the node name (`BLE_NAME_PREFIX`), or `MeshCore-` + the MAC in hex when the name is `@@MAC` | V | `examples/companion_radio/MyMesh.h` l.65-66; `SerialBLEInterface::begin` |
| A firmware build is EITHER the USB companion OR the BLE companion: a BLE board has no companion protocol on USB | V | meshcore-cli README l.15; `variants/*/platformio.ini` (`*_companion_radio_ble` vs `_usb` envs) |
| The board takes one central at a time and stops advertising while connected (a phone connected to it hides it from the deck, and the reverse) | V | esp32 l.233 (advertising stopped on connect); Bluefruit `begin(prph_count = 1)` |

### 1.2 Frames, MTU, writes, notifications

| Fact | | Source |
| --- | --- | --- |
| **One companion frame per GATT write and per notification, WITHOUT the serial wrapper** (`<` / `>` + u16 length). The payload is the same as on USB | V | `companion_protocol.md` "Frame Handling" (l.849); esp32 `onWrite` queues each write as one frame, `checkRecvFrame` sends one frame per `notify()`; nrf52 `onBleUartRX` reads what one write brought as one frame; meshcore_py `handle_rx` hands each notification to the reader |
| The firmware does **not** join writes: a frame cut into two writes is read as two (broken) frames. So the app must never cut a frame | V | the same `onWrite` / `onBleUartRX` |
| `MAX_FRAME_SIZE` is **176** now ("+4 for transport codes (region scoping)"); 172 before. Longer writes are dropped by the firmware | V | `src/helpers/BaseSerialInterface.h` l.5; esp32 l.119 |
| esp32 sets its MTU to `MAX_FRAME_SIZE` (176): 173 bytes per write or notification | V | esp32 l.29 `BLEDevice::setMTU(MAX_FRAME_SIZE)` |
| nrf52 allows an MTU of 247 (`BANDWIDTH_MAX`): 244 bytes | V | nrf52 l.138; Bluefruit `configPrphBandwidth` -> `configPrphConn(247, ...)` |
| BlueZ (as GATT client) asks for 517; the result is the smaller of the two | V / I | BlueZ `BT_ATT_MAX_LE_MTU 517`; the min is the ATT rule |
| The nrf52 Bluefruit `notify()` cuts a value longer than MTU - 3 into several notifications; a frame then arrives in pieces (only with an MTU below 179, i.e. when no MTU exchange happened) | V | Bluefruit `BLECharacteristic::notify` (`max_payload = conn->getMtu() - 3`, loop) |
| esp32: one `setValue` + `notify` per frame; a frame of 174-176 bytes cannot fit its 173 byte notification (the Arduino BLE library truncates or fails) | I | not read in the esp32 Arduino library; rare (Mesh Hop's frames from the board are at most about 170) |
| BlueZ turns a `WriteValue` longer than MTU - 3 into a long write (prepare + execute); the server joins it | V / I | BlueZ `src/gatt-client.c` `characteristic_write_value`; whether the esp32 / nrf52 stacks accept a long write on RX is not verified. Mesh Hop's longest command is 172 bytes (`kMaxTxFrame`), within esp32's 173 |
| Use write **with** response; `AcquireWrite` (a write-without-response socket) is impossible on esp32 (no such property on RX) | V | `companion_protocol.md` "BLE Write Type"; meshcore_py `write_gatt_char(..., response=True)`; BlueZ doc `AcquireWrite` "relies on write-without-response" |
| Writes one at a time: meshcore_py serialises them ("two overlapping writes drop the link") and drops the link after a 10 s write | V | `ble_cx.py` l.34, `_write_lock` |
| esp32 spaces its notifications 60 ms apart; queues: esp32 4 frames, nrf52 12 frames | V | esp32 l.189 `BLE_WRITE_MIN_INTERVAL 60`; `SerialBLEInterface.h` `FRAME_QUEUE_SIZE` |
| One command at a time, wait for the answer (5 s), match by code: as over USB | V | `companion_protocol.md` "Command Sequencing" |
| Same first commands as USB: notifications on, then `APP_START`, `DEVICE_QUERY` ... | V | `companion_protocol.md` "Connection Steps" |

### 1.3 Pairing, PIN, bonding, reconnection

| Fact | | Source |
| --- | --- | --- |
| Both characteristics need an **encrypted, authenticated (MITM) link**: pairing with a passkey is required before any frame | V | esp32 l.46/50 `ESP_GATT_PERM_*_ENC_MITM`, l.33 `ESP_LE_AUTH_REQ_SC_MITM_BOND`, l.32 `setStaticPIN`; nrf52 l.184 `SECMODE_ENC_WITH_MITM`, `setMITM(true)`, `setPIN`; rp2040 `ATT_SECURITY_AUTHENTICATED` |
| The board sends nothing until the link is secured: esp32 sets `deviceConnected` only on authentication success, nrf52 only in `onSecured`; `writeFrame` returns 0 before | V | esp32 `onAuthenticationComplete`; nrf52 l.47-51 |
| A failed pairing: the board disconnects (esp32: and advertises again 1 s later) | V | esp32 `onAuthenticationComplete`; nrf52 `onPairingComplete` |
| The board **displays** the passkey or has a fixed one; the deck **types** it (nrf52 IO capability DisplayOnly) | V | nrf52 `setIOCaps(true, false, false)`; esp32 static PIN |
| **The PIN**: build flag `BLE_PIN_CODE` = **123456** in every companion BLE env; `ble_pin` in the board's prefs (0 = not set) overrides it; when it is not set, the build has a display class, a display is really there and `BLE_PIN_CODE == 123456`, the board draws a **random 6-digit PIN at each boot** and shows it ("Pin:NNNNNN" while no app is connected); otherwise 123456 | V | `examples/companion_radio/main.cpp` l.188-202; `ui-*/UITask.cpp` "Pin:%d"; `variants/*/platformio.ini` |
| Seeed XIAO nRF52840 BLE build: `DISPLAY_CLASS=NullDisplayDriver`, whose `begin()` is false: **123456**. XIAO S3 WIO BLE build: `SSD1306Display`: random PIN on the OLED when one is attached | V | `variants/xiao_nrf52/platformio.ini`; `NullDisplayDriver.h`; `variants/xiao_s3_wio/platformio.ini` |
| `CMD_SET_DEVICE_PIN` (37): u32 LE, 0 or 100000-999999, saved in the prefs; used from the next boot | V / I | `MyMesh.cpp` l.1839-1850 (saved); "next boot": `begin()` reads it once at start |
| `DEVICE_INFO` bytes 4-7 carry the **configured** `ble_pin` (0 by default), not the random one in use | V | `MyMesh.cpp` l.1064 |
| Bonding: esp32 asks for a bond (Secure Connections); Bluefruit keeps bonds in flash | V / I | esp32 l.33; Bluefruit behaviour from its documentation, not read |
| Reconnection: esp32 advertises again 1 s after a disconnect; nrf52 `restartOnDisconnect(true)` and an advertising watchdog every 10 s; the doc: "may disconnect after periods of inactivity", reconnect with backoff | V | esp32 l.11; nrf52 l.204, `BLE_HEALTH_CHECK_INTERVAL`; `companion_protocol.md` l.84 |
| nrf52 asks for a 15-30 ms connection interval, latency 4, 2 s supervision timeout once secured | V | nrf52 l.12-15, `onSecured` |
| meshcore_py / meshcore-cli: scan filter "name starts with MeshCore", pairing is left to the OS agent (`-P`; "pair your device first (using bluetoothctl)") | V | `ble_cx.py` l.139; meshcore-cli README l.41, l.68 |
| A board re-flashed or reset forgets its bonds while BlueZ keeps the old keys: the encryption then fails until the deck forgets the device too | I | general BLE behaviour; the exact BlueZ error text is not known (BleLink matches "Authentication" / "Key Missing" / "Insufficient", see 5.3) |

## 2. BlueZ on the deck

### 2.1 What is there [D]

* Debian 13 (trixie), kernel 6.18.50+rpt-rpi-v8, **BlueZ 5.82** (`bluez 5.82-1.1+rpt2`), `bluetoothd` active, `main.conf` all
  defaults. Controller **BCM43438** on the UART (`hci_uart_bcm`, `brcm,bcm43438-bt`), Bluetooth 4.2 (version 0x08), public
  address 88:A2:9E:B1:F6:FD, powered, pairable, not discoverable, roles central and peripheral; rfkill unblocked.
* Paired: only the **M4 keyboard** (45:41:BB:00:03:37): a **Bluetooth Classic (BR/EDR) HID** keyboard (public address,
  class 0x002540, HIDP / SDP UUIDs, `hid-generic 0005`), paired, bonded, trusted; asleep (not connected) during the check.
  Report 017 assumed an LE keyboard: it is not.
* D-Bus policy (`/usr/share/dbus-1/system.d/bluetooth.conf`): **every user may call `org.bluez`** (`context="default"`), and
  root (bluetoothd) may call `org.bluez.Agent1` on any client. `osrde` is not in a `bluetooth` group and needs none.
* The launcher (`cyberdeckzero`, user osrde) talks to BlueZ through GDBus (`cp0_bluez_dbus_client.cpp`) and registers
  `/com/cardputerzero/applaunch/agent` (KeyboardDisplay) as the **default agent**; its Settings > Bluetooth types PINs.
* Libraries: `libdbus-1-3` 1.16.2, `libsystemd0` 257.13, `libglib2.0-0t64` 2.84.4 (gio included), their `-dev` packages,
  `libbluetooth3`; **no sdbus-c++**. `busctl tree org.bluez` also shows a `/org/bluez/test` node (origin unknown; not ours).

### 2.2 The D-Bus calls the link uses (BlueZ 5.82 `doc/org.bluez.*.rst`) [V]

| Step | Call | Notes |
| --- | --- | --- |
| state | `ObjectManager.GetManagedObjects` on `/`; signals `InterfacesAdded` / `InterfacesRemoved`, `Properties.PropertiesChanged` (path_namespace `/org/bluez`); `NameOwnerChanged` of `org.bluez` | adapter, devices (Address, Name, RSSI, UUIDs, Paired, Bonded, Connected, ServicesResolved), GATT objects |
| scan | `Adapter1.SetDiscoveryFilter({Transport: "le"})`, `StartDiscovery`, `StopDiscovery` | "le" = LE scan only, no BR/EDR inquiry; the discovery belongs to our D-Bus connection and ends with it |
| agent | `AgentManager1.RegisterAgent(/org/meshhop/agent, "KeyboardOnly")`, never `RequestDefaultAgent` | BlueZ asks the agent **of the process that called Pair()** (`src/device.c` `pair_device` -> `agent_get(sender)`; the bonding's agent in `new_auth`), else the default one: the launcher keeps its role |
| PIN | `Agent1.RequestPasskey(device) -> u` (also `RequestPinCode -> s`); `Cancel`; `Release`; Display* acknowledged; `RequestConfirmation`, `RequestAuthorization`, `AuthorizeService` refused | the reply is sent when the user has typed it; BlueZ waits 60 s (`src/agent.c REQUEST_TIMEOUT`) |
| pair | `Device1.Pair()` (connects too), `CancelPairing()` | before any GATT access: a GATT access on an unpaired link would make the kernel raise the security and BlueZ would then ask the **default** agent (the launcher's) for the PIN |
| connect | `Device1.Connect()`, `Disconnect()` | `Connect`/`Pair` turn BlueZ's own LE auto-connect on; `Disconnect` turns it off again for a device that is not trusted (`src/device.c`); the link never sets `Trusted` |
| notify | `GattCharacteristic1.AcquireNotify({}) -> (fd, mtu)` on TX, else `StartNotify` + `PropertiesChanged Value` | the socket gives one notification per `read()` and hangs up when the link drops |
| write | `GattCharacteristic1.WriteValue(frame, {type: "request"})` on RX, one at a time | longer than MTU - 3: BlueZ makes it a long write |

### 2.3 Which library [recommendation]

| | libdbus-1 | GLib / GIO GDBus | sdbus-c++ | **sd-bus (libsystemd0)** |
| --- | --- | --- | --- | --- |
| On the deck [D] | yes (1.16.2) | yes (2.84.4) | **no**: a new package | yes (257.13; systemd itself needs it) |
| Cross sysroot (`SDK/github_source/static_lib_v0.0.7`) [V] | headers under `usr/include/dbus-1.0` + arch dir | headers under `glib-2.0` + arch dir | headers only (report 017) | `usr/include/systemd/sd-bus.h` (default path) and `libsystemd.so` |
| Shared build change (`apps/_common/scons/main.SConstruct` has no per-app libraries or include paths) | include paths + link | include paths + 3 libraries | everything | **none when loaded with dlopen** |
| Event loop | `dbus_connection_read_write_dispatch(0)` or watches; verbose iterators | needs `g_main_context_iteration` from the UI tick and GVariant | its own thread or `processPendingEvent` | `sd_bus_process()` without waiting, fits the 20 ms UI poll; compact read/append API; fd passing and deferred agent replies built in |
| Used nearby | launcher `ext_components/bluectl` (no GATT) | launcher `cp0_bluez_dbus_client.cpp` | | |

**Recommendation: sd-bus, opened at run time** (`dlopen("libsystemd.so.0")`, the 28 functions through a table; the
sd-bus types are declared from its public C ABI, so no systemd header is needed to build). Reasons: present on every
systemd Debian (always on the deck); the smallest code for GetManagedObjects / dictionaries / fd passing / the agent; no
GLib main loop in an LVGL app; **no change in the shared build** and no new `NEEDED` entry: without libsystemd, a system bus
or bluetoothd, the link says "Bluetooth is not available" and USB works as before. The alternative, if decision D1 (a
per-app library list in the shared SConstruct) is taken anyway: link `-lsystemd` directly.

**Package Depends**: unchanged (the app binary needs nothing new). Proposed for stage B: `Recommends: bluez` (as decided for
Net Toolbox, D2: optional). `libsystemd0` need not be listed (dlopen; present on any systemd system); add
`Depends: libsystemd0` only if it is linked instead.

## 3. Coexistence with the keyboard and Wi-Fi

* One chip: the BCM43438 does Wi-Fi 2.4 GHz and Bluetooth 4.2 on one radio, time-shared [I: the usual design of this combo
  chip]. The M4 is **BR/EDR** HID [D]; a MeshCore board is LE. The controller can hold both links at once [I: dual-mode
  4.2 controller; to prove on the deck].
* The scan of 2026-10-19 (`bluetoothctl --timeout 15 scan on`, 15 s, run twice because the first output was lost on my
  side) ran while the M4 was **asleep**: it says nothing about key latency.
* Risks, largest first:
  1. **Discovery**: active LE scanning takes air time from the HID link and Wi-Fi. `Transport: "le"` avoids the BR/EDR
     inquiry (the heavier one for a Classic keyboard). The link scans only on a user request (10 s) or to find an
     unknown address (15 s), never in the background, and stops as soon as the board is found.
  2. **Connect to an absent board**: an LE connection attempt keeps the controller listening for that address until it
     times out. The reconnect backoff (2, 4, 8, 16, 32, 60 s) bounds it; BlueZ's own auto-connect (on after Connect/Pair)
     also runs a passive background scan for a bonded board that dropped [V, `src/device.c`], until our `Disconnect`.
  3. **Traffic**: a full contact download (350 x 148 bytes, about 52 KB) at esp32's 60 ms per frame takes about 21 s of
     steady notifications at a 15-30 ms connection interval (nrf52). Key latency during it is to be measured.
  4. **Agents**: Mesh Hop's agent is never the default one, so pairing the keyboard again (launcher Settings > Bluetooth)
     is untouched; our agent only answers pairings Mesh Hop starts.
  5. **A crash** of the app leaves BlueZ auto-connecting the bonded board (no `Disconnect` was sent): the board stays held
     by the deck (a phone cannot connect) until the app runs again or `bluetoothctl disconnect`.
* **Safe sequence**: leave the keyboard alone (no remove, trust, block, power or discoverable change, no default agent);
  scan only on request, LE only, at most 10-15 s, and stop it before pairing; pair once with the PIN; then connect by
  address without scanning; reconnect with the backoff; on exit `Disconnect` (the bond stays).

## 4. Measured facts still missing (for the first board test)

MTU and notify mode with the real board; time to connect and to the first answer; whether BlueZ asks the passkey through
our agent (the probe prints it); reconnect time after a power cycle; key latency of the M4 during a scan and during a
contact download; the exact BlueZ error when a bonded board lost its keys.

## 5. Design (stage A, done)

### 5.1 Layers (`src/core/ble/`, no LVGL, one thread, nothing blocks)

| File | What |
| --- | --- |
| `ble_types.hpp/.cpp` | UUIDs, `kNamePrefix`, `kBoardMaxFrame` 176, `BleDevice`, `BleEvent`, the `IBleBackend` interface, address helpers |
| `bluez_backend.hpp/.cpp` | `BluezBackend`: the real backend (section 2), sd-bus loaded at run time |
| `fake_ble.hpp` | `FakeBle`: a scripted backend for the tests (header only, not in the app) |
| `ble_framing.hpp/.cpp` | `wrap_from_device`, `ToDeviceUnframer`, `plan_write`, `NotifyAssembler` |
| `ble_link.hpp/.cpp` | `BleLink`: the state machine (5.2) |
| `ble_transport.hpp/.cpp` | `BleTransport : ITransport` |

`BleTransport` keeps the `Client` untouched: received frames are wrapped as `>` + length (the Client's `FrameParser` reads
them), the Client's `<` frames are taken apart and each goes out as ONE GATT write. `open()` returns `Ok` only once the link
is Connected (paired, notify subscribed), `NotFound` on the way, `Error` when Failed (`last_error()` says why). `read()`
returns -1 once per lost link, so the Client drops and reopens as for an unplugged cable; `close()` ends only the Client's
session, the link reconnects by itself; `shutdown()` disconnects. `poll()` advances the link while the Client is not
calling (the app calls it from its timer, before `Client::poll`).

### 5.2 State machine (`BleLink`)

| State | Enters when | Leaves to |
| --- | --- | --- |
| Idle | start, `disconnect()` | `connect(address)` |
| Connecting (Start) | BlueZ not answered yet | the next step; Failed(NoBluetooth) on "no adapter" / "not running" / 10 s |
| Scanning | BlueZ does not know the address | Pairing / Connecting when seen (scan stopped); Failed(NotFound) after 15 s |
| Pairing | not paired: `Device1.Pair` | `pin_needed` when our agent is asked: `submit_pin()` / `cancel_pairing()`; Connecting on success; Failed(PairingFailed) on a wrong PIN, a refusal or 95 s; Failed(PairingCancelled); Reconnecting when out of reach |
| Connecting (Connect, Services, Gatt) | paired: `Connect`, then `ServicesResolved`, then the NUS characteristics and notify | Connected; Failed(NotCompanion) without the NUS service; Reconnecting on an error or a time-out (35 / 20 / 15 s) |
| Connected | notify subscribed (MTU known) | Reconnecting on a disconnect, a hung-up notify socket, Bluetooth off, the device removed, or a write not answered in 10 s |
| Reconnecting | a failed try or a lost link | the next try after 2, 4, 8, 16, 32, 60, 60 ... s; forever, like the USB port |
| Failed | NoBluetooth, AdapterOff, NotFound, PairingFailed, PairingCancelled, NotCompanion, BondLost (2 authentication errors in a row on a bonded board) | only a user action: `connect()` again, `disconnect()` |

Writes: a queue of 32 frames, one in flight, `plan_write` refuses empty and longer than 176 and counts long writes. Reads:
`NotifyAssembler` passes every notification through when the MTU is 179 or more; below, a full piece (MTU - 3 bytes) may
continue and a shorter one ends the frame, a full piece followed by nothing ends after 50 ms, and a frame never grows past
176 bytes (so esp32's full 173-byte notifications stay apart from the next frame).

### 5.3 What is inferred in the design

The BondLost words ("Authentication", "Key Missing", "Insufficient", "PIN or Key") and the count of 2; the reassembly rule
for small MTUs (Bluefruit cuts at MTU - 3; nothing in MeshCore marks the end of a frame); that a long write is accepted
(never needed: Mesh Hop's commands are at most 172 bytes).

### 5.4 Device list, PIN, remembered device (for stage B)

* **List**: `BleLink::start_scan()` (LE, 10 s) and `candidates()`: devices with the NUS UUID or a `MeshCore-` name; paired
  first, then by RSSI; each with name, address, RSSI (only while scanning), paired. Meshtastic and other LE devices are
  left out (the deck's scan saw two Meshtastic nodes).
* **PIN**: when `status().pin_needed`, a box asks for 6 digits ("123456 unless the board's screen shows another"); typed on
  the keyboard, or on the launcher's on-screen keyboard under the shared rule (`apps/_common/osk_policy`) when no keyboard is
  there. The PIN is never stored (BlueZ keeps the bond).
* **Remembered**: a global file `~/.local/share/mesh-hop/link` (`transport=usb|ble`, `ble_address=`, `ble_name=`) read at
  start to build a `SerialTransport` or a `BleTransport` (and `MESHHOP_BLE=AA:BB:...` like `MESHHOP_PORT`), and per board
  (`boards/<12 hex>/link.txt`, the address that reached this board last) so the list can name known boards and "Other
  boards' data" can say "Bluetooth: MeshCore-OSR T1000".

### 5.5 Stage B code (built in 0.5.0)

The app owns the transport (`std::unique_ptr<ITransport>`: a `NullTransport` until a node is chosen, then a `SerialTransport`,
a `BleTransport` over the app's own `BleLink`, or a `TcpTransport`) and rebuilds the Client with it when the user chooses
another node. `ITransport::poll()` (a no-op for USB) moves a pairing or a TCP connect on, called before `Client::poll()`; the
app ticks the `BleLink` too (the scan of the start page). What is remembered is `nodes.txt` (CONNECT.md), not the `link` file
proposed below. Not changed: `OpenStatus` (BLE still answers `NotFound` while on its way) and the Client's own texts (the
start page shows the BLE states from `BleStatus` itself).

### 5.6 Stage B UI (the proposal of stage A; what was built instead is in CONNECT.md section 2)

The owner chose a start page instead of a Settings > Connection page: one list of USB, Bluetooth and IP nodes, no
auto-connect, the scan only while the page is shown after a first-use warning, a Home row Node to come back. Kept from the
proposal below: the PIN box, the Forget action (the only `RemoveDevice`), the row affordance, the status of the step on the
row and the line under the title.

* Settings > BOARD gets a **Connection** row (chevron, value "USB" or "Bluetooth: MeshCore-OSR T1000") that opens a
  **Connection** page with the standard header: a row per way (USB cable; each remembered or paired MeshCore board) where a
  tap on the selected row switches to it (action dot, blue while connecting), a **Find Bluetooth boards** row (action dot,
  blue during the 10 s scan) whose results appear as rows (name; grey information: signal and "paired"), and on a paired
  board **Forget this board** (asks Yes / No; the only `RemoveDevice`, for BondLost). Every row tagged with the row
  affordance (chevron / grey / dot).
* The PIN box: the launcher's text dialog, digits only, 6 at most, "Type the PIN of MeshCore-OSR T1000 (123456 unless the
  board shows another)", Cancel / OK.
* The status line under the header shows the BLE step ("Bluetooth: pairing", "reconnecting in 8 s", "Board not found").
* Open for the owner: the row's place and words; whether the app connects Bluetooth at start by itself (proposed: yes,
  to the remembered board only, never a scan); the signal shown as dBm or bars; a warning before a scan ("the keyboard may
  hiccup", as Net Toolbox does); whether the Connection page also offers the USB port choice.

## 6. Tests

* `tests/unit/test_ble.inc` (in `test_core`, run by `build.sh host` / `tests/unit/run_tests.sh`): framing, `plan_write`,
  `NotifyAssembler` (MTU 247, 23, 176), the states over `FakeBle` (no Bluetooth, no adapter, adapter off, search scan,
  not found, the PIN path, wrong PIN, cancel, agent Cancel, time-out, out of reach while pairing, not a companion,
  reconnect and backoff, BondLost, `disconnect()`, Bluetooth off), writes and notifications, `BleTransport`, and the real
  `Client` over Bluetooth against the scripted companion (`FakePeer`) at MTU 247 and with every frame cut at MTU 23, through
  a loss and a reconnect. 154 checks (test_core: 2469 before, 2623 now), also clean under ASan + UBSan.
* `tests/ble/run_mock.sh` (51 checks): `BluezBackend` + `BleLink` + `BleTransport` through `tools/ble_probe` (PC build, ASan + UBSan)
  against `tests/ble/mock_bluez.py` (BlueZ 5.82 and one board on a private D-Bus): info, scan, pairing through our agent,
  bonded, wrong PIN, StartNotify fallback, MTU 23, the board walking away and coming back, unknown address.
* `tools/ble_probe` on the deck: section 7.

## 7. First real-board test (the owner runs it; Mesh Hop closed)

* **Board**: the deck's scan saw two BLE companions advertising the NUS UUID: **MeshCore-OSR T1000** (EC:EB:45:39:28:2D,
  random static address, -54 dBm) and **MeshCore-OSR wio** (D7:C5:07:5B:55:EB, random, -43 dBm). Start with the T1000
  (Seeed T1000-E, nRF52840, no screen: PIN **123456** unless `ble_pin` was set). No phone may be connected to it.
* Build: `wsl -e sh .../apps/mesh-hop/tools/build_ble_probe.sh cross <dir>`; copy `ble_probe` to the deck
  (`scp ble_probe osrde@zero7.local:/tmp/` then `chmod +x /tmp/ble_probe`).
1. `/tmp/ble_probe info` - adapter, our agent registered, the boards BlueZ knows (no radio use).
2. `/tmp/ble_probe scan --seconds 8` - type on the M4 meanwhile; note any lag.
3. `/tmp/ble_probe connect EC:EB:45:39:28:2D --pin 123456 --log` - expect `CONNECTED ... paired now`, `DEVICE_INFO`,
   `SELF_INFO`, `PASS`; note the MTU, the notify mode and the times.
4. The same again: `already paired`, no PIN.
5. `/tmp/ble_probe connect EC:EB:45:39:28:2D --listen 300 --reconnect` - switch the board off for 20 s and on again
   (expect `link lost`, `state reconnecting`, `RECONNECTED after N s`), send it a message from another node (a `0x83`
   push), type on the M4 during the whole run.
6. If wanted afterwards: `bluetoothctl remove EC:EB:45:39:28:2D` (forgets the pairing).
