#!/usr/bin/env python3
"""Self test of the registry tools (tools/screen_info.py, make_registry.py, check_registry.py).

    python3 tools/test_registry_tools.py        # exit 0 when all pass

No network, no build: it works on a throw-away repository in a temporary folder.
"""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_registry  # noqa: E402
import make_registry  # noqa: E402
import screen_info  # noqa: E402

DESKTOP = "[Desktop Entry]\nName=Foo\nExec=foo\nX-Fullscreen=true\n%s"


def write(path, text, binary=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if binary else "w", **({} if binary else {"encoding": "utf-8", "newline": "\n"})) as handle:
        handle.write(text)


class ScreenInfo(unittest.TestCase):
    def test_parse_size_like_the_launcher(self):
        good = {"640x480": "640x480", "640X480": "640x480", " 480 x 640 ": "480x640", "0640x0480": "640x480",
                "1x1": "1x1", "65535x65535": "65535x65535"}
        for raw, want in good.items():
            self.assertEqual(screen_info.parse_size(raw), want, raw)
        for raw in ("", "640", "x480", "640x", "0x480", "640x0", "65536x480", "640x480x2", "-1x5", "6 40x480",
                    "640*480", "123456x1", None):
            self.assertIsNone(screen_info.parse_size(raw), raw)

    def make_app(self, root, desktop_extra, overlay_extra=None, meta=None):
        app = os.path.join(root, "apps", "foo")
        write(os.path.join(app, "app.json"), json.dumps(meta or {}))
        write(os.path.join(app, "root", "usr", "share", "APPLaunch", "applications", "foo.desktop"),
              DESKTOP % desktop_extra)
        if overlay_extra is not None:
            write(os.path.join(app, "root.amd64", "usr", "share", "APPLaunch", "applications", "foo.desktop"),
                  DESKTOP % overlay_extra)
        return app

    def test_fields(self):
        with tempfile.TemporaryDirectory() as root:
            app = self.make_app(root, "X-MinSize=640x480\nX-Portrait=480x640\n")
            self.assertEqual(screen_info.screen_fields(app, {}), {"min_size": "640x480", "portrait_min_size": "480x640"})
            app = self.make_app(root, "X-MinSize=640x480\n")
            self.assertEqual(screen_info.screen_fields(app, {}), {"min_size": "640x480"})
            app = self.make_app(root, "")
            self.assertEqual(screen_info.screen_fields(app, {}), {})
            # app.json fills a missing X-MinSize, never overrides one the .desktop has
            self.assertEqual(screen_info.screen_fields(app, {"min_size": "480x320"}), {"min_size": "480x320"})
            app = self.make_app(root, "X-MinSize=640x480\n")
            self.assertEqual(screen_info.screen_fields(app, {"min_size": "480x320"}), {"min_size": "640x480"})
            # malformed: no field (the launcher: no restriction / landscape only)
            app = self.make_app(root, "X-MinSize=big\nX-Portrait=tall\n")
            self.assertEqual(screen_info.screen_fields(app, {}), {})
            # the root.<arch> overlay replaces the .desktop of root/ at the same path
            app = self.make_app(root, "X-MinSize=640x480\n", overlay_extra="X-MinSize=800x480\n")
            self.assertEqual(screen_info.screen_fields(app, {}, "arm64"), {"min_size": "640x480"})
            self.assertEqual(screen_info.screen_fields(app, {}, "amd64"), {"min_size": "800x480"})


class Regenerate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.saved = make_registry.REPO
        make_registry.REPO = self.root
        meta = {"share_code": "foo", "package": "foo", "version": "1.0.0", "title": "Foo", "summary": "Foo app"}
        write(os.path.join(self.root, "apps", "foo", "app.json"), json.dumps(meta))
        write(os.path.join(self.root, "apps", "foo", "root", "usr", "share", "APPLaunch", "applications", "foo.desktop"),
              DESKTOP % "X-MinSize=640x480\nX-Portrait=480x640\n")
        self.deb = os.path.join(self.root, "packages", "foo_1.0.0_arm64.deb")
        write(self.deb, b"package one", binary=True)
        self.meta = meta

    def tearDown(self):
        make_registry.REPO = self.saved
        self.tmp.cleanup()

    def entry(self, previous=None):
        return make_registry.registry_entry("foo", self.meta, "https://x/y", "foo_1.0.0_arm64.deb", self.deb,
                                            "o", "r", "arm64", previous)

    def test_entry_carries_the_fields(self):
        entry = self.entry()
        self.assertEqual((entry["min_size"], entry["portrait_min_size"]), ("640x480", "480x640"))

    def test_unchanged_package_keeps_its_dates(self):
        old = self.entry()
        old["published_at"], old["updated_at"] = "2020-01-01T00:00:00+00:00", "2020-02-02T00:00:00+00:00"
        again = self.entry(old)
        self.assertEqual((again["published_at"], again["updated_at"]), (old["published_at"], old["updated_at"]))

    def test_changed_package_moves_updated_at_only(self):
        old = self.entry()
        old["published_at"], old["updated_at"] = "2020-01-01T00:00:00+00:00", "2020-02-02T00:00:00+00:00"
        write(self.deb, b"package two", binary=True)               # same version, other sha256
        changed = self.entry(old)
        self.assertEqual(changed["published_at"], old["published_at"])
        self.assertNotEqual(changed["updated_at"], old["updated_at"])
        self.meta = dict(self.meta, version="1.0.1")               # other version, same file
        self.assertNotEqual(self.entry(old)["updated_at"], old["updated_at"])

    def test_registry_keeps_device_targets_and_generated_at(self):
        apps = [self.entry()]
        first = make_registry.build_registry({"device_targets": ["CardputerZero"]}, apps, "id", "2026-01-01T00:00:00+00:00")
        self.assertEqual(first["device_targets"], ["CardputerZero"])
        self.assertEqual(make_registry.build_registry({}, apps, "id", "t")["device_targets"], ["CyberdeckZero"])
        again = make_registry.build_registry(first, apps, "id", "2027-01-01T00:00:00+00:00")
        self.assertEqual(again, first)                              # nothing changed: generated_at kept
        changed = make_registry.build_registry(first, apps + [dict(apps[0], version="9")], "id", "2027-01-01T00:00:00+00:00")
        self.assertEqual(changed["generated_at"], "2027-01-01T00:00:00+00:00")

    def test_check_registry_flags_a_stale_field(self):
        registry = {"apps": [self.entry()]}
        write(os.path.join(self.root, "registry.json"), json.dumps(registry))
        self.assertEqual(check_registry.check_repo(self.root), [])
        registry["apps"][0]["min_size"] = "320x170"
        write(os.path.join(self.root, "registry.json"), json.dumps(registry))
        self.assertEqual(len(check_registry.check_repo(self.root)), 1)
        del registry["apps"][0]["portrait_min_size"]
        registry["apps"][0]["min_size"] = "640x480"
        write(os.path.join(self.root, "registry.json"), json.dumps(registry))
        self.assertEqual(len(check_registry.check_repo(self.root)), 1)


if __name__ == "__main__":
    unittest.main()
