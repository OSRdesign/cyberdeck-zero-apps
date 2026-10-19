/*
 * SPDX-License-Identifier: MIT
 */

#include "ble_link.hpp"

#include <algorithm>
#include <cctype>

namespace meshzero::ble {

namespace {

bool contains(const std::string &s, const char *what)
{
    return s.find(what) != std::string::npos;
}

/* BlueZ / kernel words for an encryption or key failure (inferred: what a bonded board that lost its keys gives). */
bool auth_error(const std::string &e)
{
    return contains(e, "Authentication") || contains(e, "Key Missing") || contains(e, "key missing") ||
           contains(e, "Insufficient") || contains(e, "PIN or Key");
}

std::string upper(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

const char *state_name(BleState s)
{
    switch (s) {
    case BleState::Idle: return "idle";
    case BleState::Scanning: return "scanning";
    case BleState::Connecting: return "connecting";
    case BleState::Pairing: return "pairing";
    case BleState::Connected: return "connected";
    case BleState::Reconnecting: return "reconnecting";
    case BleState::Failed: return "failed";
    }
    return "";
}

const char *fail_text(BleFail f)
{
    switch (f) {
    case BleFail::None: return "";
    case BleFail::NoBluetooth: return "Bluetooth is not available";
    case BleFail::AdapterOff: return "Bluetooth is off";
    case BleFail::NotFound: return "Board not found";
    case BleFail::PairingFailed: return "Pairing failed (wrong PIN?)";
    case BleFail::PairingCancelled: return "Pairing cancelled";
    case BleFail::NotCompanion: return "Not a MeshCore companion (no Nordic UART service)";
    case BleFail::BondLost: return "The board forgot the pairing: forget it and pair again";
    }
    return "";
}

BleLink::BleLink(IBleBackend &backend, BleTimings timings) : be_(backend), t_(timings) {}

void BleLink::log(const std::string &line) const
{
    if (log_) log_("ble: " + line);
}

void BleLink::set_state(BleState s, const std::string &detail)
{
    if (st_.state != s || st_.detail != detail)
        log(std::string(state_name(s)) + (st_.address.empty() ? "" : " " + st_.address) + (detail.empty() ? "" : ": " + detail));
    st_.state = s;
    st_.detail = detail;
    if (s != BleState::Pairing) st_.pin_needed = false;
    if (s != BleState::Failed) st_.fail = BleFail::None;
}

bool BleLink::ensure_backend(std::string &why)
{
    if (be_.started()) return true;
    if (be_.start(why)) return true;
    unavailable_ = why;
    return false;
}

uint64_t BleLink::backoff(int attempt) const
{
    uint64_t d = t_.backoff_first_ms;
    for (int i = 1; i < attempt && d < t_.backoff_max_ms; ++i) d *= 2;
    return std::min(d, t_.backoff_max_ms);
}

/* ------------------------------------------------------------------ the device list */

namespace {
void update_scan(IBleBackend &be, BleStatus &st, bool wanted)
{
    if (!be.started()) {
        st.scanning = false;
        return;
    }
    if (wanted && !st.scanning) be.start_scan();
    if (!wanted && st.scanning) be.stop_scan();
    st.scanning = wanted;
}
} // namespace

bool BleLink::start_scan(uint64_t now_ms, std::string *why)
{
    now_ = now_ms;
    std::string w;
    if (!ensure_backend(w)) {
        if (why) *why = w;
        log("scan: " + w);
        return false;
    }
    list_scan_until_ = now_ + t_.list_scan_ms;
    update_scan(be_, st_, true);
    return true;
}

void BleLink::stop_scan()
{
    list_scan_until_ = 0;
    update_scan(be_, st_, searching_);
}

std::vector<BleDevice> BleLink::candidates() const
{
    std::vector<BleDevice> out;
    if (!be_.started()) return out;
    for (const BleDevice &d : be_.devices())
        if (is_candidate(d)) out.push_back(d);
    std::stable_sort(out.begin(), out.end(), [](const BleDevice &a, const BleDevice &b) {
        const bool pa = a.paired || a.bonded, pb = b.paired || b.bonded;
        if (pa != pb) return pa;
        if (a.has_rssi != b.has_rssi) return a.has_rssi;
        if (a.has_rssi && a.rssi != b.rssi) return a.rssi > b.rssi;
        return a.name < b.name;
    });
    return out;
}

/* ------------------------------------------------------------------ the link */

void BleLink::connect(const std::string &address, uint64_t now_ms)
{
    now_ = now_ms;
    if (st_.state != BleState::Idle) disconnect();
    const bool scanning = st_.scanning;
    st_ = BleStatus{};
    st_.scanning = scanning;
    st_.address = upper(address);
    last_failure_.clear();
    ever_connected_ = false;
    bonded_target_ = false;
    auth_errors_ = 0;
    std::string why;
    if (!valid_address(st_.address)) {
        fail(BleFail::NotFound, "not a Bluetooth address: " + address);
        return;
    }
    if (!ensure_backend(why)) {
        fail(BleFail::NoBluetooth, why);
        return;
    }
    if (!be_.ready()) {
        phase_ = Phase::Start;
        deadline_ = now_ + 10000;
        set_state(BleState::Connecting, "starting Bluetooth");
        return;
    }
    begin_attempt();
}

void BleLink::begin_attempt()
{
    phase_ = Phase::None;
    deadline_ = 0;
    if (!be_.adapter_powered()) {
        if (ever_connected_) attempt_failed("Bluetooth is off");
        else fail(BleFail::AdapterOff, "the Bluetooth adapter is off");
        return;
    }
    BleDevice d;
    if (!be_.find(st_.address, d)) {
        searching_ = true;
        update_scan(be_, st_, true);
        deadline_ = now_ + t_.search_ms;
        set_state(BleState::Scanning, "looking for the board");
        return;
    }
    if (!d.name.empty()) st_.name = d.name;
    bonded_target_ = d.paired || d.bonded;
    if (!bonded_target_) {
        deadline_ = now_ + t_.pair_ms;
        set_state(BleState::Pairing, "pairing");
        be_.pair(st_.address);
        return;
    }
    if (d.connected && d.services_resolved) go_connect_phase(Phase::Gatt);
    else if (d.connected) go_connect_phase(Phase::Services);
    else {
        go_connect_phase(Phase::Connect);
        be_.connect(st_.address);
    }
}

void BleLink::go_connect_phase(Phase p)
{
    phase_ = p;
    switch (p) {
    case Phase::Connect:
        deadline_ = now_ + t_.connect_ms;
        set_state(BleState::Connecting, "connecting");
        break;
    case Phase::Services:
        deadline_ = now_ + t_.services_ms;
        set_state(BleState::Connecting, "reading the services");
        break;
    case Phase::Gatt:
        deadline_ = now_ + t_.gatt_ms;
        set_state(BleState::Connecting, "opening the Nordic UART service");
        be_.open_gatt(st_.address);
        break;
    default: break;
    }
}

void BleLink::attempt_failed(const std::string &why)
{
    phase_ = Phase::None;
    deadline_ = 0;
    searching_ = false;
    update_scan(be_, st_, list_scan_until_ != 0);
    if (st_.pin_needed) be_.answer_passkey(false, 0);
    be_.close_gatt();
    BleDevice d;
    if (be_.find(st_.address, d) && d.connected) be_.disconnect(st_.address);     // a half-open link keeps the board busy
    ++st_.attempt;
    st_.retry_at_ms = now_ + backoff(st_.attempt);
    st_.mtu = 0;
    last_failure_ = why;
    set_state(BleState::Reconnecting, why);
}

void BleLink::link_lost(const std::string &why)
{
    ++generation_;
    tx_.clear();
    write_inflight_ = false;
    rx_.clear();
    assembler_.reset();
    st_.attempt = 0;
    attempt_failed(why);
}

void BleLink::fail(BleFail f, const std::string &why)
{
    phase_ = Phase::None;
    deadline_ = 0;
    searching_ = false;
    update_scan(be_, st_, list_scan_until_ != 0);
    if (st_.pin_needed) be_.answer_passkey(false, 0);
    if (be_.started()) {
        be_.close_gatt();
        BleDevice d;
        if (!st_.address.empty() && be_.find(st_.address, d) && d.connected) be_.disconnect(st_.address);
    }
    st_.mtu = 0;
    last_failure_ = why;
    set_state(BleState::Failed, why);
    st_.fail = f;
}

void BleLink::submit_pin(uint32_t pin)
{
    if (!st_.pin_needed || st_.state != BleState::Pairing) return;
    st_.pin_needed = false;
    be_.answer_passkey(true, pin % 1000000);
    st_.detail = "checking the PIN";
    log("PIN sent");
}

void BleLink::cancel_pairing()
{
    if (st_.state != BleState::Pairing) return;
    if (st_.pin_needed) be_.answer_passkey(false, 0);
    st_.pin_needed = false;
    be_.cancel_pairing(st_.address);
    fail(BleFail::PairingCancelled, "pairing cancelled");
}

void BleLink::disconnect()
{
    if (st_.state == BleState::Idle) return;
    if (be_.started() && !st_.address.empty()) {
        if (st_.state == BleState::Pairing) {
            if (st_.pin_needed) be_.answer_passkey(false, 0);
            be_.cancel_pairing(st_.address);
        }
        be_.close_gatt();
        BleDevice d;
        const bool linked = st_.state == BleState::Connected || st_.state == BleState::Connecting;
        if ((be_.find(st_.address, d) && d.connected) || linked) be_.disconnect(st_.address);
    }
    if (st_.state == BleState::Connected) ++generation_;
    tx_.clear();
    write_inflight_ = false;
    rx_.clear();
    assembler_.reset();
    searching_ = false;
    update_scan(be_, st_, list_scan_until_ != 0);
    phase_ = Phase::None;
    deadline_ = 0;
    st_.attempt = 0;
    st_.mtu = 0;
    set_state(BleState::Idle, "");
}

void BleLink::forget(const std::string &address)
{
    const std::string a = upper(address);
    if (!valid_address(a)) return;
    if (a == st_.address) disconnect();
    std::string why;
    if (!ensure_backend(why)) return;
    be_.remove_device(a);
    log("forgot " + a);
}

/* ------------------------------------------------------------------ events */

void BleLink::handle(const BleEvent &e)
{
    using K = BleEvent::Kind;
    switch (e.kind) {
    case K::Ready:
        unavailable_ = e.ok ? std::string() : e.error;
        if (!e.ok) {
            list_scan_until_ = 0;
            st_.scanning = false;
        }
        if (st_.state == BleState::Connecting && phase_ == Phase::Start) {
            if (e.ok) begin_attempt();
            else fail(BleFail::NoBluetooth, e.error);
        }
        break;
    case K::DeviceUpdated:
        if (!target(e)) break;
        if (!e.dev.name.empty()) st_.name = e.dev.name;
        if (searching_ && st_.state == BleState::Scanning) {
            searching_ = false;
            update_scan(be_, st_, list_scan_until_ != 0);
            begin_attempt();
        }
        break;
    case K::DeviceRemoved:
        if (target(e) && st_.state == BleState::Connected) link_lost("the device was removed from BlueZ");
        break;
    case K::AdapterPowered:
        if (!e.ok && st_.state == BleState::Connected) link_lost("Bluetooth was switched off");
        break;
    case K::ScanStopped:
        st_.scanning = false;
        list_scan_until_ = 0;
        if (searching_ && st_.state == BleState::Scanning) {
            searching_ = false;
            const std::string why = e.error.empty() ? "the scan stopped" : e.error;
            if (ever_connected_) attempt_failed(why);
            else fail(BleFail::NotFound, why);
        }
        break;
    case K::ConnectResult:
        if (!target(e) || st_.state != BleState::Connecting || phase_ != Phase::Connect) break;
        if (e.ok || contains(e.error, "AlreadyConnected")) {
            BleDevice d;
            go_connect_phase(be_.find(st_.address, d) && d.services_resolved ? Phase::Gatt : Phase::Services);
        } else {
            if (bonded_target_ && auth_error(e.error) && ++auth_errors_ >= t_.bond_lost_after) fail(BleFail::BondLost, e.error);
            else attempt_failed(e.error);
        }
        break;
    case K::PairResult:
        if (!target(e) || st_.state != BleState::Pairing) break;
        if (e.ok || contains(e.error, "AlreadyExists")) {
            bonded_target_ = true;
            auth_errors_ = 0;
            BleDevice d;
            const bool known = be_.find(st_.address, d);
            if (known && d.connected && d.services_resolved) go_connect_phase(Phase::Gatt);
            else if (known && d.connected) go_connect_phase(Phase::Services);
            else {
                go_connect_phase(Phase::Connect);
                be_.connect(st_.address);
            }
        } else if (contains(e.error, "AuthenticationCanceled")) {
            fail(BleFail::PairingCancelled, e.error);
        } else if (contains(e.error, "AuthenticationFailed") || contains(e.error, "AuthenticationRejected") ||
                   contains(e.error, "AuthenticationTimeout")) {
            fail(BleFail::PairingFailed, e.error);
        } else {
            attempt_failed(e.error);            // out of reach, page timeout ...: try again (the PIN is asked again)
        }
        break;
    case K::PasskeyRequest:
        if (target(e) && st_.state == BleState::Pairing) {
            st_.pin_needed = true;
            st_.detail = "type the board's PIN";
            deadline_ = now_ + t_.pair_ms;
            log("PIN needed for " + st_.address);
        } else {
            be_.answer_passkey(false, 0);       // not a pairing of ours
        }
        break;
    case K::PairingCancelled:
        if (target(e) && st_.state == BleState::Pairing) {
            st_.pin_needed = false;
            st_.detail = "the board stopped waiting for the PIN";
            log(st_.detail);
        }
        break;
    case K::ConnectionChanged:
        if (!target(e) || e.ok) break;
        if (st_.state == BleState::Connected) link_lost("disconnected");
        else if (st_.state == BleState::Connecting && (phase_ == Phase::Services || phase_ == Phase::Gatt)) {
            if (bonded_target_ && auth_error(e.error) && ++auth_errors_ >= t_.bond_lost_after) fail(BleFail::BondLost, e.error);
            else attempt_failed("disconnected while connecting");
        }
        break;
    case K::ServicesResolved:
        if (target(e) && e.ok && st_.state == BleState::Connecting && phase_ == Phase::Services) go_connect_phase(Phase::Gatt);
        break;
    case K::GattResult:
        if (st_.state != BleState::Connecting || phase_ != Phase::Gatt) break;
        if (e.ok) {
            phase_ = Phase::None;
            deadline_ = 0;
            st_.mtu = e.mtu > 0 ? e.mtu : kDefaultMtu;
            assembler_.set_mtu(st_.mtu);
            assembler_.reset();
            st_.attempt = 0;
            ever_connected_ = true;
            auth_errors_ = 0;
            ++stats_.connects;
            last_failure_.clear();
            set_state(BleState::Connected, "MTU " + std::to_string(st_.mtu));
            pump_writes();
        } else if (e.not_companion) {
            fail(BleFail::NotCompanion, e.error);
        } else if (bonded_target_ && auth_error(e.error) && ++auth_errors_ >= t_.bond_lost_after) {
            fail(BleFail::BondLost, e.error);
        } else {
            attempt_failed(e.error.empty() ? "the Nordic UART service did not open" : e.error);
        }
        break;
    case K::Notify:
        if (st_.state == BleState::Connected) {
            std::vector<Bytes> frames;
            assembler_.feed(e.data.data(), e.data.size(), now_, frames);
            for (Bytes &f : frames) {
                if (rx_.size() >= 512) rx_.pop_front();
                rx_.push_back(std::move(f));
                ++stats_.frames_in;
            }
            stats_.joined = assembler_.joined();
        }
        break;
    case K::NotifyClosed:
        if (st_.state == BleState::Connected) link_lost("the link closed");
        else if (st_.state == BleState::Connecting && phase_ == Phase::Gatt) attempt_failed("the link closed");
        break;
    case K::WriteResult:
        if (!write_inflight_) break;
        write_inflight_ = false;
        if (!e.ok) {
            ++stats_.write_errors;
            log("write: " + e.error);
        }
        pump_writes();
        break;
    }
}

void BleLink::tick(uint64_t now_ms)
{
    now_ = now_ms;
    std::vector<BleEvent> events;
    if (be_.started()) be_.poll(events);
    for (const BleEvent &e : events) handle(e);

    if (st_.state == BleState::Connected) {
        std::vector<Bytes> frames;
        assembler_.tick(now_, frames);
        for (Bytes &f : frames) {
            rx_.push_back(std::move(f));
            ++stats_.frames_in;
        }
        stats_.joined = assembler_.joined();
    }
    if (list_scan_until_ && now_ >= list_scan_until_) {
        list_scan_until_ = 0;
        update_scan(be_, st_, searching_);
    }
    if (deadline_ && now_ >= deadline_) {
        deadline_ = 0;
        switch (st_.state) {
        case BleState::Scanning:
            searching_ = false;
            update_scan(be_, st_, list_scan_until_ != 0);
            if (ever_connected_) attempt_failed("not seen");
            else fail(BleFail::NotFound, "not seen in " + std::to_string(t_.search_ms / 1000) + " s");
            break;
        case BleState::Pairing:
            if (st_.pin_needed) be_.answer_passkey(false, 0);
            st_.pin_needed = false;
            be_.cancel_pairing(st_.address);
            fail(BleFail::PairingFailed, "the pairing did not finish in time");
            break;
        case BleState::Connecting:
            if (phase_ == Phase::Start) fail(BleFail::NoBluetooth, "BlueZ did not answer");
            else attempt_failed(phase_ == Phase::Connect ? "connect timed out" : phase_ == Phase::Services ? "the services did not resolve"
                                                                                                       : "the Nordic UART service did not open");
            break;
        default: break;
        }
    }
    if (st_.state == BleState::Reconnecting && now_ >= st_.retry_at_ms) begin_attempt();
    if (st_.state == BleState::Connected && write_inflight_ && now_ >= write_deadline_) {
        ++stats_.write_errors;
        link_lost("a write got no answer in " + std::to_string(t_.write_ms / 1000) + " s");
    }
}

/* ------------------------------------------------------------------ data */

bool BleLink::send(const Bytes &frame)
{
    if (!connected()) return false;
    switch (plan_write(frame.size(), st_.mtu)) {
    case WritePlan::TooBig: ++stats_.refused; return false;
    case WritePlan::Long: ++stats_.long_writes; break;
    case WritePlan::Single: break;
    }
    if (tx_.size() >= 32) {
        ++stats_.refused;
        return false;
    }
    tx_.push_back(frame);
    pump_writes();
    return true;
}

void BleLink::pump_writes()
{
    if (!connected() || write_inflight_ || tx_.empty()) return;
    be_.write(tx_.front());
    tx_.pop_front();
    write_inflight_ = true;
    write_deadline_ = now_ + t_.write_ms;
    ++stats_.frames_out;
}

bool BleLink::receive(Bytes &frame)
{
    if (rx_.empty()) return false;
    frame = std::move(rx_.front());
    rx_.pop_front();
    return true;
}

} // namespace meshzero::ble
