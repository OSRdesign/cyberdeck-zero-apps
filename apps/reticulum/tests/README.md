# Reticulum: tests

Four layers, all on a PC (WSL or Linux), none needs a radio or a board. The board protocol is [BOARD-TESTS.md](BOARD-TESTS.md).

| Layer | Command | What it covers |
|---|---|---|
| Unit tests of `src/core/` | `sh apps/reticulum/build.sh host` (the `test-reticulum` step) or `tests/test_core.cpp` | the JSON reader; the link to the bridge against an in-process server on a temporary socket (replies, errors, events while idle, a silent bridge, a bridge that hangs up, reconnecting, **the transmit guard**); the model (what the bridge's answers become, the words and tones of every state, option rows, plain errors, the serial-port list, the page stack); the sources (**no network vocabulary in a string of the UI**; **the only requests that allow sending are the ones under the "Allow sending?" and "Switch on Wi-Fi LAN?" dialogs' primary buttons**; **the questions open with Cancel focused and take their own keys**; a new connection carries no `enabled` or `outgoing`); the package files (`.desktop` equals `AppSpec`, `app.json`, the unit) |
| Scenes without a bridge | `build.sh host` (`tests/scenes/*.scene`, run by `_common/build_app.sh`) | the service cannot start (`nobridge`, with `/bin/false` as systemctl and no socket), started with no launcher contract (`nocontract`) |
| Scenes against the fake bridge | `build.sh host`, or `sh tests/run_scenes.sh [--sizes WxH,..] [scene ...]` (`LAUNCHER_TREE`, `LAUNCHER_SRC` as in `build_app.sh`) | every page at 640x480, 480x320, 800x480, 720x720, 1280x720 (and 1024x600, portrait 480x800, 480x640, 800x1280 where the scene names them); layout checks after every shot; keys, touch, the on-screen keyboard rule, the PDA band, the multitasking lifecycle |
| The real bridge | `sh tests/run_real.sh` | the headless app against the real bridge (RNS 1.6.1, LXMF 1.3.1 from the bridge tests' venv) and a plain Reticulum node on 127.0.0.1: add an Internet connection, switch it on (it connects), off again; the configuration file and the bridge log are checked afterwards. `sh tests/run_real_chat.sh` (phase P2): two real bridges linked over loopback, the app on one of them: add a contact, send, delivered, an offline contact fails and a manual retry delivers, delete a conversation |

The service has its own suite: `sh apps/reticulum/bridge/tests/run_tests.sh` (about 100 tests since P2: store, messenger, and two real bridges exchanging messages), and the package test
`tests/package_test.sh DEB` (contents, unit, the bridge from the installed layout, maintainer scripts, the bridge tests again).

## Running the package test (and why it used to fail now and then)

`package_test.sh` ends with the bridge's own suite (107 tests) from the installed layout, so one run takes **4 to 5 minutes**
(`test_messaging` alone about 2.5 minutes: it waits for real LXMF tries and a proof-of-work stamp). Rules:

- Run it on a **quiet machine**, one run at a time, in the foreground (under `flock /tmp/wsl-build.lock` in this repository's WSL).
  The tests start up to six bridge processes plus 12 stamp workers each: leave **2 GB of free memory** (each bridge is 45 to 60 MB,
  the stamp workers burst to all cores for 10 to 30 s). Do not run a build at the same time, and leave no polling loop running.
- Expect `test_03` to wait for the announcement stamp (6 to 35 s here; the test allows 180 s) and `test_09` to take about 70 s (LXMF's five tries).
- A failure in `test_messaging` is reported once: the tests of that class build on each other, so after the first failure the
  rest are skipped with the name of the failing test (before, a failed `test_12` also showed up as `test_13` and `test_15`, with
  `/proc/<pid>/status` missing because the bridge the earlier failure left stopped was gone).

Cause of the intermittent failures (measured on a quiet 12-core, 8 GB WSL machine: 1 failing run in 7, always in the part of
`test_messaging` that sends right after a connection was switched: `test_05`, `test_12`; not load, not memory, not the stamp): when
the TCP server of one bridge reloads (switching "may send" off and on) the other bridge reconnects as a new interface, and RNS
and LXMF still hold the old link and path. The first message after that is not delivered at once: the old link is found closed,
the path is asked for again, the first link request can go unanswered, and the delivery takes **25 to 40 s** instead of well under
1 s (always the case in a loop of 5 reloads, even when the test waits 40 s before sending). When it takes longer than LXMF's five
tries (10 to 12 s each) the message ends `failed` / `undelivered`. A person presses Retry then, and so does `send_settled()` in
`test_messaging.py` (once; it says so on stderr; a second failure, or another failure detail, still fails the test). This is
product behaviour, not a test artefact: on the deck, the first message after a connection was switched off and on can be late or
need a manual retry. The tests wait for the link (`settle_after_reload`) but cannot see LXMF's stale link, so there is no state
to poll.

A second finding, not a cause of these failures: two `iface.outgoing` changes on a TCP server interface less than about a second apart, while
the other side is still connecting, make RNS fail to bind the port again (`[Errno 98] Address already in use`) and the bridge process exits.
The tests keep clear of it (a connection settles before the next change); it is reported for the bridge to handle.

## The fake bridge

[`fake_bridge.py`](fake_bridge.py) speaks protocol 1 (docs/dev/reticulum-bridge.md) from canned data: one independent world per
socket, one socket per harness size (`DIR/<WxH>/bridge.sock`). It imports the real bridge's `reticulum_bridge.options`, so the connection
types and the refusal of a bad value are the real ones; everything else is memory. Scenarios: `fresh` (the first configuration), `mixed`
(a LoRa radio on and listening, a server with a problem, two people heard), `radio` (a radio with only its port set), `busyport`; since P2 `chat` (a radio that may send, three contacts, four people heard, conversations in every message state; the contact *Camp base* is offline: its messages fail and a retry delivers), `chatblocked` (the same, the radio listen-only: send, retry and announce are refused with `no_transmit`; the last message of *Marek* was cancelled for that reason) and `chatempty`; since 0.3.1 `notice` (Wi-Fi LAN was left on and listening only by an older version: `notices.take` returns it once). The fake enforces the bridge's rule for a kind with no listen-only state (`iface.enable` needs `allow_sending`, `iface.outgoing false` is refused). The fake keeps contacts, messages, the heard list and the alert setting in memory, sends the events of the real service and delivers a sent message after `--deliver-after` seconds. Every
request goes to a log; `check_requests.py` reads it after each scene and fails the scene if the app asked for `lxmf.send` or
`paths`, if any `iface.outgoing` (or `iface.enable` with `allow_sending`) allowed sending beyond what the scene's header says (`# transmit-requests: N`, default 0), if the number
of `messages.send` plus `messages.retry` differs from the header's `# message-requests: N` (default 0) or of `announce` from
`# announce-requests: N` (default 0), or if
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
| `sending` | mixed | Allow sending: Cancel does nothing; Enter alone and Left then Enter are Cancel; Right then Enter makes the one request that allows sending; turning it off needs no question |
| `dialogs` | chat | the four questions that delete or send (Remove contact, Announce myself, Stop the service, Delete conversation): the focus starts on Cancel with a ring on both buttons; Enter alone, Left then Enter, Tab Tab Enter and Esc cancel; a typed letter does nothing; Right moves the ring to the main button; the checks fail the run if a destructive request reaches the bridge |
| `wifi-lan` | notice | Wi-Fi / LAN has no listen only: the one-time dialog about a connection switched off, no *Allow sending* row, *Switch on* asks (Enter / Left+Enter / Esc cancel, Right+Enter makes the one request), the Home hint in both states |
| `busyport` | busyport | switching on a port another program holds: the bridge's words in a notice |
| `remove` | mixed | Remove asks first; Cancel keeps it; confirming removes it and the page closes |
| `add` | fresh | the kind list, More..., a form (suggested name, Esc leaves), Internet filled and saved off, LoRa saved with every field empty |
| `settings` | fresh | Keep running on and off (the fake unit), the name, My address, Stop the service and Start the service |
| `keep` | fresh | Keep running on: leaving does not stop the service |
| `start`, `startfail`, `slow` | fresh / mixed | the service still starting (progress, then up by itself); `systemctl start` failing (the reason and Try again); a slow bridge (the screen answers at once, keys work while a request is out) |
| `chat-walk` | chat | phase P2: Home with Messages and Contacts, the Messages list, a conversation (the new messages are read), Contacts, a contact, the remove question (Cancel), Rename, Add a contact typed on the on-screen keyboard (the digit counter, the refusal of a short address), Heard recently (one tap adds), My address |
| `chat-messages` | chat, delivery after 2.5 s | the compose row with the keyboard and the counter, Send (sending, delivered), an offline contact (fails, Retry by keys, delivered), More, the delete question (Cancel, then Delete) |
| `chat-blocked` | chatblocked | nothing may send: the note and the row that opens Connections, Send greyed, the draft kept across a visit to Connections, Announce myself shows the blocked page; no message, retry or announcement is asked |
| `chat-announce` | chat | My address: Copy address, Announce myself (Cancel does nothing, the primary button makes the one request) |
| `chat-empty` | chatempty | nothing yet: the empty Home, Messages, the contact picker, Contacts, Heard recently; Paste address (the test hook `RETICULUM_PASTE_CMD`), Save; Announce myself without a name |
| `compact320`, `compact320-wifi`, `compact320-blocked` | chat / notice / chatblocked | **the Orange Pi's 480x320** (`ppmm 633`, the compact shell: 40 px header, 44 px rows; the other scenes run 480x320 at the deck's density, a bigger look): Home whole with a one-line hint, the title of a contact and of Rename (long and short name), the Add a contact hint, the address count after a refusal, My address whole in two lines with the page scrolling to the rows under it, the long message with the keyboard up, the Wi-Fi LAN page where *More options* meets the hint, the blocked Announce page's title; `expect whole` and `expect gap` |
| `chat-limit` | chat | the counter near the packet limit (*12 left*) and over it (*23 over*, the warning, Send still works) |
| `chat-alerts` | chat | Settings > New message alerts on and off |
| `chat-keyboard`, `chat-touch` | chat | the same pages by a physical keyboard alone (the field is ready at once; Enter sends) and by touch alone (tap the field, Send, More, Cancel) |
| `touch`, `keyboard`, `osk`, `lifecycle`, `band` | mixed / fresh / radio | every page by touch alone; every page by keys alone; the on-screen keyboard rule; hidden, shown, closed; the handwriting band in a text page |

The on-screen keyboard's Enter key has no text label, so the touch scene reaches a text page by taps and the typing and Enter of the
`osk` and `radio` scenes use the harness's key injection.
