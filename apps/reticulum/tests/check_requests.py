#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks what the app asked of the fake bridge (and of the fake systemctl) during one scene, per size.

    python3 check_requests.py REQUESTS.log SYSTEMCTL.log SCENE.scene SIZE[,SIZE...]

Always: no `lxmf.send`, no `iface.outgoing` with outgoing true, no `iface.enable` with allow_sending true, no `outgoing` or `enabled` in an
`iface.add` (a new connection is created off and listen-only by the bridge), no `paths` request. The scene file's header
lines say what else holds (each size is checked on its own):

    # expect-verbs: iface.enable, iface.edit      every one of these was asked at least once
    # forbid-verbs: iface.remove                  none of these was asked
    # transmit-requests: 1                        exactly this many iface.outgoing with true or iface.enable with allow_sending (default 0)
    # message-requests: 2                         exactly this many messages.send + messages.retry (default 0)
    # announce-requests: 1                        exactly this many announce (default 0)
    # systemctl-has: enable reticulum             this text is in the fake systemctl's calls (of every size)
    # systemctl-lacks: stop reticulum
"""
import json
import re
import sys


def header(scene, key):
    out = []
    for line in open(scene, encoding="utf-8"):
        m = re.match(r"#\s*%s:\s*(.*)$" % re.escape(key), line)
        if m:
            out.append(m.group(1).strip())
    return out


def main():
    req_log, sys_log, scene, sizes = sys.argv[1:5]
    sizes = [s for s in sizes.split(",") if s]
    reqs = {s: [] for s in sizes}
    for line in open(req_log, encoding="utf-8"):
        if line.strip():
            r = json.loads(line)
            reqs.setdefault(r["name"], []).append(r)
    calls = [l.strip() for l in open(sys_log, encoding="utf-8")] if sys_log != "-" else []
    expect = [v.strip() for line in header(scene, "expect-verbs") for v in line.split(",") if v.strip()]
    forbid = [v.strip() for line in header(scene, "forbid-verbs") for v in line.split(",") if v.strip()]
    tx_expected = int((header(scene, "transmit-requests") or ["0"])[0])
    msg_expected = int((header(scene, "message-requests") or ["0"])[0])
    ann_expected = int((header(scene, "announce-requests") or ["0"])[0])
    failed = 0

    def bad(size, why):
        nonlocal failed
        failed += 1
        print("FAIL  %s %s: %s" % (scene.rsplit("/", 1)[-1], size, why))

    for size in sizes:
        rs = reqs.get(size, [])
        verbs = [r["verb"] for r in rs]
        for forbidden in ("lxmf.send", "paths"):
            if forbidden in verbs:
                bad(size, "the app asked for %s" % forbidden)
        msgs = [r for r in rs if r["verb"] in ("messages.send", "messages.retry")]
        if len(msgs) != msg_expected:
            bad(size, "%d message send/retry request(s), expected %d" % (len(msgs), msg_expected))
        anns = [r for r in rs if r["verb"] == "announce"]
        if len(anns) != ann_expected:
            bad(size, "%d announce request(s), expected %d" % (len(anns), ann_expected))
        # allowing sending: iface.outgoing true, or switching on a kind with no listen-only state (allow_sending true)
        tx = [r for r in rs if (r["verb"] == "iface.outgoing" and r["params"].get("outgoing") is True)
              or (r["verb"] == "iface.enable" and r["params"].get("allow_sending") is True)]
        if len(tx) != tx_expected:
            bad(size, "%d request(s) allowing sending, expected %d" % (len(tx), tx_expected))
        for r in rs:
            if r["verb"] == "iface.add":
                extra = {"enabled", "outgoing"} & set(r["params"].get("options", {})) | ({"enabled", "outgoing"} & set(r["params"]))
                if extra:
                    bad(size, "iface.add carried %s" % ", ".join(sorted(extra)))
        for v in expect:
            if v not in verbs:
                bad(size, "expected a request for %s" % v)
        for v in forbid:
            if v in verbs:
                bad(size, "a request for %s was not expected" % v)
    for text in header(scene, "systemctl-has"):
        if not any(text in c for c in calls):
            bad("all", "the fake systemctl was never called with %r" % text)
    for text in header(scene, "systemctl-lacks"):
        if any(text in c for c in calls):
            bad("all", "the fake systemctl was called with %r" % text)
    total = sum(len(v) for v in reqs.values())
    print("%s  %s: %d request(s), %d systemctl call(s) checked" % ("FAIL" if failed else "PASS", scene.rsplit("/", 1)[-1], total, len(calls)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
