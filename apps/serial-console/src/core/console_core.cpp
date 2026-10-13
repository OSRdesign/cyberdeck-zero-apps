/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: page logic without LVGL (see console_core.hpp).
 */
#include "console_core.hpp"

#include "fsutil.hpp"
#include "term.hpp"

namespace sc {

Back Nav::back()
{
    if (stack_.size() <= 1) return Back::Quit;
    stack_.pop_back();
    return Back::Popped;
}

std::string speed_value(int baud, int recommended)
{
    return std::to_string(baud) + (baud == recommended ? " (recommended)" : "");
}

std::pair<std::string, std::string> port_row_text(const PortInfo &port)
{
    if (port.onboard) return {"On-board UART", port.gpio ? "GPIO 14/15" : port.tty};
    return {port.title, port.tty};
}

std::string port_caption(const PortInfo &port, const PortSettings &settings)
{
    const int rec = port.rec.baud;
    const int baud = effective_baud(settings, rec);
    if (baud == rec) return port.tty + " - " + std::to_string(rec) + " recommended";
    return port.tty + " - " + std::to_string(baud) + " (" + std::to_string(rec) + " recommended)";
}

std::vector<std::pair<std::string, std::string>> port_info_items(const PortInfo &port, bool access_ok)
{
    std::vector<std::pair<std::string, std::string>> items;
    const auto add = [&items](const char *key, const std::string &value) {
        if (!value.empty()) items.emplace_back(key, value);
    };
    add("Device", port.title);
    if (port.usb) {
        std::string maker = port.known ? port.known->maker : std::string();
        if (maker.empty()) maker = port.manufacturer;
        add("Maker", maker);
        if (port.vid || port.pid) add("USB ID", usb_id_text(port.vid, port.pid));
    }
    add("Driver", port.driver);
    add("Serial no", port.serial);
    if (port.onboard) add("Typically", port.gpio ? "Pi console, GPS, LoRa" : "a board on this UART");
    else add("Typically", port.known ? std::string(port.known->hint) : std::string("unknown device"));
    add("Recommended", std::to_string(port.rec.baud) + " " + port.rec.frame);
    add("Port", "/dev/" + port.tty);
    if (!access_ok) add("Access", "no permission");
    return items;
}

std::string terminal_title(const std::string &tty, int baud, const std::string &frame)
{
    return tty + " " + std::to_string(baud) + " " + frame;
}

std::string pill_text(const SessionStatus &status)
{
    switch (status.state) {
    case LinkState::Opening: return "Opening";
    case LinkState::Open: return "Connected";
    case LinkState::Lost: return "Port lost: waiting";
    case LinkState::Failed: return "Failed";
    case LinkState::Closed: return "Closed";
    }
    return std::string();
}

std::string status_text(const SessionStatus &status, bool paused)
{
    std::string text = "RX " + format_bytes(status.rx) + "  TX " + format_bytes(status.tx);
    if (paused) text += "  Paused";
    if (status.log_on) text += "  Log " + fs::base_name(status.log_path);
    else if (status.log_error) text += "  Log failed";
    if (status.ui_dropped) text += "  Skipped " + format_bytes(status.ui_dropped);
    if (!status.note.empty()) text += "  " + status.note;
    return text;
}

} // namespace sc
