/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the pages' logic without LVGL (unit-tested by tests/test_core.cpp): navigation, and the texts the
 * pages show (row values, the status line of a port, the "What is connected" table, the terminal's title).
 */
#pragma once

#include "ports.hpp"
#include "serial_io.hpp"
#include "settings.hpp"

#include <string>
#include <utility>
#include <vector>

namespace sc {

/* The smallest screen: X-MinSize of the .desktop file (tests/test_core.cpp compares them). */
constexpr int kMinW = 640;
constexpr int kMinH = 480;
constexpr const char *kAppTitle = "Serial Console";

/* ---------------------------------------------------------------- navigation */

enum class View { Ports, Port, Choice, Terminal };
enum class Back { Quit, Popped };

class Nav {
public:
    View current() const { return stack_.back(); }
    int depth() const { return static_cast<int>(stack_.size()); }
    void open(View view) { stack_.push_back(view); }
    /* Esc / Left / the header's Esc: one level up; at the ports page the app ends. */
    Back back();

private:
    std::vector<View> stack_{View::Ports};
};

/* ---------------------------------------------------------------- texts */

/* The port list's status line for the highlighted port: "ttyUSB0 - 115200 recommended", or with a speed of its own
 * "ttyUSB0 - 57600 (115200 recommended)". */
std::string port_caption(const PortInfo &port, const PortSettings &settings);

/* A port's row in the list: its title and, at the right, its tty ("CP210x USB-serial", "ttyUSB0"); an on-board UART is
 * "On-board UART" with "GPIO 14/15" (the target of /dev/serial0) or its tty, so the row fits at 640x480. */
std::pair<std::string, std::string> port_row_text(const PortInfo &port);

/* The Speed row's value: "115200 (recommended)" or "57600". */
std::string speed_value(int baud, int recommended);

/* "What is connected": key / value rows (rows without a value are left out). */
std::vector<std::pair<std::string, std::string>> port_info_items(const PortInfo &port, bool access_ok);

/* "ttyUSB0 115200 8N1" */
std::string terminal_title(const std::string &tty, int baud, const std::string &frame);

/* The terminal's status line after the pill: "RX 1.2 kB  TX 34 B  Paused  Log ttyUSB0-20261013-101500.log" */
std::string status_text(const SessionStatus &status, bool paused);
/* The pill: "Connected", "Port lost: waiting", "Opening", "Closed" */
std::string pill_text(const SessionStatus &status);

/* The empty port list. */
constexpr const char *kEmptyTitle = "No serial adapter found.";
constexpr const char *kEmptyText = "Plug a USB-serial adapter or a board; it appears here by itself.";

} // namespace sc
