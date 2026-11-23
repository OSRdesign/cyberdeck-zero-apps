# SPDX-License-Identifier: MIT
"""identity.name: get, set, clear and refusals, on a Bridge that was never started (no RNS, no socket, nothing sent)."""
import os
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from reticulum_bridge import BridgeError  # noqa: E402
from reticulum_bridge.service import NAME_MAX, Bridge  # noqa: E402


class FakeDest:
    display_name = "unset"


class NameTest(unittest.TestCase):
    def setUp(self):
        base = tempfile.mkdtemp()
        self.data = os.path.join(base, "data")
        self.b = Bridge(os.path.join(base, "cfg"), self.data, os.path.join(base, "run"), use_lxmf=False)
        self.path = os.path.join(self.data, "display_name")
        os.umask(0o077)

    def call(self, params):
        return self.b.verbs["identity.name"](params, None)

    def test_verb_is_listed(self):
        self.assertIn("identity.name", self.b.verbs)

    def test_get_set_clear(self):
        self.assertEqual(self.call({}), {"display_name": None})
        self.assertEqual(self.call({"name": "  Kitchen deck "}), {"display_name": "Kitchen deck"})
        self.assertEqual(self.call({}), {"display_name": "Kitchen deck"})
        with open(self.path, encoding="utf-8") as f:
            self.assertEqual(f.read(), "Kitchen deck\n")
        self.assertEqual(stat.S_IMODE(os.stat(self.path).st_mode), 0o600)
        self.assertEqual(self.call({"name": ""}), {"display_name": None})
        self.assertFalse(os.path.exists(self.path))
        self.assertEqual(self.call({"name": None}), {"display_name": None})      # clearing twice is fine

    def test_destination_follows(self):
        self.b.dest = FakeDest()
        self.call({"name": "Deck"})
        self.assertEqual(self.b.dest.display_name, "Deck")
        self.call({"name": ""})
        self.assertIsNone(self.b.dest.display_name)

    def test_refusals(self):
        for bad in ("   ", "a\nb", "x" * (NAME_MAX + 1), 5, ["a"]):
            with self.assertRaises(BridgeError) as ctx:
                self.call({"name": bad})
            self.assertEqual((ctx.exception.code, ctx.exception.extra.get("field")), ("invalid", "name"))
        self.assertFalse(os.path.exists(self.path))
        self.call({"name": "x" * NAME_MAX})                                       # the limit itself is accepted

    def test_status_shows_it(self):
        self.call({"name": "Deck"})
        self.assertEqual(self.b._display_name(), "Deck")


if __name__ == "__main__":
    unittest.main()
