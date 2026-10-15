# BLE fixtures

Output of BlueZ 5.82's `bluetoothctl`, for `tests/test_ble.cpp`.

| File | Source |
|---|---|
| `scan-3a-plus.txt` | recorded on the Pi 3A+ on 2026-10-15: `bluetoothctl` with `menu scan`, `duplicate-data on`, `transport le`, `back`, `scan on` written on its stdin, three `info <addr>` after 3 s (one for an address that is not there), `scan off` and `quit` after 8 s more. Raw bytes: colour sequences, `\r` prompt redraws and backspaces kept |
| `scan-killed-3a-plus.txt` | the first 300 lines of another recording on the Pi 3A+ (the same commands, no `info`; bluetoothctl was ended with SIGKILL after 8 s: `Discovering: no` was read 0.5 s later) |
| `show-3a-plus.txt`, `show-deck.txt` | `bluetoothctl show` on the Pi 3A+ and on the deck (the `UUID:` lines left out when recorded) |
| `show-powered-off.txt` | `show-3a-plus.txt` edited by hand: `Powered: no`, `PowerState: off`, `Class` 0 (the adapter was not switched off for a recording) |

Sanitized: the devices heard belong to other people. Every MAC address is replaced by a fake one (the first byte keeps
its two top bits, the last byte is an index; the boards' own controllers became `B8:27:EB:00:00:01` and
`88:A2:9E:00:00:02`), every device name by a made-up one of the same shape (spaces, brackets, an emoji kept), and the
bytes of the manufacturer and service data dumps by zeros. The event lines, their order and the RSSI values are as
recorded.
