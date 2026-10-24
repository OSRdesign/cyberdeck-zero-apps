/*
 * SPDX-License-Identifier: MIT
 *
 * The start page "Choose a node" (no LVGL; unit tested): one list of every way to reach a node, built from what the deck
 * sees now (the USB ports, the Bluetooth LE nodes BlueZ knows or the scan hears, the nodes added by IP address) and the
 * state of the node being connected.
 *
 *   Rows, in this order: USB nodes, Bluetooth nodes (paired first, then the strongest), IP nodes, then the Bluetooth row
 *   (scanning, Rescan, Scan for Bluetooth nodes, or a grey note when Bluetooth cannot be used), and last "Add a node by IP
 *   address". A node row: the transport icon, the node's name, a second line ("ttyACM0 · T1000", "BLE · paired",
 *   "192.168.1.20:5000", or the state of the connection), and for Bluetooth the signal bars while the scan hears it. A USB
 *   row shows the node name learned on its port only while the same USB device is behind it (NodeBook::name_for), else
 *   the USB product string ("USB JTAG/serial debug unit"), else "USB board".
 *   Markers (the row affordance rule): a node acts (the dot; blue while it connects or is connected), Add opens a dialog (the
 *   chevron), a note is information (grey, never selected).
 */

#pragma once

#include "ble/ble_types.hpp"
#include "nodes.hpp"
#include "serial_transport.hpp"
#include "ui_logic.hpp"
#include "ui_pages.hpp"

#include <string>
#include <vector>

namespace meshhop {

enum class NodeRowType { Node, Scan, AddIp, Note };

struct NodeRow {
    NodeRowType type = NodeRowType::Node;
    meshzero::NodeKind kind = meshzero::NodeKind::Usb;
    std::string key;                 // a node key, or "scan", "add-ip", "note-ble"
    std::string title, detail;
    int bars = -1;                   // Bluetooth: 0..4 while the scan hears it; -1: no bars
    Aff aff = Aff::Action;
    Tone tone = Tone::Muted;         // of the second line
    bool selectable = true;
};

/* The state of the node being connected (the one the user chose last in this run). */
enum class NodePhase { None, Connecting, Pairing, PinNeeded, Connected, Failed };

struct NodePageState {
    std::vector<meshzero::SerialCandidate> usb;
    std::vector<meshzero::ble::BleDevice> ble;      // the candidates (Nordic UART or MeshCore- name)
    enum class Bt { Unknown, Ready, Off, Missing } bt = Bt::Unknown;
    std::string bt_why;                              // Missing: why ("BlueZ (bluetoothd) is not running")
    meshzero::NodeBook::Scan scan_answer = meshzero::NodeBook::Scan::Ask;
    bool scanning = false;
    std::vector<meshzero::IpNode> ip;
    std::string active;                              // the key of the node in use
    NodePhase phase = NodePhase::None;
    std::string phase_text;                          // Failed: the short reason; else what is going on
    const meshzero::NodeBook *book = nullptr;        // the node names learned
};

std::vector<NodeRow> build_node_rows(const NodePageState &s);
int find_node_row(const std::vector<NodeRow> &rows, const std::string &key);
/* The grey line under the title: what the page does now. touch: no keyboard (say "tap twice"). */
std::string node_page_caption(const NodePageState &s, bool touch);
/* The name of a Bluetooth node: its learned name, else the advertised name without "MeshCore-", else the address. */
std::string ble_node_title(const meshzero::ble::BleDevice &d, const meshzero::NodeBook *book);
/* "ttyACM0" for "/dev/ttyACM0". */
std::string port_short(const std::string &path);
/* The transport icon of a row (an LVGL symbol: USB, Bluetooth, Wi-Fi). */
const char *node_icon(meshzero::NodeKind k);
/* What the options of a node offer (3 s hold, Ctrl+O): Forget this node (a paired Bluetooth node), Remove IP node, Info. */
std::vector<std::string> node_options(const NodeRow &r, const NodePageState &s);

} // namespace meshhop
