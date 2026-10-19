#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A small stand-in for BlueZ 5.82 and one MeshCore BLE companion, on a PRIVATE D-Bus, for tests/ble/run_mock.sh.

It plays what the Mesh Hop BLE backend (src/core/ble/bluez_backend.cpp) uses: ObjectManager (GetManagedObjects,
InterfacesAdded / Removed), Properties (PropertiesChanged), Adapter1 (SetDiscoveryFilter, Start / StopDiscovery),
AgentManager1 (RegisterAgent; RequestDefaultAgent is recorded as an error: the app must never take the default agent),
Device1 (Connect, Pair through the CALLER's agent like src/device.c, CancelPairing, Disconnect), GattService1 and two
GattCharacteristic1 of the Nordic UART service (AcquireNotify with a SOCK_SEQPACKET socket, StartNotify, WriteValue).
The "board" answers DEVICE_QUERY with the real DEVICE_INFO of a XIAO nRF52840 and APP_START with a SELF_INFO.

    python3 mock_bluez.py [--pin 123456] [--mtu 247] [--no-acquire] [--paired] [--drop-after S --away S] [--report FILE]

--mtu N cuts every notification in N - 3 byte pieces like the nRF52 Bluefruit library; --no-acquire refuses
AcquireNotify (NotPermitted) so the client must use StartNotify; --drop-after S drops the link S seconds after APP_START
and keeps the board out of range (Connect fails) for --away S. On SIGTERM it writes a report (one key=value per line)
to --report: what the client did (agent, default agent, writes, write types, pairing answer).
Uses dbus-python and PyGObject (GLib main loop); DBUS_SYSTEM_BUS_ADDRESS names the private bus.
"""

import argparse
import signal
import socket
import struct
import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
ADDR = "EC:EB:45:39:28:2D"
OTHER = "34:B7:DA:62:1E:ED"
ADAPTER = "/org/bluez/hci0"
DEV = ADAPTER + "/dev_EC_EB_45_39_28_2D"
DEV2 = ADAPTER + "/dev_34_B7_DA_62_1E_ED"
SVC = DEV + "/service000c"
RXC = SVC + "/char000d"
TXC = SVC + "/char000f"

P = "org.freedesktop.DBus.Properties"
OM = "org.freedesktop.DBus.ObjectManager"

# the DEVICE_INFO payload of the user's XIAO nRF52840 (v1.15.0), as in tests/unit/test_core.cpp
DEVICE_INFO = bytes.fromhex("0d0baf280000000031392d4170722d32303236005365656564205869616f2d6e7266353200000000000000000000"
                            "000000000000000000000000000076312e31352e302d6465653365323600000000000001")


def self_info():
    b = bytes([5, 1, 22, 30]) + bytes(range(0xA0, 0xC0))
    b += struct.pack("<ii", 48856600, 2352200) + bytes([0, 0, 0, 0])
    b += struct.pack("<II", 869525, 250000) + bytes([11, 5]) + b"Mock Board"
    return b


class Bad(dbus.exceptions.DBusException):
    def __init__(self, name, text=""):
        super().__init__(text or name)
        self._dbus_error_name = name


report = {"default_agent_requested": 0, "agent_registered": 0, "agent_capability": "", "passkey_via": "", "passkey": "",
          "writes": 0, "write_types": "", "acquire_notify": 0, "start_notify": 0, "connects": 0, "pairs": 0,
          "max_write": 0, "discovery": 0, "filter": ""}


class Obj(dbus.service.Object):
    """An object with properties per interface and PropertiesChanged."""

    def __init__(self, bus, path, props):
        super().__init__(bus, path)
        self.path = path
        self.props = props            # {iface: {name: value}}

    def set(self, iface, **kv):
        self.props[iface].update(kv)
        self.PropertiesChanged(iface, dbus.Dictionary(kv, signature="sv"), dbus.Array([], signature="s"))

    def invalidate(self, iface, name):
        self.props[iface].pop(name, None)
        self.PropertiesChanged(iface, dbus.Dictionary({}, signature="sv"), dbus.Array([name], signature="s"))

    @dbus.service.method(P, in_signature="ss", out_signature="v")
    def Get(self, iface, name):
        return self.props[iface][name]

    @dbus.service.method(P, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props.get(iface, {})

    @dbus.service.method(P, in_signature="ssv")
    def Set(self, iface, name, value):
        self.set(iface, **{name: value})

    @dbus.service.signal(P, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


class Root(dbus.service.Object):
    def __init__(self, bus, world):
        super().__init__(bus, "/")
        self.world = world

    @dbus.service.method(OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        return {o.path: o.props for o in self.world.objects()}

    @dbus.service.signal(OM, signature="oa{sa{sv}}")
    def InterfacesAdded(self, path, ifaces):
        pass

    @dbus.service.signal(OM, signature="oas")
    def InterfacesRemoved(self, path, ifaces):
        pass


class AgentManager(dbus.service.Object):
    def __init__(self, bus, world):
        super().__init__(bus, "/org/bluez")
        self.world = world

    @dbus.service.method("org.bluez.AgentManager1", in_signature="os", sender_keyword="sender")
    def RegisterAgent(self, path, capability, sender=None):
        self.world.agents[sender] = str(path)
        report["agent_registered"] += 1
        report["agent_capability"] = str(capability)

    @dbus.service.method("org.bluez.AgentManager1", in_signature="o", sender_keyword="sender")
    def UnregisterAgent(self, path, sender=None):
        self.world.agents.pop(sender, None)

    @dbus.service.method("org.bluez.AgentManager1", in_signature="o", sender_keyword="sender")
    def RequestDefaultAgent(self, path, sender=None):
        report["default_agent_requested"] += 1


class Adapter(Obj):
    def __init__(self, bus, world):
        super().__init__(bus, ADAPTER, {"org.bluez.Adapter1": {"Address": "88:A2:9E:B1:F6:FD", "Name": "mock", "Alias": "mock",
                                                                 "Powered": True, "Discovering": False, "Pairable": True}})
        self.world = world

    @dbus.service.method("org.bluez.Adapter1", in_signature="a{sv}")
    def SetDiscoveryFilter(self, f):
        report["filter"] = ",".join("%s=%s" % (k, v) for k, v in f.items())

    @dbus.service.method("org.bluez.Adapter1")
    def StartDiscovery(self):
        report["discovery"] += 1
        self.set("org.bluez.Adapter1", Discovering=True)
        GLib.timeout_add(150, self.world.advertise)

    @dbus.service.method("org.bluez.Adapter1")
    def StopDiscovery(self):
        self.set("org.bluez.Adapter1", Discovering=False)
        for d in self.world.devices:
            d.invalidate("org.bluez.Device1", "RSSI")


class Chr(Obj):
    def __init__(self, bus, world, path, uuid, flags):
        super().__init__(bus, path, {"org.bluez.GattCharacteristic1": {
            "UUID": uuid, "Service": dbus.ObjectPath(SVC), "Flags": dbus.Array(flags, signature="s"),
            "NotifyAcquired": False, "Notifying": False, "MTU": dbus.UInt16(world.mtu)}})
        self.world = world

    @dbus.service.method("org.bluez.GattCharacteristic1", in_signature="aya{sv}")
    def WriteValue(self, value, options):
        data = bytes(value)
        report["writes"] += 1
        report["max_write"] = max(report["max_write"], len(data))
        kinds = set(filter(None, report["write_types"].split(",")))
        kinds.add(str(options.get("type", "")))
        report["write_types"] = ",".join(sorted(kinds))
        if len(data) > 176:
            raise Bad("org.bluez.Error.InvalidValueLength")
        GLib.timeout_add(30, self.world.board_answer, data)

    @dbus.service.method("org.bluez.GattCharacteristic1", in_signature="a{sv}", out_signature="hq")
    def AcquireNotify(self, options):
        if self.world.no_acquire:
            raise Bad("org.bluez.Error.NotPermitted", "Notify acquisition not permitted")
        report["acquire_notify"] += 1
        mine, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.world.notify_sock = mine
        fd = dbus.types.UnixFd(theirs.fileno())     # dbus-python keeps its own copy
        theirs.close()
        self.set("org.bluez.GattCharacteristic1", NotifyAcquired=True)
        return fd, dbus.UInt16(self.world.mtu)

    @dbus.service.method("org.bluez.GattCharacteristic1")
    def StartNotify(self):
        report["start_notify"] += 1
        self.world.notifying = True
        self.set("org.bluez.GattCharacteristic1", Notifying=True)

    @dbus.service.method("org.bluez.GattCharacteristic1")
    def StopNotify(self):
        self.world.notifying = False
        self.set("org.bluez.GattCharacteristic1", Notifying=False)


class Device(Obj):
    def __init__(self, bus, world, path, addr, name, nus, rssi):
        props = {"Address": addr, "AddressType": "random", "Name": name, "Alias": name, "Paired": False, "Bonded": False,
                 "Trusted": False, "Connected": False, "ServicesResolved": False, "Adapter": dbus.ObjectPath(ADAPTER),
                 "UUIDs": dbus.Array([NUS] if nus else [], signature="s")}
        super().__init__(bus, path, {"org.bluez.Device1": props})
        self.world = world
        self.rssi = rssi
        self.companion = nus

    def resolve(self):
        self.world.add_gatt()
        self.set("org.bluez.Device1", ServicesResolved=True)

    @dbus.service.method("org.bluez.Device1", async_callbacks=("ok", "err"))
    def Connect(self, ok=None, err=None):
        report["connects"] += 1
        if not self.world.in_range:
            GLib.timeout_add(300, lambda: err(Bad("org.bluez.Error.Failed", "le-connection-abort-by-local")) and False)
            return
        self.set("org.bluez.Device1", Connected=True)

        def done():
            if self.companion:
                self.resolve()
            ok()
            return False
        GLib.timeout_add(100, done)

    @dbus.service.method("org.bluez.Device1", sender_keyword="sender", async_callbacks=("ok", "err"))
    def Pair(self, sender=None, ok=None, err=None):
        report["pairs"] += 1
        if self.props["org.bluez.Device1"]["Paired"]:
            err(Bad("org.bluez.Error.AlreadyExists"))
            return
        path = self.world.agents.get(sender)
        report["passkey_via"] = "caller" if path else "default"
        if not path:
            err(Bad("org.bluez.Error.AuthenticationFailed", "no agent of the caller (the mock does not ask the default one)"))
            return
        self.set("org.bluez.Device1", Connected=True)
        agent = dbus.Interface(self.world.bus.get_object(sender, path, introspect=False), "org.bluez.Agent1")

        def answered(passkey):
            report["passkey"] = "%06u" % int(passkey)
            if int(passkey) == self.world.pin:
                self.set("org.bluez.Device1", Paired=True, Bonded=True)
                self.resolve()
                ok()
            else:
                self.set("org.bluez.Device1", Connected=False)
                err(Bad("org.bluez.Error.AuthenticationFailed", "Authentication Failed"))

        def refused(e):
            report["passkey"] = "refused"
            self.set("org.bluez.Device1", Connected=False)
            err(Bad("org.bluez.Error.AuthenticationCanceled", "Authentication Canceled"))

        agent.RequestPasskey(dbus.ObjectPath(self.path), reply_handler=answered, error_handler=refused, timeout=60)

    @dbus.service.method("org.bluez.Device1")
    def CancelPairing(self):
        pass

    @dbus.service.method("org.bluez.Device1")
    def Disconnect(self):
        self.world.drop_link()


class World:
    def __init__(self, bus, args):
        self.bus = bus
        self.pin = args.pin
        self.mtu = args.mtu
        self.no_acquire = args.no_acquire
        self.drop_after = args.drop_after
        self.away = args.away
        self.dropped = False
        self.in_range = True
        self.agents = {}
        self.notify_sock = None
        self.notifying = False
        self.root = Root(bus, self)
        self.manager = AgentManager(bus, self)
        self.adapter = Adapter(bus, self)
        self.board = Device(bus, self, DEV, ADDR, "MeshCore-Mock", True, -52)
        self.other = Device(bus, self, DEV2, OTHER, "Meshtastic_1eec", False, -70)
        self.devices = [self.board, self.other]
        self.gatt = []
        if args.paired:
            self.board.props["org.bluez.Device1"].update(Paired=True, Bonded=True)

    def objects(self):
        return [self.adapter] + self.devices + self.gatt

    def advertise(self):
        for d in self.devices:
            d.set("org.bluez.Device1", RSSI=dbus.Int16(d.rssi))
        return False

    def add_gatt(self):
        if self.gatt:
            return
        svc = Obj(self.bus, SVC, {"org.bluez.GattService1": {"UUID": NUS, "Device": dbus.ObjectPath(DEV), "Primary": True}})
        rx = Chr(self.bus, self, RXC, RX, ["write", "write-without-response"])
        tx = Chr(self.bus, self, TXC, TX, ["read", "notify"])
        self.gatt = [svc, rx, tx]
        for o in self.gatt:
            self.root.InterfacesAdded(o.path, o.props)

    def drop_link(self):
        if self.notify_sock:
            self.notify_sock.close()
            self.notify_sock = None
        self.notifying = False
        for o in self.gatt:
            if "org.bluez.GattCharacteristic1" in o.props:
                o.props["org.bluez.GattCharacteristic1"]["NotifyAcquired"] = False
        self.board.set("org.bluez.Device1", Connected=False, ServicesResolved=False)

    def notify(self, frame):
        chunk = self.mtu - 3
        pieces = [frame[i:i + chunk] for i in range(0, len(frame), chunk)] or [frame]
        for piece in pieces:
            if self.notify_sock:
                self.notify_sock.send(piece)
            elif self.notifying:
                tx = self.gatt[2]
                tx.set("org.bluez.GattCharacteristic1", Value=dbus.Array(piece, signature="y"))

    def board_answer(self, cmd):
        if cmd[:1] == b"\x16":
            self.notify(DEVICE_INFO)
        elif cmd[:1] == b"\x01":
            self.notify(self_info())
            if self.drop_after and not self.dropped:
                self.dropped = True
                GLib.timeout_add(int(self.drop_after * 1000), self.walk_away)
        else:
            self.notify(b"\x01\x01")          # ERR_CODE_UNSUPPORTED_CMD
        return False

    def walk_away(self):
        """The board goes out of range (the link drops, Connect fails) for --away seconds, then comes back."""
        report["drops"] = report.get("drops", 0) + 1
        self.in_range = False
        self.drop_link()

        def back():
            self.in_range = True
            return False
        GLib.timeout_add(int(self.away * 1000), back)
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pin", type=int, default=123456)
    ap.add_argument("--mtu", type=int, default=247)
    ap.add_argument("--no-acquire", action="store_true")
    ap.add_argument("--paired", action="store_true")
    ap.add_argument("--drop-after", type=float, default=0, help="seconds after APP_START: the board walks away")
    ap.add_argument("--away", type=float, default=3, help="seconds out of range")
    ap.add_argument("--report", default="")
    args = ap.parse_args()
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    name = dbus.service.BusName("org.bluez", bus)          # noqa: F841 (keeps the name)
    World(bus, args)
    loop = GLib.MainLoop()

    def finish(*_):
        if args.report:
            with open(args.report, "w", encoding="utf-8") as f:
                for k, v in report.items():
                    f.write("%s=%s\n" % (k, v))
        loop.quit()
    for sig in (signal.SIGTERM, signal.SIGINT):
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, lambda: finish() or False)
    print("mock bluez ready", flush=True)
    loop.run()


if __name__ == "__main__":
    sys.exit(main())
