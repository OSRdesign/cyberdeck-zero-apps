/*
 * SPDX-License-Identifier: MIT
 *
 * BlueZ over sd-bus, loaded at run time. The few sd-bus types are declared here as the C ABI of libsystemd.so.0 has them
 * (sd-bus.h of systemd 257; sd_bus_error is public and stable), so no systemd header is needed to build.
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

namespace meshzero::ble {

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
constexpr uint64_t kSec = 1000000;   // microseconds

struct Api {
    void *lib = nullptr;
    int (*open_system)(sd::bus **) = nullptr;
    sd::bus *(*flush_close_unref)(sd::bus *) = nullptr;
    int (*process)(sd::bus *, sd::message **) = nullptr;
    int (*flush)(sd::bus *) = nullptr;
    int (*call_async)(sd::bus *, sd::slot **, sd::message *, sd::handler, void *, uint64_t) = nullptr;
    int (*add_match)(sd::bus *, sd::slot **, const char *, sd::handler, void *) = nullptr;
    int (*add_object)(sd::bus *, sd::slot **, const char *, sd::handler, void *) = nullptr;
    sd::slot *(*slot_unref)(sd::slot *) = nullptr;
    int (*new_method_call)(sd::bus *, sd::message **, const char *, const char *, const char *, const char *) = nullptr;
    sd::message *(*msg_ref)(sd::message *) = nullptr;
    sd::message *(*msg_unref)(sd::message *) = nullptr;
    int (*append_basic)(sd::message *, char, const void *) = nullptr;
    int (*append_array)(sd::message *, char, const void *, size_t) = nullptr;
    int (*open_container)(sd::message *, char, const char *) = nullptr;
    int (*close_container)(sd::message *) = nullptr;
    int (*read_basic)(sd::message *, char, void *) = nullptr;
    int (*read_array)(sd::message *, char, const void **, size_t *) = nullptr;
    int (*enter_container)(sd::message *, char, const char *) = nullptr;
    int (*exit_container)(sd::message *) = nullptr;
    int (*skip)(sd::message *, const char *) = nullptr;
    int (*peek_type)(sd::message *, char *, const char **) = nullptr;
    int (*is_method_error)(sd::message *, const char *) = nullptr;
    int (*is_method_call)(sd::message *, const char *, const char *) = nullptr;
    const sd::error *(*get_error)(sd::message *) = nullptr;
    const char *(*get_member)(sd::message *) = nullptr;
    const char *(*get_path)(sd::message *) = nullptr;
    int (*reply_return)(sd::message *, const char *, ...) = nullptr;
    int (*reply_error)(sd::message *, const sd::error *) = nullptr;

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
        sym(add_object, "sd_bus_add_object");
        sym(slot_unref, "sd_bus_slot_unref");
        sym(new_method_call, "sd_bus_message_new_method_call");
        sym(msg_ref, "sd_bus_message_ref");
        sym(msg_unref, "sd_bus_message_unref");
        sym(append_basic, "sd_bus_message_append_basic");
        sym(append_array, "sd_bus_message_append_array");
        sym(open_container, "sd_bus_message_open_container");
        sym(close_container, "sd_bus_message_close_container");
        sym(read_basic, "sd_bus_message_read_basic");
        sym(read_array, "sd_bus_message_read_array");
        sym(enter_container, "sd_bus_message_enter_container");
        sym(exit_container, "sd_bus_message_exit_container");
        sym(skip, "sd_bus_message_skip");
        sym(peek_type, "sd_bus_message_peek_type");
        sym(is_method_error, "sd_bus_message_is_method_error");
        sym(is_method_call, "sd_bus_message_is_method_call");
        sym(get_error, "sd_bus_message_get_error");
        sym(get_member, "sd_bus_message_get_member");
        sym(get_path, "sd_bus_message_get_path");
        sym(reply_return, "sd_bus_reply_method_return");
        sym(reply_error, "sd_bus_reply_method_error");
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
    std::string sig;
    std::string s;
    int64_t i = 0;
    std::vector<std::string> as;
    Bytes ay;
};
using Props = std::map<std::string, Val>;

std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

/* ================================================================== */

struct BluezBackend::Impl {
    enum class Op { Objects, RegisterAgent, Ignore, StartDiscovery, Connect, Pair, GattObjects, AcquireNotify, StartNotify, Write };
    struct Call {
        Impl *self = nullptr;
        Op op = Op::Ignore;
        std::string address;
        std::string path;           // the object called
        uint32_t seq = 0;
        bool done = false;
    };
    struct Svc {
        std::string uuid, device;
    };
    struct Chr {
        std::string uuid, service;
        std::vector<std::string> flags;
        int mtu = 0;
    };

    Api api;
    sd::bus *bus = nullptr;
    std::vector<sd::slot *> slots;
    std::string agent_path;
    bool ready = false, powered = false, scanning = false, want_scan = false, agent_registered = false, agent_exported = false,
         agent_failed = false;
    std::string adapter;
    std::map<std::string, BleDevice> devs;      // by object path
    std::map<std::string, Svc> svcs;
    std::map<std::string, Chr> chrs;
    std::vector<BleEvent> out;
    std::list<std::unique_ptr<Call>> calls;

    std::string gatt_dev, rx_path, tx_path;
    uint32_t gatt_seq = 0;
    int notify_fd = -1;
    bool notify_props = false;
    int mtu = 0;

    sd::message *agent_req = nullptr;
    bool agent_req_string = false;
    std::string agent_req_addr;

    std::function<void(const std::string &)> log;

    void say(const std::string &line) const
    {
        if (log) log("bluez: " + line);
    }
    void emit(BleEvent e) { out.push_back(std::move(e)); }
    void emit(BleEvent::Kind k, const std::string &addr, bool ok, const std::string &error = "")
    {
        BleEvent e;
        e.kind = k;
        e.address = addr;
        e.ok = ok;
        e.error = error;
        out.push_back(std::move(e));
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
            v.sig = contents;
            if (api.enter_container(m, 'v', contents) <= 0) return false;
            const std::string c = contents;
            if (c == "s" || c == "o" || c == "g") {
                const char *s = nullptr;
                if (api.read_basic(m, c[0], &s) > 0 && s) v.s = s;
            } else if (c == "b" || c == "i") {
                int x = 0;
                api.read_basic(m, c[0], &x);
                v.i = x;
            } else if (c == "n") {
                int16_t x = 0;
                api.read_basic(m, 'n', &x);
                v.i = x;
            } else if (c == "q") {
                uint16_t x = 0;
                api.read_basic(m, 'q', &x);
                v.i = x;
            } else if (c == "u") {
                uint32_t x = 0;
                api.read_basic(m, 'u', &x);
                v.i = x;
            } else if (c == "y") {
                uint8_t x = 0;
                api.read_basic(m, 'y', &x);
                v.i = x;
            } else if (c == "as" || c == "ao") {
                if (api.enter_container(m, 'a', c.c_str() + 1) > 0) {
                    const char *s = nullptr;
                    while (api.read_basic(m, c[1], &s) > 0)
                        if (s) v.as.emplace_back(s);
                    api.exit_container(m);
                }
            } else if (c == "ay") {
                const void *ptr = nullptr;
                size_t n = 0;
                if (api.read_array(m, 'y', &ptr, &n) >= 0 && ptr) v.ay.assign(static_cast<const uint8_t *>(ptr), static_cast<const uint8_t *>(ptr) + n);
            } else {
                api.skip(m, contents);
            }
            api.exit_container(m);   // v
            api.exit_container(m);   // e
            p[key] = std::move(v);
        }
        api.exit_container(m);       // a
        return true;
    }

    static bool flag(const Props &p, const char *k, bool &out)
    {
        auto it = p.find(k);
        if (it == p.end()) return false;
        out = it->second.i != 0;
        return true;
    }

    /* Applies a Device1 dictionary; tells which link flags changed. */
    void apply_device(BleDevice &d, const std::string &path, const Props &p, bool *conn_changed, bool *res_changed)
    {
        d.path = path;
        if (auto it = p.find("Address"); it != p.end()) d.address = it->second.s;
        if (d.address.empty()) d.address = address_from_path(path);
        if (auto it = p.find("AddressType"); it != p.end()) d.address_type = it->second.s;
        if (auto it = p.find("Name"); it != p.end()) d.name = it->second.s;
        if (auto it = p.find("Alias"); it != p.end() && d.name.empty()) {
            std::string dashed = d.address;
            std::replace(dashed.begin(), dashed.end(), ':', '-');
            if (it->second.s != dashed && it->second.s != d.address) d.name = it->second.s;
        }
        if (auto it = p.find("RSSI"); it != p.end()) {
            d.has_rssi = true;
            d.rssi = static_cast<int>(it->second.i);
        }
        if (auto it = p.find("UUIDs"); it != p.end()) {
            d.nus = false;
            for (const std::string &u : it->second.as)
                if (lower(u) == kNusService) d.nus = true;
        }
        flag(p, "Paired", d.paired);
        flag(p, "Bonded", d.bonded);
        flag(p, "Trusted", d.trusted);
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
            if (adapter.empty() || (path == "/org/bluez/hci0" && adapter != path)) adapter = path;
            bool on = false;
            if (path == adapter && flag(p, "Powered", on)) powered = on;
        } else if (iface == kDevice) {
            BleDevice &d = devs[path];
            apply_device(d, path, p, nullptr, nullptr);
            if (emit_events) {
                BleEvent e;
                e.kind = BleEvent::Kind::DeviceUpdated;
                e.address = d.address;
                e.dev = d;
                emit(std::move(e));
            }
        } else if (iface == kSvc) {
            Svc &s = svcs[path];
            if (auto it = p.find("UUID"); it != p.end()) s.uuid = lower(it->second.s);
            if (auto it = p.find("Device"); it != p.end()) s.device = it->second.s;
        } else if (iface == kChr) {
            Chr &c = chrs[path];
            if (auto it = p.find("UUID"); it != p.end()) c.uuid = lower(it->second.s);
            if (auto it = p.find("Service"); it != p.end()) c.service = it->second.s;
            if (auto it = p.find("Flags"); it != p.end()) c.flags = it->second.as;
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

    /* ---------------------------------------------------------------- calls */

    static int on_reply(sd::message *m, void *userdata, sd::error * /*ret*/)
    {
        Call *c = static_cast<Call *>(userdata);
        c->done = true;
        c->self->reply(*c, m);
        return 0;
    }

    using Builder = std::function<bool(sd::message *)>;

    bool call(const std::string &path, const char *iface, const char *member, Op op, const std::string &address,
              const Builder &build, uint64_t timeout_us, const char *dest = kBluez)
    {
        if (!bus) return false;
        sd::message *m = nullptr;
        int r = api.new_method_call(bus, &m, dest, path.c_str(), iface, member);
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
    bool append_empty_dict(sd::message *m)
    {
        return api.open_container(m, 'a', "{sv}") >= 0 && api.close_container(m) >= 0;
    }

    std::string error_text(sd::message *m)
    {
        if (!api.is_method_error(m, nullptr)) return "";
        const sd::error *e = api.get_error(m);
        std::string s = e && e->name ? e->name : "error";
        if (e && e->message && *e->message) s += std::string(": ") + e->message;
        return s;
    }

    void gatt_failed(const std::string &address, const std::string &why, bool not_companion)
    {
        BleEvent e;
        e.kind = BleEvent::Kind::GattResult;
        e.address = address;
        e.ok = false;
        e.not_companion = not_companion;
        e.error = why;
        emit(std::move(e));
    }

    void gatt_ok(const std::string &address, int m)
    {
        mtu = m > 0 ? m : kDefaultMtu;
        BleEvent e;
        e.kind = BleEvent::Kind::GattResult;
        e.address = address;
        e.ok = true;
        e.mtu = mtu;
        emit(std::move(e));
    }

    void reply(Call &c, sd::message *m)
    {
        const std::string err = error_text(m);
        switch (c.op) {
        case Op::Objects: {
            if (!err.empty()) {
                ready = false;
                const bool absent = err.find("ServiceUnknown") != std::string::npos || err.find("NameHasNoOwner") != std::string::npos;
                emit(BleEvent::Kind::Ready, "", false, absent ? "BlueZ (bluetoothd) is not running" : err);
                return;
            }
            read_objects(m, false);
            if (adapter.empty()) {
                ready = false;
                emit(BleEvent::Kind::Ready, "", false, "no Bluetooth adapter");
                return;
            }
            ready = true;
            say("ready: " + adapter + (powered ? " powered" : " off") + ", " + std::to_string(devs.size()) + " devices known");
            emit(BleEvent::Kind::Ready, "", true);
            emit(BleEvent::Kind::AdapterPowered, "", powered);
            if (agent_exported)
                call("/org/bluez", "org.bluez.AgentManager1", "RegisterAgent", Op::RegisterAgent, "",
                 [this](sd::message *mm) {
                     return api.append_basic(mm, 'o', agent_path.c_str()) >= 0 && api.append_basic(mm, 's', "KeyboardOnly") >= 0;
                 },
                 5 * kSec);
            if (want_scan) begin_scan();
            return;
        }
        case Op::RegisterAgent:
            agent_registered = err.empty();
            agent_failed = !err.empty();
            say(err.empty() ? "agent registered at " + agent_path : "agent not registered: " + err);
            return;
        case Op::Ignore:
            if (!err.empty()) say(err);
            return;
        case Op::StartDiscovery:
            if (!err.empty()) {
                scanning = false;
                emit(BleEvent::Kind::ScanStopped, "", false, err);
            }
            return;
        case Op::Connect: emit(BleEvent::Kind::ConnectResult, c.address, err.empty(), err); return;
        case Op::Pair: emit(BleEvent::Kind::PairResult, c.address, err.empty(), err); return;
        case Op::GattObjects: {
            if (c.seq != gatt_seq) return;
            if (!err.empty()) {
                gatt_failed(c.address, err, false);
                return;
            }
            read_objects(m, false);
            const std::string dev = device_path(adapter, c.address);
            std::string svc;
            for (const auto &kv : svcs)
                if (kv.second.device == dev && kv.second.uuid == kNusService) svc = kv.first;
            if (svc.empty()) {
                gatt_failed(c.address, "no Nordic UART service on the device", true);
                return;
            }
            for (const auto &kv : chrs) {
                if (kv.second.service != svc) continue;
                if (kv.second.uuid == kNusTx) tx_path = kv.first;
                if (kv.second.uuid == kNusRx) rx_path = kv.first;
            }
            if (tx_path.empty() || rx_path.empty()) {
                gatt_failed(c.address, "the Nordic UART characteristics are missing", true);
                return;
            }
            call(tx_path, kChr, "AcquireNotify", Op::AcquireNotify, c.address, [this](sd::message *mm) { return append_empty_dict(mm); },
                 15 * kSec);
            return;
        }
        case Op::AcquireNotify: {
            if (c.seq != gatt_seq) return;          // closed meanwhile: the reply (and its fd) goes with the message
            if (!err.empty()) {
                say("AcquireNotify: " + err + ", using StartNotify");
                call(tx_path, kChr, "StartNotify", Op::StartNotify, c.address, nullptr, 15 * kSec);
                return;
            }
            int fd = -1;
            uint16_t m16 = 0;
            if (api.read_basic(m, 'h', &fd) <= 0 || fd < 0 || api.read_basic(m, 'q', &m16) <= 0) {
                gatt_failed(c.address, "AcquireNotify gave no socket", false);
                return;
            }
            notify_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);     // the message owns fd
            if (notify_fd < 0) {
                gatt_failed(c.address, std::string("dup: ") + std::strerror(errno), false);
                return;
            }
            ::fcntl(notify_fd, F_SETFL, ::fcntl(notify_fd, F_GETFL) | O_NONBLOCK);
            say("notify acquired, MTU " + std::to_string(m16));
            gatt_ok(c.address, m16);
            return;
        }
        case Op::StartNotify: {
            if (c.seq != gatt_seq) {                // closed meanwhile: undo the subscription
                if (err.empty()) call(c.path, kChr, "StopNotify", Op::Ignore, "", nullptr, 5 * kSec);
                return;
            }
            if (!err.empty()) {
                gatt_failed(c.address, "StartNotify: " + err, false);
                return;
            }
            notify_props = true;
            auto it = chrs.find(tx_path);
            gatt_ok(c.address, it != chrs.end() ? it->second.mtu : 0);
            return;
        }
        case Op::Write: emit(BleEvent::Kind::WriteResult, c.address, err.empty(), err); return;
        }
    }

    /* ---------------------------------------------------------------- signals */

    static int on_props(sd::message *m, void *userdata, sd::error * /*ret*/)
    {
        static_cast<Impl *>(userdata)->props_changed(m);
        return 0;
    }
    void props_changed(sd::message *m)
    {
        const char *p = api.get_path(m);
        if (!p) return;
        const std::string path = p;
        if (path.rfind("/org/bluez", 0) != 0) return;
        const char *iface = nullptr;
        if (api.read_basic(m, 's', &iface) <= 0 || !iface) return;
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
            BleEvent e;
            e.kind = BleEvent::Kind::DeviceUpdated;
            e.address = d.address;
            e.dev = d;
            emit(std::move(e));
            if (conn) emit(BleEvent::Kind::ConnectionChanged, d.address, d.connected);
            if (res) emit(BleEvent::Kind::ServicesResolved, d.address, d.services_resolved);
        } else if (i == kAdapter && path == adapter) {
            bool on = false;
            if (flag(changed, "Powered", on) && on != powered) {
                powered = on;
                emit(BleEvent::Kind::AdapterPowered, "", on);
            }
            if (flag(changed, "Discovering", on) && !on && scanning) {
                scanning = false;
                emit(BleEvent::Kind::ScanStopped, "", true);
            }
        } else if (i == kChr) {
            auto it = changed.find("MTU");
            if (it != changed.end()) chrs[path].mtu = static_cast<int>(it->second.i);
            it = changed.find("Value");
            if (notify_props && path == tx_path && it != changed.end() && !it->second.ay.empty()) {
                BleEvent e;
                e.kind = BleEvent::Kind::Notify;
                e.address = gatt_dev;
                e.data = it->second.ay;
                emit(std::move(e));
            }
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
                self->ready = self->powered = false;
                self->emit(BleEvent::Kind::AdapterPowered, "", false);
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
        if (agent_req) {
            api.msg_unref(agent_req);
            agent_req = nullptr;
        }
        close_notify();
        devs.clear();
        svcs.clear();
        chrs.clear();
        ready = powered = scanning = agent_registered = agent_failed = false;
        adapter.clear();
        if (had_notify) emit(BleEvent::Kind::NotifyClosed, gatt_dev, false);
        emit(BleEvent::Kind::AdapterPowered, "", false);
    }

    /* ---------------------------------------------------------------- agent */

    static int on_agent(sd::message *m, void *userdata, sd::error * /*ret*/)
    {
        return static_cast<Impl *>(userdata)->agent_call(m);
    }
    int agent_call(sd::message *m)
    {
        if (!api.is_method_call(m, "org.bluez.Agent1", nullptr)) return 0;
        const char *mem = api.get_member(m);
        const std::string member = mem ? mem : "";
        if (member == "RequestPasskey" || member == "RequestPinCode") {
            const char *dev = nullptr;
            api.read_basic(m, 'o', &dev);
            if (agent_req) {                         // a new request replaces an old one
                sd::error e{"org.bluez.Error.Canceled", "replaced", 0};
                api.reply_error(agent_req, &e);
                api.msg_unref(agent_req);
            }
            agent_req = api.msg_ref(m);
            agent_req_string = member == "RequestPinCode";
            agent_req_addr = dev ? address_from_path(dev) : "";
            say(member + " for " + agent_req_addr);
            emit(BleEvent::Kind::PasskeyRequest, agent_req_addr, true);
            return 1;
        }
        if (member == "Cancel") {
            if (agent_req) {
                api.msg_unref(agent_req);
                agent_req = nullptr;
                emit(BleEvent::Kind::PairingCancelled, agent_req_addr, true);
            }
            api.reply_return(m, "");
            return 1;
        }
        if (member == "Release" || member == "DisplayPasskey" || member == "DisplayPinCode") {
            api.reply_return(m, "");
            return 1;
        }
        sd::error e{"org.bluez.Error.Rejected", "Mesh Hop only enters a passkey", 0};
        api.reply_error(m, &e);
        return 1;
    }

    /* ---------------------------------------------------------------- the rest */

    void begin_scan()
    {
        want_scan = false;
        scanning = true;
        call(adapter, kAdapter, "SetDiscoveryFilter", Op::Ignore, "", [this](sd::message *m) { return append_dict_s(m, "Transport", "le"); },
             5 * kSec);
        call(adapter, kAdapter, "StartDiscovery", Op::StartDiscovery, "", nullptr, 5 * kSec);
    }

    void close_notify()
    {
        if (notify_fd >= 0) ::close(notify_fd);
        notify_fd = -1;
    }

    void read_notify()
    {
        if (notify_fd < 0) return;
        uint8_t buf[600];
        bool closed = false;
        for (int i = 0; i < 64; ++i) {                // one read = one notification (a SOCK_SEQPACKET socket)
            const ssize_t n = ::read(notify_fd, buf, sizeof(buf));
            if (n > 0) {
                BleEvent e;
                e.kind = BleEvent::Kind::Notify;
                e.address = gatt_dev;
                e.data.assign(buf, buf + n);
                emit(std::move(e));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                pollfd p{notify_fd, POLLIN, 0};
                closed = ::poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL)) && !(p.revents & POLLIN);
                break;                               // nothing more for now (or hung up)
            }
            closed = true;                           // 0: hung up, or an error
            break;
        }
        if (!closed) return;
        say("notify socket closed");
        close_notify();
        emit(BleEvent::Kind::NotifyClosed, gatt_dev, false);
    }

    void stop()
    {
        if (!bus) return;
        if (agent_req) {
            sd::error e{"org.bluez.Error.Canceled", "Mesh Hop is closing", 0};
            api.reply_error(agent_req, &e);
            api.msg_unref(agent_req);
            agent_req = nullptr;
        }
        close_notify();
        if (notify_props && !tx_path.empty()) call(tx_path, kChr, "StopNotify", Op::Ignore, "", nullptr, 2 * kSec);
        notify_props = false;
        if (scanning) call(adapter, kAdapter, "StopDiscovery", Op::Ignore, "", nullptr, 2 * kSec);
        if (agent_registered)
            call("/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent", Op::Ignore, "",
                 [this](sd::message *m) { return api.append_basic(m, 'o', agent_path.c_str()) >= 0; }, 2 * kSec);
        api.flush(bus);
        for (sd::slot *s : slots) api.slot_unref(s);
        slots.clear();
        bus = api.flush_close_unref(bus);
        bus = nullptr;
        calls.clear();
        devs.clear();
        svcs.clear();
        chrs.clear();
        out.clear();
        ready = powered = scanning = want_scan = agent_registered = agent_exported = agent_failed = false;
        adapter.clear();
        gatt_dev.clear();
        rx_path.clear();
        tx_path.clear();
        ++gatt_seq;
    }
};

/* ================================================================== */

BluezBackend::BluezBackend(std::string agent_path) : d_(std::make_unique<Impl>())
{
    d_->agent_path = std::move(agent_path);
}

BluezBackend::~BluezBackend()
{
    d_->stop();
}

bool BluezBackend::start(std::string &why)
{
    Impl &d = *d_;
    if (d.bus) return true;
    if (!d.api.load(why)) return false;
    sd::bus *b = nullptr;
    const int r = d.api.open_system(&b);
    if (r < 0 || !b) {
        why = std::string("no system D-Bus (") + std::strerror(-r) + ")";
        return false;
    }
    d.bus = b;
    struct M {
        const char *rule;
        sd::handler fn;
    };
    // no sender= in the rules: the paths and the handlers keep to /org/bluez (a well-known sender name is resolved by the
    // bus daemon, not by sd-bus's local filter)
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
    sd::slot *agent = nullptr;
    d.agent_exported = d.api.add_object(d.bus, &agent, d.agent_path.c_str(), &Impl::on_agent, &d) >= 0;
    if (d.agent_exported) d.slots.push_back(agent);
    else d.say("the agent object could not be exported: pairing would go to the default agent");
    if (!d.call("/", kObjMgr, "GetManagedObjects", Impl::Op::Objects, "", nullptr, 10 * kSec)) {
        why = "cannot ask BlueZ for its objects";
        d.stop();
        return false;
    }
    return true;
}

void BluezBackend::stop()
{
    d_->stop();
}

bool BluezBackend::started() const { return d_->bus != nullptr; }
bool BluezBackend::ready() const { return d_->ready; }
bool BluezBackend::adapter_powered() const { return d_->powered; }
std::string BluezBackend::adapter_path() const { return d_->adapter; }
bool BluezBackend::agent_registered() const { return d_->agent_registered; }
bool BluezBackend::notify_acquired() const { return d_->notify_fd >= 0; }
void BluezBackend::set_log(std::function<void(const std::string &)> log) { d_->log = std::move(log); }

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
    if (!d.ready) {
        d.want_scan = true;
        return;
    }
    d.begin_scan();
}

void BluezBackend::stop_scan()
{
    Impl &d = *d_;
    d.want_scan = false;
    if (!d.bus || !d.scanning) return;
    d.scanning = false;
    d.call(d.adapter, kAdapter, "StopDiscovery", Impl::Op::Ignore, "", nullptr, 5 * kSec);
}

std::vector<BleDevice> BluezBackend::devices() const
{
    std::vector<BleDevice> v;
    for (const auto &kv : d_->devs)
        if (kv.first.rfind(d_->adapter + "/", 0) == 0) v.push_back(kv.second);
    return v;
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
    if (!d.call(device_path(d.adapter, address), kDevice, "Connect", Impl::Op::Connect, address, nullptr, 40 * kSec))
        d.emit(BleEvent::Kind::ConnectResult, address, false, "the D-Bus call failed");
}

void BluezBackend::pair(const std::string &address)
{
    Impl &d = *d_;
    // RegisterAgent went out before (in the Ready step) and BlueZ handles our calls in order; only a refused or missing
    // agent is worth a line: BlueZ would then ask the default agent (the launcher's)
    if (!d.agent_exported || d.agent_failed) d.say("pairing without our agent: BlueZ will ask the default agent (the launcher's)");
    if (!d.call(device_path(d.adapter, address), kDevice, "Pair", Impl::Op::Pair, address, nullptr, 100 * kSec))
        d.emit(BleEvent::Kind::PairResult, address, false, "the D-Bus call failed");
}

void BluezBackend::answer_passkey(bool ok, uint32_t passkey)
{
    Impl &d = *d_;
    if (!d.agent_req) return;
    if (ok) {
        if (d.agent_req_string) {
            char s[12];
            std::snprintf(s, sizeof(s), "%06u", static_cast<unsigned>(passkey));
            d.api.reply_return(d.agent_req, "s", s);
        } else {
            d.api.reply_return(d.agent_req, "u", static_cast<uint32_t>(passkey));
        }
    } else {
        sd::error e{"org.bluez.Error.Canceled", "cancelled by the user", 0};
        d.api.reply_error(d.agent_req, &e);
    }
    d.api.msg_unref(d.agent_req);
    d.agent_req = nullptr;
}

void BluezBackend::cancel_pairing(const std::string &address)
{
    Impl &d = *d_;
    d.call(device_path(d.adapter, address), kDevice, "CancelPairing", Impl::Op::Ignore, address, nullptr, 5 * kSec);
}

void BluezBackend::disconnect(const std::string &address)
{
    Impl &d = *d_;
    d.call(device_path(d.adapter, address), kDevice, "Disconnect", Impl::Op::Ignore, address, nullptr, 10 * kSec);
}

void BluezBackend::remove_device(const std::string &address)
{
    Impl &d = *d_;
    if (d.adapter.empty()) return;
    const std::string path = device_path(d.adapter, address);
    d.call(d.adapter, kAdapter, "RemoveDevice", Impl::Op::Ignore, address,
           [&d, &path](sd::message *m) { return d.api.append_basic(m, 'o', path.c_str()) >= 0; }, 10 * kSec);
}

void BluezBackend::open_gatt(const std::string &address)
{
    Impl &d = *d_;
    close_gatt();
    d.gatt_dev = address;
    if (!d.call("/", kObjMgr, "GetManagedObjects", Impl::Op::GattObjects, address, nullptr, 10 * kSec))
        d.gatt_failed(address, "the D-Bus call failed", false);
}

void BluezBackend::close_gatt()
{
    Impl &d = *d_;
    ++d.gatt_seq;
    d.close_notify();
    if (d.notify_props && !d.tx_path.empty()) d.call(d.tx_path, kChr, "StopNotify", Impl::Op::Ignore, "", nullptr, 5 * kSec);
    d.notify_props = false;
    d.tx_path.clear();
    d.rx_path.clear();
    d.gatt_dev.clear();
    d.mtu = 0;
}

void BluezBackend::write(const Bytes &frame)
{
    Impl &d = *d_;
    if (d.rx_path.empty()) {
        d.emit(BleEvent::Kind::WriteResult, d.gatt_dev, false, "the Nordic UART service is not open");
        return;
    }
    // "request" (Write Request, answered by the board): the firmwares' RX characteristic has the write property
    // (esp32: PROPERTY_WRITE only; nrf52 Bluefruit: write and write-without-response)
    const char *type = "request";
    auto it = d.chrs.find(d.rx_path);
    if (it != d.chrs.end()) {
        const auto &f = it->second.flags;
        if (std::find(f.begin(), f.end(), "write") == f.end() &&
            std::find(f.begin(), f.end(), "write-without-response") != f.end())
            type = "command";
    }
    const bool ok = d.call(d.rx_path, kChr, "WriteValue", Impl::Op::Write, d.gatt_dev,
                           [&d, &frame, type](sd::message *m) {
                               return d.api.append_array(m, 'y', frame.data(), frame.size()) >= 0 && d.append_dict_s(m, "type", type);
                           },
                           10 * kSec);
    if (!ok) d.emit(BleEvent::Kind::WriteResult, d.gatt_dev, false, "the D-Bus call failed");
}

} // namespace meshzero::ble
