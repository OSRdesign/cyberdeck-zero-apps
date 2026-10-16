# Wi-Fi fixtures

For `tests/test_wifiscan.cpp` and `tests/test_walktest.cpp` (0.4: Wi-Fi networks and the walk test).

| File | Source |
|---|---|
| `nmcli-3a-plus.txt` | recorded on the Pi 3A+ (NetworkManager on Raspberry Pi OS trixie) on 2026-10-16: `LC_ALL=C nmcli -t -e yes -f IN-USE,BSSID,SSID,MODE,CHAN,FREQ,RATE,SIGNAL,SECURITY device wifi list ifname wlan0` (default `--rescan auto`, 63 ms). Four networks: the one in use on 2.4 GHz channel 9, a hidden one with a locally administered BSSID on the same radio, a neighbour's on channel 1, a hidden one on 5 GHz channel 36. A hidden network's SSID is an empty field |
| `nmcli-notfound-3a-plus.txt` | the same with `ifname nosuch0` (exit code 10) |
| `nmcli-not-wifi-3a-plus.txt` | the same with `ifname lo` (exit code 10) |
| `iw-link-3a-plus.txt` | `LC_ALL=C /usr/sbin/iw dev wlan0 link` at the same time (`signal: -65 dBm`; nmcli said 57 %, which NetworkManager's formula maps back to -66 dBm) |
| `iw-nodev-3a-plus.txt` | `iw dev wlan1 link` with no wlan1 (exit code 237) |
| `proc-wireless-3a-plus.txt` | `/proc/net/wireless` at the same time |
| `proc-wireless-3a-plus-1hz.txt` | five reads of its data line one second apart (`date +%s.%N`, then the line), the board standing still: -62, -64, -65, -65, -66 dBm |
| `nmcli-made-up.txt` | made-up lines in the same format for the cases the recording does not have: an escaped `:` and `\` in an SSID, a hidden SSID shown as `--`, WPA2/WPA3, WPA/WPA2, WPA3, Enterprise (802.1X), Enhanced Open (OWE), WEP, an open network (empty and `--` security), ad-hoc, 5 and 6 GHz (6 GHz channel 37), the same SSID on two BSSIDs, a BSSID printed twice (the weaker one is dropped), an SSID with a `:` that was not escaped, and lines that are not networks (an error, a blank line, a bad BSSID, no frequency nor channel, too few fields). SSIDs with bytes that are not UTF-8 are built in the test itself |

Sanitized: the networks around belong to other people. The SSIDs are replaced (`HomeNet`, `Neighbour-0002`) and the last
three bytes of every BSSID (the first three, the vendor's prefix, are kept); the rest is as recorded.
