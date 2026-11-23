# SPDX-License-Identifier: MIT
"""Config location, first-run file, never-overwrite, surgical edits, atomic write (parses with RNS's ConfigObj)."""
import os
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from reticulum_bridge import BridgeError, rnsconfig  # noqa: E402


def rd(path, mode="r"):
    with open(path, mode) as f:
        return f.read()


HAND = """\
# my own notes: keep me
[reticulum]
  enable_transport = No
  share_instance = Yes   # inline note

[interfaces]
  # comment about the first interface
  [[Default Interface]]
    type = AutoInterface
    enabled = no
    # devices = wlan0
  [[Multi]]
    type = RNodeMultiInterface
    port = /dev/ttyACM9
    [[[Sub A]]]
      vport = 0

[logging]
  loglevel = 4
"""


class FirstRun(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def test_creates_only_when_absent(self):
        cfg = os.path.join(self.dir, "new")
        self.assertTrue(rnsconfig.ensure_first_run(cfg))
        path = os.path.join(cfg, "config")
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
        c = rnsconfig.parse(rd(path))
        self.assertEqual(c["reticulum"]["share_instance"], "Yes")
        self.assertEqual(c["reticulum"]["enable_transport"], "No")
        self.assertEqual(c["logging"]["loglevel"], "3")
        ifs = rnsconfig.interfaces(rd(path))
        self.assertEqual(list(ifs), ["Default Interface"])
        self.assertEqual((ifs["Default Interface"]["enabled"], ifs["Default Interface"]["outgoing"]), ("no", "False"))
        before = rd(path, "rb")
        self.assertFalse(rnsconfig.ensure_first_run(cfg))
        self.assertEqual(rd(path, "rb"), before)

    def test_never_overwrites_a_hand_config(self):
        path = os.path.join(self.dir, "config")
        with open(path, "w") as f:
            f.write(HAND)
        os.chmod(path, 0o644)
        self.assertFalse(rnsconfig.ensure_first_run(self.dir))
        self.assertEqual(rd(path), HAND)
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o644)

    def test_resolve_dir(self):
        self.assertEqual(rnsconfig.resolve_dir("/x/y/../z"), "/x/z")
        old = os.environ.get("HOME")
        os.environ["HOME"] = self.dir
        try:
            if not os.path.isfile("/etc/reticulum/config"):
                self.assertEqual(rnsconfig.resolve_dir(), os.path.join(self.dir, ".reticulum"))
                os.makedirs(os.path.join(self.dir, ".config", "reticulum"))
                open(os.path.join(self.dir, ".config", "reticulum", "config"), "w").close()
                self.assertEqual(rnsconfig.resolve_dir(), os.path.join(self.dir, ".config", "reticulum"))
        finally:
            os.environ["HOME"] = old


class Edits(unittest.TestCase):
    def test_add_keeps_everything_else(self):
        new = rnsconfig.add_section(HAND, "Peer", [("type", "TCPClientInterface"), ("enabled", "no"),
                                                   ("outgoing", "False"), ("target_host", "127.0.0.1"),
                                                   ("target_port", "4242"), ("passphrase", '"a, b # c"')])
        for line in HAND.splitlines():
            self.assertIn(line, new.splitlines())
        ifs = rnsconfig.interfaces(new)
        self.assertEqual(ifs["Peer"]["passphrase"], "a, b # c")
        self.assertEqual(rnsconfig.parse(new)["logging"]["loglevel"], "4")       # still its own section
        self.assertLess(new.index("[[Peer]]"), new.index("[logging]"))
        with self.assertRaises(BridgeError) as cm:
            rnsconfig.add_section(new, "Peer", [("type", "UDPInterface")])
        self.assertEqual(cm.exception.code, "exists")

    def test_add_creates_interfaces_section(self):
        new = rnsconfig.add_section("[reticulum]\n  share_instance = Yes\n", "A", [("type", "AutoInterface")])
        self.assertEqual(rnsconfig.interfaces(new)["A"]["type"], "AutoInterface")

    def test_edit_one_section_only(self):
        new = rnsconfig.edit_section(HAND, "Default Interface", {"enabled": "yes", "devices": "wlan0, eth0",
                                                                  "nothing": None})
        ifs = rnsconfig.interfaces(new)
        self.assertEqual(ifs["Default Interface"]["enabled"], "yes")
        self.assertEqual(ifs["Default Interface"]["devices"], ["wlan0", "eth0"])
        self.assertIn("    # devices = wlan0", new)                      # its comment stays
        self.assertEqual(ifs["Multi"]["Sub A"], {"vport": "0"})
        new2 = rnsconfig.edit_section(new, "Multi", {"id_interval": "600"})   # goes before the sub-section
        self.assertEqual(rnsconfig.interfaces(new2)["Multi"]["id_interval"], "600")
        self.assertEqual(rnsconfig.interfaces(new2)["Multi"]["Sub A"], {"vport": "0"})
        new3 = rnsconfig.edit_section(new2, "Default Interface", {"devices": None})
        self.assertNotIn("devices", rnsconfig.interfaces(new3)["Default Interface"])
        with self.assertRaises(BridgeError) as cm:
            rnsconfig.edit_section(HAND, "Nope", {"x": "1"})
        self.assertEqual(cm.exception.code, "not_found")

    def test_edit_refuses_multiline_values(self):
        text = HAND.replace("    # devices = wlan0", "    group_id = '''a\n b'''")
        with self.assertRaises(BridgeError) as cm:
            rnsconfig.edit_section(text, "Default Interface", {"enabled": "yes"})
        self.assertEqual(cm.exception.code, "not_supported")

    def test_remove(self):
        new = rnsconfig.remove_section(HAND, "Default Interface")
        self.assertEqual(list(rnsconfig.interfaces(new)), ["Multi"])
        self.assertIn("  # comment about the first interface", new)
        self.assertIn("# my own notes: keep me", new)
        new2 = rnsconfig.remove_section(new, "Multi")
        self.assertEqual(rnsconfig.interfaces(new2), {})
        self.assertEqual(rnsconfig.parse(new2)["logging"]["loglevel"], "4")

    def test_bad_config_is_an_error(self):
        with self.assertRaises(BridgeError) as cm:
            rnsconfig.interfaces("[interfaces]\n  [[A]\n")
        self.assertEqual(cm.exception.code, "config_error")


class AtomicWrite(unittest.TestCase):
    def test_backup_and_mode(self):
        d = tempfile.mkdtemp()
        path = os.path.join(d, "config")
        rnsconfig.write_atomic(path, "one\n")
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
        self.assertFalse(os.path.exists(path + ".bak"))
        os.chmod(path, 0o640)
        rnsconfig.write_atomic(path, "two\n")
        self.assertEqual(rd(path), "two\n")
        self.assertEqual(rd(path + ".bak"), "one\n")
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o640)
        self.assertEqual(sorted(os.listdir(d)), ["config", "config.bak"])


if __name__ == "__main__":
    unittest.main()
