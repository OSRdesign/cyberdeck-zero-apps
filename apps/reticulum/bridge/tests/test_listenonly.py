# SPDX-License-Identifier: MIT
"""Connections of a kind that has no listen-only state (QA of 0.3.0, finding F1; owner decision 2026-11-25).

RNS's AutoInterface (Wi-Fi / LAN) multicasts a discovery token about every 1.6 s whatever `outgoing` says, so "on but
listen only" was not silent. Now such a kind is on (sending allowed) or off. This measures it with a real multicast
listener on the machine's link-local address and the real bridge:

  * off (never switched on): zero datagrams;
  * listen only is refused (`iface.outgoing false`), and switching on needs `allow_sending: true`;
  * on: datagrams are seen (the positive control: the listener works);
  * off again: zero datagrams;
  * a stored "on, listen only" connection (an older version, or a hand edit) is switched off by the bridge BEFORE RNS
    reads the file: zero datagrams during the start, the user is told once (`notices.take`), the file says `enabled = no`;
    also for the hand-written `interface_enabled` key.

Skipped when the machine has no non-loopback interface with an IPv6 link-local address. About 70 seconds.
"""
import os
import socket
import struct
import threading
import time
import unittest

from bridgekit import Node, free_port, rd, wait_for

try:
    import RNS
except ImportError:                                         # pragma: no cover
    RNS = None


def link_local_interfaces():
    """[(index, name)] of the non-loopback interfaces that have an fe80:: address (read from /proc/net/if_inet6)."""
    out = []
    try:
        for line in rd("/proc/net/if_inet6").splitlines():
            parts = line.split()
            if parts[0].startswith("fe80") and parts[5] != "lo":
                out.append((int(parts[1], 16), parts[5]))
    except OSError:
        pass
    return out


def mcast_address(group_id):
    """The discovery address AutoInterface derives from group_id (AutoInterface.py: temporary type 2, link scope 2)."""
    g = RNS.Identity.full_hash(group_id.encode("utf-8"))
    gt = "0" + "".join(":%02x" % (g[i + 1] + (g[i] << 8)) for i in (2, 4, 6, 8, 10, 12))
    return "ff22:" + gt


class Sniffer:
    """Counts the datagrams that reach the discovery group and port, on every link-local interface."""

    def __init__(self, group_id, port):
        self.times = []
        self.lock = threading.Lock()
        self.sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        self.sock.bind(("", port))
        group = socket.inet_pton(socket.AF_INET6, mcast_address(group_id))
        for idx, _name in link_local_interfaces():
            self.sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, group + struct.pack("I", idx))
        self.sock.settimeout(0.2)
        self.stop_flag = False
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self):
        while not self.stop_flag:
            try:
                data, _ = self.sock.recvfrom(2048)
            except (socket.timeout, OSError):
                continue
            with self.lock:
                self.times.append((time.monotonic(), len(data)))

    def count_since(self, t0):
        with self.lock:
            return sum(1 for t, _ in self.times if t >= t0)

    def close(self):
        self.stop_flag = True
        self.thread.join(2)
        self.sock.close()


WINDOW = 7.0                                                # the token comes every 1.6 s: four or five in a window


def wifi_options(group_id, port):
    return {"group_id": group_id, "discovery_port": port, "data_port": free_port()}


@unittest.skipIf(RNS is None, "rns and lxmf are not installed (use run_tests.sh)")
@unittest.skipUnless(link_local_interfaces(), "no non-loopback interface with an IPv6 link-local address")
class ListenOnlyKinds(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.group = "listenonly-%d" % os.getpid()
        cls.port = free_port()
        cls.sniffer = Sniffer(cls.group, cls.port)
        cls.node = Node("lo").start()
        cls.extra = []

    @classmethod
    def tearDownClass(cls):
        cls.node.stop()
        for n in cls.extra:
            n.stop()
        cls.sniffer.close()

    @staticmethod
    def section(node, name="Wi-Fi LAN"):
        from reticulum_bridge import rnsconfig
        return rnsconfig.interfaces(rd(os.path.join(node.cfg, "config")))[name]

    def silent_for(self, seconds=WINDOW):
        t0 = time.monotonic()
        time.sleep(seconds)
        return self.sniffer.count_since(t0)

    def test_01_the_kinds_say_what_listen_only_means(self):
        n = self.node
        types = {t["type"]: t["listen_only"] for t in n.call("iface.types")["types"]}
        self.assertEqual(types["AutoInterface"], "none")
        for t in ("RNodeInterface", "SerialInterface", "KISSInterface", "AX25KISSInterface", "UDPInterface", "PipeInterface"):
            self.assertEqual(types[t], "silent", t)
        for t in ("TCPClientInterface", "TCPServerInterface", "BackboneInterface", "I2PInterface"):
            self.assertEqual(types[t], "handshake", t)
        n.call("iface.add", {"name": "Wi-Fi LAN", "type": "AutoInterface", "options": wifi_options(self.group, self.port)})
        e = n.iface("Wi-Fi LAN")
        self.assertEqual((e["listen_only"], e["enabled"]), ("none", False))

    def test_02_off_sends_nothing(self):
        self.assertEqual(self.silent_for(), 0, "a Wi-Fi / LAN connection that was never switched on sent datagrams")

    def test_03_listen_only_is_refused(self):
        n = self.node
        self.assertEqual(n.code("iface.outgoing", {"name": "Wi-Fi LAN", "outgoing": False}), "invalid")
        self.assertEqual(n.code("iface.enable", {"name": "Wi-Fi LAN"}), "confirm")            # no allow_sending
        self.assertEqual(n.code("iface.enable", {"name": "Wi-Fi LAN", "allow_sending": "yes"}), "confirm")
        self.assertFalse(n.iface("Wi-Fi LAN")["enabled"])
        self.assertEqual(self.section(n)["enabled"], "no")
        self.assertEqual(self.silent_for(3), 0)

    def test_04_on_sends_and_off_stops(self):
        n = self.node
        t0 = time.monotonic()
        e = n.call("iface.enable", {"name": "Wi-Fi LAN", "allow_sending": True}, timeout=30)
        self.assertTrue(e["enabled"] and e["outgoing"])
        self.assertEqual((self.section(n)["enabled"], self.section(n)["outgoing"]), ("yes", "True"))
        time.sleep(WINDOW)
        seen = self.sniffer.count_since(t0)
        self.assertGreaterEqual(seen, 2, "the positive control failed: the listener saw nothing of an ON connection")
        # still no way to get a listen-only state, also while it is on
        self.assertEqual(n.code("iface.outgoing", {"name": "Wi-Fi LAN", "outgoing": False}), "invalid")
        self.assertTrue(n.iface("Wi-Fi LAN")["outgoing"])
        n.call("iface.enable", {"name": "Wi-Fi LAN", "enabled": False}, timeout=30)
        time.sleep(2.5)                                                 # a token may be on its way
        self.assertEqual(self.silent_for(), 0, "switched off, and still sending")

    def _stored_listen_only(self, label, key):
        """A node whose configuration has the connection on and listen only (as 0.3.0 could leave it)."""
        from reticulum_bridge import rnsconfig
        node = Node(label)
        self.extra.append(node)
        cfg = os.path.join(node.cfg, "config")
        text = rnsconfig.add_section(rd(cfg), "Wi-Fi LAN", [("type", "AutoInterface"), (key, "yes"), ("outgoing", "False"),
                                                            ("group_id", self.group), ("discovery_port", str(self.port)),
                                                            ("data_port", str(free_port()))])
        with open(cfg, "w") as f:
            f.write(text)
        return node

    def test_05_stored_listen_only_is_switched_off_before_it_can_send(self):
        node = self._stored_listen_only("m1", "enabled")
        t0 = time.monotonic()
        node.start()
        e = node.iface("Wi-Fi LAN")
        self.assertFalse(e["enabled"])
        self.assertEqual(self.silent_for(), 0, "the upgrade transmitted before the connection was switched off")
        self.assertEqual(self.sniffer.count_since(t0), 0)
        self.assertEqual(self.section(node)["enabled"], "no")
        self.assertTrue(os.path.exists(os.path.join(node.cfg, "config.bak")))
        self.assertIn("switched off", node.logtext())
        notes = node.call("notices.take")["notices"]
        self.assertEqual([(n["kind"], n["name"], n["type"]) for n in notes],
                         [("listen_only_removed", "Wi-Fi LAN", "AutoInterface")])
        self.assertEqual(node.call("notices.take")["notices"], [])         # told once
        self.assertEqual(node.code("iface.outgoing", {"name": "Wi-Fi LAN", "outgoing": False}), "invalid")

    def test_06_the_other_spelling_of_enabled(self):
        node = self._stored_listen_only("m2", "interface_enabled")
        t0 = time.monotonic()
        node.start()
        self.assertFalse(node.iface("Wi-Fi LAN")["enabled"])
        sec = self.section(node)
        self.assertEqual((sec["enabled"], sec["interface_enabled"]), ("no", "no"))
        self.assertEqual(self.silent_for(), 0)
        self.assertEqual(self.sniffer.count_since(t0), 0)
        self.assertEqual(len(node.call("notices.take")["notices"]), 1)

    def test_07_sending_on_stays_on_across_a_restart(self):
        """A connection that is on and allowed to send is left alone by the check, and a restart keeps it (no notice)."""
        n = self.node
        n.call("iface.enable", {"name": "Wi-Fi LAN", "allow_sending": True}, timeout=30)
        n.stop()
        n.start()
        e = n.iface("Wi-Fi LAN")
        self.assertTrue(e["enabled"] and e["outgoing"])
        self.assertEqual(n.call("notices.take")["notices"], [])
        n.call("iface.enable", {"name": "Wi-Fi LAN", "enabled": False}, timeout=30)


if __name__ == "__main__":
    unittest.main()
