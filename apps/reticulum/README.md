# Reticulum (work in progress, not published)

The Reticulum service of the CyberdeckZero apps, phase P1a part 1 of launcher report 057: a Python **bridge** that is
the RNS shared instance, runs an LXMF router with the deck's own identity, and offers our native apps a JSON-lines
socket, with an interface manager for every documented Reticulum interface type (Wi-Fi/Ethernet, TCP, UDP, I2P, RNode,
serial and KISS modems). Nothing transmits until the user switches an interface on and allows it to send. There is no
app tile, no package and no `app.json` yet: packaging is part 2, the status app is phase P1b.

- Protocol, files, transmit rules, tests and what is not done: [docs/dev/reticulum-bridge.md](../../docs/dev/reticulum-bridge.md).
- Source: [`bridge/reticulum_bridge/`](bridge/reticulum_bridge/); tests: `sh bridge/tests/run_tests.sh` (PC, Linux or WSL).

## Credits and licences

- Our code (the bridge and its tests): MIT. It was written with AI assistance, from the public Python API of RNS and
  LXMF; it is not a Reticulum implementation and is not part of the upstream projects.
- It imports [Reticulum (RNS)](https://github.com/markqvist/Reticulum) 1.6.1 and [LXMF](https://github.com/markqvist/LXMF)
  1.3.1 by Mark Qvist, under the Reticulum License (the MIT text plus two use conditions). They are pinned in
  [`bridge/requirements.txt`](bridge/requirements.txt) and not bundled yet; the package will ship their licence texts.
- The configuration file is read and checked with the ConfigObj parser that RNS ships (BSD-3-Clause).
