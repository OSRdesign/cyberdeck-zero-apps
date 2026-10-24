/*
 * SPDX-License-Identifier: MIT
 *
 * The CLI page without LVGL (see cli_logic.hpp).
 */

#include "cli_logic.hpp"

#include <algorithm>

namespace meshhop {

namespace {

bool ends_with(const std::string &s, const std::string &tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

} // namespace

/* ------------------------------------------------------------------ the output */

CliScreen::CliScreen(size_t max_lines, size_t max_cols) : buf_(max_lines, max_cols) {}

void CliScreen::feed(const std::string &bytes)
{
    size_t i = 0;
    // after a password: meshcore-cli's echo of it (prompt_toolkit's dumb mode prints every character it reads) is dropped up to
    // the line end, which prompt_toolkit writes when it accepts the line
    while (masking_ && i < bytes.size()) {
        const char c = bytes[i];
        if (c == '\r' || c == '\n') {
            masking_ = false;
            if (!shown_.empty()) buf_.feed(shown_.data(), shown_.size());   // 0.7.2: the short text of Mesh Hop's own line
            shown_.clear();
            break;
        }
        ++i;
        ++masked_;
        if (++mask_run_ >= kCliMaskMax) {                     // never hide the output for long (a prompt text upstream changed)
            masking_ = false;
            shown_.clear();
        }
    }
    if (i < bytes.size()) buf_.feed(bytes.data() + i, bytes.size() - i);
}

void CliScreen::note(const std::string &text)
{
    masking_ = false;
    shown_.clear();
    if (!current_line().empty()) buf_.feed("\r\n");
    buf_.feed(text + "\r\n");
}

void CliScreen::clear()
{
    buf_.clear();
    masking_ = false;
    mask_run_ = 0;
    shown_.clear();
}

void CliScreen::sent_line_shown(const std::string &shown)
{
    masking_ = true;
    mask_run_ = 0;
    shown_ = shown;
}

void CliScreen::sent_line(bool password)
{
    if (password) {
        masking_ = true;
        mask_run_ = 0;
    }
}

bool is_password_prompt(const std::string &line)
{
    if (line == "Password: ") return true;
    return line.rfind("Password for ", 0) == 0 && line.size() > 15 && ends_with(line, ": ");
}

bool CliScreen::password_prompt() const
{
    return !masking_ && is_password_prompt(current_line());
}

bool CliScreen::at_prompt() const
{
    const std::string l = current_line();
    return !masking_ && l.size() > 2 && ends_with(l, "> ") && !is_password_prompt(l);
}

std::string CliScreen::current_line() const
{
    return buf_.line(buf_.end() - 1);
}

std::string CliScreen::text() const
{
    std::string out;
    for (uint64_t i = buf_.first(); i < buf_.end(); ++i) {
        if (i != buf_.first()) out += '\n';
        out += buf_.line(i);
    }
    return out;
}

/* ------------------------------------------------------------------ history */

void CliHistory::add(const std::string &line)
{
    if (!line.empty() && (entries_.empty() || entries_.back() != line)) {
        entries_.push_back(line);
        if (entries_.size() > cap_) entries_.erase(entries_.begin());
    }
    at_ = entries_.size();
    draft_.clear();
}

bool CliHistory::up(const std::string &current, std::string &out)
{
    if (entries_.empty() || at_ == 0) return false;
    if (at_ >= entries_.size()) {
        at_ = entries_.size();
        draft_ = current;
    }
    --at_;
    out = entries_[at_];
    return true;
}

bool CliHistory::down(std::string &out)
{
    if (at_ >= entries_.size()) return false;
    ++at_;
    out = at_ == entries_.size() ? draft_ : entries_[at_];
    return true;
}

/* ------------------------------------------------------------------ lines, inserts, endings */

std::string cli_line_bytes(const std::string &typed)
{
    std::string out;
    out.reserve(typed.size() + 1);
    for (const char ch : typed) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) continue;
        out += ch;
    }
    out += '\n';
    return out;
}

std::string cli_insert(const std::string &entry, size_t cursor, const std::string &what, size_t *cursor_after)
{
    cursor = std::min(cursor, entry.size());
    const std::string before = entry.substr(0, cursor);
    const bool space = !before.empty() && before.back() != ' ';
    const std::string out = before + (space ? " " : "") + what + entry.substr(cursor);
    if (cursor_after) *cursor_after = cursor + (space ? 1 : 0) + what.size();
    return out;
}

std::string cli_exit_text(int exit_code, int term_signal, bool python_found)
{
    if (term_signal > 0) return "meshcore-cli was stopped (signal " + std::to_string(term_signal) + ")";
    switch (exit_code) {
    case 0: return "meshcore-cli ended";
    case 3: return "meshcore-cli does not match Mesh Hop's wrapper (mccli.py, exit 3): update Mesh Hop";
    case 126: return "meshcore-cli could not start (exit 126: not allowed to run)";
    case 127:
        if (!python_found) return "python3 is missing: install it (sudo apt install python3), then open CLI again";
        return "meshcore-cli could not start (exit 127)";
    default: return "meshcore-cli ended (exit " + std::to_string(exit_code) + ")";
    }
}

std::string cli_relay_end_text(const std::string &reason)
{
    if (reason == "the link to the node was lost") return "The link to the node was lost";
    if (reason == "the console did not connect") return "meshcore-cli did not connect to Mesh Hop in time";
    if (reason == "the console closed its connection") return "meshcore-cli closed its connection";
    if (reason.empty()) return "The session ended";
    std::string r = reason;
    if (r[0] >= 'a' && r[0] <= 'z') r[0] = static_cast<char>(r[0] - 'a' + 'A');
    return r;
}

const char *cli_phase_name(CliPhase p)
{
    switch (p) {
    case CliPhase::Off: return "off";
    case CliPhase::Waiting: return "waiting";
    case CliPhase::Starting: return "starting";
    case CliPhase::Running: return "running";
    case CliPhase::Ended: return "ended";
    }
    return "?";
}

/* ------------------------------------------------------------------ the Help page
 * Every name below was checked against meshcore-cli v1.6.6 (meshcore_cli.py: the chat loop, process_contact_chat_line, the
 * repeater completion list, usage()) and, for what a repeater does with it, the firmware (MeshCore 81dbae5: CommonCLI.cpp,
 * simple_repeater MyMesh.cpp). */
const std::vector<HelpItem> &cli_help_items()
{
    static const std::vector<HelpItem> items = {
        {HelpKind::Section, "How it works", "", ""},
        {HelpKind::Insert, "to <name>", "Picks a repeater, room or contact. On a repeater or room your lines are sent to it as commands, "
                                        "on a chat contact as messages. to / goes back to your own node. (Contacts > a repeater > More > "
                                        "Open in CLI types it for you.)", "to "},
        {HelpKind::Insert, "/<command>", "Runs the command on your own node while a contact is picked.", "/"},
        {HelpKind::Insert, "?<command>", "Shows the help of a command, for example ?get, ?set, ?trace.", "?"},
        {HelpKind::Insert, "login", "On a repeater or room, once: asks its password (typed as *) and keeps it for that repeater, "
                                    "so the next logins ask nothing. fp forgets a wrong one.", "login"},
        {HelpKind::Info, "Manage", "Use Manage on the Contacts page to log in: it also works here."},
        {HelpKind::Info, "PgUp, PgDn", "Scroll the output back and forth (or drag it).", ""},
        {HelpKind::Info, "Ctrl+C", "Sends an interrupt: at its prompt meshcore-cli ends (open CLI again to restart it).", ""},
        {HelpKind::Info, "Esc", "Leaves the CLI: meshcore-cli stops and Mesh Hop takes the node back.", ""},

        {HelpKind::Section, "Your node", "", ""},
        {HelpKind::Insert, "infos", "Information about your node (i).", "infos"},
        {HelpKind::Insert, "ver", "Its firmware version (v).", "ver"},
        {HelpKind::Insert, "clock", "Its clock.", "clock"},
        {HelpKind::Insert, "clock sync", "Sets its clock to the deck's time (st).", "clock sync"},
        {HelpKind::Insert, "advert", "Sends an advert (a).", "advert"},
        {HelpKind::Insert, "floodadv", "Sends a flood advert.", "floodadv"},
        {HelpKind::Insert, "contacts", "The contact list (lc).", "contacts"},
        {HelpKind::Insert, "msg <name> <text>", "Sends a message to a contact (m).", "msg "},
        {HelpKind::Insert, "chan <nb> <text>", "Sends a message to channel number nb (ch).", "chan "},
        {HelpKind::Insert, "node_discover <type>", "Discovers nodes by their type (nd).", "node_discover "},
        {HelpKind::Insert, "get <param>", "Reads a setting; get help lists them.", "get "},
        {HelpKind::Insert, "set <param> <value>", "Changes a setting; set help lists them.", "set "},
        {HelpKind::Insert, "reboot", "Reboots your node: the link drops and the CLI ends.", "reboot"},

        {HelpKind::Section, "Repeaters and rooms (after to <name>)", "", ""},
        {HelpKind::Insert, "login", "Logs in (see How it works).", "login"},
        {HelpKind::Insert, "logout", "Logs out.", "logout"},
        {HelpKind::Insert, "cmd <command>", "Sends a command to it (no acknowledgement); a plain line does the same.", "cmd "},
        {HelpKind::Insert, "rs", "Its status (req_status).", "rs"},
        {HelpKind::Insert, "rn", "Its neighbours (req_neighbours).", "rn"},
        {HelpKind::Insert, "rt", "Its telemetry (req_telemetry).", "rt"},
        {HelpKind::Insert, "trace", "Runs a trace to it.", "trace"},
        {HelpKind::Insert, "ver", "Its firmware version.", "ver"},
        {HelpKind::Insert, "board", "Its board.", "board"},
        {HelpKind::Insert, "clock", "Its clock; clock sync sets it to your time (a clock only goes forward).", "clock"},
        {HelpKind::Insert, "get name", "Its name; set name <name> changes it.", "get name"},
        {HelpKind::Insert, "get radio", "Its radio settings; set radio <freq,bw,sf,cr> changes them (see Careful).", "get radio"},

        {HelpKind::Section, "Careful: no confirmation", "", ""},
        {HelpKind::Warning, "set prv.key", "Over LoRa it replaces the repeater's identity key, effective at its next reboot. There is no way back.", ""},
        {HelpKind::Warning, "poweroff, reboot, clkreboot", "Take the repeater off the air.", ""},
        {HelpKind::Warning, "start ota", "Puts the repeater into firmware-update mode.", ""},
        {HelpKind::Warning, "set radio, tempradio", "Can move the repeater off your network's frequency: you lose contact with it.", ""},
        {HelpKind::Warning, "password", "Changes the admin password.", ""},
        {HelpKind::Warning, "setperm", "Changes who may administer the repeater.", ""},
        {HelpKind::Warning, "No acknowledgement", "A remote command gets no delivery acknowledgement: never retry one blindly.", ""},
        {HelpKind::Info, "Cable only", "get prv.key, erase, stats-* and set freq only work over a cable, not over LoRa (the firmware's "
                                       "get acl too: meshcore-cli turns get acl into req_acl, which works over LoRa).", ""},

        {HelpKind::Section, "All commands", "", ""},
        {HelpKind::Usage, "All commands", "meshcore-cli -h: every command and its shortcut.", ""},
    };
    return items;
}

} // namespace meshhop
