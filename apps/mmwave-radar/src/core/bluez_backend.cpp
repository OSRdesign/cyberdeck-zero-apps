/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: BlueZ over sd-bus, loaded at run time (see bluez_backend.hpp). The few sd-bus types are declared here as
 * the C ABI of libsystemd.so.0 has them (sd-bus.h; sd_bus_error is public and stable), so no systemd header is needed to
 * build: the same declarations as Mesh Hop's backend, which runs on the deck.
 */
#include "bluez_backend.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <list>
#include <map>
#include <poll.h>
#include <type_traits>
#include <unistd.h>

namespace mmw {

namespace sd {
struct bus;
struct message;
struct slot;
struct error {
    const char *name;
    const char *message;
    int need_free;
};
using handler = int (*)(message *, void *, error *);
} // namespace sd

namespace {

constexpr const char *kBluez = "org.bluez";
constexpr const char *kObjMgr = "org.freedesktop.DBus.ObjectManager";
constexpr const char *kDevice = "org.bluez.Device1";
constexpr const char *kAdapter = "org.bluez.Adapter1";
constexpr const char *kChr = "org.bluez.GattCharacteristic1";
constexpr const char *kSvc = "org.bluez.GattService1";
constexpr std::uint64_t kSec = 1000000; /* microseconds */

struct Api {
    void *lib = nullptr;
    int (*open_system)(sd::bus **) = nullptr;
    sd::bus *(*flush_close_unref)(sd::bus *) = nullptr;
    int (*process)(sd::bus *, sd::message **) = nullptr;
    int (*flush)(sd::bus *) = nullptr;
    int (*call_async)(sd::bus *, sd::slot **, sd::message *, sd::handler, void *, std::uint64_t) = nullptr;
    int (*add_match)(sd::bus *, sd::slot **, const char *, sd::handler, void *) = nullptr;
    sd::slot *(*slot_unref)(sd::slot *) = nullptr;
    int (*new_method_call)(sd::bus *, sd::message **, const char *, const char *, const char *, const char *) = nullptr;
    sd::message *(*msg_unref)(sd::message *) = nullptr;
    int (*append_basic)(sd::message *, char, const void *) = nullptr;
    int (*open_container)(sd::message *, char, const char *) = nullptr;
    int (*close_container)(sd::message *) = nullptr;
    int (*read_basic)(sd::message *, char, void *) = nullptr;
    int (*read_array)(sd::message *, char, const void **, std::size_t *) = nullptr;
    int (*enter_container)(sd::message *, char, const char *) = nullptr;
    int (*exit_container)(sd::message *) = nullptr;
    int (*skip)(sd::message *, const char *) = nullptr;
    int (*peek_type)(sd::message *, char *, const char **) = nullptr;
    int (*is_method_error)(sd::message *, const char *) = nullptr;
    const sd::error *(*get_error)(sd::message *) = nullptr;
    const char *(*get_path)(sd::message *) = nullptr;

    bool load(std::string &why)
    {
        if (lib) return true;
        lib = ::dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            const char *e = ::dlerror();
            why = std::string("libsystemd.so.0 is missing (") + (e ? e : "dlopen failed") + ")";
            return false;
        }
        bool ok = true;
        auto sym = [&](auto &fn, const char *name) {
            void *p = ::dlsym(lib, name);
            if (!p) ok = false;
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
        };
        sym(open_system, "sd_bus_open_system");
        sym(flush_close_unref, "sd_bus_flush_close_unref");
        sym(process, "sd_bus_process");
        sym(flush, "sd_bus_flush");
        sym(call_async, "sd_bus_call_async");
        sym(add_match, "sd_bus_add_match");
        sym(slot_unref, "sd_bus_slot_unref");
        sym(new_method_call, "sd_bus_message_new_method_call");
        sym(msg_unref, "sd_bus_message_unref");
        sym(append_basic, "sd_bus_message_append_basic");
        sym(open_container, "sd_bus_message_open_container");
        sym(close_container, "sd_bus_message_close_container");
        sym(read_basic, "sd_bus_message_read_basic");
        sym(read_array, "sd_bus_message_read_array");
        sym(enter_container, "sd_bus_message_enter_container");
        sym(exit_container, "sd_bus_message_exit_container");
        sym(skip, "sd_bus_message_skip");
        sym(peek_type, "sd_bus_message_peek_type");
        sym(is_method_error, "sd_bus_message_is_method_error");
        sym(get_error, "sd_bus_message_get_error");
        sym(get_path, "sd_bus_message_get_path");
        if (!ok) {
            ::dlclose(lib);
            lib = nullptr;
            why = "libsystemd.so.0 lacks sd-bus functions (too old)";
        }
        return ok;
    }
    ~Api()
    {
        if (lib) ::dlclose(lib);
    }
};

/* One value of an a{sv} dictionary, the types BlueZ uses for the properties read here. */
struct Val {
    std::string s;
    std::int64_t i = 0;
    std::vector<std::string> as;
};
using Props = std::map<std::string, Val>;

std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/* "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF" -> "AA:BB:CC:DD:EE:FF" ("" when it is not a device path) */
std::string address_from_path(const std::string &path)
{
    const std::size_t p = path.rfind("/dev_");
    if (p == std::string::npos || path.size() - (p + 5) != 17) return std::string();
    std::string a = path.substr(p + 5);
    std::replace(a.begin(), a.end(), '_', ':');
    return valid_ble_address(a) ? upper(a) : std::string();
}

std::string device_path(const std::string &adapter, const std::string &address)
{
    std::string a = upper(address);
    std::replace(a.begin(), a.end(), ':', '_');
    return adapter + "/dev_" + a;
}

} // namespace

/* ================================================================================================================ */

struct BluezBackend::Impl {
    enum class Op { Objects, Ignore, StartDiscovery, Connect, Disconnect, GattObjects, AcquireNotify, StartNotify };
    struct Call {
        Impl *self = nullptr;
        Op op = Op::Ignore;
        std::string address;
        std::string path;   /* the object called */
        std::uint32_t seq = 0;
        bool done = false;
    };
    struct Svc {
        std::string uuid, device;
    };
    struct Chr {
        std::string uuid, service;
        int mtu = 0;
    };

    Api api;
    sd::bus *bus = nullptr;
    std::vector<sd::slot *> slots;
    bool answered = false;          /* the first GetManagedObjects came back (ok or not) */
    bool absent = false;            /* bluetoothd is not on the bus */
    bool powered = false, scanning = false;
    std::string adapter;
    std::string why;                /* why Bluetooth cannot be used (the last reason) */
    std::map<std::string, BleDevice> devs;  /* by object path */
    std::map<std::string, Svc> svcs;
    std::map<std::string, Chr> chrs;
    std::vector<BleEvent> out;
    std::list<std::unique_ptr<Call>> calls;

    std::string notify_dev, notify_path;
    std::uint32_t gatt_seq = 0;
    int notify_fd = -1;
    bool notify_props = false;      /* StartNotify: the values come as PropertiesChanged */
    int mtu = 0;
    NotifySink *sink = nullptr;

    std::function<void(const std::string &)> log;
    std::function<void(const std::string &)> diag; /* the diagnostic log (null: off) */

    void say(const std::string &line) const
    {
        if (log) log("bluez: " + line);
    }
    void emit(BleEvent::Kind k, const std::string &addr, bool ok, const std::string &error = std::string())
    {
        BleEvent e;
        e.kind = k;
        e.address = addr;
        e.ok = ok;
        e.error = error;
        out.push_back(std::move(e));
    }

    BleAvail availability() const
    {
        if (!bus) return why.empty() ? BleAvail::Unknown : BleAvail::NoBluez;
        if (!answered) return BleAvail::Unknown;
        if (absent) return BleAvail::NoBluez;
        if (adapter.empty()) return BleAvail::NoAdapter;
        return powered ? BleAvail::Ok : BleAvail::Off;
    }

    /* ---------------------------------------------------------------- reading messages */

    bool read_props(sd::message *m, Props &p)
    {
        if (api.enter_container(m, 'a', "{sv}") <= 0) return false;
        while (api.enter_container(m, 'e', "sv") > 0) {
            const char *key = nullptr;
            if (api.read_basic(m, 's', &key) <= 0 || !key) return false;
            char t = 0;
            const char *contents = nullptr;
            if (api.peek_type(m, &t, &contents) <= 0 || t != 'v' || !contents) return false;
            Val v;
            if (api.enter_container(m, 'v', contents) <= 0) return false;
            const std::string c = contents;
            if (c == "s" || c == "o") {
                const char *s = nullptr;
                if (api.read_basic(m, c[0], &s) > 0 && s) v.s = s;
            } else if (c == "b") {
                int x = 0;
                api.read_basic(m, 'b', &x);
                v.i = x;
            } else if (c == "n") {
                std::int16_t x = 0;
                api.read_basic(m, 'n', &x);
                v.i = x;
            } else if (c == "q") {
                std::uint16_t x = 0;
                api.read_basic(m, 'q', &x);
                v.i = x;
            } else if (c == "as") {
                if (api.enter_container(m, 'a', "s") > 0) {
                    const char *s = nullptr;
                    while (api.read_basic(m, 's', &s) > 0)
                        if (s) v.as.emplace_back(s);
                    api.exit_container(m);
                }
            } else {
                api.skip(m, contents);
            }
            api.exit_container(m); /* v */
            api.exit_container(m); /* e */
            p[key] = std::move(v);
        }
        api.exit_container(m); /* a */
        return true;
    }

    static bool flag(const Props &p, const char *k, bool &out)
    {
        auto it = p.find(k);
        if (it == p.end()) return false;
        out = it->second.i != 0;
        return true;
    }

    void apply_device(BleDevice &d, const std::string &path, const Props &p, bool *conn_changed, bool *res_changed)
    {
        if (auto it = p.find("Address"); it != p.end()) d.address = upper(it->second.s);
        if (d.address.empty()) d.address = address_from_path(path);
        if (auto it = p.find("Name"); it != p.end()) d.name = it->second.s;
        if (auto it = p.find("Alias"); it != p.end() && d.name.empty()) {
            std::string dashed = d.address;
            std::replace(dashed.begin(), dashed.end(), ':', '-');
            if (upper(it->second.s) != dashed && upper(it->second.s) != d.address) d.name = it->second.s;
        }
        if (auto it = p.find("RSSI"); it != p.end()) {
            d.has_rssi = true;
            d.rssi = static_cast<int>(it->second.i);
        }
        bool v = false;
        if (flag(p, "Connected", v)) {
            if (conn_changed) *conn_changed = v != d.connected;
            d.connected = v;
            if (!v) d.services_resolved = false;
        }
        if (flag(p, "ServicesResolved", v)) {
            if (res_changed) *res_changed = v != d.services_resolved;
            d.services_resolved = v;
        }
    }

    void apply_iface(const std::string &path, const std::string &iface, const Props &p, bool emit_events)
    {
        if (path.rfind("/org/bluez", 0) != 0) return;
        if (iface == kAdapter) {
            const bool was = !adapter.empty();
            if (adapter.empty() || (path == "/org/bluez/hci0" && adapter != path)) adapter = path;
            bool on = false;
            if (path == adapter && flag(p, "Powered", on)) powered = on;
            if (emit_events && !was) emit(BleEvent::Kind::AdapterChanged, "", powered);
        } else if (iface == kDevice) {
            BleDevice &d = devs[path];
            apply_device(d, path, p, nullptr, nullptr);
            if (emit_events) emit(BleEvent::Kind::DeviceUpdated, d.address, true);
        } else if (iface == kSvc) {
            Svc &s = svcs[path];
            if (auto it = p.find("UUID"); it != p.end()) s.uuid = lower(it->second.s);
            if (auto it = p.find("Device"); it != p.end()) s.device = it->second.s;
        } else if (iface == kChr) {
            Chr &c = chrs[path];
            if (auto it = p.find("UUID"); it != p.end()) c.uuid = lower(it->second.s);
            if (auto it = p.find("Service"); it != p.end()) c.service = it->second.s;
            if (auto it = p.find("MTU"); it != p.end()) c.mtu = static_cast<int>(it->second.i);
        }
    }

    /* a{sa{sv}} of one object */
    void read_ifaces(sd::message *m, const std::string &path, bool emit_events)
    {
        if (api.enter_container(m, 'a', "{sa{sv}}") <= 0) return;
        while (api.enter_container(m, 'e', "sa{sv}") > 0) {
            const char *iface = nullptr;
            if (api.read_basic(m, 's', &iface) <= 0 || !iface) break;
            Props p;
            if (!read_props(m, p)) break;
            api.exit_container(m);
            apply_iface(path, iface, p, emit_events);
        }
        api.exit_container(m);
    }

    /* a{oa{sa{sv}}} (GetManagedObjects) */
    void read_objects(sd::message *m, bool emit_events)
    {
        if (api.enter_container(m, 'a', "{oa{sa{sv}}}") <= 0) return;
        while (api.enter_container(m, 'e', "oa{sa{sv}}") > 0) {
            const char *path = nullptr;
            if (api.read_basic(m, 'o', &path) <= 0 || !path) break;
            read_ifaces(m, path, emit_events);
            api.exit_container(m);
        }
        api.exit_container(m);
    }

    /* ---------------------------------------------------------------- calls (all asynchronous) */

    static int on_reply(sd::message *m, void *userdata, sd::error * /*ret*/)
    {
        Call *c = static_cast<Call *>(userdata);
        c->done = true;
        c->self->reply(*c, m);
        return 0;
    }

    using Builder = std::function<bool(sd::message *)>;

    bool call(const std::string &path, const char *iface, const char *member, Op op, const std::string &address,
              const Builder &build, std::uint64_t timeout_us)
    {
        if (!bus) return false;
        sd::message *m = nullptr;
        int r = api.new_method_call(bus, &m, kBluez, path.c_str(), iface, member);
        if (r < 0 || !m) {
            say(std::string(member) + ": " + std::strerror(-r));
            return false;
        }
        if (build && !build(m)) {
            api.msg_unref(m);
            say(std::string(member) + ": could not build the call");
            return false;
        }
        auto c = std::make_unique<Call>();
        c->self = this;
        c->op = op;
        c->address = address;
        c->path = path;
        c->seq = gatt_seq;
        r = api.call_async(bus, nullptr, m, &Impl::on_reply, c.get(), timeout_us);
        api.msg_unref(m);
        if (r < 0) {
            say(std::string(member) + ": " + std::strerror(-r));
            return false;
        }
        calls.push_back(std::move(c));
        return true;
    }

    bool append_dict_s(sd::message *m, const char *key, const char *value)
    {
        return api.open_container(m, 'a', "{sv}") >= 0 && api.open_container(m, 'e', "sv") >= 0 &&
               api.append_basic(m, 's', key) >= 0 && api.open_container(m, 'v', "s") >= 0 &&
               api.append_basic(m, 's', value) >= 0 && api.close_container(m) >= 0 && api.close_container(m) >= 0 &&
               api.close_container(m) >= 0;
    }
    bool append_empty_dict(sd::message *m) { return api.open_container(m, 'a', "{sv}") >= 0 && api.close_container(m) >= 0; }

    std::string error_text(sd::message *m)
    {
        if (!api.is_method_error(m, nullptr)) return std::string();
        const sd::error *e = api.get_error(m);
        std::string s = e && e->name ? e->name : "error";
        if (e && e->message && *e->message) s += std::string(": ") + e->message;
        return s;
    }

    void subscribed(const std::string &address, bool ok, const std::string &error)
    {
        emit(BleEvent::Kind::SubscribeResult, address, ok, error);
    }

    void reply(Call &c, sd::message *m)
    {
        const std::string err = error_text(m);
        switch (c.op) {
        case Op::Objects: {
            answered = true;
            if (!err.empty()) {
                absent = true;
                const bool gone = err.find("ServiceUnknown") != std::string::npos || err.find("NameHasNoOwner") != std::string::npos;
                why = gone ? "bluetoothd is not running" : err;
                say(why);
                emit(BleEvent::Kind::Ready, "", false, why);
                return;
            }
            absent = false;
            read_objects(m, false);
            why = adapter.empty() ? "no Bluetooth adapter" : (powered ? std::string() : "the adapter is powered off");
            say("ready: " + (adapter.empty() ? std::string("no adapter") : adapter + (powered ? " powered" : " off")) + ", " +
                std::to_string(devs.size()) + " devices known");
            emit(BleEvent::Kind::Ready, "", availability() == BleAvail::Ok, why);
            return;
        }
        case Op::Ignore:
            if (!err.empty()) say(c.path + ": " + err);
            return;
        case Op::StartDiscovery:
            if (!err.empty()) {
                scanning = false;
                emit(BleEvent::Kind::ScanStopped, "", false, err);
            }
            return;
        case Op::Connect: emit(BleEvent::Kind::ConnectResult, c.address, err.empty(), err); return;
        case Op::Disconnect: {
            const std::string res = ble_disconnect_result(err);
            if (diag) diag("ble-disconnect addr=" + c.address + " result=" + res);
            if (res.compare(0, 6, "error:") == 0) say(c.path + ": " + err);
            return;
        }
        case Op::GattObjects: {
            if (c.seq != gatt_seq) return;
            if (!err.empty()) {
                subscribed(c.address, false, err);
                return;
            }
            read_objects(m, false);
            const std::string dev = device_path(adapter, c.address);
            std::string svc;
            for (const auto &kv : svcs)
                if (kv.second.device == dev && kv.second.uuid == kHlkDataService) svc = kv.first;
            if (svc.empty()) {
                subscribed(c.address, false, "no 0xfff0 service: not an HLK-LD radar module");
                return;
            }
            notify_path.clear();
            for (const auto &kv : chrs)
                if (kv.second.service == svc && kv.second.uuid == kHlkDataNotify) notify_path = kv.first;
            if (notify_path.empty()) {
                subscribed(c.address, false, "no 0xfff1 characteristic: not an HLK-LD radar module");
                return;
            }
            call(notify_path, kChr, "AcquireNotify", Op::AcquireNotify, c.address,
                 [this](sd::message *mm) { return append_empty_dict(mm); }, 10 * kSec);
            return;
        }
        case Op::AcquireNotify: {
            if (c.seq != gatt_seq) return; /* unsubscribed meanwhile: the socket goes with the message */
            if (!err.empty()) {
                say("AcquireNotify: " + err + ", using StartNotify");
                if (diag) diag("ble-subscribe-note AcquireNotify failed (" + err + "), using StartNotify");
                call(notify_path, kChr, "StartNotify", Op::StartNotify, c.address, nullptr, 10 * kSec);
                return;
            }
            int fd = -1;
            std::uint16_t m16 = 0;
            if (api.read_basic(m, 'h', &fd) <= 0 || fd < 0 || api.read_basic(m, 'q', &m16) <= 0) {
                subscribed(c.address, false, "AcquireNotify gave no socket");
                return;
            }
            notify_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 3); /* the message owns fd */
            if (notify_fd < 0) {
                subscribed(c.address, false, std::string("dup: ") + std::strerror(errno));
                return;
            }
            ::fcntl(notify_fd, F_SETFL, ::fcntl(notify_fd, F_GETFL) | O_NONBLOCK);
            mtu = m16;
            say("notify acquired, MTU " + std::to_string(m16));
            subscribed(c.address, true, std::string());
            return;
        }
        case Op::StartNotify: {
            if (c.seq != gatt_seq) { /* unsubscribed meanwhile: undo it */
                if (err.empty()) call(c.path, kChr, "StopNotify", Op::Ignore, "", nullptr, 5 * kSec);
                return;
            }
            if (!err.empty()) {
                subscribed(c.address, false, "StartNotify: " + err);
                return;
            }
            notify_props = true;
            auto it = chrs.find(notify_path);
            mtu = it != chrs.end() ? it->second.mtu : 0;
            subscribed(c.address, true, std::string());
            return;
        }
        }
    }

    /* ---------------------------------------------------------------- signals */

    /* The StartNotify path: the notification is the "Value" of a PropertiesChanged; handed to the sink from the message's
     * own buffer (no copy, no allocation per notification). */
    void notify_from_props(sd::message *m)
    {
        if (api.enter_container(m, 'a', "{sv}") <= 0) return;
        while (api.enter_container(m, 'e', "sv") > 0) {
            const char *key = nullptr;
            if (api.read_basic(m, 's', &key) <= 0 || !key) return;
            if (std::strcmp(key, "Value") == 0 && api.enter_container(m, 'v', "ay") > 0) {
                const void *ptr = nullptr;
                std::size_t n = 0;
                if (api.read_array(m, 'y', &ptr, &n) >= 0 && ptr && n && sink)
                    sink->on_notify(static_cast<const std::uint8_t *>(ptr), n);
                api.exit_container(m);
            } else {
                api.skip(m, "v");
            }
            api.exit_container(m);
        }
        api.exit_container(m);
    }

    static int on_props(sd::message *m, void *userdata, sd::error * /*ret*/)
    {
        static_cast<Impl *>(userdata)->props_changed(m);
        return 0;
    }
    void props_changed(sd::message *m)
    {
        const char *p = api.get_path(m);
        if (!p || std::strncmp(p, "/org/bluez", 10) != 0) return;
        const char *iface = nullptr;
        if (api.read_basic(m, 's', &iface) <= 0 || !iface) return;
        if (notify_props && std::strcmp(iface, kChr) == 0 && notify_path == p) {
            notify_from_props(m);
            return;
        }
        const std::string path = p;
        Props changed;
        if (!read_props(m, changed)) return;
        std::vector<std::string> invalidated;
        if (api.enter_container(m, 'a', "s") > 0) {
            const char *s = nullptr;
            while (api.read_basic(m, 's', &s) > 0)
                if (s) invalidated.emplace_back(s);
            api.exit_container(m);
        }
        const std::string i = iface;
        if (i == kDevice) {
            BleDevice &d = devs[path];
            bool conn = false, res = false;
            apply_device(d, path, changed, &conn, &res);
            for (const std::string &k : invalidated)
                if (k == "RSSI") d.has_rssi = false;
            emit(BleEvent::Kind::DeviceUpdated, d.address, true);
            if (conn) emit(BleEvent::Kind::ConnectionChanged, d.address, d.connected);
            if (res) emit(BleEvent::Kind::ServicesResolved, d.address, d.services_resolved);
        } else if (i == kAdapter && path == adapter) {
            bool on = false;
            if (flag(changed, "Powered", on) && on != powered) {
                powered = on;
                why = on ? std::string() : "the adapter is powered off";
                emit(BleEvent::Kind::AdapterChanged, "", on);
            }
            if (flag(changed, "Discovering", on) && !on && scanning) {
                scanning = false;
                emit(BleEvent::Kind::ScanStopped, "", true);
            }
        } else if (i == kChr) {
            auto it = changed.find("MTU");
            if (it != changed.end()) chrs[path].mtu = static_cast<int>(it->second.i);
        }
    }

    static int on_added(sd::message *m, void *userdata, sd::error * /*ret*/)
    {
        Impl *self = static_cast<Impl *>(userdata);
        const char *path = nullptr;
        if (self->api.read_basic(m, 'o', &path) <= 0 || !path) return 0;
        self->read_ifaces(m, path, true);
        return 0;
    }

    static int on_removed(sd::message *m, void *userdata, sd::error * /*ret*/)
    {
        Impl *self = static_cast<Impl *>(userdata);
        const char *p = nullptr;
        if (self->api.read_basic(m, 'o', &p) <= 0 || !p) return 0;
        const std::string path = p;
        if (path.rfind("/org/bluez", 0) != 0) return 0;
        if (self->api.enter_container(m, 'a', "s") <= 0) return 0;
        const char *s = nullptr;
        while (self->api.read_basic(m, 's', &s) > 0) {
            if (!s) continue;
            const std::string iface = s;
            if (iface == kDevice) {
                auto it = self->devs.find(path);
                const std::string addr = it != self->devs.end() ? it->second.address : address_from_path(path);
                if (it != self->devs.end()) self->devs.erase(it);
                self->emit(BleEvent::Kind::DeviceRemoved, addr, true);
            } else if (iface == kSvc) {
                self->svcs.erase(path);
            } else if (iface == kChr) {
                self->chrs.erase(path);
            } else if (iface == kAdapter && path == self->adapter) {
                self->adapter.clear();
                self->powered = self->scanning = false;
                self->why = "no Bluetooth adapter";
                self->emit(BleEvent::Kind::AdapterChanged, "", false);
            }
        }
        self->api.exit_container(m);
        return 0;
    }

    static int on_owner(sd::message *m, void *userdata, sd::error * /*ret*/)
    {
        Impl *self = static_cast<Impl *>(userdata);
        const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
        if (self->api.read_basic(m, 's', &name) <= 0 || self->api.read_basic(m, 's', &old_owner) <= 0 ||
            self->api.read_basic(m, 's', &new_owner) <= 0)
            return 0;
        if (!name || std::strcmp(name, kBluez) != 0) return 0;
        if (!new_owner || !*new_owner) {
            self->say("bluetoothd went away");
            self->bluez_gone();
        } else {
            self->say("bluetoothd is back");
            self->call("/", kObjMgr, "GetManagedObjects", Op::Objects, "", nullptr, 10 * kSec);
        }
        return 0;
    }

    void bluez_gone()
    {
        const bool had_notify = notify_fd >= 0 || notify_props;
        close_notify();
        notify_props = false;
        devs.clear();
        svcs.clear();
        chrs.clear();
        powered = scanning = false;
        absent = true;
        answered = true;
        adapter.clear();
        why = "bluetoothd is not running";
        if (had_notify) emit(BleEvent::Kind::NotifyClosed, notify_dev, false);
        emit(BleEvent::Kind::AdapterChanged, "", false);
    }

    /* ---------------------------------------------------------------- the rest */

    void close_notify()
    {
        if (notify_fd >= 0) ::close(notify_fd);
        notify_fd = -1;
    }

    void read_notify()
    {
        if (notify_fd < 0) return;
        std::uint8_t buf[1024];
        bool closed = false;
        for (int i = 0; i < 64; ++i) { /* one read = one notification (a SOCK_SEQPACKET socket) */
            const ssize_t n = ::read(notify_fd, buf, sizeof(buf));
            if (n > 0) {
                if (sink) sink->on_notify(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                pollfd p{notify_fd, POLLIN, 0};
                closed = ::poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL)) && !(p.revents & POLLIN);
                break; /* nothing more for now (or hung up) */
            }
            closed = true; /* 0: hung up, or an error */
            break;
        }
        if (!closed) return;
        say("notify socket closed");
        close_notify();
        emit(BleEvent::Kind::NotifyClosed, notify_dev, false);
    }

    void stop()
    {
        if (!bus) return;
        close_notify();
        if (notify_props && !notify_path.empty()) call(notify_path, kChr, "StopNotify", Op::Ignore, "", nullptr, 2 * kSec);
        notify_props = false;
        if (scanning && !adapter.empty()) call(adapter, kAdapter, "StopDiscovery", Op::Ignore, "", nullptr, 2 * kSec);
        api.flush(bus); /* the disconnection BleSys queued, the subscription and the discovery ended, before closing */
        /* with the diagnostic log on: wait (at most 0.5 s) for BlueZ's answer to a pending Disconnect, to log its result */
        if (diag) {
            for (int i = 0; i < 50; ++i) {
                bool pending = false;
                for (const auto &c : calls)
                    if (c->op == Op::Disconnect && !c->done) pending = true;
                if (!pending) break;
                while (api.process(bus, nullptr) > 0) {
                }
                ::usleep(10000);
            }
        }
        for (sd::slot *s : slots) api.slot_unref(s);
        slots.clear();
        api.flush_close_unref(bus);
        bus = nullptr;
        calls.clear();
        devs.clear();
        svcs.clear();
        chrs.clear();
        out.clear();
        answered = absent = powered = scanning = false;
        adapter.clear();
        why.clear();
        notify_dev.clear();
        notify_path.clear();
        mtu = 0;
        ++gatt_seq;
    }
};

/* ================================================================================================================ */

BluezBackend::BluezBackend() : d_(std::make_unique<Impl>()) {}

BluezBackend::~BluezBackend() { d_->stop(); }

bool BluezBackend::start(std::string &why)
{
    Impl &d = *d_;
    if (d.bus) return true;
    if (!d.api.load(why)) {
        d.why = why;
        return false;
    }
    sd::bus *b = nullptr;
    const int r = d.api.open_system(&b);
    if (r < 0 || !b) {
        why = std::string("no system D-Bus (") + std::strerror(-r) + ")";
        d.why = why;
        return false;
    }
    d.bus = b;
    d.why.clear();
    struct M {
        const char *rule;
        sd::handler fn;
    };
    /* no sender= in the rules: the paths and the handlers keep to /org/bluez (Mesh Hop's note: a well-known sender name is
     * resolved by the bus daemon, not by sd-bus's local filter) */
    const M rules[] = {
        {"type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path_namespace='/org/bluez'",
         &Impl::on_props},
        {"type='signal',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded',path='/'", &Impl::on_added},
        {"type='signal',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved',path='/'", &Impl::on_removed},
        {"type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',"
         "member='NameOwnerChanged',arg0='org.bluez'",
         &Impl::on_owner},
    };
    for (const M &m : rules) {
        sd::slot *s = nullptr;
        const int rr = d.api.add_match(d.bus, &s, m.rule, m.fn, &d);
        if (rr < 0) d.say(std::string("match: ") + std::strerror(-rr));
        else d.slots.push_back(s);
    }
    if (!d.call("/", kObjMgr, "GetManagedObjects", Impl::Op::Objects, "", nullptr, 10 * kSec)) {
        why = "cannot ask BlueZ for its objects";
        d.stop();
        d.why = why;
        return false;
    }
    return true;
}

void BluezBackend::stop() { d_->stop(); }
bool BluezBackend::started() const { return d_->bus != nullptr; }
BleAvail BluezBackend::availability() const { return d_->availability(); }
std::string BluezBackend::unavailable_why() const { return d_->why; }
void BluezBackend::set_log(std::function<void(const std::string &)> log) { d_->log = std::move(log); }
void BluezBackend::set_diag(std::function<void(const std::string &)> sink) { d_->diag = std::move(sink); }
void BluezBackend::set_sink(NotifySink *sink) { d_->sink = sink; }

void BluezBackend::poll(std::vector<BleEvent> &out)
{
    Impl &d = *d_;
    if (d.bus) {
        for (int i = 0; i < 256; ++i) {
            const int r = d.api.process(d.bus, nullptr);
            if (r < 0) {
                d.say(std::string("D-Bus: ") + std::strerror(-r));
                d.bluez_gone();
                break;
            }
            if (r == 0) break;
        }
        d.calls.remove_if([](const std::unique_ptr<Impl::Call> &c) { return c->done; });
        d.read_notify();
    }
    for (BleEvent &e : d.out) out.push_back(std::move(e));
    d.out.clear();
}

void BluezBackend::start_scan()
{
    Impl &d = *d_;
    if (!d.bus || d.scanning) return;
    if (d.adapter.empty()) {
        d.emit(BleEvent::Kind::ScanStopped, "", false, "no Bluetooth adapter");
        return;
    }
    d.scanning = true;
    d.call(d.adapter, kAdapter, "SetDiscoveryFilter", Impl::Op::Ignore, "",
           [&d](sd::message *m) { return d.append_dict_s(m, "Transport", "le"); }, 5 * kSec);
    d.call(d.adapter, kAdapter, "StartDiscovery", Impl::Op::StartDiscovery, "", nullptr, 5 * kSec);
}

void BluezBackend::stop_scan()
{
    Impl &d = *d_;
    if (!d.bus || !d.scanning) return;
    d.scanning = false;
    d.call(d.adapter, kAdapter, "StopDiscovery", Impl::Op::Ignore, "", nullptr, 5 * kSec);
}

bool BluezBackend::find(const std::string &address, BleDevice &out) const
{
    if (d_->adapter.empty()) return false;
    auto it = d_->devs.find(device_path(d_->adapter, address));
    if (it == d_->devs.end()) return false;
    out = it->second;
    return true;
}

void BluezBackend::connect(const std::string &address)
{
    Impl &d = *d_;
    if (d.adapter.empty() ||
        !d.call(device_path(d.adapter, address), kDevice, "Connect", Impl::Op::Connect, upper(address), nullptr, 12 * kSec))
        d.emit(BleEvent::Kind::ConnectResult, upper(address), false, "the D-Bus call failed");
}

void BluezBackend::disconnect(const std::string &address)
{
    Impl &d = *d_;
    if (d.adapter.empty()) return;
    d.call(device_path(d.adapter, address), kDevice, "Disconnect", Impl::Op::Disconnect, upper(address), nullptr, 5 * kSec);
}

void BluezBackend::subscribe(const std::string &address)
{
    Impl &d = *d_;
    unsubscribe();
    d.notify_dev = upper(address);
    if (!d.call("/", kObjMgr, "GetManagedObjects", Impl::Op::GattObjects, d.notify_dev, nullptr, 10 * kSec))
        d.subscribed(d.notify_dev, false, "the D-Bus call failed");
}

void BluezBackend::unsubscribe()
{
    Impl &d = *d_;
    ++d.gatt_seq;
    d.close_notify();
    if (d.notify_props && !d.notify_path.empty()) d.call(d.notify_path, kChr, "StopNotify", Impl::Op::Ignore, "", nullptr, 5 * kSec);
    d.notify_props = false;
    d.notify_path.clear();
    d.notify_dev.clear();
    d.mtu = 0;
}

std::string BluezBackend::describe() const
{
    if (d_->notify_fd >= 0) return "notify=AcquireNotify mtu=" + std::to_string(d_->mtu);
    if (d_->notify_props) return "notify=StartNotify mtu=" + std::to_string(d_->mtu);
    return std::string();
}

} // namespace mmw
