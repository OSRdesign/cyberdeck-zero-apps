# Home Assistant (`home-assistant`)

A client of your own Home Assistant server on the local network, for the cyberdeck: sign in by scanning a QR code with your
phone, then see your favourites and rooms with their live state, switch lights and switches, set a light's brightness, run
scenes and scripts, open covers and unlock locks after a confirmation, and read sensors. Native fullscreen app on the
launcher's `cp0_lvgl` (kind A of the [guidelines](../../docs/APP-GUIDELINES.md): its own pages and live data), MIT, version
0.1.0, **draft** (phase 1 of design 052a; not in the registry yet).

It is a pure client: the deck is not registered in Home Assistant as a device, nothing of the deck appears in Home Assistant
except the sign-in (a refresh token) in the user's profile. Everything stays on the local network: no cloud, no account other
than your Home Assistant user. Not affiliated with or endorsed by the Home Assistant project; the icon is the app's own (a
house with a signal, ink on a very light cool grey badge `#E6EBF0`; `tools/make_icon.py`).

## Screenshots

Rendered by the PC harness with the simulated Home Assistant (`build.sh host`, `HA_DEMO=<world>`), in `screenshots/`:
`01-connect-640x480`, `02-home-640x480`, `03-room-800x480`, `04-light-640x480`, `05-confirm-640x480`,
`06-settings-640x480`, `07-offline-640x480`, `08-connect-480x320`, `09-home-480x800` (portrait).

## Pages

- **Connect** (first run, after Sign out, or when Home Assistant refuses the sign-in): "Scan with your phone to connect to
  Home Assistant", the QR code (left, or on top on a portrait screen), the address line (`http://homeassistant.local:8123`),
  the status line, and the row `Server  homeassistant.local  >` across the bottom (the host whole beside the label;
  "..." only for a host too long for the row). The status line says
  "Waiting for the phone...", then "Signing in...", "Connected as <user>" (green) and Home follows after 1.5 s; or which step
  failed: "Cannot reach Home Assistant at ... Check the server address." (the server), "Home Assistant refused the sign-in
  (access_denied). Scan the code again." (the login on the phone), "Nothing came back from the phone yet. If it could not
  open the page, use Home Assistant's IP address as the server address." (after 3 minutes without an answer: the phone could
  not open the page or could not reach the deck). Esc ends the app.
- **Server address**: one field (the toolkit's `TextField`; the fallback keyboard shows only when no keyboard is connected, the
  shared rule). `homeassistant.local:8123`, `192.168.1.5:8123`, `http://...`, `https://...` are understood; without a port, the
  scheme's own (80 / 443), as in a browser. A wrong address is named under the field. Enter keeps it, Esc goes back.
- **Home**: FAVOURITES (starred on the deck, in the order starred; "No favourites yet" before the first), ROOMS (Home Assistant's
  areas, by name, with the count of their entities; "No room" collects the entities without an area), then Settings.
- **Room**: the room's entities, by name. Typing on a keyboard filters the room ("FILTER: ..."); Backspace and Esc undo it. No
  on-screen keyboard for the filter (the fallback keyboard is only for the server address).
- **Entity page**: the state, the read-only attributes (room, last change; climate temperatures and action; a cover's position),
  for a dimmable light the Brightness slider row, for a cover Open and Close, and Add to / Remove from favourites.
- **Settings**: Server (the address with its port when it fits, else the host) and Signed in as (read only), Sign out
  (asks first; Home Assistant revokes the sign-in, the
  file goes, the Connect page shows).

The line under the title: **Connected** (green), **Connecting...** / **Reconnecting...** (amber), **Offline** (red, after three
failed tries), with "last update HH:MM" while not connected. The rows keep their last values in grey; no spinner animation
(CPU on the Zero 2 W). A command sent while not connected says so on the line instead of queueing.

### Rows and what they do

| Domain | Row | Enter, or a tap on the highlighted row's name | Right, or a tap on its value (right 40 %) |
|---|---|---|---|
| light | Action (dot) | on / off ("..." until Home Assistant reports it) | entity page (brightness) |
| switch, input_boolean | Action | toggle | entity page |
| scene, script | Action (blue while a script runs) | run ("Done" for 1.5 s) | entity page |
| lock | Action | asks "Unlock Front door?" / "Lock Front door?" Yes / Cancel | entity page |
| cover | Open (chevron) | its page: Open, Close; garage, gate and door covers ask first | the same |
| sensor, binary_sensor, climate | Info (grey value) | entity page | entity page |

Phase 1 domains only: light, switch, input_boolean, scene, script, sensor, binary_sensor, cover, lock and climate (read only).
Hidden entities and Home Assistant's configuration / diagnostic entities are left out of the rooms. No alarm panels. Values:
"On", "21.5 C" (the toolkit's fonts are ASCII: no degree sign), "Open" / "Closed", "Detected" / "Clear" (a binary sensor's
word from its class), "Heat, 21.4 C", grey "Unavailable" / "Unknown".

### Keys and touch

| Action | Touch | Keys |
|---|---|---|
| Move | tap a row (it highlights), drag to scroll | Up, Down, Home, End, Page Up, Page Down |
| Act on a row | tap the highlighted row (on its name) | Enter |
| Entity page of a row | tap the highlighted row on its value side | Right |
| Brightness | tap the highlighted slider row left or right of the knob (10 %) | Left / Right (10 %) |
| Filter a room | | type letters; Backspace |
| Confirm / cancel | Yes / Cancel | Enter / Esc |
| Back, quit at Home or Connect | the header's Esc | Esc (Left in a list) |

The app takes every key itself (the toolkit's keypad path is intercepted, as Mesh Hop does), so that letters typed into a
room's filter never move the list; outside a room the navigation letters work as in the other apps.

## How it works

- **Pairing** (spike probe 3, run by the owner against Home Assistant 2026.10.4): the deck checks the server with
  `GET /auth/providers` (no login, answers 200), opens a listener on its LAN address and a free port, and shows the QR code of
  `<server>/auth/authorize?client_id=http://<deck>:<port>/&redirect_uri=http://<deck>:<port>/callback&state=<random>`
  (Home Assistant accepts a client id on a local IP address and a redirect to the same host and port). The phone logs in and
  comes back to the listener with a one-time code; the deck exchanges it at `POST /auth/token`. The listener answers only
  while the Connect page shows, only `/callback` with the right state counts, and it closes after the sign-in.
- **Tokens**: the access token (30 minutes) stays in memory; the refresh token and the exact client id are kept in
  `~/.config/home-assistant/credentials.json` (mode 0600, folder 0700, written atomically), never logged or shown. Each
  connect uses a fresh access token (refreshed first when less than a minute is left) and the token is refreshed 2 minutes
  before it ends while connected.
- **Failed logins count toward Home Assistant's IP ban** (when enabled): an `auth_invalid` with a token older than a minute
  is retried once with a new token; any other refusal (a second `auth_invalid`, a 4xx from `/auth/token`) stops, revokes what
  is left and goes back to the Connect page. It never loops.
- **WebSocket** (`ws://<server>/api/websocket`, libcurl 8.11 or newer, `CURLOPT_CONNECT_ONLY` 2): auth, `supported_features`
  (coalesced messages), `get_config` (temperature unit), `auth/current_user`, `config/area_registry/list`,
  `config/device_registry/list` (only areas and names kept), `config/entity_registry/list_for_display`, then one
  `subscribe_entities` for the favourites plus the room or entity shown (spike verdict: never the whole state). Changes arrive
  as `a` / `c` / `r` and are applied to the model; only the attributes the pages show are kept.
- **Link**: `SO_RCVBUF` 64 KB (spike: libcurl otherwise copies whatever the kernel holds), messages put back together from
  frames by the app (at most 4 MB; a larger one is dropped and logged, the link stays), libcurl answers the server's pings
  while the thread reads, a `ping` after 30 s without a message and the link dropped after 15 s without an answer, TCP keepalive,
  IPv4 only (Home Assistant also publishes an IPv6 link-local address without a zone). Reconnect after 2, 4, 8, 16, 32, then
  60 s. After the launcher stopped the app (multitasking freeze) it reconnects at once.
- **Threads**: one connection thread owns the engine (a state machine with no I/O, `src/core/engine.*`), libcurl, the
  listener and the files, sleeping in `poll()`; the LVGL thread sends commands and copies the view when it changed (polled
  every 200 ms, the lists rebuilt at most about twice a second). libcurl is opened at run time (`dlopen("libcurl.so.4")`, as
  Mesh Hop opens libsystemd), so the shared build is unchanged; its constants are checked against the real headers by
  `tests/integration/check_curl_abi.cpp`.

## Requirements

- Home Assistant on the same network (tested against a fake server only; the owner's is 2026.10.4), a user to sign in with
  (a dedicated user without administrator rights is best: see below), and a phone on the same Wi-Fi that can open the
  server's address (`homeassistant.local` or its IP address).
- `libcurl4t64` (Debian 13 and Ubuntu 24.04 or later; the boards already have it through `curl`) with WebSocket support
  (curl 8.11 or newer; Ubuntu 24.04's 8.5.0 has none: the app then says so on the Connect page).
- A screen of 480x320 or larger (`X-MinSize=480x320`; portrait `X-Portrait=480x640` in PDA mode). Touch and keyboard both
  complete.
- Launcher with full-screen app support. `X-Background=freeze`: hidden, the app is stopped; shown again, it reconnects.

### On the Home Assistant side

1. Settings > People > Users > Add user (for example `deck`): **Administrator off**; "Can only log in from the local
   network" on unless you use the deck away from home. Home Assistant has no per-entity permissions in its UI: this user
   still controls every entity, so the locks and garage covers ask on the deck.
2. Log in as that user on the phone when you scan the code.
3. Lost deck: log in to Home Assistant as that user > profile > Security > Refresh tokens, and delete the entry named
   `http://<deck address>:<port>/`. The deck's open link closes at once; it shows the Connect page and never retries.

## Install

Settings > Apps once published. Today (draft): `sudo dpkg -i home-assistant_0.1.0_arm64.deb` (or `_amd64.deb` on a PC);
remove with `sudo dpkg -r home-assistant`.

## Files the app keeps

| Path | What |
|---|---|
| `~/.config/home-assistant/settings.json` | the server address, and `"qr_use_ip": true` to put Home Assistant's IP address in the QR code instead of its name (for a phone that cannot resolve `homeassistant.local`; no row for it in phase 1) |
| `~/.config/home-assistant/favourites.json` | the starred entity ids |
| `~/.config/home-assistant/names.json` | the favourites' names as last seen, written when one changes: a start without a connection shows them |
| `~/.config/home-assistant/credentials.json` | server, client id, refresh token; mode 0600; deleted by Sign out or when refused |

Reset: Sign out in the app (revokes the token), then delete the folder. `HA_CONFIG_DIR` moves it (tests).
`HA_DEMO=<world>` runs the app against the simulated Home Assistant, with no network (the harness's worlds: `connect`,
`home`, `empty`, `big`, `connecting`, `reconnecting`, `offline`).

## Known limits (phase 1)

- Not tested against a real Home Assistant, nor on a board: only against the simulated server (unit tests, scenes) and the
  fake server (integration test). The owner's board protocol is `tests/BOARD-TESTS.md`.
- https servers: libcurl checks the certificate against the system CAs; a self-signed certificate is refused (no pinning
  yet). Plain http is accepted on any address.
- No discovery (mDNS) yet: the address is typed or the default is used. No token-file or typed-token fallback.
- No climate control, media players, cameras, fans, buttons, alerts, Assist or wallboard (later phases).
- Without a connection at start, the favourites show the names cached by the last session (`names.json`); an entity starred
  but never seen online shows its entity id's words.
- Rows are the toolkit's 7 mm rows: about four per screen on the deck. A long name is cut with dots. The Server row is
  fitted the other way ("Server" stays whole, the value gives way): `homeassistant.local` is whole at 640x480 and up and on
  the real 480x320 panel (the Pi 3A+'s 3.5", 6.1 px/mm); only a 480x320 screen at the deck's 11.3 px/mm (the harness's
  default, no board has one) shortens it with "...".
- The Brightness slider row is the toolkit's; its bar is a picture (not clickable): the row is the touch target.
- The QR code holds a plain-http address of the deck: a phone browser in HTTPS-only mode refuses the redirect.

## Credits and licences

- The app: MIT, 2026 OSRdesign.
- [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library) by Project Nayuki, C++ version 1.8.0, MIT,
  vendored unchanged in `src/core/qrcodegen.{cpp,hpp}` (its notice is in the files and in the package's copyright file).
<!-- branding-allow-begin: credits -->
- The LVGL based `cp0_lvgl` runtime of the CyberdeckZero Launcher (derived from the MIT-licensed launcher of M5Stack Technology
  CO LTD) and LVGL (MIT), linked into the binary.
<!-- branding-allow-end -->
- libcurl (curl licence) is not shipped: the system's `libcurl.so.4` is opened at run time.
- Home Assistant's APIs as documented on contributors.home-assistant.io and read in its core sources (study 052 and its spike);
  the JSON reader is adapted from Mine Field's (same repository).

## Build and test

```
sh apps/home-assistant/build.sh host            # unit tests (core: 500+ checks) and the scenes at ten sizes
sh apps/home-assistant/build.sh cross           # the aarch64 binary into root/
sh apps/home-assistant/build.sh cross --arch amd64   # the x86-64 binary into root.amd64/ (Mesh Hop's README: private dev root)
sh apps/home-assistant/build.sh integration     # the real client against tests/fake_ha/fake_ha.py (needs a WebSocket libcurl)
python tools/make_registry.py --only home-assistant --out <folder outside packages/> [--arch amd64]
```

- `src/core/` (no LVGL): `json` (the reader), `text` (URL / form encoding, the server address), `model` (registry, rooms,
  a / c / r), `format` (the words of the rows), `engine` (the client state machine), `store` (the files), `qr` + `qrcodegen`
  (the code), `sim` (the simulated Home Assistant of the tests and the harness), `curl_api` + `net` (libcurl at run time, the
  WebSocket, HTTP, the listener), `runner` (the connection thread). `src/ui/`: the pages.
- `tests/*.cpp`: the unit tests (`test-home-assistant`). `tests/scenes/*.scene`: every page and state at 480x320, 640x480,
  800x480, 1024x600, 1280x720, 1280x800, 720x720 and the portrait 480x800, 480x640, 800x1280; the notice under 480x320; Esc;
  no contract; the multitasking lifecycle. `tests/fake_ha/fake_ha.py` (Python stdlib) and `tests/integration/`: pairing,
  2,000 registry entries, 100 subscribed entities, 1 KB frames, service calls, server pings, a 5 MB message, a TCP reset, an
  expired token, the token refresh, Sign out with revoke, a sign-in deleted on the server; every issued token is searched for
  in the logs. `tests/BOARD-TESTS.md`: the owner's protocol.

## Changes

- 0.1.0 (draft): phase 1 of design 052a: QR pairing, Home (favourites, rooms), rooms with a keyboard filter, toggles, scenes,
  scripts, sensors, the light's brightness, locks and garage covers with a confirmation, covers' Open / Close, favourites,
  Sign out with revoke, connection states with back-off and greyed values.
