# Reticulum: tests

Four layers, all on a PC (WSL or Linux), none needs a radio or a board. The board protocol is [BOARD-TESTS.md](BOARD-TESTS.md).

| Layer | Command | What it covers |
|---|---|---|
| Unit tests of `src/core/` | `sh apps/reticulum/build.sh host` (the `test-reticulum` step) or `tests/test_core.cpp` | the JSON reader; the link to the bridge against an in-process server on a temporary socket (replies, errors, events while idle, a silent bridge, a bridge that hangs up, reconnecting, **the transmit guard**); the model (what the bridge's answers become, the words and tones of every state, option rows, plain errors, the serial-port list, the page stack); the sources (**no network vocabulary in a string of the UI**; **the only request that allows sending is the one under the "Allow sending?" dialog's primary button**; a new connection carries no `enabled` or `outgoing`); the package files (`.desktop` equals `AppSpec`, `app.json`, the unit) |
| Scenes without a bridge | `build.sh host` (`tests/scenes/*.scene`, run by `_common/build_app.sh`) | the service cannot start (`nobridge`, with `/bin/false` as systemctl and no socket), started with no launcher contract (`nocontract`) |
| Scenes against the fake bridge | `build.sh host`, or `sh tests/run_scenes.sh [--sizes WxH,..] [scene ...]` | every page at 640x480, 480x320, 800x480, 720x720, 1280x720 (and 1024x600, portrait 480x800, 480x640, 800x1280 where the scene names them); layout checks after every shot; keys, touch, the on-screen keyboard rule, the PDA band, the multitasking lifecycle |
| The real bridge | `sh tests/run_real.sh` | the headless app against the real bridge (RNS 1.6.1, LXMF 1.3.1 from the bridge tests' venv) and a plain Reticulum node on 127.0.0.1: add an Internet connection, switch it on (it connects), off again; the configuration file and the bridge log are checked afterwards |

The service has its own suite: `sh apps/reticulum/bridge/tests/run_tests.sh` (52 tests with `identity.name`), and the package test
`tests/package_test.sh DEB` (contents, unit, the bridge from the installed layout, maintainer scripts, the bridge tests again).

## The fake bridge

[`fake_bridge.py`](fake_bridge.py) speaks protocol 1 (docs/dev/reticulum-bridge.md) from canned data: one independent world per
socket, one socket per harness size (`DIR/<WxH>/bridge.sock`). It imports the real bridge's `reticulum_bridge.options`, so the connection
types and the refusal of a bad value are the real ones; everything else is memory. Scenarios: `fresh` (the first configuration), `mixed`
(a LoRa radio on and listening, a server with a problem, two people heard), `radio` (a radio with only its port set), `busyport`. Every
request goes to a log; `check_requests.py` reads it after each scene and fails the scene if the app asked for `announce`, `lxmf.send` or
`paths`, if any `iface.outgoing` allowed sending beyond what the scene's header says (`# transmit-requests: N`, default 0), or if
`iface.add` carried `enabled` or `outgoing`. The scene headers also say which verbs must have been asked
(`# expect-verbs:`) and what the fake `systemctl` must or must not have seen (`# systemctl-has:` / `# systemctl-lacks:`).
**The radio numbers in the fake world and in `radio.scene` are synthetic placeholders** inside the ranges the bridge accepts; they are not a
band plan and not a recommendation.

[`fake_systemctl.sh`](fake_systemctl.sh) stands in for `systemctl --user` (the app runs the program named by `RETICULUM_SYSTEMCTL`):
it logs the calls, keeps an `enabled` / `disabled` state per size, and can fail `start` or `enable` on request (`# fake-start-rc:` ...).

## Test hooks

Two environment variables exist for the tests and are never set on a deck: `RETICULUM_BRIDGE_RUN` (the folder of the bridge's socket;
`{canvas}` becomes the harness size, `$APPLAUNCH_CANVAS`, so every size of a scene has its own world) and `RETICULUM_SYSTEMCTL` (the
program run in place of `systemctl`).

## The scenes

| Scene | Fake world | What it does |
|---|---|---|
| `scenes/nobridge`, `scenes/nocontract` | none | the service does not start: message, Try again, Esc; started by hand without the launcher's contract |
| `walk-mixed` | mixed | Home, Connections, Status (channel, noise, heard), Settings, leaving stops the service |
| `radio` | radio | a LoRa radio from `not set` to listening: refusal while incomplete, port list and typed path, a value out of range, synthetic values, yes/no pick list, Allow sending asked and refused by Esc |
| `sending` | mixed | Allow sending: Cancel does nothing, Enter on the primary button makes the one request that allows sending, turning it off needs no question |
| `busyport` | busyport | switching on a port another program holds: the bridge's words in a notice |
| `remove` | mixed | Remove asks first; Cancel keeps it; confirming removes it and the page closes |
| `add` | fresh | the kind list, More..., a form (suggested name, Esc leaves), Internet filled and saved off, LoRa saved with every field empty |
| `settings` | fresh | Keep running on and off (the fake unit), the name, My address, Stop the service and Start the service |
| `keep` | fresh | Keep running on: leaving does not stop the service |
| `start`, `startfail`, `slow` | fresh / mixed | the service still starting (progress, then up by itself); `systemctl start` failing (the reason and Try again); a slow bridge (the screen answers at once, keys work while a request is out) |
| `touch`, `keyboard`, `osk`, `lifecycle`, `band` | mixed / fresh / radio | every page by touch alone; every page by keys alone; the on-screen keyboard rule; hidden, shown, closed; the handwriting band in a text page |

The on-screen keyboard's Enter key has no text label, so the touch scene reaches a text page by taps and the typing and Enter of the
`osk` and `radio` scenes use the harness's key injection.
