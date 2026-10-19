// 0.5: the rows of the start page "Choose a node" (ui_nodes.cpp). Included by test_ui.cpp.

#include "ui_nodes.hpp"

#include <unistd.h>

static meshzero::ble::BleDevice ble_dev(const char *addr, const char *name, bool paired, int rssi, bool heard)
{
    meshzero::ble::BleDevice d;
    d.address = addr;
    d.name = name;
    d.paired = d.bonded = paired;
    d.nus = true;
    d.has_rssi = heard;
    d.rssi = rssi;
    return d;
}

static void test_node_rows()
{
    using meshzero::NodeBook;
    using meshzero::NodeKind;
    const std::string dir = "/tmp/meshzero-test-nodes-ui-" + std::to_string(::getpid());
    NodeBook book(dir);
    NodePageState s;
    s.book = &book;

    // nothing found: the scan question is not answered yet, so only "Add a node by IP address" (a chevron: it opens a dialog)
    {
        const std::vector<NodeRow> r = build_node_rows(s);
        CHECK(r.size() == 1 && r[0].type == NodeRowType::AddIp && r[0].aff == Aff::Open && r[0].title == "Add a node by IP address");
        CHECK(node_page_caption(s, true) == "Choose a node: tap it, then tap it again to connect");
        CHECK(node_page_caption(s, false) == "Choose the node to connect to");
    }

    // one list: USB, Bluetooth (paired first, then the strongest), IP, the scan row, Add last
    meshzero::SerialCandidate c;
    c.path = "/dev/ttyACM0";
    c.vendor = "Seeed Studio";
    c.product = "T1000-E";
    s.usb = {c};
    s.bt = NodePageState::Bt::Ready;
    s.scan_answer = NodeBook::Scan::Yes;
    s.scanning = true;
    s.ble = {ble_dev("11:11:11:11:11:11", "MeshCore-Far", false, -96, true), ble_dev("22:22:22:22:22:22", "MeshCore-Near", false, -45, true),
             ble_dev("EC:EB:45:39:28:2D", "MeshCore-OSR T1000", true, 0, false)};
    meshzero::IpNode ip;
    ip.host = "192.168.1.20";
    ip.port = 5000;
    s.ip = {ip};
    book.set_name("usb:/dev/ttyACM0", "OSR wio");
    {
        const std::vector<NodeRow> r = build_node_rows(s);
        CHECK(r.size() == 7);
        CHECK(r[0].kind == NodeKind::Usb && r[0].key == "usb:/dev/ttyACM0" && r[0].title == "OSR wio" && r[0].detail == "ttyACM0 \xC2\xB7 T1000-E");
        CHECK(r[0].aff == Aff::Action && r[0].bars == -1);
        CHECK(r[1].kind == NodeKind::Ble && r[1].title == "OSR T1000" && r[1].detail == "BLE \xC2\xB7 paired" && r[1].bars == -1);   // paired, not heard
        CHECK(r[2].title == "Near" && r[2].bars == 4 && r[2].detail == "BLE \xC2\xB7 not paired yet (PIN)");
        CHECK(r[3].title == "Far" && r[3].bars == 1);
        CHECK(r[4].kind == NodeKind::Ip && r[4].title == "192.168.1.20:5000" && r[4].detail == "192.168.1.20:5000");
        CHECK(r[5].type == NodeRowType::Scan && r[5].title == "Scanning for Bluetooth nodes..." && r[5].aff == Aff::Running && r[5].detail == "2 nodes heard");
        CHECK(r[6].type == NodeRowType::AddIp);
        CHECK(find_node_row(r, "ip:192.168.1.20:5000") == 4 && find_node_row(r, "nope") == -1);
        CHECK(node_page_caption(s, true) == "Scanning for Bluetooth nodes...");
        CHECK(std::string(node_icon(NodeKind::Usb)) == "\xEF\x8a\x87" && std::string(node_icon(NodeKind::Ble)) == "\xEF\x8a\x93");
    }
    s.scanning = false;
    CHECK(build_node_rows(s)[5].title == "Rescan for Bluetooth nodes" && build_node_rows(s)[5].aff == Aff::Action);
    s.scan_answer = NodeBook::Scan::No;
    CHECK(build_node_rows(s)[5].title == "Scan for Bluetooth nodes");

    // Bluetooth off or missing: a grey note in place of the scan, never selected
    s.bt = NodePageState::Bt::Off;
    {
        const std::vector<NodeRow> r = build_node_rows(s);
        const int i = find_node_row(r, "note-ble");
        CHECK(i >= 0 && r[static_cast<size_t>(i)].title == "Bluetooth is off" && !r[static_cast<size_t>(i)].selectable && r[static_cast<size_t>(i)].aff == Aff::Info);
        CHECK(find_node_row(r, "scan") == -1);
    }
    s.bt = NodePageState::Bt::Missing;
    s.bt_why = "BlueZ (bluetoothd) is not running";
    {
        const std::vector<NodeRow> r = build_node_rows(s);
        const int i = find_node_row(r, "note-ble");
        CHECK(i >= 0 && r[static_cast<size_t>(i)].title == "Bluetooth is not available" && r[static_cast<size_t>(i)].detail == "BlueZ (bluetoothd) is not running");
    }
    s.bt = NodePageState::Bt::Ready;

    // the node being connected: its state on its second line, the dot blue; failed: the reason and "tap to retry"
    s.active = "ble:EC:EB:45:39:28:2D";
    s.phase = NodePhase::PinNeeded;
    {
        const std::vector<NodeRow> r = build_node_rows(s);
        const NodeRow &n = r[static_cast<size_t>(find_node_row(r, s.active))];
        CHECK(n.detail == "BLE \xC2\xB7 paired \xC2\xB7 Type the PIN" && n.aff == Aff::Running && n.tone == Tone::Gold);
        CHECK(node_page_caption(s, true) == "Pairing with OSR T1000: type its PIN");
    }
    s.phase = NodePhase::Connected;
    {
        const std::vector<NodeRow> r = build_node_rows(s);
        const NodeRow &n = r[static_cast<size_t>(find_node_row(r, s.active))];
        CHECK(n.tone == Tone::Green && n.aff == Aff::Running && n.detail.find("Connected") != std::string::npos);
    }
    s.phase = NodePhase::Failed;
    s.phase_text = "Pairing failed (wrong PIN?)";
    {
        const std::vector<NodeRow> r = build_node_rows(s);
        const NodeRow &n = r[static_cast<size_t>(find_node_row(r, s.active))];
        CHECK(n.detail == "Failed: Pairing failed (wrong PIN?) \xC2\xB7 tap to retry" && n.tone == Tone::Red && n.aff == Aff::Action);
        CHECK(node_page_caption(s, false) == "Could not connect to OSR T1000: Pairing failed (wrong PIN?)");
    }
    s.active = "ip:192.168.1.20:5000";
    s.phase = NodePhase::Connecting;
    CHECK(node_page_caption(s, false) == "Connecting to 192.168.1.20:5000...");

    // the options: Forget for a paired Bluetooth node, Remove for an IP node, Info for all
    {
        const std::vector<NodeRow> r = build_node_rows(s);
        CHECK((node_options(r[static_cast<size_t>(find_node_row(r, "ble:EC:EB:45:39:28:2D"))], s) == std::vector<std::string>{"Forget this node", "Info"}));
        CHECK((node_options(r[static_cast<size_t>(find_node_row(r, "ble:22:22:22:22:22:22"))], s) == std::vector<std::string>{"Info"}));
        CHECK((node_options(r[static_cast<size_t>(find_node_row(r, "ip:192.168.1.20:5000"))], s) == std::vector<std::string>{"Remove IP node", "Info"}));
        CHECK((node_options(r[0], s) == std::vector<std::string>{"Info"}));
        CHECK(node_options(r.back(), s).empty());
    }
    // names: learned, else the advertised name without the prefix, else the address
    CHECK(ble_node_title(ble_dev("33:33:33:33:33:33", "", false, 0, false), nullptr) == "33:33:33:33:33:33");
    CHECK(ble_node_title(ble_dev("33:33:33:33:33:33", "Other LE", false, 0, false), nullptr) == "Other LE");
    CHECK(port_short("/dev/ttyUSB1") == "ttyUSB1" && port_short("ttyX") == "ttyX");
}
