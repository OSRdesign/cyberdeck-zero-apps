/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: a Hi-Link HLK-LD radar over Bluetooth LE as a listen-only port (see ble_sys.hpp).
 */
#include "ble_sys.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace mmw {

namespace {

std::string upper(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string secs(Ms ms)
{
    const Ms s = (ms + 999) / 1000;
    return std::to_string(s < 0 ? 0 : s) + " s";
}

} // namespace

bool hlk_candidate(const std::string &name)
{
    const std::size_t n = std::strlen(kHlkNamePrefix);
    if (name.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (std::toupper(static_cast<unsigned char>(name[i])) != kHlkNamePrefix[i]) return false;
    return true;
}

std::string address_tail(const std::string &a)
{
    return valid_ble_address(a) ? upper(a.substr(12)) : std::string();
}

const char *ble_state_name(BleState s)
{
    switch (s) {
    case BleState::Off: return "off";
    case BleState::Starting: return "starting";
    case BleState::Unavailable: return "unavailable";
    case BleState::Idle: return "idle";
    case BleState::Scanning: return "scanning";
    case BleState::Connecting: return "connecting";
    case BleState::Subscribing: return "subscribing";
    case BleState::Connected: return "connected";
    case BleState::Waiting: return "waiting";
    }
    return "?";
}

Ms ble_backoff_ms(int attempt)
{
    static const Ms kSteps[] = {2000, 4000, 8000, 15000, 30000};
    if (attempt < 1) attempt = 1;
    const std::size_t i = static_cast<std::size_t>(attempt - 1);
    return i < sizeof(kSteps) / sizeof(kSteps[0]) ? kSteps[i] : 30000;
}

BleSys::BleSys(std::unique_ptr<BleBackend> backend, const BleTimings &timings) : be_(std::move(backend)), t_(timings)
{
    events_.reserve(16);
    if (be_) be_->set_sink(this);
}

BleSys::~BleSys() { shutdown(); }

void BleSys::set_state(BleState s, const std::string &why)
{
    if (s == st_ && why.empty()) return;
    st_ = s;
    log_ = std::string(ble_state_name(s)) + (target_addr_.empty() ? "" : " " + target_addr_) + (why.empty() ? "" : ": " + why);
    log_new_ = true;
}

bool BleSys::take_log(std::string &line)
{
    if (!log_new_) return false;
    log_new_ = false;
    line = log_;
    return true;
}

/* ------------------------------------------------------------------------------------------------ control */

void BleSys::shutdown()
{
    if (!be_ || !be_->started()) {
        enabled_ = false;
        return;
    }
    drop_link();
    stop_scan();
    be_->stop();
    enabled_ = false;
    st_ = BleState::Off;
}

void BleSys::set_enabled(bool on, Ms now)
{
    now_ = now;
    if (!be_ || on == enabled_) return;
    enabled_ = on;
    if (!on) {
        drop_link();
        stop_scan();
        if (be_->started()) be_->stop();
        avail_ = BleAvail::Unknown;
        attempt_ = 0;
        set_state(BleState::Off);
        return;
    }
    std::string why;
    if (!be_->started() && !be_->start(why)) {
        avail_ = BleAvail::NoBluez;
        last_error_ = why;
        set_state(BleState::Unavailable, why);
        return;
    }
    avail_ = be_->availability();
    if (avail_ == BleAvail::Unknown) {
        deadline_ = now + t_.start_ms;
        set_state(BleState::Starting);
        return;
    }
    after_ready(now);
}

void BleSys::set_active(bool on, Ms now)
{
    if (on == active_) return;
    active_ = on;
    now_ = now;
    if (!on && scanning_) { /* the radio is free while nobody looks */
        stop_scan();
        if (st_ == BleState::Scanning) set_state(BleState::Idle);
        searching_ = false;
    }
    if (on && st_ == BleState::Idle) next_window_ = std::min(next_window_, now); /* a fresh window when shown again */
}

void BleSys::set_remembered(const std::string &address, const std::string &name)
{
    rem_addr_ = upper(address);
    rem_name_ = name;
    if (target_addr_.empty() && valid_ble_address(rem_addr_)) {
        target_addr_ = rem_addr_;
        target_name_ = rem_name_;
    }
}

void BleSys::choose(const std::string &address, Ms now)
{
    now_ = now;
    const std::string a = upper(address);
    if (!enabled_ || !valid_ble_address(a)) return;
    if (st_ == BleState::Unavailable || st_ == BleState::Starting || st_ == BleState::Off) return;
    if (a == target_addr_ && (st_ == BleState::Connecting || st_ == BleState::Subscribing || st_ == BleState::Connected))
        return;
    drop_link();
    std::string name;
    for (const BleCandidate &c : seen_)
        if (c.address == a) name = c.name;
    target_addr_ = a;
    target_name_ = name;
    rem_addr_ = a;
    rem_name_ = name;
    chosen_ = true;
    attempt_ = 0;
    lost_ = false;
    last_error_.clear();
    begin_attempt(now);
}

bool BleSys::take_chosen(std::string &address, std::string &name)
{
    if (!chosen_) return false;
    chosen_ = false;
    address = rem_addr_;
    name = rem_name_;
    return true;
}

void BleSys::rescan(Ms now)
{
    if (!enabled_ || !be_) return;
    switch (st_) {
    case BleState::Unavailable:
        if (!be_->started()) {
            enabled_ = false;
            set_enabled(true, now);
        } else {
            avail_ = be_->availability();
            if (avail_ == BleAvail::Ok) after_ready(now);
        }
        break;
    case BleState::Waiting: retry_at_ = now; break;
    case BleState::Idle: begin_window(now, false); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ the machine */

void BleSys::poll(Ms now)
{
    now_ = now;
    if (!enabled_ || !be_ || !be_->started()) return;
    events_.clear();
    be_->poll(events_);
    for (const BleEvent &e : events_) handle(e, now);

    if (st_ == BleState::Starting && now >= deadline_) {
        avail_ = BleAvail::NoBluez;
        last_error_ = "BlueZ did not answer";
        set_state(BleState::Unavailable, last_error_);
    }
    if (scanning_ && now >= scan_until_) end_window(now);
    if ((st_ == BleState::Connecting || st_ == BleState::Subscribing) && now >= deadline_)
        fail_attempt(st_ == BleState::Connecting ? "connect timed out" : "the data characteristic did not open", now);
    if (st_ == BleState::Waiting && active_ && now >= retry_at_) begin_attempt(now);
    if (st_ == BleState::Idle && active_ && !scanning_ && now >= next_window_) {
        if (!target_addr_.empty() && valid_ble_address(target_addr_)) begin_attempt(now);
        else if (browse_) begin_window(now, false);
    }
}

void BleSys::after_ready(Ms now)
{
    avail_ = be_->availability();
    if (avail_ != BleAvail::Ok) {
        drop_link();
        scanning_ = false;
        last_error_ = be_->unavailable_why();
        set_state(BleState::Unavailable, last_error_);
        return;
    }
    if (st_ == BleState::Starting || st_ == BleState::Unavailable || st_ == BleState::Off) {
        next_window_ = now;
        if (!target_addr_.empty() && active_) begin_attempt(now);
        else set_state(BleState::Idle); /* poll() starts the attempt or the window once the page is shown */
    }
}

void BleSys::begin_attempt(Ms now)
{
    deadline_ = 0;
    connect_answered_ = false;
    if (target_addr_.empty()) {
        set_state(BleState::Idle);
        next_window_ = now;
        return;
    }
    BleDevice d;
    if (!be_->find(target_addr_, d)) { /* BlueZ does not know it (now): look for it */
        begin_window(now, true);
        return;
    }
    stop_scan(); /* never discover while connecting */
    if (!d.name.empty()) target_name_ = d.name;
    if (d.connected && d.services_resolved) {
        connect_answered_ = true;
        deadline_ = now + t_.subscribe_ms;
        set_state(BleState::Subscribing);
        be_->subscribe(target_addr_);
        return;
    }
    deadline_ = now + t_.connect_ms;
    set_state(BleState::Connecting);
    if (d.connected) connect_answered_ = true; /* wait for the services */
    else be_->connect(target_addr_);
}

void BleSys::begin_window(Ms now, bool searching)
{
    searching_ = searching;
    scan_until_ = now + t_.scan_window_ms;
    if (!scanning_) be_->start_scan();
    scanning_ = true;
    set_state(BleState::Scanning, searching ? "looking for " + shown_name() : std::string());
}

void BleSys::end_window(Ms now)
{
    stop_scan();
    const bool was_searching = searching_;
    searching_ = false;
    next_window_ = now + t_.scan_gap_ms;
    if (st_ != BleState::Scanning) return;
    if (was_searching) {
        last_error_ = shown_name() + " not seen";
        set_state(BleState::Idle, last_error_);
    } else {
        set_state(BleState::Idle);
    }
}

void BleSys::stop_scan()
{
    if (scanning_ && be_ && be_->started()) be_->stop_scan();
    scanning_ = false;
}

void BleSys::fail_attempt(const std::string &why, Ms now)
{
    drop_link();
    ++attempt_;
    retry_at_ = now + ble_backoff_ms(attempt_);
    last_error_ = why;
    set_state(BleState::Waiting, why + ", retry in " + secs(retry_at_ - now));
}

void BleSys::lose(const std::string &why, Ms now)
{
    const bool had_data = got_data_;
    drop_link();
    /* a link that streamed for a while starts the waits again at 2 s; a quick end (a verdict of the detector, a radar
     * that drops at once) keeps growing them, so a wrong Sensor type or a flaky module is not hammered */
    if (had_data && now - connected_at_ >= kStableMs) attempt_ = 0;
    lost_ = had_data;
    fail_attempt(why, now);
}

/* Ends what this process holds of the link: the port goes away (a reader gets -1), the subscription ends, the device is
 * disconnected (or its connection attempt cancelled). */
void BleSys::drop_link()
{
    const bool linked = st_ == BleState::Connecting || st_ == BleState::Subscribing || st_ == BleState::Connected;
    if (st_ == BleState::Connected) ++gen_;
    if (handle_ >= 0) handle_dead_ = true;
    got_data_ = false;
    count_ = 0;
    head_ = 0;
    deadline_ = 0;
    if (!be_ || !be_->started() || target_addr_.empty()) return;
    if (st_ == BleState::Subscribing || st_ == BleState::Connected) be_->unsubscribe();
    BleDevice d;
    if (linked || (be_->find(target_addr_, d) && d.connected)) be_->disconnect(target_addr_);
    if (linked) st_ = BleState::Idle;
}

void BleSys::note_device(const std::string &address)
{
    BleDevice d;
    if (!be_->find(address, d) || !hlk_candidate(d.name)) return;
    auto it = std::find_if(seen_.begin(), seen_.end(), [&](const BleCandidate &c) { return c.address == d.address; });
    if (it == seen_.end()) {
        if (!d.has_rssi) return; /* listed when seen in a window of ours, not from BlueZ's cache */
        seen_.push_back(BleCandidate{upper(d.address), d.name, d.has_rssi, d.rssi});
        return;
    }
    it->name = d.name;
    if (d.has_rssi) {
        it->has_rssi = true;
        it->rssi = d.rssi;
    }
}

void BleSys::handle(const BleEvent &e, Ms now)
{
    using K = BleEvent::Kind;
    const bool target = !target_addr_.empty() && upper(e.address) == target_addr_;
    switch (e.kind) {
    case K::Ready:
    case K::AdapterChanged: {
        const BleAvail a = be_->availability();
        if (a == BleAvail::Ok) {
            if (st_ == BleState::Starting || st_ == BleState::Unavailable) after_ready(now);
            avail_ = a;
        } else if (a != BleAvail::Unknown) {
            if (st_ == BleState::Connected) lose("Bluetooth went away", now);
            after_ready(now);
        }
        break;
    }
    case K::DeviceUpdated:
        if (scanning_) note_device(e.address);
        if (target && st_ == BleState::Scanning && searching_) {
            BleDevice d;
            if (be_->find(target_addr_, d)) {
                searching_ = false;
                begin_attempt(now);
            }
        }
        break;
    case K::DeviceRemoved:
        if (target && st_ == BleState::Connected) lose("the device went away", now);
        break;
    case K::ScanStopped:
        if (!scanning_) break;
        scanning_ = false;
        if (st_ == BleState::Scanning) {
            if (!e.error.empty()) last_error_ = e.error;
            end_window(now);
        }
        break;
    case K::ConnectResult:
        if (!target || st_ != BleState::Connecting) break;
        if (e.ok || e.error.find("AlreadyConnected") != std::string::npos) {
            connect_answered_ = true;
            BleDevice d;
            if (be_->find(target_addr_, d) && d.services_resolved) {
                deadline_ = now + t_.subscribe_ms;
                set_state(BleState::Subscribing);
                be_->subscribe(target_addr_);
            }
        } else {
            fail_attempt(e.error.empty() ? "could not connect" : e.error, now);
        }
        break;
    case K::ServicesResolved:
        if (target && e.ok && st_ == BleState::Connecting && connect_answered_) {
            deadline_ = now + t_.subscribe_ms;
            set_state(BleState::Subscribing);
            be_->subscribe(target_addr_);
        }
        break;
    case K::ConnectionChanged:
        if (!target || e.ok) break;
        if (st_ == BleState::Connected) lose("disconnected", now);
        else if ((st_ == BleState::Connecting && connect_answered_) || st_ == BleState::Subscribing)
            fail_attempt("disconnected while connecting", now);
        break;
    case K::SubscribeResult:
        if (st_ != BleState::Subscribing || (!e.address.empty() && !target)) break;
        if (e.ok) {
            deadline_ = 0;
            count_ = head_ = 0;
            got_data_ = false;
            lost_ = false;
            connected_at_ = now;
            ++gen_; /* the port is listed now */
            set_state(BleState::Connected, be_->describe());
        } else {
            fail_attempt(e.error.empty() ? "no data characteristic" : e.error, now);
        }
        break;
    case K::NotifyClosed:
        if (st_ == BleState::Connected) lose("the link closed", now);
        else if (st_ == BleState::Subscribing) fail_attempt("the link closed", now);
        break;
    }
}

void BleSys::on_notify(const std::uint8_t *data, std::size_t n)
{
    if (st_ != BleState::Connected || n == 0) return;
    ++notifications_;
    got_data_ = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (count_ == kRing) { /* nobody reads: the oldest byte goes */
            head_ = (head_ + 1) % kRing;
            --count_;
            ++dropped_;
        }
        ring_[(head_ + count_) % kRing] = data[i];
        ++count_;
    }
}

/* ------------------------------------------------------------------------------------------------ texts */

std::string BleSys::shown_name() const
{
    if (!target_name_.empty()) return target_name_;
    return target_addr_.empty() ? std::string("the radar") : target_addr_;
}

std::vector<BleCandidate> BleSys::candidates() const
{
    std::vector<BleCandidate> v = seen_;
    std::stable_sort(v.begin(), v.end(), [](const BleCandidate &a, const BleCandidate &b) {
        if (a.has_rssi != b.has_rssi) return a.has_rssi;
        if (a.has_rssi && a.rssi != b.rssi) return a.rssi > b.rssi;
        return a.name < b.name;
    });
    return v;
}

std::string BleSys::title() const
{
    switch (st_) {
    case BleState::Off: return std::string();
    case BleState::Starting: return "Searching for radars over Bluetooth...";
    case BleState::Unavailable:
        return avail_ == BleAvail::Off ? "Bluetooth is off" : avail_ == BleAvail::NoAdapter ? "No Bluetooth adapter"
                                                                                             : "BlueZ not available";
    case BleState::Idle: return seen_.empty() ? "No radar found over Bluetooth" : "Choose a radar";
    case BleState::Scanning:
        return searching_ ? "Looking for " + shown_name() : std::string("Searching for radars over Bluetooth...");
    case BleState::Connecting:
    case BleState::Subscribing: return "Connecting to " + shown_name();
    case BleState::Connected: return "Connected to " + shown_name();
    case BleState::Waiting: return lost_ ? std::string("Signal lost, reconnecting") : "Could not connect to " + shown_name();
    }
    return std::string();
}

std::string BleSys::detail(Ms now) const
{
    switch (st_) {
    case BleState::Scanning: return "Scanning for " + secs(scan_until_ - now) + ": HLK-LD radars only.";
    case BleState::Idle:
        if (!active_) return std::string();
        if (!target_addr_.empty()) return "Looking again in " + secs(next_window_ - now) + ".";
        return seen_.empty() ? "Searching again in " + secs(next_window_ - now) + ". Is the radar powered?"
                             : std::string("Tap a radar to connect. Searching again in ") + secs(next_window_ - now) + ".";
    case BleState::Connecting: return "Waiting up to " + secs(deadline_ - now) + ".";
    case BleState::Subscribing: return "Turning its data on.";
    case BleState::Connected: return "Listening: the app sends nothing to the radar.";
    case BleState::Waiting:
        if (!active_) return "Paused while the Radar page is hidden.";
        return "Trying again in " + secs(retry_at_ - now) + (last_error_.empty() ? std::string(".") : " (" + last_error_ + ").");
    default: return std::string();
    }
}

/* ------------------------------------------------------------------------------------------------ Sys */

std::vector<PortMeta> BleSys::list_ports()
{
    std::vector<PortMeta> v;
    if (st_ != BleState::Connected) return v;
    PortMeta m;
    m.tty = target_name_.empty() ? target_addr_ : target_name_;
    m.path = "ble:" + target_addr_;
    m.product = target_name_;
    m.driver = "bluez";
    m.transport = Transport::Ble;
    m.address = target_addr_;
    v.push_back(m);
    return v;
}

bool BleSys::held_by_other(const std::string &) { return false; }

int BleSys::open(const std::string &path, OpenErr &err)
{
    err = OpenErr::None;
    if (st_ != BleState::Connected || path != "ble:" + target_addr_) {
        err = OpenErr::Gone;
        return -1;
    }
    if (handle_ >= 0 && !handle_dead_) {
        err = OpenErr::Busy;
        return -1;
    }
    handle_ = kBleHandleBase + static_cast<int>(++handle_seq_ & 0xFFFF);
    handle_dead_ = false;
    return handle_;
}

bool BleSys::configure(int handle, int) { return handle == handle_ && !handle_dead_; }

void BleSys::flush_input(int) {}

long BleSys::read(int handle, std::uint8_t *buf, std::size_t n)
{
    if (handle != handle_ || handle_ < 0 || handle_dead_) return -1;
    std::size_t k = 0;
    while (k < n && count_ > 0) {
        buf[k++] = ring_[head_];
        head_ = (head_ + 1) % kRing;
        --count_;
    }
    return static_cast<long>(k);
}

void BleSys::restore(int) {}

/* The reader let go (a verdict of the detector, the link's Lost, the app ending): the link is not needed any more. */
void BleSys::close(int handle)
{
    if (handle != handle_ || handle_ < 0) return;
    const bool dead = handle_dead_;
    handle_ = -1;
    handle_dead_ = false;
    if (!dead && st_ == BleState::Connected) lose("released by the reader", now_);
}

std::string BleSys::describe(int handle)
{
    if (handle != handle_ || handle_ < 0) return std::string();
    std::string s = "transport=ble addr=" + target_addr_ + " name=" + (target_name_.empty() ? "-" : target_name_);
    const std::string b = be_ ? be_->describe() : std::string();
    if (!b.empty()) s += " " + b;
    return s;
}

} // namespace mmw
