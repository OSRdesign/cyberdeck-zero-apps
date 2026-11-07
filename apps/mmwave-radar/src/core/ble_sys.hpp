/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: a Hi-Link HLK-LD radar over Bluetooth LE, read through the same listen-only port interface as the USB
 * serial ports (port_sys.hpp). No LVGL, no D-Bus here: BleSys is a state machine over a BleBackend (the real one is
 * bluez_backend.hpp, the tests and the scenes use fakes), driven by poll(now) from the UI timer; nothing blocks.
 *
 * The module (measured on the owner's HLK-LD2451, README "Bluetooth"): advertises as "HLK-LD2451_xxxx" (public address,
 * connectable, no bonding needed); GATT service 0xfff0 with the characteristic 0xfff1 (notify): one whole LD2451 frame
 * per notification, a real-time mirror of the UART stream (0 to 0.5 s late). The module's other characteristics
 * (0xfff2, 0xae01, the Device Name) are writable without authentication: this app never touches them.
 *
 * LISTEN-ONLY BY CONSTRUCTION: BleBackend has no call that changes a characteristic value or reads one, none that bonds
 * with a device, marks it as known-good or changes an adapter setting (it never powers the adapter on). The only thing
 * sent to the module is the standard notification subscription of 0xfff1 (its CCCD, through BlueZ's AcquireNotify or
 * StartNotify). BleSys has no write call either: as a Sys it only reads (tests/test_package.cpp greps these sources).
 *
 * The lifecycle:
 *   Off --set_enabled--> Starting --BlueZ answers--> Unavailable (no BlueZ / no adapter / adapter off: a message only)
 *                                                  | Idle (no target: discovery windows for the list while browsing)
 *   with a target (the remembered radar, or the one the user chose):
 *     known to BlueZ -> Connecting (Device1.Connect, up to connect_ms with the services) -> Subscribing (0xfff1 notify)
 *                    -> Connected (the port "ble:<address>" is listed, notifications are its bytes)
 *     not known      -> Scanning (a discovery window) -> seen: Connecting; not seen: Idle, the next window later
 *   a failed step, a lost link or the reader letting go (the link's Lost, a detector verdict) -> the device is
 *   disconnected, Waiting: retry after 2, 4, 8, 15, then 30 s (ble_backoff_ms), only while active (the Radar page is
 *   visible); a link that streamed for 20 s or more starts the waits again at 2 s when it ends. Discovery is never continuous: windows of scan_window_ms with scan_gap_ms between them (the Pi 3A+ shares
 *   its radio with Wi-Fi), stopped as soon as a connection starts. set_enabled(false) and the destructor disconnect.
 */
#pragma once

#include "port_sys.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mmw {

using Ms = std::int64_t;

/* The name prefix the HLK-LD modules advertise ("HLK-LD2451_49F0") and their data characteristic (lower case, as BlueZ
 * reports UUIDs). */
constexpr const char *kHlkNamePrefix = "HLK-LD";
constexpr const char *kHlkDataService = "0000fff0-0000-1000-8000-00805f9b34fb";
constexpr const char *kHlkDataNotify = "0000fff1-0000-1000-8000-00805f9b34fb";
/* The handles BleSys gives out (above any file descriptor and the demo's handles). */
constexpr int kBleHandleBase = 1 << 24;

/* A device BlueZ knows. */
struct BleDevice {
    std::string address;        /* "AA:BB:CC:DD:EE:FF" */
    std::string name;           /* the advertised name, "" when unknown */
    bool has_rssi = false;      /* RSSI is only valid while discovering */
    int rssi = 0;
    bool connected = false;
    bool services_resolved = false;
};

/* A radar for the list: seen during a discovery window of this session (the last RSSI kept). */
struct BleCandidate {
    std::string address, name;
    bool has_rssi = false;
    int rssi = 0;
};

/* True when the advertised name starts with "HLK-LD" (any case): the only devices listed. */
bool hlk_candidate(const std::string &name);
/* The last two bytes of an address, "49:F0" ("" for a bad address). */
std::string address_tail(const std::string &address);

/* The diagnostic log's word for BlueZ's answer to Device1.Disconnect (error = "" when it succeeded): "ok", "ok-already-
 * disconnected" for the expected answer when the link is gone already ("No matching connection", NotConnected), else
 * "error:<text>". */
std::string ble_disconnect_result(const std::string &error);

/* Why Bluetooth cannot be used here. */
enum class BleAvail { Unknown, Ok, NoBluez, NoAdapter, Off };

struct BleEvent {
    enum class Kind {
        Ready,              /* BlueZ answered (or not): availability() changed */
        AdapterChanged,     /* the adapter was powered on / off, appeared or went away */
        DeviceUpdated,      /* address: a device appeared or changed (name, RSSI, connected ...) */
        DeviceRemoved,      /* address */
        ScanStopped,        /* the discovery ended without us (BlueZ, the adapter); error when it could not start */
        ConnectResult,      /* address, ok, error: Device1.Connect answered */
        ConnectionChanged,  /* address, ok = connected */
        ServicesResolved,   /* address, ok = resolved */
        SubscribeResult,    /* address, ok, error: the 0xfff1 notifications are on (or why not) */
        NotifyClosed,       /* the notification channel closed (the link dropped) */
    };
    Kind kind = Kind::Ready;
    std::string address;
    bool ok = false;
    std::string error;
};

/* Where the notifications go: called from BleBackend::poll with each notification's bytes (no copy kept). */
class NotifySink {
public:
    virtual ~NotifySink() {}
    virtual void on_notify(const std::uint8_t *data, std::size_t n) = 0;
};

/* What BlueZ does for the app. Every call returns at once; answers come back as events from poll(). The interface
 * offers discovery, connect / disconnect and the notification subscription of 0xfff1, nothing else (see the top). */
class BleBackend {
public:
    virtual ~BleBackend() {}
    /* Connects to the system bus and asks BlueZ for its objects. False at once when Bluetooth cannot be used here (why:
     * no libsystemd, no system bus); else Ready follows. */
    virtual bool start(std::string &why) = 0;
    /* Ends the subscription and the discovery of this process and closes the bus (it disconnects nothing by itself:
     * BleSys asks for the disconnection first). */
    virtual void stop() = 0;
    virtual bool started() const = 0;
    virtual BleAvail availability() const = 0;     /* Unknown until Ready */
    virtual std::string unavailable_why() const = 0; /* the detail for the log ("bluetoothd is not running") */
    virtual void poll(std::vector<BleEvent> &out) = 0;
    virtual void start_scan() = 0;               /* Adapter1 discovery, LE only */
    virtual void stop_scan() = 0;
    virtual bool find(const std::string &address, BleDevice &out) const = 0;
    virtual void connect(const std::string &address) = 0;      /* Device1.Connect -> ConnectResult */
    virtual void disconnect(const std::string &address) = 0;   /* Device1.Disconnect (also cancels a Connect) */
    virtual void subscribe(const std::string &address) = 0;    /* 0xfff1 notifications on -> SubscribeResult */
    virtual void unsubscribe() = 0;
    virtual void set_sink(NotifySink *sink) = 0;
    /* "notify=AcquireNotify mtu=512" for the diagnostic log. */
    virtual std::string describe() const { return std::string(); }
    /* The diagnostic log (null = off): lines without a time stamp ("ble-disconnect addr=... result=...") for the events
     * only the backend sees, as BlueZ answers. Not set when logging is off. */
    virtual void set_diag(std::function<void(const std::string &)> /*sink*/) {}
};

enum class BleState { Off, Starting, Unavailable, Idle, Scanning, Connecting, Subscribing, Connected, Waiting };
const char *ble_state_name(BleState s);

/* The Bluetooth line of the "No radar found" page's TRIED list (Connection Auto), from the state the app already has:
 * module_seen = an HLK-LD module is in the scan list; no_radar_chosen = Auto has no remembered Bluetooth radar (so
 * the Bluetooth fallback never started). */
std::string ble_tried_line(BleState s, bool module_seen, bool no_radar_chosen);

struct BleTimings {
    Ms scan_window_ms = 10000;  /* one discovery window */
    Ms scan_gap_ms = 20000;     /* between two windows while nothing is connected and the list is shown */
    Ms connect_ms = 10000;      /* Device1.Connect and the services resolved */
    Ms subscribe_ms = 5000;     /* the characteristic found and the notifications on */
    Ms start_ms = 5000;         /* BlueZ answers the first question */
};

/* The wait before retry number `attempt` (1, 2, ...): 2, 4, 8, 15 s, then 30 s. */
Ms ble_backoff_ms(int attempt);

class BleSys : public Sys, private NotifySink {
public:
    explicit BleSys(std::unique_ptr<BleBackend> backend, const BleTimings &timings = BleTimings());
    ~BleSys() override;
    BleSys(const BleSys &) = delete;
    BleSys &operator=(const BleSys &) = delete;

    /* --- control (the app, through MuxSys) */
    /* Wanted (Connection Bluetooth, or Auto's fallback): false disconnects, stops the discovery and closes BlueZ. */
    void set_enabled(bool on, Ms now);
    /* The Radar page is visible: discovery windows and retries run only then (a connection stays while hidden). */
    void set_active(bool on, Ms now);
    /* Browsing: discovery windows for the list while nothing is connected (Connection Bluetooth). Without it the
     * discovery only looks for the target (Auto). */
    void set_browse(bool on) { browse_ = on; }
    /* The remembered radar: the target when nothing else was chosen. */
    void set_remembered(const std::string &address, const std::string &name);
    /* The user picked a radar of the list: it becomes the target (and the remembered one), connected now. */
    void choose(const std::string &address, Ms now);
    /* The user's Rescan: a discovery window now, or the waiting retry now, or BlueZ asked again. */
    void rescan(Ms now);
    void poll(Ms now);
    /* Disconnects and closes BlueZ now (also done by the destructor). */
    void shutdown();

    /* --- for the UI */
    BleState state() const { return st_; }
    BleAvail availability() const { return avail_; }
    bool enabled() const { return enabled_; }
    bool scanning() const { return scanning_; }
    const std::string &target_address() const { return target_addr_; }
    const std::string &target_name() const { return target_name_; }
    /* The HLK-LD radars seen this session, strongest first (has RSSI before none, then by name). */
    std::vector<BleCandidate> candidates() const;
    int attempt() const { return attempt_; }
    /* The headline of the Radar page while no radar streams ("" when Off), and the line under it. */
    std::string title() const;
    std::string detail(Ms now) const;
    /* The last failure, for the log and the Sensor page ("" when none). */
    const std::string &last_error() const { return last_error_; }
    /* The user chose a radar since the last call (the app stores it): true once, with its address and name. */
    bool take_chosen(std::string &address, std::string &name);
    std::uint64_t notifications() const { return notifications_; }
    std::uint64_t dropped_bytes() const { return dropped_; }
    /* A log line for each change of state (the app prints it): true once per change. */
    bool take_log(std::string &line);
    /* The diagnostic log (mmwave.log): `sink` gets one line per event, "t=<ms> ble-scan ..." (scan windows and the
     * radars seen, the user's choice, connect / subscribe with their duration and result, the retry waits, the
     * disconnection). Never per notification, never a secret. Null (the default) switches it off: nothing is built. */
    void set_diag(std::function<void(const std::string &)> sink);

    /* --- Sys (read only; one port: the connected radar) */
    std::vector<PortMeta> list_ports() override;
    bool held_by_other(const std::string &path) override;
    int open(const std::string &path, OpenErr &err) override;
    bool configure(int handle, int baud) override;
    void flush_input(int handle) override;
    long read(int handle, std::uint8_t *buf, std::size_t n) override;
    void restore(int handle) override;
    void close(int handle) override;
    std::string describe(int handle) override;
    std::uint64_t ports_generation() override { return gen_; }

private:
    void on_notify(const std::uint8_t *data, std::size_t n) override;
    void handle(const BleEvent &e, Ms now);
    void after_ready(Ms now);
    void begin_attempt(Ms now);
    void begin_window(Ms now, bool searching);
    void end_window(Ms now);
    void stop_scan(const char *why = "stopped");
    void fail_attempt(const std::string &why, Ms now);
    void lose(const std::string &why, Ms now);
    void drop_link(const char *why = "dropped");
    void start_subscribe(Ms now);
    void connect_done(const std::string &result, Ms now);
    void subscribe_done(const std::string &result, Ms now);
    void scan_done(const std::string &reason);
    void dlog(const std::string &line) const;
    void set_state(BleState s, const std::string &why = std::string());
    void note_device(const std::string &address);
    std::string shown_name() const;

    std::unique_ptr<BleBackend> be_;
    BleTimings t_;
    BleState st_ = BleState::Off;
    BleAvail avail_ = BleAvail::Unknown;
    Ms now_ = 0;                   /* the latest time given (close() has none) */
    bool enabled_ = false, active_ = false, browse_ = false;
    std::string target_addr_, target_name_;
    std::string rem_addr_, rem_name_;
    bool chosen_ = false;
    Ms deadline_ = 0;
    bool scanning_ = false, searching_ = false;
    Ms scan_until_ = 0, next_window_ = 0;
    bool connect_answered_ = false;
    int attempt_ = 0;
    Ms retry_at_ = 0;
    bool lost_ = false;            /* Waiting after a connection that worked (vs. one that never came up) */
    Ms connected_at_ = 0;
    static constexpr Ms kStableMs = 20000; /* a link that lasted this long resets the retry waits when it ends */
    std::string last_error_;
    std::vector<BleCandidate> seen_;
    bool disc_sent_ = false;       /* Device1.Disconnect was asked and the link did not report down yet: not asked twice */
    /* the diagnostic log */
    std::function<void(const std::string &)> diag_;
    Ms connect_t0_ = 0, sub_t0_ = 0;
    bool connect_open_ = false, sub_open_ = false;
    bool win_open_ = false;
    std::set<std::string> win_found_;                          /* the devices BlueZ reported in this window */
    std::vector<std::pair<std::string, std::string>> win_kept_; /* the HLK-LD ones: address, "name=... addr=... rssi=..." */
    /* the port */
    int handle_ = -1;
    bool handle_dead_ = false;
    std::uint32_t handle_seq_ = 0;
    std::uint64_t gen_ = 1;
    bool got_data_ = false;
    std::uint64_t notifications_ = 0, dropped_ = 0;
    static constexpr std::size_t kRing = 8192;
    std::uint8_t ring_[kRing] = {};
    std::size_t head_ = 0, count_ = 0;   /* the oldest byte, the bytes waiting */
    std::string log_;
    bool log_new_ = false;
    std::vector<BleEvent> events_;
};

} // namespace mmw
