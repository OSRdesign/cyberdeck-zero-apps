/*
 * SPDX-License-Identifier: MIT
 *
 * A pretend world for the start page "Choose a node", for the screenshots and the hand checks without hardware (like the
 * Serial Console's SERIAL_CONSOLE_DEMO): MESHHOP_DEMO=<scenario> gives a list of USB ports and a scripted Bluetooth
 * (FakeBle) instead of /dev and BlueZ. Nothing of the real system is touched while it is set. Scenarios:
 *
 *   empty   no USB port, Bluetooth on and nothing heard, the scan question answered (Scan)
 *   usb     two USB boards, the Bluetooth question answered Not now
 *   ble     five Bluetooth nodes from -45 to -96 dBm heard by the scan (four bars to none), one of them paired
 *   first   one USB board, the Bluetooth scan question not asked yet (the first-use warning)
 *   pin     a Bluetooth node not paired yet: it asks for its PIN (123456)
 *   fail    a paired Bluetooth node that cannot be reached, and a USB port that does not open
 *   ip      two nodes added by IP address (no TCP server: their connect fails)
 *   off     the Bluetooth adapter is off
 *   nobt    no Bluetooth service (start fails)
 */

#pragma once

#include "ble/ble_types.hpp"
#include "nodes.hpp"
#include "serial_transport.hpp"

#include <memory>
#include <string>
#include <vector>

namespace meshzero {

struct NodeDemo {
    std::string name;
    std::vector<SerialCandidate> usb;
    NodeBook::Scan scan = NodeBook::Scan::Yes;
    std::vector<IpNode> ip;
    std::string last;
    /* A Bluetooth backend for this world (FakeBle with the scenario's devices and behaviour). */
    std::unique_ptr<ble::IBleBackend> make_backend() const;
};

/* The world MESHHOP_DEMO asks for; nullptr when it is unset or names no scenario. */
std::unique_ptr<NodeDemo> node_demo_from_env();
std::unique_ptr<NodeDemo> node_demo(const std::string &name);

} // namespace meshzero
