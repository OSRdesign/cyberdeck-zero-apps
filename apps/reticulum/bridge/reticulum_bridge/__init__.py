# SPDX-License-Identifier: MIT
"""reticulum_bridge: the Reticulum service of the CyberdeckZero apps (report 057, architecture A).

One Python process: the RNS shared instance, an LXMF router with the deck's own identity, and a JSON-lines Unix socket
for our native apps. It uses only the public Python API of RNS and LXMF; it is not a Reticulum implementation.
Protocol and files: docs/dev/reticulum-bridge.md. Written with AI assistance.
"""
__version__ = "0.1.0"
PROTOCOL = 1


class BridgeError(Exception):
    """A request that cannot be done; code is one word of the protocol (docs/dev/reticulum-bridge.md)."""

    def __init__(self, code, message, **extra):
        super().__init__(message)
        self.code = code
        self.message = message
        self.extra = extra

    def as_dict(self):
        d = {"code": self.code, "message": self.message}
        d.update(self.extra)
        return d
