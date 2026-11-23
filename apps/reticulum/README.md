# Reticulum (service package, draft, not published)

The Reticulum service of the CyberdeckZero apps, phase P1a part 1 of launcher report 057: a Python **bridge** that is
the RNS shared instance, runs an LXMF router with the deck's own identity, and offers our native apps a JSON-lines
socket, with an interface manager for every documented Reticulum interface type (Wi-Fi/Ethernet, TCP, UDP, I2P, RNode,
serial and KISS modems). Nothing transmits until the user switches an interface on and allows it to send. The package
(`reticulum` 0.1.0, `"draft": true` in `app.json`, so `make_registry.py` skips it) has no app tile: it is a
background service that the Reticulum apps start with `systemctl --user start reticulum` (not enabled at boot). The
status app is phase P1b.

- Protocol, files, transmit rules, tests and what is not done: [docs/dev/reticulum-bridge.md](../../docs/dev/reticulum-bridge.md).
- Source: [`bridge/reticulum_bridge/`](bridge/reticulum_bridge/); tests: `sh bridge/tests/run_tests.sh` (PC, Linux or WSL).
- Package: [`root/`](root/) (`/opt/reticulum/bridge`, `/opt/reticulum/py` with the pinned `rns` and `lxmf` wheels
  unpacked unmodified, the user unit `reticulum.service`, `/usr/share/doc/reticulum/` with `LICENSE.rns`,
  `LICENSE.lxmf` and `copyright`), [`DEBIAN/`](DEBIAN/), [`tools/vendor.py`](tools/vendor.py), [`build.sh`](build.sh)
  and [`tests/package_test.sh`](tests/package_test.sh); layout, unit limits and build steps: section 9 of the
  document above.
- Needs `python3` 3.10 or later, `python3-cryptography` and `python3-serial` (Debian packages, not bundled); USB
  radios and serial modems need the `dialout` group.

## Credits and licences

- Our code (the bridge and its tests): MIT. It was written with AI assistance, from the public Python API of RNS and
  LXMF; it is not a Reticulum implementation and is not part of the upstream projects.
- It imports [Reticulum (RNS)](https://github.com/markqvist/Reticulum) 1.6.1 and [LXMF](https://github.com/markqvist/LXMF)
  1.3.1 by Mark Qvist, under the Reticulum License (the MIT text plus two use conditions). They are pinned in
  [`bridge/requirements.txt`](bridge/requirements.txt) and bundled unmodified in the package, with their licence texts
  (`LICENSE.rns`, `LICENSE.lxmf`; the package's licence field is `MIT AND BSD-3-Clause AND LicenseRef-Reticulum`).
  Read the two added conditions (no use in a system that can purposefully harm people; no use for AI or
  language-model training datasets) before you redistribute it.
- The configuration file is read and checked with the ConfigObj parser that RNS ships (BSD-3-Clause).
