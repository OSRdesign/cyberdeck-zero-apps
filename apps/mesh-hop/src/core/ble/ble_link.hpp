/*
 * SPDX-License-Identifier: MIT
 *
 * The Bluetooth LE link to one MeshCore companion board: a state machine over an IBleBackend (no LVGL, no D-Bus; unit
 * tested with FakeBle). Driven by tick(now) from the UI timer; never blocks.
 *
 *   Idle --connect()--> [Starting] --> Scanning (only when BlueZ does not know the address) --> Pairing (not paired yet)
 *        --> Connecting (Connect, then ServicesResolved, then the NUS characteristics and notify) --> Connected
 *   Pairing: Device1.Pair from this process, so BlueZ asks THIS app's agent for the passkey (pin_needed, then
 *            submit_pin()); a wrong PIN ends in Failed(PairingFailed): the user tries again, nothing retries by itself.
 *   Connected --link lost--> Reconnecting --backoff 2, 4, 8, 16, 30, 60 s--> Connecting ... (forever, like the USB port)
 *   Failed: the user must act (no Bluetooth, adapter off at the start, device not found, wrong PIN, not a companion, the
 *           board forgot the bond). disconnect() goes back to Idle from anywhere.
 */

#pragma once

#include "ble_framing.hpp"
#include "ble_types.hpp"

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace meshzero::ble {

enum class BleState { Idle, Scanning, Connecting, Pairing, Connected, Reconnecting, Failed };

enum class BleFail {
    None,
    NoBluetooth,        // no libsystemd / system bus / BlueZ / adapter
    AdapterOff,         // the adapter is powered off (Settings > Bluetooth of the launcher)
    NotFound,           // the address was not seen in the search scan
    PairingFailed,      // wrong PIN, rejected or timed out
    PairingCancelled,   // cancel_pairing(), or BlueZ gave up waiting for the PIN
    NotCompanion,       // connected, but no Nordic UART service: not a MeshCore BLE companion
    BondLost,           // a bonded board refuses the stored keys (re-flashed or reset): forget it and pair again
};

const char *state_name(BleState s);
/* Plain words for a failure ("" for None). */
const char *fail_text(BleFail f);

struct BleStatus {
    BleState state = BleState::Idle;
    BleFail fail = BleFail::None;
    bool pin_needed = false;        // Pairing: the board waits for its PIN (submit_pin)
    std::string address;            // the target
    std::string name;               // its advertised name when known
    std::string detail;             // the last BlueZ error or step, for the log and the status line
    int attempt = 0;                // failed tries since the last good link
    uint64_t retry_at_ms = 0;       // Reconnecting: when the next try starts
    int mtu = 0;                    // Connected: the ATT MTU
    bool scanning = false;          // a discovery of ours runs
};

struct BleTimings {
    uint64_t connect_ms = 35000;    // Device1.Connect (BlueZ's own call timeout is shorter)
    uint64_t pair_ms = 95000;       // Device1.Pair including the PIN typing (BlueZ's agent waits 60 s)
    uint64_t services_ms = 20000;   // ServicesResolved after the connection
    uint64_t gatt_ms = 15000;       // the characteristics and the notify socket
    uint64_t write_ms = 10000;      // one GATT write (meshcore_py: 10 s, then it drops the link)
    uint64_t search_ms = 15000;     // the search scan for an address BlueZ does not know
    uint64_t list_scan_ms = 10000;  // start_scan() for the device list
    uint64_t backoff_first_ms = 2000;
    uint64_t backoff_max_ms = 60000;
    int bond_lost_after = 2;        // authentication errors in a row on a bonded board -> BondLost
};

class BleLink {
public:
    explicit BleLink(IBleBackend &backend, BleTimings timings = {});

    /* The device list (the stage B "Bluetooth" page): a discovery of list_scan_ms. False when Bluetooth cannot be used
     * (why). candidates(): devices with the Nordic UART service or the MeshCore name, paired first, then the strongest. */
    bool start_scan(uint64_t now_ms, std::string *why = nullptr);
    void stop_scan();
    std::vector<BleDevice> candidates() const;

    /* Starts (or restarts) the link to this address. */
    void connect(const std::string &address, uint64_t now_ms);
    /* The board's PIN (6 digits, shown on its screen or 123456 by default); ignored unless status().pin_needed. */
    void submit_pin(uint32_t pin);
    void cancel_pairing();
    /* Ends the link (Device1.Disconnect, the bond stays) and stops every timer: Idle. */
    void disconnect();

    void tick(uint64_t now_ms);

    const BleStatus &status() const { return st_; }
    bool connected() const { return st_.state == BleState::Connected; }
    /* Grows each time a Connected link ends (lost or disconnect()): the transport reports the loss once per value. */
    uint32_t generation() const { return generation_; }

    /* One frame for one GATT write; false when not connected, empty or too long, or 32 frames wait already. */
    bool send(const Bytes &frame);
    /* The next frame from the board. */
    bool receive(Bytes &frame);

    struct Stats {
        size_t frames_in = 0, frames_out = 0, long_writes = 0, joined = 0, write_errors = 0, refused = 0, connects = 0;
    };
    const Stats &stats() const { return stats_; }

    void set_log(std::function<void(const std::string &)> log) { log_ = std::move(log); }

private:
    enum class Phase { None, Start, Connect, Services, Gatt };

    void handle(const BleEvent &e);
    bool ensure_backend(std::string &why);
    void begin_attempt();
    void go_connect_phase(Phase p);
    void attempt_failed(const std::string &why);
    void link_lost(const std::string &why);
    void fail(BleFail f, const std::string &why);
    void set_state(BleState s, const std::string &detail);
    void pump_writes();
    uint64_t backoff(int attempt) const;
    bool target(const BleEvent &e) const { return !st_.address.empty() && e.address == st_.address; }
    void log(const std::string &line) const;

    IBleBackend &be_;
    BleTimings t_;
    BleStatus st_;
    Phase phase_ = Phase::None;
    uint64_t now_ = 0;
    uint64_t deadline_ = 0;           // of the step in progress (0: none)
    uint64_t list_scan_until_ = 0;
    bool searching_ = false;          // the scan is the search for the target
    bool ever_connected_ = false;
    bool bonded_target_ = false;
    int auth_errors_ = 0;
    uint32_t generation_ = 0;

    NotifyAssembler assembler_;
    std::deque<Bytes> tx_;
    bool write_inflight_ = false;
    uint64_t write_deadline_ = 0;
    std::deque<Bytes> rx_;

    Stats stats_;
    std::function<void(const std::string &)> log_;
};

} // namespace meshzero::ble
