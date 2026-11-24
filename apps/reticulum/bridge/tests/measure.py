# SPDX-License-Identifier: MIT
"""Indicative memory and CPU of an idle bridge (PC numbers are not Zero 2 W numbers).

    python3 tests/measure.py [--seconds 60] [--no-lxmf]

Starts the bridge on the first-run configuration (every interface off) in a temporary folder, takes the time until its
socket answers, waits, then prints VmRSS, VmHWM, threads and the CPU used during the idle window, and stops it.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from reticulum_bridge import rnsconfig  # noqa: E402
from reticulum_bridge.client import Connection  # noqa: E402


def cpu_seconds(pid):
    with open("/proc/%d/stat" % pid) as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")      # utime + stime


def status(pid):
    with open("/proc/%d/status" % pid) as f:
        return {k: v.strip() for k, v in (line.split(":", 1) for line in f) if k in ("VmRSS", "VmHWM", "Threads")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--no-lxmf", action="store_true")
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="rnsbridge-measure-")
    cfg, data, run = (os.path.join(tmp, n) for n in ("cfg", "data", "run"))
    os.makedirs(cfg)
    with open(os.path.join(cfg, "config"), "w") as f:
        f.write(rnsconfig.FIRST_RUN.replace("  share_instance = Yes\n", "  share_instance = Yes\n  instance_name = "
                                            "rnsbridgemeasure%d\n" % os.getpid()))
    env = dict(os.environ, PYTHONPATH=os.path.dirname(HERE))
    env.pop("XDG_RUNTIME_DIR", None)
    args = [sys.executable, "-m", "reticulum_bridge", "--config", cfg, "--data", data, "--run", run]
    t0 = time.monotonic()
    with open(os.path.join(tmp, "bridge.log"), "w") as log:
        p = subprocess.Popen(args + (["--no-lxmf"] if a.no_lxmf else []), env=env, stdout=log, stderr=log)
        c = None
        while c is None and time.monotonic() - t0 < 60:
            try:
                c = Connection(os.path.join(run, "bridge.sock"))
                c.call("hello")
            except (OSError, RuntimeError):
                c = None
                time.sleep(0.05)
        ready = time.monotonic() - t0
        c0, w0 = cpu_seconds(p.pid), time.monotonic()
        time.sleep(a.seconds)
        c1, w1 = cpu_seconds(p.pid), time.monotonic()
        st = status(p.pid)
        c.call("service.stop")
        c.close()
        rc = p.wait(30)
    print("lxmf=%s ready_s=%.2f idle_window_s=%.0f idle_cpu_pct=%.2f cpu_s_total=%.2f VmRSS=%s VmHWM=%s threads=%s "
          "exit=%d" % ("off" if a.no_lxmf else "on", ready, w1 - w0, 100 * (c1 - c0) / (w1 - w0), c1, st["VmRSS"],
                       st["VmHWM"], st["Threads"], rc))


if __name__ == "__main__":
    main()
