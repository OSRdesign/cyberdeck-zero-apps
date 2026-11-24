# SPDX-License-Identifier: MIT
"""Serial port arbitration (report 057 4.4): one owner per device.

RNS opens a serial device without flock or TIOCEXCL, so before RNS opens a configured port the bridge:
  1. refuses a port another process has open (/proc/<pid>/fd), naming that program ("used by mesh-hop");
  2. opens the device itself and takes flock(LOCK_EX | LOCK_NB), refusing it when another program holds the lock
     (Mesh Hop takes one), and keeps that descriptor for as long as the interface is up, so the other serial apps'
     checks see the port as busy.
No TIOCEXCL here: it would also keep RNS's own open (same process, later) out. ble:// and tcp:// ports are not devices.
"""
import errno
import fcntl
import os

from . import BridgeError


def is_device(port):
    return isinstance(port, str) and port.startswith("/") and "://" not in port


def holders(path, proc="/proc", self_pid=None):
    """[(pid, comm)] of the other processes that have the device open (only those we may read: the user's own)."""
    self_pid = os.getpid() if self_pid is None else self_pid
    try:
        target = os.path.realpath(path)
    except OSError:
        return []
    found = []
    for pid in os.listdir(proc):
        if not pid.isdigit() or int(pid) == self_pid:
            continue
        fd_dir = os.path.join(proc, pid, "fd")
        try:
            links = [os.readlink(os.path.join(fd_dir, fd)) for fd in os.listdir(fd_dir)]
        except OSError:
            continue                                     # not ours to read, or gone: unknown, not busy
        if any(link == target or (link.startswith("/dev/") and os.path.realpath(link) == target) for link in links):
            try:
                with open(os.path.join(proc, pid, "comm")) as f:
                    comm = f.read().strip()
            except OSError:
                comm = ""
            found.append((int(pid), comm))
    return found


def busy_error(path, who):
    name = ", ".join(sorted({c or "another program" for _, c in who})) or "another program"
    return BridgeError("busy", "%s is used by %s. Close that program first." % (path, name), port=path,
                       holders=[{"pid": p, "name": c} for p, c in who])


class PortLocks:
    """Interface name -> the descriptor that holds its port's flock."""

    def __init__(self, proc="/proc"):
        self.proc = proc
        self.held = {}

    def acquire(self, name, path):
        """Locks path for interface name (a new path is locked before the old one is let go)."""
        if name in self.held and self.held[name][0] == path:
            return
        fd = self._open_locked(path)
        self.release(name)
        self.held[name] = (path, fd)

    def _open_locked(self, path):
        who = holders(path, self.proc)
        if who:
            raise busy_error(path, who)
        try:
            fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC)
        except OSError as e:
            if e.errno in (errno.EACCES, errno.EPERM):
                raise BridgeError("permission", "Cannot open %s: your user is not in the dialout group. Run: sudo "
                                  "usermod -aG dialout $USER, then log out and in." % path, port=path)
            if e.errno == errno.EBUSY:
                raise busy_error(path, [])
            if e.errno in (errno.ENOENT, errno.ENODEV, errno.ENXIO):
                raise BridgeError("not_found", "%s: no such device (is it plugged in?)" % path, port=path)
            raise BridgeError("port_error", "%s: %s" % (path, e.strerror), port=path)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as e:
            os.close(fd)
            if e.errno in (errno.EWOULDBLOCK, errno.EAGAIN):
                raise busy_error(path, [])
            raise BridgeError("port_error", "%s: cannot lock (%s)" % (path, e.strerror), port=path)
        return fd

    def release(self, name):
        path_fd = self.held.pop(name, None)
        if path_fd:
            try:
                fcntl.flock(path_fd[1], fcntl.LOCK_UN)
            finally:
                os.close(path_fd[1])

    def release_all(self):
        for name in list(self.held):
            self.release(name)
