/*
 * SPDX-License-Identifier: MIT
 *
 * What the app knows about the bridge and how it says it in plain words (no LVGL; tests/test_core.cpp): the parsed
 * answers (connection types and their options, the configured connections, this deck, the heard announcements), the
 * words and tones of every state, the labels and values of the options, the bridge's errors in plain language, the
 * serial ports of the machine, and the page stack. Nothing here sends anything.
 *
 * Words: the screens never use the network's own vocabulary; a "connection" is what the bridge calls an interface,
 * a "heard" entry an announcement. Kinds of connection come from one table (kind_info); the options of a kind come
 * from the bridge's `iface.types` answer, never from code per medium.
 */
#pragma once

#include "json.hpp"
#include "link.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ret {

enum class Tone { Normal, Ok, Warn, Bad, Muted };

/* ------------------------------------------------------------------ the bridge's answers */

enum class OptKind { Int, Float, Bool, Str, List };

struct OptionDef {
    std::string name;
    OptKind kind = OptKind::Str;
    bool has_min = false, has_max = false;
    double min = 0, max = 0;
    std::vector<std::string> choices;
    std::string unit;
    bool has_default = false;
    std::string def; /* the bridge's default, as text */
    bool required = false, secret = false, device = false;
};

struct TypeDef {
    std::string type;
    bool supported = true;
    bool port_lock = false;
    std::string reason;
    std::vector<OptionDef> options;
};

struct Catalog {
    bool loaded = false;
    std::vector<TypeDef> types;
    std::vector<OptionDef> common;
    const TypeDef *find(const std::string &type) const;
};
bool parse_catalog(const json::Value &result, Catalog &out);

struct Iface {
    std::string name, type;
    bool enabled = false, outgoing = false, online = false, supported = true;
    std::vector<std::pair<std::string, std::string>> options; /* what is set, as text (secrets left out) */
    std::vector<std::string> secrets_set;
    std::vector<std::string> missing; /* required options not set */
    std::string lock, error;
    bool has_channel = false, has_noise = false;
    double channel_load = 0; /* percent */
    double noise = 0;        /* dBm */
    /* the option as text; present says whether it is set */
    std::string value(const std::string &key, bool *present = nullptr) const;
    bool secret_set(const std::string &key) const;
};
bool parse_ifaces(const json::Value &result, std::vector<Iface> &out);

struct Deck {
    bool known = false;
    std::string address;      /* the 32-hex address, empty when the service runs without it */
    std::string display_name; /* empty = not set */
    int total = 0, up = 0;
    std::int64_t uptime = 0;
};
bool parse_deck(const json::Value &status, Deck &out);

struct Heard {
    double time = 0;
    std::string destination, name, kind, iface;
    int hops = -1;
};
/* An announce event line ({"event":"announce","data":{...}}) or its data object. */
bool parse_heard(const json::Value &event, Heard &out);

/* The heard list: newest first, one entry per destination (a later announce replaces the earlier), at most 64. */
class HeardList {
public:
    void add(const Heard &h);
    const std::vector<Heard> &items() const { return items_; }
    /* entries heard within `seconds` before `now` */
    int count_within(double now, double seconds) const;

private:
    std::vector<Heard> items_;
};

/* ------------------------------------------------------------------ words */

struct KindInfo {
    std::string type;    /* the bridge's type name */
    std::string title;   /* "Wi-Fi / LAN" */
    std::string summary; /* "nearby decks" */
    std::string section; /* title of the options section on its page */
    std::string note;    /* a line under that title (the radio kinds: who is responsible), or empty */
    bool main = false;   /* on the first list of Add a connection; the others are under "More..." */
};
const KindInfo &kind_info(const std::string &type);
std::vector<const KindInfo *> kinds_main();
std::vector<const KindInfo *> kinds_more(const Catalog &catalog); /* the supported types that are not main */

/* The name a connection is shown under (the first configuration's "Default Interface" is "Wi-Fi LAN"). */
std::string conn_title(const Iface &i);
/* A name for a new connection of this kind that no existing connection has: "LoRa radio", "LoRa radio 2", ... The
 * bridge accepts letters, digits, spaces and . ( ) + - _ in a name, so the slash of the title is dropped. */
std::string suggest_name(const std::string &type, const std::vector<Iface> &existing);

struct State {
    std::string word;
    Tone tone = Tone::Muted;
};
State conn_state(const Iface &i);
/* Home's value of the Connections row: "all off" or "N on". */
std::string connections_summary(const std::vector<Iface> &ifaces);
/* Home's value of the Status row. */
std::string heard_summary(int heard_count);
/* How many are on and how many may send (for the log). */
int count_on(const std::vector<Iface> &ifaces);

std::string option_label(const std::string &name);
/* A value the user can pick or sees, in plain words: a table (option name, value) first, else the value humanised
 * (underscores to spaces, first letter capital), so an unknown value is never shown raw. */
std::string value_label(const std::string &option, const std::string &value);
/* A systemctl failure line in plain words ("masked" -> switched off by the system). */
std::string plain_systemctl(const std::string &line);
struct OptText {
    std::string text;
    Tone tone = Tone::Normal;
};
/* The value shown on an option row. `value` is the option as text (nullptr = not set); secret_set: a secret that is set. */
OptText option_text(const OptionDef &def, const std::string *value, bool secret_set);
/* "whole number from 5 to 12", "yes or no" ... : the line under an editor. */
std::string option_hint(const OptionDef &def);
/* The text an editor starts with (the stored text; empty when not set). */
std::string option_edit_text(const OptionDef &def, const std::string *value);
/* yes / no from the stored text (yes, true, 1 / no, false, 0), empty when it is neither. */
std::string bool_text(const std::string &stored);
/* 868000000 -> "868 MHz", 125000 -> "125 kHz", else the number with its unit. Pure display. */
std::string format_hz(double hz);

/* The bridge's error in plain words. */
std::string plain_error(const Response &r);
/* "interface" -> "connection" (and the plural) in a text that came from the bridge. */
std::string plainify(const std::string &text);

std::string short_address(const std::string &hex);   /* "329a...c109" */
std::string grouped_address(const std::string &hex); /* groups of 8 separated by spaces */
std::string ago_text(double seconds);                /* "just now", "5 min ago", "2 h ago", "3 days ago" */
std::string trim_text(const std::string &s);

/* ------------------------------------------------------------------ serial ports */

struct PortChoice {
    std::string path;  /* what goes into the option */
    std::string label; /* what the list shows */
};
/* /dev/serial/by-id entries, then /dev/ttyACM* and /dev/ttyUSB* that no by-id entry points to. `root` is "/dev"
 * (a test passes a folder of its own). */
std::vector<PortChoice> list_ports(const std::string &root = "/dev");

/* ------------------------------------------------------------------ page stack */

enum class View {
    Home,
    Connections,
    Connection,
    AddKind,
    AddMore,
    AddForm,
    Option,  /* a text editor of one option */
    Choice,  /* a pick list for one option */
    Status,
    Settings,
    Name,
    Address,
};
enum class Back { Closed, Quit };

class Nav {
public:
    View current() const { return stack_.empty() ? View::Home : stack_.back(); }
    int depth() const { return static_cast<int>(stack_.size()); }
    void open(View v) { stack_.push_back(v); }
    /* Back one level; Quit at the root (a short Esc there ends the app). */
    Back back()
    {
        if (stack_.empty()) return Back::Quit;
        stack_.pop_back();
        return Back::Closed;
    }
    void reset() { stack_.clear(); }
    bool has(View v) const;
    const std::vector<View> &stack() const { return stack_; }

private:
    std::vector<View> stack_; /* the pages above Home */
};

/* ------------------------------------------------------------------ the service */

/* The folder of the socket: $RETICULUM_BRIDGE_RUN (a test hook; {canvas} becomes $APPLAUNCH_CANVAS), else
 * $XDG_RUNTIME_DIR/reticulum, else empty. */
std::string run_dir(const char *bridge_run, const char *xdg_runtime, const char *canvas);
/* `systemctl --user <args>`; argv[0] is $RETICULUM_SYSTEMCTL (a test hook) or "systemctl". */
std::vector<std::string> systemctl_argv(const char *override_bin, const std::vector<std::string> &args);
/* The output of `systemctl --user is-enabled reticulum`: true for "enabled" (also "enabled-runtime"). */
bool unit_is_enabled(const std::string &output);

} // namespace ret
