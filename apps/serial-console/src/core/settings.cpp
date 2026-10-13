/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: line settings and the settings file (see settings.hpp).
 */
#include "settings.hpp"

#include "fsutil.hpp"

#include <cstdlib>
#include <sstream>

namespace sc {
namespace {

// branding-allow-begin: the launcher's config folder, a technical name inherited from upstream (launcher NAMING.md)
constexpr const char *kConfigFolder = "cardputerzero";
// branding-allow-end
constexpr const char *kConfigFile = "serial-console.conf";

std::string trim(const std::string &text)
{
    const std::size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return std::string();
    const std::size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

bool parse_int(const std::string &text, int &out)
{
    if (text.empty() || text.size() > 9) return false;
    for (const char c : text)
        if (c < '0' || c > '9') return false;
    out = std::atoi(text.c_str());
    return true;
}

bool parse_onoff(const std::string &text, bool &out)
{
    if (text == "on" || text == "1") out = true;
    else if (text == "off" || text == "0") out = false;
    else return false;
    return true;
}

bool parse_ending(const std::string &text, Ending &out)
{
    if (text == "CR") out = Ending::CR;
    else if (text == "LF") out = Ending::LF;
    else if (text == "CRLF") out = Ending::CRLF;
    else return false;
    return true;
}

const char *ending_key(Ending e)
{
    return e == Ending::LF ? "LF" : e == Ending::CRLF ? "CRLF" : "CR";
}

/* One key of a port section (both formats use the same names and values). */
void apply_port_key(PortSettings &s, const std::string &key, const std::string &value)
{
    int n = 0;
    bool b = false;
    Frame f;
    Ending e;
    if (key == "baud") {
        if (parse_int(value, n) && (n == 0 || valid_baud(n))) s.baud = n;
    } else if (key == "frame") {
        if (parse_frame(value, f)) s.frame = f.text();
    } else if (key == "echo") {
        if (parse_onoff(value, b)) s.echo = b;
    } else if (key == "ending") {
        if (parse_ending(value, e)) s.ending = e;
    } else if (key == "backspace") {
        if (value == "DEL") s.backspace = Backspace::Del;
        else if (value == "BS") s.backspace = Backspace::Bs;
    } else if (key == "log") {
        if (parse_onoff(value, b)) s.log = b;
    } else if (key == "detect_enter") {
        if (parse_onoff(value, b)) s.detect_enter = b;
    }
}

} // namespace

std::string Frame::text() const
{
    return std::to_string(data) + parity + std::to_string(stop);
}

bool parse_frame(const std::string &text, Frame &out)
{
    if (text.size() != 3) return false;
    if (text[0] < '5' || text[0] > '8') return false;
    if (text[1] != 'N' && text[1] != 'E' && text[1] != 'O') return false;
    if (text[2] != '1' && text[2] != '2') return false;
    out.data = text[0] - '0';
    out.parity = text[1];
    out.stop = text[2] - '0';
    return true;
}

const std::vector<std::string> &frame_list()
{
    static const std::vector<std::string> list = {"8N1", "7E1", "8E1", "8O1", "7O1", "8N2"};
    return list;
}

const std::vector<int> &speed_list()
{
    static const std::vector<int> list = {1200,  2400,   4800,   9600,   19200,  38400,
                                          57600, 74880, 115200, 230400, 460800, 921600};
    return list;
}

bool valid_baud(int baud)
{
    return baud >= 50 && baud <= 4000000;
}

const char *ending_name(Ending ending)
{
    return ending == Ending::LF ? "LF" : ending == Ending::CRLF ? "CR LF" : "CR";
}

std::string ending_bytes(Ending ending)
{
    return ending == Ending::LF ? "\n" : ending == Ending::CRLF ? "\r\n" : "\r";
}

const char *backspace_name(Backspace bs)
{
    return bs == Backspace::Bs ? "BS (0x08)" : "DEL (0x7F)";
}

char backspace_byte(Backspace bs)
{
    return bs == Backspace::Bs ? '\x08' : '\x7f';
}

bool PortSettings::operator==(const PortSettings &o) const
{
    return baud == o.baud && frame == o.frame && echo == o.echo && ending == o.ending && backspace == o.backspace &&
           log == o.log && detect_enter == o.detect_enter;
}

PortSettings Config::settings(const std::string &id) const
{
    const auto it = ports.find(id);
    return it == ports.end() ? PortSettings() : it->second;
}

Identity Config::identity(const std::string &id) const
{
    const auto it = idents.find(id);
    return it == idents.end() ? Identity() : it->second;
}

void Config::set_identity(const std::string &id, const Identity &identity)
{
    if (!ports.count(id)) ports[id] = PortSettings(); /* its section */
    if (identity.kind == Kind::None) idents.erase(id);
    else idents[id] = identity;
}

std::string Config::serialize() const
{
    std::ostringstream out;
    out << "# Serial Console settings, written by the app: the global keys, then one [port <id>] section per port\n";
    out << "v=2\n";
    out << "onboard=" << (show_onboard ? 1 : 0) << "\n";
    if (!last_port.empty()) out << "last=" << last_port << "\n";
    for (const auto &entry : ports) {
        if (entry.first.empty() || entry.first.find_first_of("\n]") != std::string::npos) continue;
        const PortSettings &s = entry.second;
        out << "\n[port " << entry.first << "]\n";
        out << "baud=" << s.baud << "\n";
        out << "frame=" << s.frame << "\n";
        out << "echo=" << (s.echo ? "on" : "off") << "\n";
        out << "ending=" << ending_key(s.ending) << "\n";
        out << "backspace=" << (s.backspace == Backspace::Bs ? "BS" : "DEL") << "\n";
        out << "log=" << (s.log ? "on" : "off") << "\n";
        out << "detect_enter=" << (s.detect_enter ? "on" : "off") << "\n";
        const auto ident = idents.find(entry.first);
        if (ident != idents.end())
            for (const auto &kv : identity_to_keys(ident->second)) out << kv.first << "=" << kv.second << "\n";
    }
    return out.str();
}

Config Config::parse(const std::string &text, bool *migrated)
{
    Config config;
    if (migrated) *migrated = false;
    std::istringstream in(text);
    std::string line;
    bool v2 = false;
    std::string section;      /* the port id of the current section ("" = global) */
    PortSettings old;         /* a 0.1.0 file: its one set of settings */
    std::string old_port;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            const std::string head = "[port ";
            const std::size_t close = line.rfind(']');
            section.clear();
            if (line.compare(0, head.size(), head) == 0 && close != std::string::npos && close > head.size()) {
                section = trim(line.substr(head.size(), close - head.size()));
                if (!section.empty()) config.ports[section] = config.settings(section);
            }
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (!section.empty()) {
            if (key.compare(0, 3, "id_") == 0) {
                Identity &id = config.idents[section];
                identity_apply_key(id, key, value);
                continue;
            }
            apply_port_key(config.ports[section], key, value);
            continue;
        }
        bool b = false;
        if (key == "v") {
            v2 = value == "2";
        } else if (key == "onboard" || key == "all") {
            if (parse_onoff(value, b)) config.show_onboard = b;
        } else if (key == "last") {
            config.last_port = value;
        } else if (key == "port") { /* 0.1.0: the by-id name or the /dev path of the last port */
            old_port = value.size() > 5 && value.compare(0, 5, "/dev/") == 0 ? fs::base_name(value) : value;
        } else {
            apply_port_key(old, key, value);
        }
    }
    for (auto it = config.idents.begin(); it != config.idents.end();) { /* a section with id_ keys but no kind */
        if (it->second.kind == Kind::None) it = config.idents.erase(it);
        else ++it;
    }
    if (!v2 && !old_port.empty()) {
        if (old.baud == 115200) old.baud = 0; /* 0.1.0's default, not a choice: follow the recommendation */
        config.ports[old_port] = old;
        config.last_port = old_port;
        if (migrated) *migrated = true;
    }
    return config;
}

std::string config_path()
{
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    std::string base;
    if (xdg && xdg[0] == '/') base = xdg;
    else if (home && home[0]) base = std::string(home) + "/.config";
    else return std::string();
    return base + "/" + kConfigFolder + "/" + kConfigFile;
}

Config load_config(const std::string &path, bool *migrated)
{
    bool ok = false;
    const std::string text = path.empty() ? std::string() : fs::read_all(path, &ok);
    if (!ok) {
        if (migrated) *migrated = false;
        return Config();
    }
    return Config::parse(text, migrated);
}

bool save_config(const std::string &path, const Config &config)
{
    if (path.empty()) return false;
    fs::make_dirs(fs::parent(path));
    return fs::write_atomic(path, config.serialize());
}

std::string log_dir()
{
    const char *home = std::getenv("HOME");
    return std::string(home && home[0] ? home : "/tmp") + "/serial-logs";
}

std::string log_file_name(const std::string &tty, std::time_t when)
{
    struct tm tm;
    localtime_r(&when, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    return tty + "-" + stamp + ".log";
}

} // namespace sc
