#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A fake of the launcher's launcher.sock for the app's scenes: the clipboard verbs and `hello`, nothing else.

    python3 fake_launcher.py --dir DIR [--mode ok|old|oldquiet] [--clip TEXT]

Serves DIR/launcher.sock and writes DIR/board.state (APPLAUNCH_SOCKET=DIR/launcher.sock in the scene's
environment). Modes:
  ok        a launcher with the clipboard: `clipboard` is in board.state's requests= and every form works (set, set-len, get,
            get-len, clear, 4096 bytes at most, the text kept in memory only)
  old       a launcher from before the verb: board.state has no `clipboard` in requests=, every `clipboard` request is
            answered `err unknown`
  oldquiet  the same answers, but board.state is not written (the app learns it only from the answer)
--clip TEXT preloads the clipboard (ok mode; a backslash and n in it is a newline).

Every request is appended to DIR/launcher.log as one line: `<verb> <n bytes>` for the clipboard verbs and, only here in the
test fake, `set-text <text>` so that a scene can check what the app copied (the real launcher never logs the text). The
checks of run_scenes.sh read it (header lines `# clipboard-set:`, `# clipboard-sets:`, `# clipboard-gets:`).
Stops on SIGTERM or when DIR/stop appears.
"""
import argparse
import os
import socket
import sys
import threading
import time

LIMIT = 4096


def serve(conn, state, mode, log):
    f = conn.makefile("rwb", buffering=0)

    def say(text):
        f.write(text.encode("utf-8") + b"\n")

    try:
        while True:
            line = f.readline(300)
            if not line:
                return
            req = line.decode("utf-8", "replace").rstrip("\n")
            verb, _, rest = req.partition(" ")
            if verb == "hello":
                say("ok 1 launcher=0.10.12-fake app=reticulum verbs=hello" + ("" if mode != "ok" else ",clipboard"))
                continue
            if verb != "clipboard":
                say("err unknown")
                continue
            sub, _, arg = rest.partition(" ")
            if mode != "ok":
                log(sub if sub in ("get", "get-len", "clear") else "set %d" % len(arg))  # what was asked, for the checks
                say("err unknown")
                continue
            if sub == "set":
                data = arg.encode("utf-8")
                if not data or len(data) > 240:
                    say("err bad")
                    continue
                state["text"] = data
                log("set %d" % len(data))
                log("set-text %s" % arg)
                say("ok %d" % len(data))
            elif sub == "set-len":
                try:
                    n = int(arg)
                except ValueError:
                    say("err bad")
                    continue
                if n < 1 or n > LIMIT:
                    say("err size")
                    return
                data = b""
                while len(data) < n + 1:
                    chunk = f.read(n + 1 - len(data))
                    if not chunk:
                        return
                    data += chunk
                if data[-1:] != b"\n":
                    say("err bad")
                    return
                state["text"] = data[:-1]
                log("set-len %d" % n)
                log("set-text %s" % data[:-1].decode("utf-8", "replace").replace("\n", "\\n"))
                say("ok %d" % n)
            elif sub == "get":
                log("get")
                t = state.get("text")
                if not t:
                    say("err empty")
                elif b"\n" in t or b"\t" in t or len(t) > 240:
                    say("err size")
                else:
                    say("ok " + t.decode("utf-8", "replace"))
            elif sub == "get-len":
                log("get-len")
                t = state.get("text")
                if not t:
                    say("err empty")
                else:
                    f.write(("ok %d\n" % len(t)).encode() + t + b"\n")
            elif sub == "clear":
                state["text"] = b""
                log("clear")
                say("ok")
            else:
                say("err bad")
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--mode", default="ok", choices=["ok", "old", "oldquiet"])
    ap.add_argument("--clip", default="")
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    sock_path = os.path.join(a.dir, "launcher.sock")
    logf = open(os.path.join(a.dir, "launcher.log"), "a", encoding="utf-8", buffering=1)
    lock = threading.Lock()

    def log(s):
        with lock:
            logf.write(s + "\n")

    state = {"text": a.clip.replace("\\n", "\n").encode("utf-8")}
    if a.mode != "oldquiet":
        requests = "hello,caps,toast,beep,volume,brightness,dim,screen" + (",clipboard" if a.mode == "ok" else "")
        tmp = os.path.join(a.dir, "board.state.tmp")
        with open(tmp, "w", encoding="utf-8") as bs:
            bs.write("v=1\ngen=1\npid=%d\nlauncher=0.10.12-fake\nrequests=%s\n" % (os.getpid(), requests))
        os.replace(tmp, os.path.join(a.dir, "board.state"))
    if os.path.exists(sock_path):
        os.unlink(sock_path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock_path)
    srv.listen(8)
    srv.settimeout(0.2)
    print("ready", flush=True)
    stop = os.path.join(a.dir, "stop")
    while not os.path.exists(stop):
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        threading.Thread(target=serve, args=(conn, state, a.mode, log), daemon=True).start()
    srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
