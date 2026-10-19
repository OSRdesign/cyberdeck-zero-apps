/*
 * SPDX-License-Identifier: MIT
 *
 * Bluetooth LE link to a MeshCore companion radio: the shared types (no LVGL, no D-Bus).
 *
 * A MeshCore BLE companion exposes the Nordic UART service (NUS). The app writes one companion frame per GATT write to the
 * RX characteristic and receives one frame per notification of the TX characteristic, WITHOUT the serial wrapper
 * ('<' / '>' + u16 length). Facts and sources: docs/BLE.md.
 *
 * The layers (all non-blocking, driven by polls from the UI timer, one thread):
 *   IBleBackend   what BlueZ does for us: discovery, connect, pair (with the app's own agent), GATT notify / write. The real
 *                 one is BluezBackend (sd-bus, loaded at run time); the tests use FakeBle.
 *   BleLink       the state machine: Idle, Scanning, Connecting, Pairing (PIN needed), Connected, Reconnecting, Failed.
 *   BleTransport  an ITransport over a BleLink, so the Client is the same for USB and Bluetooth.
 */

#pragma once

#include "../util.hpp"

#include <string>
#include <vector>

namespace meshzero::ble {

/* Nordic UART service and characteristics, lower case as BlueZ reports them. */
constexpr const char *kNusService = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char *kNusRx = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";   // app -> board: write (with response)
constexpr const char *kNusTx = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";   // board -> app: notify
/* BLE_NAME_PREFIX of the companion firmware (examples/companion_radio/MyMesh.h): "MeshCore-" + the node name. */
constexpr const char *kNamePrefix = "MeshCore-";
/* MAX_FRAME_SIZE of the firmware (src/helpers/BaseSerialInterface.h, 176 since the region scoping codes; 172 before). */
constexpr size_t kBoardMaxFrame = 176;
/* The ATT default MTU (23): 20 bytes of payload per notification or write before the MTU exchange. */
constexpr int kDefaultMtu = 23;

/* One device BlueZ knows (from a scan or bonded earlier). */
struct BleDevice {
    std::string address;        // "AA:BB:CC:DD:EE:FF"
    std::string path;           // "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
    std::string name;           // Name, else Alias when it is not the address; "" when unknown
    std::string address_type;   // "public" / "random"
    bool has_rssi = false;      // RSSI is only valid while discovering
    int rssi = 0;
    bool nus = false;           // advertises (or has) the Nordic UART service
    bool paired = false;
    bool bonded = false;
    bool trusted = false;
    bool connected = false;
    bool services_resolved = false;
};

/* A device worth listing for Mesh Hop: it advertises the Nordic UART service or carries the MeshCore name prefix. */
bool is_candidate(const BleDevice &d);
/* "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF" -> "AA:BB:CC:DD:EE:FF"; "" when it is not a device path. */
std::string address_from_path(const std::string &path);
/* adapter path + address -> device path. */
std::string device_path(const std::string &adapter_path, const std::string &address);
/* True for "AA:BB:CC:DD:EE:FF" (hex digits, either case). */
bool valid_address(const std::string &address);

struct BleEvent {
    enum class Kind {
        Ready,                // the answer to start(): ok, or error ("BlueZ is not running", "no Bluetooth adapter")
        DeviceUpdated,        // dev: a device appeared or changed (name, RSSI, UUIDs, paired, connected ...)
        DeviceRemoved,        // address
        AdapterPowered,       // ok = powered
        ScanStopped,          // discovery ended (by us, BlueZ or the adapter); error when StartDiscovery failed
        ConnectResult,        // address, ok, error (Device1.Connect answered)
        PairResult,           // address, ok, error (Device1.Pair answered)
        PasskeyRequest,       // address (our agent was asked for the 6 digit passkey)
        PairingCancelled,     // address (Agent1.Cancel: BlueZ gave up waiting for the passkey)
        ConnectionChanged,    // address, ok = connected
        ServicesResolved,     // address, ok = resolved
        GattResult,           // ok, mtu; error; not_companion = no Nordic UART service on the device
        Notify,               // data: one notification of the TX characteristic
        NotifyClosed,         // the notify socket hung up (link lost)
        WriteResult,          // ok, error (one GATT write answered)
    };
    Kind kind = Kind::DeviceUpdated;
    std::string address;
    bool ok = false;
    bool not_companion = false;
    int mtu = 0;
    std::string error;          // D-Bus error name and text, e.g. "org.bluez.Error.AuthenticationFailed: ..."
    BleDevice dev;
    Bytes data;
};

/* What BlueZ does for the link. Every call returns at once; answers come back as events from poll(). */
class IBleBackend {
public:
    virtual ~IBleBackend() = default;

    /* Connects to the system bus and asks BlueZ for its objects; registers the agent (NOT as the default agent: the
     * launcher's stays the default, BlueZ asks ours only for the pairings our own Pair() call starts). False at once when
     * Bluetooth cannot be used here (no libsystemd, no system bus), why says it in plain words; else the Ready event
     * follows (BlueZ not running, no adapter). */
    virtual bool start(std::string &why) = 0;
    virtual void stop() = 0;
    virtual bool started() const = 0;           // start() succeeded and stop() was not called
    virtual bool ready() const = 0;             // Ready ok came: the adapter is known
    virtual bool adapter_powered() const = 0;

    /* Processes the bus and the notify socket without waiting; appends the events. */
    virtual void poll(std::vector<BleEvent> &out) = 0;

    /* Discovery of LE devices (no UUID filter in BlueZ: the MeshCore name prefix counts too, see is_candidate). */
    virtual void start_scan() = 0;
    virtual void stop_scan() = 0;
    /* Every device BlueZ knows now (cached, bonded or seen in the scan). */
    virtual std::vector<BleDevice> devices() const = 0;
    virtual bool find(const std::string &address, BleDevice &out) const = 0;

    virtual void connect(const std::string &address) = 0;          // Device1.Connect
    virtual void pair(const std::string &address) = 0;             // Device1.Pair, our agent answers the passkey
    virtual void answer_passkey(bool ok, uint32_t passkey) = 0;    // reply to the waiting PasskeyRequest (ok false: cancel)
    virtual void cancel_pairing(const std::string &address) = 0;   // Device1.CancelPairing
    virtual void disconnect(const std::string &address) = 0;       // Device1.Disconnect (the bond stays)

    /* Finds the NUS characteristics of the connected device and subscribes to the TX one (AcquireNotify, else StartNotify);
     * answers with GattResult. */
    virtual void open_gatt(const std::string &address) = 0;
    virtual void close_gatt() = 0;
    /* One GATT write (WriteValue, type "request") of one whole frame; answers with WriteResult. One at a time. */
    virtual void write(const Bytes &frame) = 0;
};

} // namespace meshzero::ble
