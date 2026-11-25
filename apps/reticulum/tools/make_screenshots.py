#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Collects the app's screenshots from a run of the headless scenes into apps/reticulum/screenshots/ (numbered, as the
Store shows them in file-name order).

These are RENDERED pictures from the headless harness (the real UI code on a plain LVGL display, the fake bridge's canned
data, a fixed clock), not photographs of a board. The numbers a LoRa radio shows in them (the "mixed" scenario) are
SYNTHETIC placeholders, not a band plan.

    python3 tools/make_screenshots.py --out ~/cdk-build/reticulum/host   # the folder that holds out/ and out-bridge/

The sizes: 640x480 (the deck, the reference size), 480x320 (small) and 480x800 (portrait, PDA mode). A page that a size
did not render is skipped with a note. Phase P2 (contacts and messages) goes on from number 45 in the same way, at 640x480,
480x320, 480x800 and 800x480 (a selection at the last three); the first 44 files keep their names.
"""
import argparse
import os
import shutil
import sys

# (page name, folder under the scenes' output, scene, shot)
PAGES = [
    ("home", "out-bridge", "walk-mixed", "home"),
    ("connections", "out-bridge", "walk-mixed", "connections"),
    ("connection-listening", "out-bridge", "radio", "lora-on"),
    ("connection-radio-empty", "out-bridge", "radio", "lora-off"),
    ("radio-value-refused", "out-bridge", "radio", "frequency-error"),
    ("allow-sending", "out-bridge", "sending", "allow-dialog"),
    ("add-kinds", "out-bridge", "add", "kinds"),
    ("add-form", "out-bridge", "add", "form-lora"),
    ("status", "out-bridge", "walk-mixed", "status"),
    ("status-heard", "out-bridge", "walk-mixed", "status-end"),
    ("settings", "out-bridge", "walk-mixed", "settings"),
    ("name", "out-bridge", "settings", "name-field"),
    ("my-address", "out-bridge", "settings", "address"),
    ("port-busy", "out-bridge", "busyport", "busy"),
    ("service-starting", "out-bridge", "start", "starting"),
    ("service-not-running", "out", "nobridge", "not-running"),
]
SIZES = ["640x480", "480x320", "480x800"]

# phase P2: (page name, folder, scene, shot); numbered after the 44 above, per size in SIZES_P2
PAGES_P2 = [
    ("contacts", "out-bridge", "chat-walk", "contacts"),
    ("contacts-empty", "out-bridge", "chat-empty", "contacts-empty"),
    ("messages", "out-bridge", "chat-walk", "messages"),
    ("conversation", "out-bridge", "chat-walk", "conversation"),
    ("compose-keys", "out-bridge", "chat-messages", "compose-keys"),
    ("contact", "out-bridge", "chat-walk", "contact"),
    ("remove-confirm", "out-bridge", "chat-walk", "remove-confirm"),
    ("add-contact", "out-bridge", "chat-walk", "add-contact"),
    ("add-address-keys", "out-bridge", "chat-walk", "add-address-keys"),
    ("announce-confirm", "out-bridge", "chat-announce", "announce-confirm"),
    ("my-address", "out-bridge", "chat-walk", "my-address"),
    ("heard", "out-bridge", "chat-walk", "heard"),
    ("messages-empty", "out-bridge", "chat-empty", "messages-empty"),
    ("conversation-failed", "out-bridge", "chat-messages", "retry-focus"),
    ("compose-blocked", "out-bridge", "chat-blocked", "blocked"),
    ("announce-blocked", "out-bridge", "chat-blocked", "announce-blocked"),
    ("conversation-options", "out-bridge", "chat-messages", "options"),
    ("delete-confirm", "out-bridge", "chat-messages", "delete-confirm"),
    ("compose-near-limit", "out-bridge", "chat-limit", "near-limit"),
    ("compose-over-limit", "out-bridge", "chat-limit", "over-limit"),
    ("settings-alerts", "out-bridge", "chat-alerts", "alerts-off"),
    ("home-contacts-messages", "out-bridge", "chat-walk", "home"),
    ("compose-typed", "out-bridge", "chat-messages", "compose-typed"),
    ("conversation-sending", "out-bridge", "chat-messages", "sending"),
    ("conversation-delivered", "out-bridge", "chat-messages", "delivered"),
]
# every phase P2 page at the deck's size, a selection at the others (the numbers stay under 100, the Store lists by file name)
SIZES_P2 = {
    "640x480": None,
    "480x320": ["contacts", "messages", "conversation", "compose-keys", "contact", "my-address", "compose-blocked", "compose-over-limit"],
    "480x800": ["contacts", "messages", "conversation", "compose-typed", "add-contact", "heard", "conversation-options", "announce-confirm"],
    "800x480": ["messages", "conversation", "compose-keys", "contacts", "home-contacts-messages"],
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="the build's host folder (holds out/ and out-bridge/)")
    ap.add_argument("--dest", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "screenshots"))
    args = ap.parse_args()
    os.makedirs(args.dest, exist_ok=True)
    for old in os.listdir(args.dest):
        if old.endswith(".png"):
            os.unlink(os.path.join(args.dest, old))
    n = 0
    for size in SIZES:
        for page, folder, scene, shot in PAGES:
            if size == "480x800" and page in ("radio-value-refused", "service-starting", "port-busy", "status-heard"):
                continue
            src = os.path.join(args.out, folder, scene, size, shot + ".png")
            if not os.path.exists(src):
                print("skipped %s at %s: no %s" % (page, size, src), file=sys.stderr)
                continue
            n += 1
            shutil.copyfile(src, os.path.join(args.dest, "%02d-%s-%s.png" % (n, page, size)))
    for size, only in SIZES_P2.items():
        for page, folder, scene, shot in PAGES_P2:
            if only is not None and page not in only:
                continue
            src = os.path.join(args.out, folder, scene, size, shot + ".png")
            if not os.path.exists(src):
                print("skipped %s at %s: no %s" % (page, size, src), file=sys.stderr)
                continue
            n += 1
            shutil.copyfile(src, os.path.join(args.dest, "%02d-%s-%s.png" % (n, page, size)))
    print("%d screenshots in %s" % (n, args.dest))


if __name__ == "__main__":
    main()
