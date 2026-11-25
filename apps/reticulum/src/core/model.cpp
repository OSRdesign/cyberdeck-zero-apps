/*
 * SPDX-License-Identifier: MIT
 *
 * What the app knows about the bridge, in plain words (see model.hpp).
 */
#include "model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <limits.h>

namespace ret {

namespace {

std::string scalar_text(const json::Value &v)
{
    switch (v.kind()) {
    case json::Value::Kind::String: return v.as_string();
    case json::Value::Kind::Number: return json::number_text(v.as_number());
    case json::Value::Kind::Bool: return v.as_bool() ? "yes" : "no";
    default: return std::string();
    }
}

bool parse_option(const json::Value &o, OptionDef &d)
{
    if (!o.is_object() || !o["name"].is_string()) return false;
    d.name = o["name"].as_string();
    const std::string kind = o["kind"].as_string();
    if (kind == "int") d.kind = OptKind::Int;
    else if (kind == "float") d.kind = OptKind::Float;
    else if (kind == "bool") d.kind = OptKind::Bool;
    else if (kind == "list") d.kind = OptKind::List;
    else d.kind = OptKind::Str;
    if (o["min"].is_number()) {
        d.has_min = true;
        d.min = o["min"].as_number();
    }
    if (o["max"].is_number()) {
        d.has_max = true;
        d.max = o["max"].as_number();
    }
    for (const json::Value &c : o["choices"].items()) d.choices.push_back(scalar_text(c));
    d.unit = o["unit"].as_string();
    if (!o["default"].is_null()) {
        d.has_default = true;
        d.def = scalar_text(o["default"]);
    }
    d.required = o["required"].as_bool(false);
    d.secret = o["secret"].as_bool(false);
    d.device = o["device"].as_bool(false);
    return true;
}

bool parse_options(const json::Value &list, std::vector<OptionDef> &out)
{
    for (const json::Value &o : list.items()) {
        OptionDef d;
        if (parse_option(o, d)) out.push_back(std::move(d));
    }
    return true;
}

std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string cap_first(std::string s)
{
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::string base_name(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool starts_with(const std::string &s, const std::string &p)
{
    return s.compare(0, p.size(), p) == 0;
}

} // namespace

const TypeDef *Catalog::find(const std::string &type) const
{
    for (const TypeDef &t : types)
        if (t.type == type) return &t;
    return nullptr;
}

bool parse_catalog(const json::Value &result, Catalog &out)
{
    if (!result["types"].is_array()) return false;
    Catalog c;
    for (const json::Value &t : result["types"].items()) {
        TypeDef d;
        d.type = t["type"].as_string();
        if (d.type.empty()) continue;
        d.supported = t["supported"].as_bool(true);
        d.port_lock = t["port_lock"].as_bool(false);
        d.reason = t["reason"].as_string();
        parse_options(t["options"], d.options);
        c.types.push_back(std::move(d));
    }
    parse_options(result["common"], c.common);
    c.loaded = !c.types.empty();
    out = std::move(c);
    return true;
}

std::string Iface::value(const std::string &key, bool *present) const
{
    for (const auto &kv : options)
        if (kv.first == key) {
            if (present) *present = !kv.second.empty();
            return kv.second;
        }
    if (present) *present = false;
    return std::string();
}

bool Iface::secret_set(const std::string &key) const
{
    return std::find(secrets_set.begin(), secrets_set.end(), key) != secrets_set.end();
}

bool parse_ifaces(const json::Value &result, std::vector<Iface> &out)
{
    if (!result["interfaces"].is_array()) return false;  // vocab-ok: the bridge's own word, never shown
    std::vector<Iface> list;
    for (const json::Value &v : result["interfaces"].items()) {  // vocab-ok: the bridge's own word, never shown
        Iface i;
        i.name = v["name"].as_string();
        if (i.name.empty()) continue;
        i.type = v["type"].as_string();
        i.enabled = v["enabled"].as_bool(false);
        i.outgoing = v["outgoing"].as_bool(false);
        i.listen_only = v["listen_only"].as_string();
        if (i.listen_only != "silent" && i.listen_only != "handshake" && i.listen_only != "none")
            i.listen_only = i.type == "AutoInterface" ? "none" : "silent"; /* an older service does not say */
        i.online = v["online"].as_bool(false);
        i.supported = v["supported"].as_bool(true);
        for (const auto &kv : v["options"].members()) i.options.emplace_back(kv.first, scalar_text(kv.second));
        for (const json::Value &s : v["secrets_set"].items()) i.secrets_set.push_back(s.as_string());
        for (const json::Value &s : v["missing"].items()) i.missing.push_back(s.as_string());
        i.lock = v["lock"].as_string();
        i.error = v["error"].as_string();
        const json::Value &st = v["stats"];
        if (st["channel_load_short"].is_number()) {
            i.has_channel = true;
            i.channel_load = st["channel_load_short"].as_number();
        }
        if (st["noise_floor"].is_number()) {
            i.has_noise = true;
            i.noise = st["noise_floor"].as_number();
        }
        list.push_back(std::move(i));
    }
    out = std::move(list);
    return true;
}

bool parse_deck(const json::Value &s, Deck &out)
{
    if (!s.is_object()) return false;
    Deck d;
    d.known = true;
    d.address = s["lxmf_address"].as_string();  // vocab-ok: the bridge's own word, never shown
    d.display_name = s["display_name"].as_string();
    d.total = static_cast<int>(s["interfaces_total"].as_int(0));  // vocab-ok: the bridge's own word, never shown
    d.up = static_cast<int>(s["interfaces_up"].size());  // vocab-ok: the bridge's own word, never shown
    d.uptime = s["uptime"].as_int(0);
    d.unread = static_cast<int>(s["unread"].as_int(0));
    d.alerts = s["alerts"].as_bool(false);
    out = std::move(d);
    return true;
}

bool parse_heard(const json::Value &event, Heard &out)
{
    const json::Value &d = event.has("data") ? event["data"] : event;
    if (!d.is_object() || !d["destination"].is_string()) return false;  // vocab-ok: the bridge's own word, never shown
    Heard h;
    h.time = d["time"].as_number(0);
    h.destination = d["destination"].as_string();  // vocab-ok: the bridge's own word, never shown
    h.name = d["name"].as_string();
    h.kind = d["kind"].as_string();
    h.iface = d["interface"].as_string();  // vocab-ok: the bridge's own word, never shown
    h.hops = d["hops"].is_number() ? static_cast<int>(d["hops"].as_int(-1)) : -1;
    out = std::move(h);
    return true;
}

void HeardList::add(const Heard &h)
{
    for (Heard &e : items_)
        if (e.destination == h.destination) {
            if (h.time >= e.time) e = h;
            std::stable_sort(items_.begin(), items_.end(), [](const Heard &a, const Heard &b) { return a.time > b.time; });
            return;
        }
    items_.push_back(h);
    std::stable_sort(items_.begin(), items_.end(), [](const Heard &a, const Heard &b) { return a.time > b.time; });
    if (items_.size() > 64) items_.resize(64);
}

int HeardList::count_within(double now, double seconds) const
{
    int n = 0;
    for (const Heard &h : items_)
        if (now - h.time <= seconds) ++n;
    return n;
}

/* ------------------------------------------------------------------ words */

namespace {

const std::vector<KindInfo> &kind_table()
{
    static const std::vector<KindInfo> t = {
        {"AutoInterface", "Wi-Fi / LAN", "nearby decks", "Settings", "", true},
        {"TCPClientInterface", "Internet", "a server address", "Settings", "", true},
        {"RNodeInterface", "LoRa radio", "an RNode on USB", "Radio: you choose", "Check your local rules", true},
        {"BackboneInterface", "Fast internet link", "a server address or a port", "Settings", "", false},
        {"TCPServerInterface", "Internet server", "let others connect to you", "Settings", "", false},
        {"UDPInterface", "UDP on the LAN", "broadcast on the local network", "Settings", "", false},
        {"I2PInterface", "I2P", "an anonymous network", "Settings", "", false},
        {"SerialInterface", "Serial link", "a wired serial port", "Settings", "", false},
        {"PipeInterface", "Program pipe", "a program on this deck", "Settings", "", false},
        {"KISSInterface", "Radio modem (KISS)", "a modem on a serial port", "Radio: you choose", "Check your local rules", false},
        {"AX25KISSInterface", "Packet radio (AX.25)", "a modem with a call sign", "Radio: you choose", "Check your local rules", false},
    };
    return t;
}

} // namespace

const KindInfo &kind_info(const std::string &type)
{
    for (const KindInfo &k : kind_table())
        if (k.type == type) return k;
    static KindInfo other;
    other.type = type;
    other.title = type;
    const std::size_t at = other.title.rfind("Interface");  // vocab-ok: the bridge's own word, never shown
    if (at != std::string::npos && at > 0) other.title.erase(at);
    if (other.title.empty()) other.title = "Other";
    other.summary = "another kind";
    other.section = "Settings";
    other.note.clear();
    other.main = false;
    return other;
}

std::vector<const KindInfo *> kinds_main()
{
    std::vector<const KindInfo *> out;
    for (const KindInfo &k : kind_table())
        if (k.main) out.push_back(&k);
    return out;
}

std::vector<const KindInfo *> kinds_more(const Catalog &catalog)
{
    std::vector<const KindInfo *> out;
    for (const KindInfo &k : kind_table()) {
        if (k.main) continue;
        const TypeDef *t = catalog.find(k.type);
        if (t && t->supported) out.push_back(&k);
    }
    return out;
}

std::string conn_title(const Iface &i)
{
    if (i.name == "Default Interface" && i.type == "AutoInterface") return "Wi-Fi LAN";  // vocab-ok: the bridge's own word, never shown
    return i.name;
}

std::string suggest_name(const std::string &type, const std::vector<Iface> &existing)
{
    std::string base;
    for (const char c : kind_info(type).title)
        if (c != '/') base += c;
    std::string squeezed;
    for (const char c : base) {
        if (c == ' ' && (squeezed.empty() || squeezed.back() == ' ')) continue;
        squeezed += c;
    }
    while (!squeezed.empty() && squeezed.back() == ' ') squeezed.pop_back();
    if (squeezed.empty()) squeezed = "Connection";
    auto taken = [&](const std::string &n) {
        for (const Iface &i : existing)
            if (i.name == n || conn_title(i) == n) return true;
        return false;
    };
    if (!taken(squeezed)) return squeezed;
    for (int n = 2; n < 100; ++n) {
        const std::string c = squeezed + " " + std::to_string(n);
        if (!taken(c)) return c;
    }
    return squeezed;
}

State conn_state(const Iface &i)
{
    State s;
    if (!i.supported) {
        s.word = i.enabled ? "on, set by hand" : "set by hand";
        s.tone = Tone::Muted;
        return s;
    }
    if (!i.enabled) {
        s.word = "off";
        s.tone = Tone::Muted;
        return s;
    }
    if (!i.error.empty()) {
        s.word = "problem";
        s.tone = Tone::Bad;
        return s;
    }
    if (i.online) {
        s.word = i.outgoing ? "on, sending" : "on, listen only";
        s.tone = i.outgoing ? Tone::Warn : Tone::Ok;
        return s;
    }
    s.word = "on, not connected";
    s.tone = Tone::Warn;
    return s;
}

bool has_listen_choice(const Iface &i)
{
    return i.listen_only != "none";
}

std::string listen_note(const Iface &i)
{
    if (i.listen_only == "handshake")
        return "Listen only still opens the link: a short hello, no messages, no name.";
    return std::string();
}

std::string switch_on_question(const Iface &i)
{
    (void)i;
    return "While it is on, this deck makes itself known to others on it and may send. It cannot only listen. "
           "Messages and your name still go out only when you send them.";
}

int count_on(const std::vector<Iface> &ifaces)
{
    int n = 0;
    for (const Iface &i : ifaces)
        if (i.enabled) ++n;
    return n;
}

std::string connections_summary(const std::vector<Iface> &ifaces)
{
    const int n = count_on(ifaces);
    if (n == 0) return "all off";
    return std::to_string(n) + " on";
}

std::string heard_summary(int heard_count)
{
    if (heard_count <= 0) return "nobody heard yet";
    return std::to_string(heard_count) + " heard";
}

std::string option_label(const std::string &name)
{
    static const std::map<std::string, std::string> table = {
        {"frequency", "Frequency"},
        {"bandwidth", "Bandwidth"},
        {"txpower", "Transmit power"},
        {"spreadingfactor", "Spreading factor"},
        {"codingrate", "Coding rate"},
        {"airtime_limit_short", "Airtime limit, short"},
        {"airtime_limit_long", "Airtime limit, long"},
        {"port", "Port"},
        {"target_host", "Server address"},
        {"target_port", "Server port"},
        {"listen_ip", "Listen address"},
        {"listen_port", "Listen port"},
        {"forward_ip", "Forward address"},
        {"forward_port", "Forward port"},
        {"id_callsign", "Call sign"},
        {"id_interval", "Call sign every"},
        {"flow_control", "Flow control"},
        {"speed", "Speed"},
        {"databits", "Data bits"},
        {"stopbits", "Stop bits"},
        {"parity", "Parity"},
        {"mode", "Mode"},
        {"network_name", "Network name"},
        {"passphrase", "Passphrase"},
        {"ifac_size", "Passphrase size"},
        {"announce_cap", "Sharing limit"},  // vocab-ok: the bridge's own word, never shown
        {"bitrate", "Bit rate"},
        {"gravity", "Priority"},
        {"bootstrap_only", "Bootstrap only"},
        {"kiss_framing", "KISS framing"},
        {"fixed_mtu", "Packet size"},
        {"i2p_tunneled", "Through I2P"},
        {"group_id", "Group name"},
        {"multicast_address_type", "Group address type"},
        {"devices", "Use only these devices"},
        {"ignored_devices", "Skip these devices"},
        {"discovery_scope", "Reach"},
        {"discovery_port", "Discovery port"},
        {"data_port", "Data port"},
        {"listen_on", "Listen address"},
        {"remote", "Remote address"},
        {"prefer_ipv6", "Prefer IPv6"},
        {"command", "Command"},
        {"respawn_delay", "Restart after"},
        {"preamble", "Preamble"},
        {"txtail", "Transmit tail"},
        {"persistence", "Persistence"},
        {"slottime", "Slot time"},
        {"callsign", "Call sign"},
        {"ssid", "SSID"},
        {"connectable", "Let others connect"},
        {"peers", "Peers"},
    };
    const auto it = table.find(name);
    if (it != table.end()) return it->second;
    std::string s = name;
    for (char &c : s)
        if (c == '_') c = ' ';
    return cap_first(s);
}

std::string value_label(const std::string &option, const std::string &value)
{
    static const std::map<std::string, std::map<std::string, std::string>> table = {
        {"mode",
         {{"full", "Normal"},
          {"gateway", "Gateway"},
          {"access_point", "Access point"},
          {"roaming", "Roaming"},
          {"boundary", "Boundary"},
          {"internal", "Internal"}}},
        {"discovery_scope",
         {{"link", "This link"}, {"admin", "Administrator"}, {"site", "Site"}, {"organisation", "Organisation"}, {"global", "Global"}}},
        {"multicast_address_type", {{"temporary", "Temporary"}, {"permanent", "Permanent"}}},
        {"parity", {{"none", "None"}, {"even", "Even"}, {"odd", "Odd"}}},
    };
    const auto o = table.find(option);
    if (o != table.end()) {
        const auto v = o->second.find(value);
        if (v != o->second.end()) return v->second;
    }
    std::string s = value;
    for (char &c : s)
        if (c == '_') c = ' ';
    return cap_first(s);
}

std::string plain_systemctl(const std::string &line)
{
    const std::string l = lower(line);
    if (l.find("masked") != std::string::npos) return "The service is switched off by the system.";
    if (l.find("not found") != std::string::npos || l.find("not-found") != std::string::npos)
        return "The service is not installed.";
    if (l.find("access denied") != std::string::npos || l.find("permission") != std::string::npos)
        return "The system did not allow it.";
    return line;
}

std::string format_hz(double hz)
{
    char buf[48];
    if (hz >= 1e6) {
        std::snprintf(buf, sizeof buf, "%.6f", hz / 1e6);
        std::string s = buf;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s + " MHz";
    }
    if (hz >= 1e3) {
        std::snprintf(buf, sizeof buf, "%.3f", hz / 1e3);
        std::string s = buf;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s + " kHz";
    }
    return json::number_text(hz) + " Hz";
}

std::string bool_text(const std::string &stored)
{
    const std::string l = lower(stored);
    if (l == "yes" || l == "true" || l == "1" || l == "on") return "yes";
    if (l == "no" || l == "false" || l == "0" || l == "off") return "no";
    return std::string();
}

OptText option_text(const OptionDef &def, const std::string *value, bool secret_set)
{
    OptText t;
    const bool set = value && !value->empty();
    if (def.secret) {
        if (secret_set) {
            t.text = "set";
            t.tone = Tone::Normal;
        } else {
            t.text = "not set";
            t.tone = def.required ? Tone::Warn : Tone::Muted;
        }
        return t;
    }
    if (!set) {
        if (def.required) {
            t.text = "not set";
            t.tone = Tone::Warn;
        } else if (def.has_default) {
            std::string d = def.kind == OptKind::Bool ? bool_text(def.def) : def.def;
            if (!def.choices.empty() && def.kind != OptKind::Bool) d = value_label(def.name, d);
            if (def.kind == OptKind::Int && def.unit == "Hz") d = format_hz(std::atof(d.c_str()));
            else if (!def.unit.empty() && def.kind != OptKind::Bool) d += " " + def.unit;
            t.text = "default (" + d + ")";
            t.tone = Tone::Muted;
        } else {
            t.text = "not set";
            t.tone = Tone::Muted;
        }
        return t;
    }
    t.tone = Tone::Normal;
    if (def.kind == OptKind::Bool) {
        const std::string b = bool_text(*value);
        t.text = b.empty() ? *value : b;
    } else if (!def.choices.empty()) {
        t.text = value_label(def.name, *value);
    } else if (def.device) {
        t.text = base_name(*value);
    } else if (def.unit == "Hz" && (def.kind == OptKind::Int || def.kind == OptKind::Float)) {
        t.text = format_hz(std::atof(value->c_str()));
    } else if (!def.unit.empty() && (def.kind == OptKind::Int || def.kind == OptKind::Float)) {
        t.text = *value + " " + def.unit;
    } else {
        t.text = *value;
    }
    return t;
}

std::string option_hint(const OptionDef &def)
{
    std::string h;
    switch (def.kind) {
    case OptKind::Int: h = "A whole number"; break;
    case OptKind::Float: h = "A number"; break;
    case OptKind::Bool: return "Yes or no.";
    case OptKind::List: return "Names separated by commas.";
    case OptKind::Str: h = "Text"; break;
    }
    static const std::map<std::string, std::string> hints = {
        {"target_host", "The server's name or IP address, like the one its owner gave you"},
        {"network_name", "Only connections with the same network name see each other; leave it empty for none"},
        {"listen_ip", "The address of this deck to listen on; leave it empty for all"},
        {"forward_ip", "The address to send to"},
        {"listen_on", "The address of this deck to listen on"},
        {"remote", "The address of the other side"},
        {"id_callsign", "Your call sign, sent as an identification beacon"},
        {"callsign", "Your call sign"},
        {"command", "The program to run, with its arguments"},
        {"group_id", "Decks with the same group name find each other"},
        {"device", "The name of a network device, like wlan0"},
        {"port", "The port of the device, for example /dev/ttyACM0"},
    };
    if (def.kind == OptKind::Str && def.choices.empty() && !def.secret) {
        const auto it = hints.find(def.name);
        if (it != hints.end()) return it->second + ".";
    }
    if (def.kind == OptKind::Int || def.kind == OptKind::Float) {
        if (def.has_min && def.has_max)
            h += " from " + json::number_text(def.min) + " to " + json::number_text(def.max);
        else if (def.has_min) h += ", at least " + json::number_text(def.min);
        else if (def.has_max) h += ", at most " + json::number_text(def.max);
        if (!def.unit.empty()) h += " (" + def.unit + ")";
    }
    if (def.secret) h += ". It is kept on this deck and never shown again";
    return h + ".";
}

std::string option_edit_text(const OptionDef &def, const std::string *value)
{
    if (def.secret || !value) return std::string();
    return *value;
}

std::string plainify(const std::string &text)
{
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::string rest = lower(text.substr(i, 10));
        if (starts_with(rest, "interfaces")) {  // vocab-ok: the bridge's own word, never shown
            out += text[i] == 'I' ? "Connections" : "connections";
            i += 10;
        } else if (starts_with(rest, "interface")) {  // vocab-ok: the bridge's own word, never shown
            /* the article follows: a connection */  // vocab-ok: comment
            const std::size_t n = out.size();
            if (n >= 3 && lower(out.substr(n - 3)) == "an " && (n == 3 || out[n - 4] == ' ')) {
                out.erase(n - 3);
                out += out.empty() && text[i - 3] == 'A' ? "A " : "a ";
            }
            out += text[i] == 'I' ? "Connection" : "connection";
            i += 9;
        } else {
            out += text[i++];
        }
    }
    return out;
}

std::string plain_error(const Response &r)
{
    if (!r.transport) {
        if (r.code == "no_service") return "The service is not running.";
        if (r.code == "no_answer") return "The service is slow to answer. Try again in a moment.";
        return "The service stopped answering.";
    }
    const std::string &c = r.code;
    std::string msg = plainify(r.message);
    if (c == "blocked") return "Not done: sending must be allowed by you first.";
    if (c == "timeout") return "The service took too long. Try again.";
    if (c == "no_transmit") return "No connection is allowed to send"; /* the owner's words (canvas boards 17, 22) */
    if (c == "lxmf_off") return "Messages are not available: the service runs without them.";  // vocab-ok: a bridge code, never shown
    if (c == "exists" && r.field == "address") return "That address is already a contact.";
    if (c == "exists") return "A connection with that name already exists.";
    if (c == "incomplete") {
        std::string list;
        for (const json::Value &m : r.error["missing"].items()) {
            if (!list.empty()) list += ", ";
            list += option_label(m.as_string());
        }
        return list.empty() ? "Not ready to switch on yet." : "Not ready to switch on. Still to set: " + list + ".";
    }
    if (c == "busy" && msg.empty()) return "That port is in use by another program.";
    if (c == "invalid" && !r.field.empty() && starts_with(msg, r.field + ": "))
        msg = option_label(r.field) + ": " + msg.substr(r.field.size() + 2);
    if (c == "invalid" && r.field == "name" && starts_with(msg, "name: ")) msg = "Name: " + msg.substr(6);
    if (msg.empty()) return "Something went wrong (" + (c.empty() ? std::string("unknown") : c) + ").";
    return msg;
}

std::string trim_text(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string short_address(const std::string &hex)
{
    if (hex.size() < 12) return hex;
    return hex.substr(0, 4) + "..." + hex.substr(hex.size() - 4);
}

std::string grouped_address(const std::string &hex)
{
    std::string out;
    for (std::size_t i = 0; i < hex.size(); i += 8) {
        if (!out.empty()) out += ' ';
        out += hex.substr(i, 8);
    }
    return out;
}

std::string ago_text(double seconds)
{
    if (seconds < 45) return "just now";
    if (seconds < 3600) return std::to_string(std::max(1, static_cast<int>(seconds / 60 + 0.5))) + " min ago";
    if (seconds < 86400) return std::to_string(static_cast<int>(seconds / 3600 + 0.5)) + " h ago";
    const int days = static_cast<int>(seconds / 86400);
    return std::to_string(days) + (days == 1 ? " day ago" : " days ago");
}

/* ------------------------------------------------------------------ serial ports */

std::vector<PortChoice> list_ports(const std::string &root)
{
    std::vector<PortChoice> out;
    std::vector<std::string> covered; /* real paths the by-id entries point to */
    auto list_dir = [](const std::string &dir) {
        std::vector<std::string> names;
        if (DIR *d = opendir(dir.c_str())) {
            while (const dirent *e = readdir(d)) {
                const std::string n = e->d_name;
                if (n != "." && n != "..") names.push_back(n);
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        return names;
    };
    const std::string byid = root + "/serial/by-id";
    for (const std::string &n : list_dir(byid)) {
        char real[PATH_MAX];
        std::string target;
        if (realpath((byid + "/" + n).c_str(), real)) target = real;
        std::string label = starts_with(n, "usb-") ? n.substr(4) : n;
        if (!target.empty()) {
            covered.push_back(target);
            label += " (" + base_name(target) + ")";
        }
        out.push_back({byid + "/" + n, label});
    }
    for (const std::string &n : list_dir(root)) {
        if (!(starts_with(n, "ttyACM") || starts_with(n, "ttyUSB"))) continue;
        char real[PATH_MAX];
        const std::string path = root + "/" + n;
        const std::string resolved = realpath(path.c_str(), real) ? std::string(real) : path;
        if (std::find(covered.begin(), covered.end(), resolved) != covered.end()) continue;
        out.push_back({path, n});
    }
    return out;
}

/* ------------------------------------------------------------------ page stack and service */

bool Nav::has(View v) const
{
    return std::find(stack_.begin(), stack_.end(), v) != stack_.end();
}

std::string run_dir(const char *bridge_run, const char *xdg_runtime, const char *canvas)
{
    if (bridge_run && *bridge_run) {
        std::string s = bridge_run;
        const std::string key = "{canvas}";
        for (std::size_t at; (at = s.find(key)) != std::string::npos;) s.replace(at, key.size(), canvas ? canvas : "");
        return s;
    }
    if (xdg_runtime && *xdg_runtime) return std::string(xdg_runtime) + "/reticulum";
    return std::string();
}

std::vector<std::string> systemctl_argv(const char *override_bin, const std::vector<std::string> &args)
{
    std::vector<std::string> v;
    v.push_back(override_bin && *override_bin ? override_bin : "systemctl");
    v.push_back("--user");
    for (const std::string &a : args) v.push_back(a);
    return v;
}

bool unit_is_enabled(const std::string &output)
{
    const std::string t = trim_text(output);
    return t == "enabled" || t == "enabled-runtime";
}

} // namespace ret
