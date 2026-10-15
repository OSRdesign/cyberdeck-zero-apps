/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the BLE scan (see ble.hpp).
 */
#include "ble.hpp"

#include "text.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace nt {
namespace {

bool valid_bdaddr(const std::string &s)
{
    if (s.size() != 17) return false;
    for (std::size_t i = 0; i < 17; ++i) {
        const char c = s[i];
        if (i % 3 == 2) {
            if (c != ':') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string upper(std::string s)
{
    for (char &c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

/* "0xffffffb8 (-72)" -> -72; "-72" -> -72; "0x004c (76)" -> 76; "0x004c" -> 76 */
bool bt_number(const std::string &text, int &out)
{
    const std::string t = trim(text);
    const std::size_t open = t.find('('), close = t.find(')');
    if (open != std::string::npos && close != std::string::npos && close > open)
        return parse_int(t.substr(open + 1, close - open - 1), out);
    const std::string first = split_ws(t).empty() ? std::string() : split_ws(t)[0];
    if (starts_with(first, "0x") || starts_with(first, "0X")) {
        std::uint32_t v = 0;
        if (!parse_hex(first, v) || v > 0x7FFFFFFF) return false;
        out = static_cast<int>(v);
        return true;
    }
    return parse_int(first, out);
}

/* a name for the table: printable, bounded */
std::string bt_text(const std::string &s)
{
    std::string t = trim(s);
    if (t.size() > 64) {
        std::size_t cut = 64;
        while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut; /* never in a UTF-8 sequence */
        t.resize(cut);
    }
    return printable(t);
}

bool after(const std::string &s, const std::string &prefix, std::string &rest)
{
    if (!starts_with(s, prefix)) return false;
    rest = s.substr(prefix.size());
    return true;
}

} // namespace

/* ---------------------------------------------------------------- bluetoothctl's output */

std::string clean_btctl_line(const std::string &raw)
{
    std::string s = raw;
    const std::size_t cr = s.rfind('\r');
    if (cr != std::string::npos) s.erase(0, cr + 1);
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1B) { /* ESC [ params final, or ESC and one more byte */
            if (i + 1 < s.size() && s[i + 1] == '[') {
                i += 2;
                while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
            } else {
                ++i;
            }
            continue;
        }
        if (c == 0x08) {
            if (!out.empty()) out.pop_back();
            continue;
        }
        if (c < 0x20 && c != '\t') continue;
        if (c == 0x7F) continue;
        out += static_cast<char>(c);
    }
    if (!out.empty() && out[0] == '[') { /* the prompt: "[bluetoothctl]> ", "[PI3A]> ", old "[bluetooth]# " */
        const std::size_t close = out.find(']');
        const bool mark = close != std::string::npos && close + 1 < out.size() &&
                          (out[close + 1] == '>' || out[close + 1] == '#');
        if (mark && (close + 2 == out.size() || out[close + 2] == ' ')) out.erase(0, std::min(out.size(), close + 3));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return out;
}

bool parse_btctl_line(const std::string &line, BtEvent &out)
{
    out = BtEvent();
    if (line.empty()) return false;
    std::string rest;
    if (line[0] == '\t') { /* an `info` field */
        const std::size_t colon = line.find(": ");
        const std::size_t bare = line.find(':');
        if (colon == std::string::npos && (bare == std::string::npos || bare + 1 != line.size())) return false;
        out.kind = BtEvent::Kind::InfoField;
        const std::size_t at = colon != std::string::npos ? colon : bare;
        out.key = trim(line.substr(1, at - 1));
        out.text = colon != std::string::npos ? trim(line.substr(colon + 2)) : std::string();
        return !out.key.empty();
    }
    if (line.size() > 6 && line[0] == '[' && line[4] == ']' && line[5] == ' ') {
        const std::string tag = line.substr(1, 3);
        if (tag != "NEW" && tag != "DEL" && tag != "CHG") return false;
        const std::string body = line.substr(6);
        if (after(body, "Controller ", rest)) {
            const std::size_t sp = rest.find(' ');
            if (sp == std::string::npos) return false;
            const std::string what = rest.substr(sp + 1);
            if (what == "Discovering: yes" || what == "Discovering: no") {
                out.kind = BtEvent::Kind::Discovering;
                out.address = upper(rest.substr(0, sp));
                out.value = what == "Discovering: yes" ? 1 : 0;
                return true;
            }
            return false;
        }
        if (!after(body, "Device ", rest) || rest.size() < 17 || !valid_bdaddr(rest.substr(0, 17))) return false;
        out.address = upper(rest.substr(0, 17));
        const std::string what = rest.size() > 18 ? rest.substr(18) : std::string();
        if (tag == "NEW" || tag == "DEL") {
            out.kind = tag == "NEW" ? BtEvent::Kind::NewDevice : BtEvent::Kind::DelDevice;
            out.text = bt_text(what);
            return true;
        }
        std::string v;
        int n = 0;
        if (what == "RSSI is nil") {
            out.kind = BtEvent::Kind::RssiGone;
            return true;
        }
        if (after(what, "RSSI: ", v)) {
            if (!bt_number(v, n) || n < -127 || n > 20) return false;
            out.kind = BtEvent::Kind::Rssi;
            out.value = n;
            return true;
        }
        if (after(what, "TxPower: ", v)) {
            if (!bt_number(v, n) || n < -127 || n > 127) return false;
            out.kind = BtEvent::Kind::TxPower;
            out.value = n;
            return true;
        }
        if (after(what, "Name: ", v)) {
            out.kind = BtEvent::Kind::Name;
            out.text = bt_text(v);
            return true;
        }
        if (after(what, "Alias: ", v)) {
            out.kind = BtEvent::Kind::Alias;
            out.text = bt_text(v);
            return true;
        }
        if (after(what, "ManufacturerData.Key: ", v) || after(what, "ManufacturerData Key: ", v)) {
            if (!bt_number(v, n) || n < 0 || n > 0xFFFF) return false;
            out.kind = BtEvent::Kind::Company;
            out.value = n;
            return true;
        }
        return false;
    }
    if (line == "Discovery started" || line == "Discovery stopped") {
        out.kind = BtEvent::Kind::Discovering;
        out.value = line == "Discovery started" ? 1 : 0;
        return true;
    }
    if (after(line, "Failed to start discovery: ", rest) || after(line, "SetDiscoveryFilter failed: ", rest)) {
        out.kind = BtEvent::Kind::Failed;
        out.text = bt_text(rest);
        return true;
    }
    if (starts_with(line, "No default controller available")) {
        out.kind = BtEvent::Kind::NoController;
        return true;
    }
    if (starts_with(line, "Waiting to connect to bluetoothd")) {
        out.kind = BtEvent::Kind::WaitingDaemon;
        return true;
    }
    if (after(line, "Device ", rest) && rest.size() >= 17 && valid_bdaddr(rest.substr(0, 17))) {
        out.address = upper(rest.substr(0, 17));
        const std::string what = trim(rest.substr(17));
        if (what == "not available") {
            out.kind = BtEvent::Kind::InfoMissing;
            return true;
        }
        if (what.size() > 2 && what.front() == '(' && what.back() == ')') {
            out.kind = BtEvent::Kind::InfoHeader;
            out.text = bt_text(what.substr(1, what.size() - 2));
            return true;
        }
    }
    return false;
}

std::string bt_failure_words(const std::string &error)
{
    const std::string e = lower(error);
    if (e.find("rfkill") != std::string::npos || e.find("blocked") != std::string::npos)
        return "Bluetooth is switched off (rfkill block).";
    if (e.find("notready") != std::string::npos) return "Bluetooth is not ready: the adapter is powered off.";
    if (e.find("inprogress") != std::string::npos) return "A scan is already starting. Try again in a moment.";
    if (e.find("notauthorized") != std::string::npos || e.find("accessdenied") != std::string::npos)
        return "This user may not scan for Bluetooth devices (D-Bus policy).";
    return "Bluetooth refused the scan (" + (error.empty() ? std::string("no reason given") : error) + ").";
}

BtAdapter parse_btctl_show(const std::string &text)
{
    BtAdapter a;
    for (const std::string &raw : split_lines(text)) {
        const std::string l = clean_btctl_line(raw);
        std::string rest;
        if (after(l, "Controller ", rest) && rest.size() >= 17 && valid_bdaddr(rest.substr(0, 17))) {
            if (rest.find("not available") != std::string::npos) continue;
            if (a.present) break; /* the first controller only */
            a.present = true;
            a.address = upper(rest.substr(0, 17));
            continue;
        }
        if (starts_with(l, "No default controller available")) a.no_controller = true;
        if (starts_with(l, "Waiting to connect to bluetoothd")) a.waiting_daemon = true;
        if (!a.present || l.empty() || l[0] != '\t') continue;
        const std::string t = trim(l);
        if (after(t, "Name: ", rest)) a.name = bt_text(rest);
        else if (after(t, "Powered: ", rest)) a.powered = trim(rest) == "yes";
        else if (after(t, "PowerState: ", rest)) a.power_state = trim(rest);
        else if (after(t, "Discovering: ", rest)) a.discovering = trim(rest) == "yes";
    }
    return a;
}

BtSysState read_bt_sys(const Source &src)
{
    BtSysState s;
    for (const std::string &n : src.list("/sys/class/bluetooth"))
        if (starts_with(n, "hci")) s.adapter = true;
    for (const std::string &n : src.list("/sys/class/rfkill")) {
        const std::string dir = "/sys/class/rfkill/" + n;
        if (trim(src.read(dir + "/type")) != "bluetooth") continue;
        s.rfkill_found = true;
        if (trim(src.read(dir + "/soft")) == "1") s.soft_blocked = true;
        if (trim(src.read(dir + "/hard")) == "1") s.hard_blocked = true;
    }
    return s;
}

const char *company_name(int id)
{
    /* Bluetooth SIG assigned numbers (company identifiers): a few common ones */
    switch (id) {
    case 0x0001: return "Nokia";
    case 0x0002: return "Intel";
    case 0x0006: return "Microsoft";
    case 0x000A: return "Qualcomm (CSR)";
    case 0x000D: return "Texas Instruments";
    case 0x000F: return "Broadcom";
    case 0x001D: return "Qualcomm";
    case 0x0030: return "STMicroelectronics";
    case 0x0046: return "MediaTek";
    case 0x004C: return "Apple";
    case 0x0059: return "Nordic Semiconductor";
    case 0x005D: return "Realtek";
    case 0x0075: return "Samsung";
    case 0x0087: return "Garmin";
    case 0x009E: return "Bose";
    case 0x00C4: return "LG Electronics";
    case 0x00E0: return "Google";
    case 0x012D: return "Sony";
    case 0x0131: return "Cypress Semiconductor";
    case 0x0157: return "Huami";
    case 0x0171: return "Amazon";
    case 0x01DA: return "Logitech";
    case 0x027D: return "Huawei";
    case 0x02E5: return "Espressif";
    case 0x02FF: return "Silicon Labs";
    case 0x038F: return "Xiaomi";
    case 0x0499: return "Ruuvi Innovations";
    default: return "";
    }
}

DistanceClass distance_class(double rssi)
{
    if (rssi >= -60) return DistanceClass::Near;
    if (rssi >= -80) return DistanceClass::Medium;
    return DistanceClass::Far;
}

const char *distance_words(DistanceClass c)
{
    switch (c) {
    case DistanceClass::Near: return "near";
    case DistanceClass::Medium: return "medium";
    case DistanceClass::Far: return "far";
    case DistanceClass::Unknown: break;
    }
    return "";
}

/* ---------------------------------------------------------------- the table */

BleTable::BleTable(int expiry_ms, double alpha)
    : expiry_ms_(std::max(1, expiry_ms)), alpha_(std::min(1.0, std::max(0.01, alpha)))
{
}

BleTable::Entry &BleTable::entry(const std::string &address, std::int64_t now_ms)
{
    auto it = all_.find(address);
    if (it != all_.end()) return it->second;
    if (all_.size() >= kBleMaxDevices) { /* the one unheard the longest, a silent one first */
        auto victim = all_.end();
        for (auto i = all_.begin(); i != all_.end(); ++i) {
            if (i->first == info_address_) continue;
            if (victim == all_.end() || (victim->second.live && !i->second.live) ||
                (victim->second.live == i->second.live && i->second.d.last_seen_ms < victim->second.d.last_seen_ms))
                victim = i;
        }
        if (victim != all_.end()) all_.erase(victim);
    }
    Entry &e = all_[address];
    e.d.address = address;
    e.d.first_seen_ms = e.d.last_seen_ms = now_ms;
    return e;
}

void BleTable::set_name(Entry &e, const std::string &name, bool is_alias)
{
    if (name.empty()) return;
    std::string dashed = e.d.address;
    std::replace(dashed.begin(), dashed.end(), ':', '-');
    if (is_alias && (upper(name) == dashed || upper(name) == e.d.address)) return; /* bluez's alias for "no name" */
    if (!is_alias || e.d.name.empty()) e.d.name = name;
}

void BleTable::set_company(Entry &e, int id)
{
    e.d.company = id;
    const char *n = company_name(id);
    if (n[0]) {
        e.d.vendor = n;
    } else {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "company 0x%04x", static_cast<unsigned>(id) & 0xFFFFu);
        e.d.vendor = buf;
    }
}

void BleTable::apply(const BtEvent &e, std::int64_t now)
{
    if (e.kind != BtEvent::Kind::InfoField) info_address_.clear();
    switch (e.kind) {
    case BtEvent::Kind::NewDevice: set_name(entry(e.address, now), e.text, true); break;
    case BtEvent::Kind::DelDevice: {
        auto it = all_.find(e.address);
        if (it != all_.end()) it->second.live = false;
        break;
    }
    case BtEvent::Kind::Rssi: {
        Entry &x = entry(e.address, now);
        BleDevice &d = x.d;
        d.smooth_dbm = d.has_rssi ? alpha_ * e.value + (1.0 - alpha_) * d.smooth_dbm : e.value;
        d.has_rssi = true;
        d.rssi_dbm = e.value;
        d.rssi_at_ms = now;
        if (!d.readings) d.first_seen_ms = now;
        d.last_seen_ms = now;
        ++d.readings;
        d.distance = distance_class(d.smooth_dbm);
        x.live = true;
        if (!x.counted) {
            x.counted = true;
            ++seen_;
        }
        break;
    }
    case BtEvent::Kind::TxPower: {
        Entry &x = entry(e.address, now);
        x.d.has_tx = true;
        x.d.tx_dbm = e.value;
        if (x.live) x.d.last_seen_ms = now;
        break;
    }
    case BtEvent::Kind::Name: set_name(entry(e.address, now), e.text, false); break;
    case BtEvent::Kind::Alias: set_name(entry(e.address, now), e.text, true); break;
    case BtEvent::Kind::Company: {
        Entry &x = entry(e.address, now);
        set_company(x, e.value);
        if (x.live) x.d.last_seen_ms = now;
        break;
    }
    case BtEvent::Kind::InfoHeader: {
        Entry &x = entry(e.address, now);
        if (e.text == "public" || e.text == "random") x.d.address_type = e.text;
        info_address_ = e.address;
        break;
    }
    case BtEvent::Kind::InfoField: {
        auto it = all_.find(info_address_);
        if (info_address_.empty() || it == all_.end()) break;
        Entry &x = it->second;
        int n = 0;
        if (e.key == "Name") set_name(x, bt_text(e.text), false);
        else if (e.key == "Alias") set_name(x, bt_text(e.text), true);
        else if (e.key == "TxPower" && bt_number(e.text, n) && n >= -127 && n <= 127) {
            x.d.has_tx = true;
            x.d.tx_dbm = n;
        } else if (e.key == "ManufacturerData.Key" && bt_number(e.text, n) && n >= 0 && n <= 0xFFFF) {
            set_company(x, n);
        }
        break;
    }
    default: break;
    }
}

void BleTable::expire(std::int64_t now)
{
    for (auto &kv : all_)
        if (kv.second.live && now - kv.second.d.last_seen_ms > expiry_ms_) kv.second.live = false;
}

std::vector<BleDevice> BleTable::live() const
{
    std::vector<BleDevice> out;
    for (const auto &kv : all_)
        if (kv.second.live) out.push_back(kv.second.d);
    std::sort(out.begin(), out.end(), [](const BleDevice &a, const BleDevice &b) {
        if (a.smooth_dbm != b.smooth_dbm) return a.smooth_dbm > b.smooth_dbm;
        return a.address < b.address;
    });
    return out;
}

int BleTable::live_count() const
{
    int n = 0;
    for (const auto &kv : all_)
        if (kv.second.live) ++n;
    return n;
}

const BleDevice *BleTable::find(const std::string &address) const
{
    auto it = all_.find(upper(address));
    return it == all_.end() ? nullptr : &it->second.d;
}

/* ---------------------------------------------------------------- the scan */

std::string bt_unavailable_words(const BtSysState &sys, const CommandResult &show, const BtAdapter &a)
{
    static const char *const kSoft =
        "Bluetooth is switched off (rfkill soft block). Switch it on, for example with: sudo rfkill unblock bluetooth";
    if (!sys.adapter) return "This board has no Bluetooth adapter (or its driver is not loaded).";
    if (sys.hard_blocked) return "Bluetooth is switched off by a hardware switch (rfkill hard block).";
    if (sys.soft_blocked) return kSoft;
    if (show.cancelled) return "";
    if (!show.started || show.exit_code == 127) return "bluetoothctl is missing: Bluetooth needs the bluez package.";
    if (a.no_controller) return "BlueZ sees no Bluetooth adapter.";
    if (!a.present && (show.timed_out || a.waiting_daemon)) return "The Bluetooth service (bluetoothd) is not running.";
    if (!a.present) return "Bluetooth does not answer: bluetoothctl shows no adapter.";
    if (!a.powered) {
        if (a.power_state == "off-blocked") return kSoft;
        return "Bluetooth is powered off. Switch it on, for example with: bluetoothctl power on";
    }
    return "";
}

namespace {

class ScanClient : public StreamClient {
public:
    ScanClient(const BleConfig &cfg, BleTable &table, std::function<void(std::int64_t)> publish)
        : cfg_(cfg), table_(table), publish_(std::move(publish))
    {
    }
    void on_line(const std::string &line, std::int64_t at_us, StreamOrders &orders) override
    {
        const std::int64_t now = at_us / 1000;
        if (start_ms < 0) start_ms = now;
        BtEvent e;
        if (!parse_btctl_line(clean_btctl_line(line), e)) return;
        switch (e.kind) {
        case BtEvent::Kind::WaitingDaemon: waiting = true; break;
        case BtEvent::Kind::NoController:
            if (!stopping) halt("BlueZ sees no Bluetooth adapter.", now, orders);
            break;
        case BtEvent::Kind::Failed:
            if (!stopping) halt(bt_failure_words(e.text), now, orders);
            break;
        case BtEvent::Kind::Discovering:
            if (e.value && !started) {
                started = true;
                scan_start = now;
            }
            discovering = e.value != 0;
            break;
        case BtEvent::Kind::NewDevice:
            if (cfg_.ask_address_type && !stopping && asked < kBleMaxInfo && asked_set.insert(e.address).second) {
                orders.send += "info " + e.address + "\n";
                ++asked;
            }
            break;
        default: break;
        }
        table_.apply(e, now);
    }
    void on_tick(const StreamTick &tick, StreamOrders &orders) override
    {
        const std::int64_t now = tick.at_us / 1000;
        last_ms = now;
        if (start_ms < 0) start_ms = now;
        if (!started && !stopping && now - start_ms > 8000)
            halt(waiting ? "The Bluetooth service (bluetoothd) does not answer." : "The scan did not start.", now, orders);
        if (started && !stopping && now - scan_start >= static_cast<std::int64_t>(cfg_.seconds) * 1000) {
            stopping = true; /* the polite end: discovery off, then quit, then end of input */
            by_time = true;
            stop_at = now;
            orders.send += "scan off\nquit\n";
            orders.end_input = true;
        }
        if (stopping && now - stop_at > 3000) orders.stop = true; /* it does not quit: farewell, SIGTERM, SIGKILL */
        if (last_publish < 0 || now - last_publish >= cfg_.publish_ms) {
            last_publish = now;
            table_.expire(now);
            if (publish_) publish_(now);
        }
    }

    std::string error;
    bool started = false, discovering = false, waiting = false, stopping = false, by_time = false;
    std::int64_t start_ms = -1, scan_start = 0, stop_at = 0, last_publish = -1, last_ms = 0;

private:
    void halt(const std::string &why, std::int64_t now, StreamOrders &orders)
    {
        error = why;
        stopping = true;
        stop_at = now;
        orders.stop = true;
    }
    BleConfig cfg_;
    BleTable &table_;
    std::function<void(std::int64_t)> publish_;
    std::set<std::string> asked_set;
    int asked = 0;
};

} // namespace

BleSnapshot run_ble_scan(const BleConfig &input, const Source &src, CommandRunner &cmd, StreamRunner &stream,
                         const CancelToken &cancel, const std::function<void(const BleSnapshot &)> &publish)
{
    BleConfig cfg = input;
    cfg.seconds = std::min(60, std::max(5, cfg.seconds));
    cfg.expiry_ms = std::min(600000, std::max(1000, cfg.expiry_ms));
    cfg.alpha = std::min(1.0, std::max(0.05, cfg.alpha));
    cfg.publish_ms = std::min(2000, std::max(100, cfg.publish_ms));
    if (cfg.program.empty() || cfg.program[0].empty()) cfg.program = {"bluetoothctl"};
    BleSnapshot snap;
    snap.state = BleSnapshot::State::Checking;
    snap.seconds = cfg.seconds;
    const auto emit = [&] {
        if (publish) publish(snap);
    };
    const auto end = [&](BleSnapshot::State state, const std::string &error) {
        snap.state = state;
        snap.error = error;
        snap.discovering = false;
        emit();
        return snap;
    };
    emit();

    const BtSysState sys = read_bt_sys(src);
    CommandResult shown;
    BtAdapter adapter;
    if (sys.adapter && !sys.soft_blocked && !sys.hard_blocked) {
        CommandSpec spec;
        spec.argv = cfg.program;
        spec.argv.push_back("show");
        spec.timeout_ms = 5000;
        spec.max_output = 16 * 1024;
        shown = cmd.run(spec, cancel);
        if (shown.cancelled || cancel.cancelled()) return end(BleSnapshot::State::Cancelled, "");
        adapter = parse_btctl_show(shown.output);
    }
    const std::string why = bt_unavailable_words(sys, shown, adapter);
    if (!why.empty()) return end(BleSnapshot::State::Failed, why);
    snap.adapter = adapter.name.empty() ? adapter.address : adapter.name + " (" + adapter.address + ")";
    snap.other_discovery = adapter.discovering;

    snap.state = BleSnapshot::State::Scanning;
    emit();
    BleTable table(cfg.expiry_ms, cfg.alpha);
    ScanClient *self = nullptr;
    ScanClient client(cfg, table, [&](std::int64_t now) {
        snap.devices = table.live();
        snap.live = static_cast<int>(snap.devices.size());
        snap.seen = table.seen_count();
        snap.discovering = self->discovering;
        snap.elapsed_s = self->started ? static_cast<double>(now - self->scan_start) / 1000.0 : 0;
        emit();
    });
    self = &client;
    StreamSpec spec;
    spec.argv = cfg.program;
    spec.input = StreamSpec::Input::Text;
    spec.text = "menu scan\nduplicate-data on\ntransport le\nback\nscan on\n";
    spec.farewell = "scan off\nquit\n";
    spec.timeout_ms = (cfg.seconds + 20) * 1000;
    spec.grace_ms = 1000;
    spec.max_text = 4096;
    const StreamResult r = stream.run(spec, cancel, client);

    table.expire(client.last_ms);
    snap.devices = table.live();
    snap.live = static_cast<int>(snap.devices.size());
    snap.seen = table.seen_count();
    if (client.started) {
        const std::int64_t until = client.by_time ? client.stop_at : client.last_ms;
        snap.elapsed_s = static_cast<double>(std::max<std::int64_t>(0, until - client.scan_start)) / 1000.0;
    }
    if (r.cancelled || cancel.cancelled()) {
        snap.stop_note = "Stopped: discovery turned off.";
        return end(BleSnapshot::State::Cancelled, "");
    }
    if (!client.error.empty()) return end(BleSnapshot::State::Failed, client.error);
    if (!client.started) {
        if (!r.started || r.exit_code == 127) return end(BleSnapshot::State::Failed, "bluetoothctl is missing: Bluetooth needs the bluez package.");
        return end(BleSnapshot::State::Failed, client.waiting ? "The Bluetooth service (bluetoothd) does not answer."
                                                               : "The scan did not start.");
    }
    if (client.by_time) {
        snap.stop_note = "Scan stopped after " + std::to_string(cfg.seconds) + " s: discovery turned off. Run it again to go on.";
    } else if (r.timed_out) {
        snap.stop_note = "bluetoothctl did not stop in time and was ended.";
    } else {
        snap.stop_note = "bluetoothctl ended by itself.";
    }
    return end(BleSnapshot::State::Done, "");
}

BleSession::BleSession()
    : src_(make_system_source()), cmd_(std::make_shared<DirectRunner>()), stream_(std::make_shared<DirectStreamRunner>())
{
}

BleSession::BleSession(std::shared_ptr<const Source> src, std::shared_ptr<CommandRunner> cmd,
                       std::shared_ptr<StreamRunner> stream)
    : src_(std::move(src)), cmd_(std::move(cmd)), stream_(std::move(stream))
{
}

bool BleSession::start(const BleConfig &cfg)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    BleSnapshot first;
    first.state = BleSnapshot::State::Checking;
    first.seconds = std::min(60, std::max(5, cfg.seconds));
    channel_.begin(gen, first);
    const std::shared_ptr<const Source> src = src_;
    const std::shared_ptr<CommandRunner> cmd = cmd_;
    const std::shared_ptr<StreamRunner> stream = stream_;
    const std::uint64_t started = worker_.start([this, gen, cfg, src, cmd, stream](const CancelToken &cancel) {
        run_ble_scan(cfg, *src, *cmd, *stream, cancel, [this, gen](const BleSnapshot &s) { channel_.publish(gen, s); });
    });
    if (!started) {
        first.state = BleSnapshot::State::Failed;
        first.error = "Could not start the scan (no thread).";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void BleSession::stop()
{
    worker_.cancel();
}

} // namespace nt
