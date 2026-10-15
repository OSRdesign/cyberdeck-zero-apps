/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the BLE scan behind the radar (no LVGL; tests/test_ble.cpp on output recorded on a Pi 3A+).
 *
 * How (decided on the Pi 3A+, bluez 5.82, 2026-10-15; the deck has the same bluez): BlueZ's own `bluetoothctl`, run by the
 * stream runner (no shell, no new library, no change in the shared build), its commands written on its stdin:
 *     menu scan / duplicate-data on / transport le / back / scan on
 * With duplicate data on, BlueZ reports every advertisement it hears: one `[CHG] Device <addr> RSSI: 0xffffffb8 (-72)`
 * line each time (busy devices several per second, slow ones about once per second or less, as they advertise); a plain
 * `bluetoothctl --timeout N scan on` gave no RSSI line at all. A discovery started with `busctl call` ends with the call
 * (BlueZ ties discovery to the D-Bus client), and an in-process D-Bus client (sd-bus) would need libsystemd in the shared
 * build. Each new device gets one `info <addr>` (its answer's first line, `Device <addr> (random)`, gives the address
 * type), at most kBleMaxInfo per run.
 *
 * Stopping, on every path: the scan time is up (`scan off`, `quit`, stdin closed), the user stops it, the session is
 * destroyed, a refusal from BlueZ or the runner's timeout (the stream runner's farewell `scan off` + `quit`, then
 * SIGTERM and SIGKILL to the process group). BlueZ also ends the discovery of a client whose D-Bus connection goes: on
 * the Pi 3A+ `Discovering: no` was read 0.5 s after a SIGKILL of bluetoothctl in the middle of a scan.
 *
 * Before the scan, plain-words reasons instead of a failure: no adapter (/sys/class/bluetooth), rfkill (soft or hard
 * block, /sys/class/rfkill), bluez missing (no bluetoothctl), bluetoothd not running, the adapter powered off
 * (`bluetoothctl show`).
 *
 * The device table: address, address type when known, name (or alias when it is not the address), RSSI raw and smoothed
 * (exponential, alpha 0.3) with the time of the last reading, TX power, the company id of the manufacturer data and its
 * name from a short table (the app ships no OUI table; random addresses have no OUI anyway), first and last seen. A
 * device not heard for expiry_ms (20 s) leaves the live list; `seen` counts every device heard in the run. A rough
 * distance class from the smoothed RSSI only: near from -60 dBm, medium from -80 dBm, far below: not metres.
 */
#pragma once

#include "cmdrun.hpp"
#include "source.hpp"
#include "worker.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nt {

constexpr const char *kBleWarn =
    "Wi-Fi and Bluetooth share one radio: a Bluetooth keyboard or mouse may hiccup while the scan runs.";
constexpr const char *kBleDistanceCaveat =
    "A rough guess from the signal strength only, not metres: walls, bodies and each device's own power change it.";
constexpr std::size_t kBleMaxDevices = 256; /* entries kept in a run (the oldest silent ones go first) */
constexpr int kBleMaxInfo = 64;             /* `info` questions in a run */

/* ---------------------------------------------------------------- bluetoothctl's output (pure) */

/* One output line as the user would see it: the part after the last '\r' (bluetoothctl redraws its prompt), colour
 * sequences and backspaces removed, a leading prompt "[bluetoothctl]> " (or "[name]> ") removed, blanks at the end
 * removed; a leading tab (an `info` field) is kept. */
std::string clean_btctl_line(const std::string &raw);

struct BtEvent {
    enum class Kind {
        None,
        NewDevice,     /* [NEW] Device <addr> <name or alias> */
        DelDevice,     /* [DEL] Device <addr> ... */
        Rssi,          /* [CHG] Device <addr> RSSI: 0xffffffb8 (-72)   (older bluez: RSSI: -72) */
        RssiGone,      /* [CHG] Device <addr> RSSI is nil   (the scan ended) */
        TxPower,       /* [CHG] Device <addr> TxPower: 0x0008 (8) */
        Name,          /* [CHG] Device <addr> Name: ... */
        Alias,         /* [CHG] Device <addr> Alias: ... */
        Company,       /* [CHG] Device <addr> ManufacturerData.Key: 0x004c (76) */
        InfoHeader,    /* Device <addr> (public|random): the start of an `info` answer; text = the type */
        InfoField,     /* "\t<key>: <value>" inside an `info` answer; key, text = the value */
        InfoMissing,   /* Device <addr> not available */
        Discovering,   /* [CHG] Controller <addr> Discovering: yes|no, "Discovery started|stopped"; value 1 / 0 */
        Failed,        /* Failed to start discovery: org.bluez.Error.NotReady (text = the error) */
        NoController,  /* No default controller available */
        WaitingDaemon  /* Waiting to connect to bluetoothd... */
    };
    Kind kind = Kind::None;
    std::string address;      /* "AA:BB:CC:DD:EE:FF" */
    int value = 0;            /* dBm, the company id, 1 / 0 */
    std::string key;          /* InfoField */
    std::string text;         /* a name, an address type, a value, an error */
};
bool parse_btctl_line(const std::string &clean, BtEvent &out);

/* "Bluetooth is switched off (powered off) ..." for a Failed event's error name */
std::string bt_failure_words(const std::string &error);

/* `bluetoothctl show` */
struct BtAdapter {
    bool present = false;         /* a "Controller <addr>" line */
    std::string address, name;
    bool powered = false;
    std::string power_state;      /* bluez 5.6x+: "on", "off", "off-blocked", ... */
    bool discovering = false;
    bool no_controller = false;   /* "No default controller available" */
    bool waiting_daemon = false;  /* "Waiting to connect to bluetoothd..." */
};
BtAdapter parse_btctl_show(const std::string &text);

/* /sys: an hci adapter, and its rfkill state */
struct BtSysState {
    bool adapter = false;         /* /sys/class/bluetooth/hci* */
    bool rfkill_found = false;
    bool soft_blocked = false, hard_blocked = false;
};
BtSysState read_bt_sys(const Source &src);

/* "Apple", "Microsoft", ... for a Bluetooth SIG company id (a short table), else "" */
const char *company_name(int id);

enum class DistanceClass { Unknown, Near, Medium, Far };
DistanceClass distance_class(double rssi_dbm); /* near >= -60, medium >= -80, far below */
const char *distance_words(DistanceClass c);    /* "near", "medium", "far", "" */

/* ---------------------------------------------------------------- the device table (pure, injected clock) */

struct BleDevice {
    std::string address;
    std::string address_type;     /* "public", "random", "" (not asked yet) */
    std::string name;             /* the name, else an alias that is not just the address, else "" */
    bool has_rssi = false;
    int rssi_dbm = 0;             /* the last reading */
    double smooth_dbm = 0;        /* exponential smoothing of the readings */
    std::int64_t rssi_at_ms = 0;
    bool has_tx = false;
    int tx_dbm = 0;
    int company = -1;             /* manufacturer data's company id, -1 none */
    std::string vendor;           /* company_name(), or "company 0x1234" */
    std::int64_t first_seen_ms = 0, last_seen_ms = 0;
    int readings = 0;
    DistanceClass distance = DistanceClass::Unknown;
};

class BleTable {
public:
    explicit BleTable(int expiry_ms = 20000, double alpha = 0.3);
    void apply(const BtEvent &e, std::int64_t now_ms);
    /* devices unheard for expiry_ms leave the live list (they stay counted in seen) */
    void expire(std::int64_t now_ms);
    std::vector<BleDevice> live() const; /* strongest smoothed RSSI first */
    int live_count() const;
    int seen_count() const { return seen_; }
    const BleDevice *find(const std::string &address) const; /* live or not */
    std::size_t size() const { return all_.size(); }

private:
    struct Entry {
        BleDevice d;
        bool live = false;
        bool counted = false;
    };
    Entry &entry(const std::string &address, std::int64_t now_ms);
    void set_name(Entry &e, const std::string &name, bool is_alias);
    void set_company(Entry &e, int id);

    int expiry_ms_;
    double alpha_;
    std::map<std::string, Entry> all_;
    std::string info_address_;    /* the `info` answer being read */
    int seen_ = 0;
};

/* ---------------------------------------------------------------- the scan */

struct BleConfig {
    int seconds = 60;             /* 5 .. 60; the scan stops by itself */
    int expiry_ms = 20000;
    double alpha = 0.3;
    bool ask_address_type = true; /* one `info` per new device */
    int publish_ms = 500;
    std::vector<std::string> program = {"bluetoothctl"}; /* tests: a fake */
};

struct BleSnapshot {
    enum class State { Idle, Checking, Scanning, Done, Failed, Cancelled };
    State state = State::Idle;
    std::string warn = kBleWarn;  /* shown before and while scanning */
    std::string error;            /* Failed: plain words */
    std::string adapter;          /* "PI3A (B8:27:EB:..)" */
    bool other_discovery = false; /* another program was scanning already */
    std::vector<BleDevice> devices; /* live, strongest first */
    int live = 0, seen = 0;
    double elapsed_s = 0;         /* since the scan started */
    int seconds = 60;
    bool discovering = false;
    std::string stop_note;        /* how it ended: "Scan stopped after 60 s." */
    bool running() const { return state == State::Checking || state == State::Scanning; }
};

/* The words for an adapter that cannot scan ("" when it can): /sys first, then `bluetoothctl show` (its result r). */
std::string bt_unavailable_words(const BtSysState &sys, const CommandResult &show, const BtAdapter &adapter);

/* The whole run on the calling thread (the session's worker; tests call it directly). */
BleSnapshot run_ble_scan(const BleConfig &cfg, const Source &src, CommandRunner &cmd, StreamRunner &stream,
                         const CancelToken &cancel, const std::function<void(const BleSnapshot &)> &publish);

class BleSession {
public:
    BleSession(); /* the system's /sys, DirectRunner, DirectStreamRunner */
    BleSession(std::shared_ptr<const Source> src, std::shared_ptr<CommandRunner> cmd, std::shared_ptr<StreamRunner> stream);
    bool start(const BleConfig &cfg); /* LVGL thread; a running scan is stopped first */
    void stop();                      /* discovery off, bluetoothctl ended (within its grace) */
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const BleSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    std::shared_ptr<const Source> src_;
    std::shared_ptr<CommandRunner> cmd_;
    std::shared_ptr<StreamRunner> stream_;
    Channel<BleSnapshot> channel_;
    Worker worker_;
};

} // namespace nt
