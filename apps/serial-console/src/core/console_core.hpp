/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the pages' logic without LVGL (unit-tested by tests/test_core.cpp): navigation, and the texts the
 * pages show (row values, the status line of a port, the "What is connected" table, the terminal's title, the
 * identification card and the probes' confirmations).
 */
#pragma once

#include "identify.hpp"
#include "ports.hpp"
#include "probe.hpp"
#include "serial_io.hpp"
#include "settings.hpp"

#include <string>
#include <utility>
#include <vector>

namespace sc {

/* The smallest screen: X-MinSize of the .desktop file (tests/test_core.cpp compares them). */
constexpr int kMinW = 640;
constexpr int kMinH = 480;
/* The smallest portrait canvas (PDA mode): X-Portrait of the .desktop file (tests/test_core.cpp compares them). At
 * 480x640 the terminal keeps about a dozen rows of text above the on-screen keyboard and its line field. */
constexpr int kPortraitW = 480;
constexpr int kPortraitH = 640;
constexpr const char *kAppTitle = "Serial Console";

/* ---------------------------------------------------------------- navigation */

enum class View { Ports, Port, Choice, Terminal, Ask, Card, Detail };
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

/* ---------------------------------------------------------------- "What is on the other end?" */

/* A USB-serial adapter or cable (or a debug probe's serial port): it says nothing about what is wired to it. */
bool bare_adapter(const PortInfo &port);
/* two lines of a 640x480 row (an Info row with no value wraps once, then ends with dots) */
constexpr const char *kAdapterNote = "The adapter cannot tell what is behind it; the probes below ask";
/* After Listen (or Reset and listen) heard nothing: MicroPython / CircuitPython boards print only when asked */
constexpr const char *kSilentNote = "Many boards stay silent until asked: try the rows below";
constexpr const char *kSilentResetNote = "Silent after a reset too: try Ask the device";

/* The recommendation once something was identified: its speed and reason replace the device table's. */
Recommendation identified_recommendation(const Recommendation &base, const Identity &id);
/* What to remember after a probe: a new result that found nothing keeps an earlier one that did; a result about the
 * same board keeps the chip and board an earlier probe found (the ROM's chip under an ESP-IDF log). */
Identity remembered_identity(const Identity &old, const Identity &now);

enum class CardAction { None, Boot, Evidence, Apply, Again, Copy, Reset, Ask };
struct CardRow {
    std::string key;
    std::string value;     /* Info: the value ("" = the key alone across the row); a row with an action: its value */
    CardAction action = CardAction::None;
};
/* The result card: Device (the adapter), Behind it, Chip, Firmware, Board, Details, Framing, Confidence, Found,
 * Boot output, Evidence, Recommended, then the actions (Apply settings: the recommended ones; Run again; Copy summary
 * to a log file: the labels are short so that they fit a 640x480 row with their value); for a bare adapter where
 * nothing was identified the adapter note and the other probes; after a Listen or a Reset that heard nothing (any
 * port) the silent note, "Try Ask the device" (CardAction::Ask: the Ask page, each probe confirmed) and, after Listen,
 * Reset and listen. `in_use`: the port's speed now (Apply says "In use"
 * when it is the recommended one already). */
std::vector<CardRow> card_rows(const Identity &id, const PortInfo &port, Probe probe, int in_use);
/* What tapping a row does, for the list's row look (chevron, grey, blue): Info = nothing (a read-out), Open = a page,
 * Edit = changes a value, Action = performs something at once. */
enum class RowKind { Info, Open, Edit, Action };
/* The kind of a card row: the read-outs (None) are Info; Boot output and Evidence open a page; the Ask row opens the
 * Ask page; Apply settings, Run again, Copy summary and Reset and listen perform at once. */
RowKind card_action_kind(CardAction action);
/* "Likely (listened)", "Sure (asked)" */
std::string confidence_text(const Identity &id);
/* "2026-10-13 14:05" (local time; "" when unknown) */
std::string when_text(std::time_t when);

/* The port page's rows of the identification: their status line. */
std::string probe_caption(Probe p, ResetStyle style);
/* The confirmation dialog of a probe that resets the board or sends something. */
std::string confirm_title(Probe p, ResetStyle style);
std::string confirm_text(Probe p, ResetStyle style, int baud);
std::string confirm_button(Probe p, ResetStyle style);
/* The Ask page: a probe's row value and its status line. */
std::string ask_value(Probe p);
std::string ask_caption(Probe p);

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
