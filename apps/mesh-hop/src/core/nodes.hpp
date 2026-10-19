/*
 * SPDX-License-Identifier: MIT
 *
 * The ways to reach a node (the start page "Choose a node"): what the deck remembers about them (no LVGL; unit tested).
 *
 *   A node key names one way: "usb:/dev/ttyACM0", "ble:EC:EB:45:39:28:2D", "ip:192.168.1.20:5000" ("ip:[fe80::1]:5000").
 *   NodeBook is <data dir>/nodes.txt: the nodes added by IP address (always listed), the node chosen last (highlighted at the
 *   start, never connected by itself), the answer to the first Bluetooth scan question, and the node names learned once a
 *   node answered (a USB port or a Bluetooth address has no node name of its own).
 */

#pragma once

#include <string>
#include <vector>

namespace meshzero {

enum class NodeKind { Usb, Ble, Ip };

std::string node_key(NodeKind kind, const std::string &id);
/* false for a key of none of the three forms. */
bool parse_node_key(const std::string &key, NodeKind &kind, std::string &id);

struct IpNode {
    std::string host;
    int port = 5000;
    std::string name;            // optional; "" shows the address
    std::string key() const;     // "ip:host:port"
    std::string address() const; // "192.168.1.20:5000", "[fe80::1]:5000"
};

/* What the user typed for an IP node: "192.168.1.20", "192.168.1.20:5000", "node.lan:5001", "[fe80::1]:5000". Empty when it
 * is fine (host and port filled in), else the message to show. default_port when no port is typed. */
std::string parse_ip_input(const std::string &text, int default_port, std::string &host, int &port);
/* The name of an IP node: empty is fine (the address is shown); else 1..32 printable characters. */
std::string validate_node_name(const std::string &name);

/* Signal bars 0..4 for an RSSI in dBm, the launcher's rule (its Wi-Fi rows and status strip: cp0_ui_status_signal_percent
 * maps -100 dBm to 0 % and -50 dBm to 100 %, cp0_statusbar.c lights bar i from 1, 30, 60, 80 %): 4 bars from -60 dBm,
 * 3 from -70, 2 from -85, 1 above -100, 0 at -100 and below. */
int signal_bars(int rssi_dbm);

class NodeBook {
public:
    explicit NodeBook(std::string dir);

    bool load();
    bool save() const;
    const std::string &path() const { return path_; }

    const std::vector<IpNode> &ip_nodes() const { return ip_; }
    /* Adds a node by its address. "" when added, else why not (a bad address or port, a bad name, already listed). */
    std::string add_ip(const std::string &host, int port, const std::string &name);
    bool remove_ip(const std::string &key);
    const IpNode *find_ip(const std::string &key) const;

    const std::string &last() const { return last_; }
    void set_last(const std::string &key) { last_ = key; }

    /* The first Bluetooth scan question: not asked yet, Scan (the page scans while it is open) or Not now. */
    enum class Scan { Ask, Yes, No };
    Scan ble_scan() const { return scan_; }
    void set_ble_scan(Scan s) { scan_ = s; }

    /* The node name learned for a key ("" when none); forget_name when the user forgets the node. */
    std::string name_of(const std::string &key) const;
    void set_name(const std::string &key, const std::string &name);
    void forget_name(const std::string &key);

private:
    std::string path_;
    std::vector<IpNode> ip_;
    std::string last_;
    Scan scan_ = Scan::Ask;
    std::vector<std::pair<std::string, std::string>> names_;
};

} // namespace meshzero
