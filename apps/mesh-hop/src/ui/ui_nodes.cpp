/*
 * SPDX-License-Identifier: MIT
 */

#include "ui_nodes.hpp"

#include <algorithm>

using namespace meshzero;

namespace meshhop {

namespace {

const char *const kDot = " \xC2\xB7 ";       // " · "

std::string phase_word(NodePhase p)
{
    switch (p) {
    case NodePhase::Connecting: return "Connecting...";
    case NodePhase::Pairing: return "Pairing...";
    case NodePhase::PinNeeded: return "Type the PIN";
    case NodePhase::Connected: return "Connected";
    default: return "";
    }
}

/* The second line and marker of the node in use. */
void apply_phase(NodeRow &r, const NodePageState &s)
{
    if (r.key != s.active || s.phase == NodePhase::None) return;
    if (s.phase == NodePhase::Failed) {
        r.detail = "Failed" + (s.phase_text.empty() ? std::string() : ": " + s.phase_text) + kDot + "tap to retry";
        r.tone = Tone::Red;
        r.aff = Aff::Action;
        return;
    }
    r.detail = r.detail + kDot + phase_word(s.phase);
    r.tone = s.phase == NodePhase::Connected ? Tone::Green : Tone::Gold;
    r.aff = Aff::Running;
}

} // namespace

std::string port_short(const std::string &path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

const char *node_icon(NodeKind k)
{
    switch (k) {
    case NodeKind::Usb: return "\xEF\x8a\x87";        // LV_SYMBOL_USB
    case NodeKind::Ble: return "\xEF\x8a\x93";        // LV_SYMBOL_BLUETOOTH
    case NodeKind::Ip: return "\xEF\x87\xAB";         // LV_SYMBOL_WIFI: a node on Wi-Fi
    }
    return "";
}

std::string ble_node_title(const ble::BleDevice &d, const NodeBook *book)
{
    if (book) {
        const std::string n = book->name_of(node_key(NodeKind::Ble, d.address));
        if (!n.empty()) return n;
    }
    const std::string prefix = ble::kNamePrefix;
    if (d.name.rfind(prefix, 0) == 0 && d.name.size() > prefix.size()) return d.name.substr(prefix.size());
    return d.name.empty() ? d.address : d.name;
}

std::vector<NodeRow> build_node_rows(const NodePageState &s)
{
    std::vector<NodeRow> rows;
    // USB: the ports there now
    for (const SerialCandidate &c : s.usb) {
        NodeRow r;
        r.kind = NodeKind::Usb;
        r.key = node_key(NodeKind::Usb, c.path);
        // the name learned on this port, only while the same USB device is behind it (another board: its product string)
        const std::string learned = s.book ? s.book->name_for(r.key, usb_identity(c)) : "";
        r.title = !learned.empty() ? learned : !c.product.empty() ? c.product : "USB board";
        r.detail = port_short(c.path);
        if (!c.product.empty() && c.product != r.title) r.detail += kDot + c.product;
        else if (!c.vendor.empty()) r.detail += kDot + c.vendor;
        apply_phase(r, s);
        rows.push_back(r);
    }
    // Bluetooth: paired first, then the strongest
    std::vector<ble::BleDevice> bt = s.ble;
    std::stable_sort(bt.begin(), bt.end(), [](const ble::BleDevice &a, const ble::BleDevice &b) {
        const bool pa = a.paired || a.bonded, pb = b.paired || b.bonded;
        if (pa != pb) return pa;
        if (a.has_rssi != b.has_rssi) return a.has_rssi;
        if (a.has_rssi && a.rssi != b.rssi) return a.rssi > b.rssi;
        return a.name < b.name;
    });
    for (const ble::BleDevice &d : bt) {
        NodeRow r;
        r.kind = NodeKind::Ble;
        r.key = node_key(NodeKind::Ble, d.address);
        r.title = ble_node_title(d, s.book);
        r.detail = std::string("BLE") + kDot + (d.paired || d.bonded ? "paired" : "not paired yet (PIN)");
        r.bars = d.has_rssi ? signal_bars(d.rssi) : -1;
        apply_phase(r, s);
        rows.push_back(r);
    }
    // the nodes added by IP address: always listed
    for (const IpNode &n : s.ip) {
        NodeRow r;
        r.kind = NodeKind::Ip;
        r.key = n.key();
        r.title = n.name.empty() ? n.address() : n.name;
        r.detail = n.address();
        apply_phase(r, s);
        rows.push_back(r);
    }
    // the Bluetooth row: a grey note when Bluetooth cannot be used, else the scan
    NodeRow b;
    b.kind = NodeKind::Ble;
    if (s.bt == NodePageState::Bt::Missing || s.bt == NodePageState::Bt::Off) {
        b.type = NodeRowType::Note;
        b.key = "note-ble";
        b.aff = Aff::Info;
        b.selectable = false;
        if (s.bt == NodePageState::Bt::Off) {
            b.title = "Bluetooth is off";
            b.detail = "Switch it on in the launcher's Settings > Bluetooth";
        } else {
            b.title = "Bluetooth is not available";
            b.detail = s.bt_why.empty() ? std::string("No Bluetooth adapter or service on this deck") : s.bt_why;
        }
        rows.push_back(b);
    } else if (s.scan_answer != NodeBook::Scan::Ask || s.scanning) {
        b.type = NodeRowType::Scan;
        b.key = "scan";
        if (s.scanning) {
            b.title = "Scanning for Bluetooth nodes...";
            const size_t heard = static_cast<size_t>(std::count_if(s.ble.begin(), s.ble.end(), [](const ble::BleDevice &d) { return d.has_rssi; }));
            b.detail = heard == 0 ? std::string("Nothing heard yet") : std::to_string(heard) + (heard == 1 ? " node heard" : " nodes heard");
            b.aff = Aff::Running;
        } else if (s.scan_answer == NodeBook::Scan::No) {
            b.title = "Scan for Bluetooth nodes";
            b.detail = "A Bluetooth keyboard may lag briefly while it scans";
        } else {
            b.title = "Rescan for Bluetooth nodes";
            b.detail = "Look again for 10 s";
        }
        rows.push_back(b);
    }
    NodeRow add;
    add.type = NodeRowType::AddIp;
    add.kind = NodeKind::Ip;
    add.key = "add-ip";
    add.title = "Add a node by IP address";
    add.detail = "A MeshCore node on Wi-Fi (TCP port 5000)";
    add.aff = Aff::Open;
    rows.push_back(add);
    return rows;
}

int find_node_row(const std::vector<NodeRow> &rows, const std::string &key)
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].key == key) return static_cast<int>(i);
    return -1;
}

std::string node_page_caption(const NodePageState &s, bool touch)
{
    std::string who;
    for (const NodeRow &r : build_node_rows(s))
        if (r.key == s.active) who = r.title;
    switch (s.phase) {
    case NodePhase::Connecting:
        if (!who.empty()) return "Connecting to " + who + "...";
        break;
    case NodePhase::Pairing: return "Pairing with " + (who.empty() ? std::string("the node") : who) + "...";
    case NodePhase::PinNeeded: return "Pairing with " + (who.empty() ? std::string("the node") : who) + ": type its PIN";
    case NodePhase::Failed: return "Could not connect" + (who.empty() ? std::string() : " to " + who) + (s.phase_text.empty() ? std::string() : ": " + s.phase_text);
    default: break;
    }
    if (s.scanning) return "Scanning for Bluetooth nodes...";
    return touch ? "Choose a node: tap it, then tap it again to connect" : "Choose the node to connect to";
}

std::vector<std::string> node_options(const NodeRow &r, const NodePageState &s)
{
    std::vector<std::string> o;
    if (r.type != NodeRowType::Node) return o;
    if (r.kind == NodeKind::Ble) {
        for (const ble::BleDevice &d : s.ble)
            if (node_key(NodeKind::Ble, d.address) == r.key && (d.paired || d.bonded)) o.push_back("Forget this node");
    } else if (r.kind == NodeKind::Ip) {
        o.push_back("Remove IP node");
    }
    o.push_back("Info");
    return o;
}

} // namespace meshhop
