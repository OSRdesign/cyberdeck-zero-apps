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
        std::string chip = port.chip;
        if (!chip.empty() && port.title.find(chip) != std::string::npos && port.bcd == 0) chip.clear();
        if (!chip.empty() && port.bcd) chip += " (rev " + bcd_text(port.bcd) + ")"; /* bcdDevice: the chip's release */
        add("Chip", chip);
        std::string maker = port.known ? port.known->maker : std::string();
        if (maker.empty()) maker = port.manufacturer;
        add("Maker", maker);
        if (!port.product.empty() && port.product != port.title) add("Product", port.product);
        if (port.vid || port.pid) add("USB ID", usb_id_text(port.vid, port.pid));
        std::string usb = usb_speed_text(port.speed);
        if (port.interfaces > 0)
            usb += (usb.empty() ? "" : ", ") + std::to_string(port.interfaces) +
                   (port.interfaces == 1 ? " interface" : " interfaces");
        add("USB", usb);
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

/* ---------------------------------------------------------------- "What is on the other end?" */

bool bare_adapter(const PortInfo &port)
{
    if (!port.usb) return false;
    if (!port.known) return true;
    switch (port.known->family) {
    case Family::Bridge:
    case Family::EspBridge:
    case Family::Debugger:
    case Family::Console: return true;
    default: return false;
    }
}

Recommendation identified_recommendation(const Recommendation &base, const Identity &id)
{
    Recommendation r = base;
    int baud = 0;
    std::string reason;
    if (!identity_recommendation(id, &baud, &reason)) return r;
    r.baud = baud;
    r.reason = reason;
    r.why = std::to_string(r.baud) + " " + r.frame + ": " + r.reason;
    return r;
}

namespace {
bool esp_kind(Kind k)
{
    return k == Kind::EspIdf || k == Kind::EspRom || k == Kind::ArduinoEsp32 || k == Kind::Meshtastic;
}
} // namespace

Identity remembered_identity(const Identity &old, const Identity &now)
{
    if (!now.found() && old.found()) return old;
    if (!now.found() || !old.found()) return now;
    if (now.kind != old.kind && !(esp_kind(now.kind) && esp_kind(old.kind))) return now;
    Identity r = now;
    if (r.chip.empty() || r.chip == "ESP32") r.chip = old.chip.empty() ? r.chip : old.chip;
    if (r.board.empty()) r.board = old.board;
    if (r.detail.empty()) r.detail = old.detail;
    return r;
}

std::string when_text(std::time_t when)
{
    if (!when) return std::string();
    struct tm tm;
    localtime_r(&when, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string confidence_text(const Identity &id)
{
    const char *how = id.level == 3 ? "asked" : id.level == 2 ? "after a reset" : "listened";
    return std::string(confidence_word(id.confidence)) + " (" + how + ")";
}

std::vector<CardRow> card_rows(const Identity &id, const PortInfo &port, Probe probe, int in_use)
{
    std::vector<CardRow> rows;
    const auto info = [&rows](const char *key, const std::string &value) {
        if (!value.empty()) rows.push_back(CardRow{key, value, CardAction::None});
    };
    std::string device = port.title;
    if (!port.chip.empty() && device.find(port.chip) == std::string::npos) device += " (" + port.chip + ")";
    info("Device", device);
    const std::string behind = behind_text(id);
    info("Behind it", behind);
    if (id.found() && behind.find(id.chip) == std::string::npos) info("Chip", id.chip);
    info("Firmware", firmware_text(id));
    info("Board", id.board);
    info("Details", id.detail);
    info("Framing", id.kind == Kind::Binary || id.kind == Kind::Klipper ? id.framing : std::string());
    if (id.found()) info("Confidence", confidence_text(id));
    info("Found", when_text(id.when));
    if (!id.boot.empty())
        rows.push_back(CardRow{"Boot output", std::to_string(id.boot.size()) + (id.boot.size() == 1 ? " line" : " lines"),
                               CardAction::Boot});
    if (!id.evidence.empty())
        rows.push_back(CardRow{"Evidence", std::to_string(id.evidence.size()) + (id.evidence.size() == 1 ? " line" : " lines"),
                               CardAction::Evidence});
    int baud = 0;
    std::string reason;
    if (identity_recommendation(id, &baud, &reason)) {
        const std::string settings = std::to_string(baud) + " 8N1";
        info("Recommended", settings);
        rows.push_back(CardRow{"Apply settings", baud == in_use ? std::string("In use") : settings, CardAction::Apply});
    }
    if (!id.found() && bare_adapter(port)) {
        rows.push_back(CardRow{kAdapterNote, std::string(), CardAction::None});
        if (probe != Probe::Reset) rows.push_back(CardRow{"Reset and listen", "resets", CardAction::Reset});
        rows.push_back(CardRow{"Ask the device", "sends", CardAction::Ask});
    }
    rows.push_back(CardRow{"Run again", probe_name(probe), CardAction::Again});
    rows.push_back(CardRow{"Copy summary", "to a log file", CardAction::Copy});
    return rows;
}

std::string probe_caption(Probe p, ResetStyle style)
{
    switch (p) {
    case Probe::Listen: return "Listens at each speed; sends nothing, no reset";
    case Probe::Reset:
        return style == ResetStyle::None ? "No reset line: listens while you press reset"
                                         : "Resets the board (DTR/RTS), reads its boot";
    default: return "Asks the device a question you choose";
    }
}

std::string confirm_title(Probe p, ResetStyle style)
{
    if (p == Probe::Reset) return style == ResetStyle::None ? "Listen while you reset it?" : "Reset the board?";
    return std::string("Ask: ") + probe_name(p) + "?";
}

std::string confirm_text(Probe p, ResetStyle style, int baud)
{
    if (p == Probe::Reset) {
        if (style == ResetStyle::None)
            return "This board's own USB has no reset line (an RP2040 or a similar board). After Start, press the "
                   "board's reset button if it has one; the app listens for a few seconds at " +
                   std::to_string(baud) + ". Nothing is sent.";
        return "This resets the board (DTR/RTS pulse); a running program restarts. Then the app listens 3 s at " +
               std::to_string(baud) + " and reads what it prints. Nothing is sent; the lines are put back as they "
               "were.";
    }
    (void)style;
    return probe_sends_text(p); /* each says what it sends, and whether it restarts the board */
}

std::string confirm_button(Probe p, ResetStyle style)
{
    if (p == Probe::Reset) return style == ResetStyle::None ? "Start" : "Reset and listen";
    return "Send";
}

std::string ask_value(Probe p)
{
    switch (p) {
    case Probe::MicroPython: return "Ctrl-C, Ctrl-B";
    case Probe::At: return "AT, ATI";
    case Probe::Marlin: return "M115";
    case Probe::Grbl: return "$I";
    case Probe::Stk500: return "STK500";
    case Probe::EspRom: return "chip, MAC";
    default: return std::string();
    }
}

std::string ask_caption(Probe p)
{
    switch (p) {
    case Probe::MicroPython: return "Stops its program; prints the REPL banner";
    case Probe::At: return "A modem answers OK and its model";
    case Probe::Marlin: return "A printer answers its firmware and machine";
    case Probe::Grbl: return "GRBL answers its version";
    case Probe::Stk500: return "Resets; reads the AVR chip from its boot loader";
    case Probe::EspRom: return "Resets into the ROM; reads the chip and MAC";
    default: return std::string();
    }
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
