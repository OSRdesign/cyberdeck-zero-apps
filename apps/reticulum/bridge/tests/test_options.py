# SPDX-License-Identifier: MIT
"""Option lists and validation (no RNS needed)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from reticulum_bridge import BridgeError, options  # noqa: E402

DOCUMENTED = {"AutoInterface", "BackboneInterface", "TCPServerInterface", "TCPClientInterface", "UDPInterface",
              "I2PInterface", "RNodeInterface", "RNodeMultiInterface", "SerialInterface", "PipeInterface",
              "KISSInterface", "AX25KISSInterface"}


def opt(type_name, name):
    return options.options_of(type_name)[name]


class TypeList(unittest.TestCase):
    def test_documented_types_only(self):
        tl = options.type_list()
        self.assertEqual({t["type"] for t in tl["types"]}, DOCUMENTED)
        self.assertNotIn("WeaveInterface", options.TYPES)
        multi = next(t for t in tl["types"] if t["type"] == "RNodeMultiInterface")
        self.assertFalse(multi["supported"])
        self.assertIn("reason", multi)
        self.assertEqual(tl["switches"], {"enabled": False, "outgoing": False})

    def test_rnode_radio_fields_start_empty(self):
        ranges = {"frequency": (137000000, 3000000000), "bandwidth": (7800, 1625000), "txpower": (0, 37),
                  "spreadingfactor": (5, 12), "codingrate": (5, 8)}
        for name, (lo, hi) in ranges.items():
            o = opt("RNodeInterface", name)
            self.assertEqual((o["min"], o["max"]), (lo, hi), name)
            self.assertIsNone(o["default"], name)              # D10: no prefilled value
            self.assertTrue(o.get("required"), name)
        for name in ("airtime_limit_short", "airtime_limit_long"):
            o = opt("RNodeInterface", name)
            self.assertEqual((o["min"], o["max"], o["default"]), (0, 100, None))
            self.assertFalse(o.get("required"))
        for t in options.TYPES.values():                       # no example values anywhere
            for o in t + options.COMMON:
                self.assertNotIn("example", o)

    def test_common_options(self):
        names = {o["name"] for o in options.COMMON}
        self.assertEqual(names, {"mode", "network_name", "passphrase", "ifac_size", "announce_cap", "bitrate",
                                 "gravity", "bootstrap_only"})
        self.assertTrue(opt("TCPClientInterface", "passphrase")["secret"])
        self.assertEqual((opt("UDPInterface", "ifac_size")["min"], opt("UDPInterface", "ifac_size")["max"]), (8, 512))

    def test_unknown_and_unsupported_type(self):
        for t in ("WeaveInterface", "RNodeMultiInterface", None):
            with self.assertRaises(BridgeError) as cm:
                options.validate(t, {})
            self.assertEqual(cm.exception.code, "not_supported")


class Validate(unittest.TestCase):
    def bad(self, t, values, field):
        with self.assertRaises(BridgeError) as cm:
            options.validate(t, values)
        self.assertEqual(cm.exception.code, "invalid")
        self.assertEqual(cm.exception.extra.get("field"), field)

    def test_numbers(self):
        v = options.validate("RNodeInterface", {"frequency": "868000000", "txpower": 7.0, "airtime_limit_long": "1.5",
                                                "spreadingfactor": 8})
        self.assertEqual(v, {"frequency": "868000000", "txpower": "7", "airtime_limit_long": "1.5",
                             "spreadingfactor": "8"})
        self.bad("RNodeInterface", {"frequency": 136999999}, "frequency")
        self.bad("RNodeInterface", {"bandwidth": 1625001}, "bandwidth")
        self.bad("RNodeInterface", {"spreadingfactor": 7.5}, "spreadingfactor")
        self.bad("RNodeInterface", {"codingrate": True}, "codingrate")
        self.bad("RNodeInterface", {"airtime_limit_short": "nan"}, "airtime_limit_short")
        self.bad("TCPClientInterface", {"target_port": 70000}, "target_port")
        self.bad("SerialInterface", {"databits": 9}, "databits")

    def test_switches_and_unknown_keys_refused(self):
        self.bad("TCPClientInterface", {"enabled": True}, "enabled")
        self.bad("TCPClientInterface", {"outgoing": True}, "outgoing")
        self.bad("TCPClientInterface", {"frequency": 1}, "frequency")

    def test_empty_means_unset(self):
        self.assertEqual(options.validate("RNodeInterface", {"frequency": "", "port": None}),
                         {"frequency": None, "port": None})

    def test_bool_list_and_text(self):
        v = options.validate("AutoInterface", {"devices": ["wlan0", "eth0"], "ignored_devices": "usb0,usb1",
                                               "group_id": "family net"})
        self.assertEqual(v, {"devices": "wlan0, eth0", "ignored_devices": "usb0, usb1", "group_id": '"family net"'})
        self.assertEqual(options.validate("RNodeInterface", {"flow_control": "yes"}), {"flow_control": "yes"})
        self.assertEqual(options.validate("RNodeInterface", {"port": "ble://RNode 3B87"}),
                         {"port": '"ble://RNode 3B87"'})
        self.assertEqual(options.validate("RNodeInterface", {"port": "/dev/serial/by-id/usb-X_Y-if00"}),
                         {"port": "/dev/serial/by-id/usb-X_Y-if00"})
        self.bad("AutoInterface", {"devices": ["wl an0"]}, "devices")
        self.bad("TCPClientInterface", {"passphrase": "a\nb"}, "passphrase")
        self.bad("TCPClientInterface", {"passphrase": "it's \"x\""}, "passphrase")
        self.bad("TCPClientInterface", {"target_host": " host"}, "target_host")
        self.bad("RNodeInterface", {"id_callsign": "x" * 33}, "id_callsign")
        self.bad("TCPClientInterface", {"mode": "relay"}, "mode")

    def test_names(self):
        for good in ("Wi-Fi LAN", "RNode (Heltec)", "tcp_1", "Überall"):
            self.assertEqual(options.check_name(good), good)
        for bad in ("", " x", "x ", "a[b]", "a=b", "#x", "x" * 49, 5, "a,b"):
            with self.assertRaises(BridgeError):
                options.check_name(bad)

    def test_required_and_pairs(self):
        self.assertEqual(options.missing_required("RNodeInterface", {"port": "/dev/ttyACM0"}),
                         ["frequency", "bandwidth", "txpower", "spreadingfactor", "codingrate"])
        self.assertEqual(options.missing_required("TCPClientInterface", {"target_host": "h", "target_port": "1"}), [])
        self.assertEqual(options.missing_required("SomeCustomInterface", {}), [])
        with self.assertRaises(BridgeError):
            options.check_pairs("RNodeInterface", {"id_callsign": "N0CALL"})
        options.check_pairs("RNodeInterface", {"id_callsign": "N0CALL", "id_interval": "600"})
        options.check_pairs("TCPClientInterface", {})


if __name__ == "__main__":
    unittest.main()
