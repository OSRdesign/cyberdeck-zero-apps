/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the line settings and the settings file (no LVGL; tests/test_settings.cpp).
 *
 * The file is the one version 0.1.0 used, ${XDG_CONFIG_HOME:-$HOME/.config}/<launcher config folder>/
 * serial-console.conf (the launcher's config folder keeps its upstream name: launcher NAMING.md). Version 0.1.0 wrote
 * one set of key=value lines (port, baud, frame, echo, ending, log, all); version 0.2.0 writes "v=2", the global keys
 * and one [port <id>] section per port, and reads a 0.1.0 file as the settings of its last port (the old default speed
 * 115200 becomes "the recommended speed"). The file is parsed, never executed; bad values are ignored; it is written
 * through a temporary file and a rename.
 */
#pragma once

#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace sc {

enum class Ending { CR, LF, CRLF };       /* what Enter sends */
enum class Backspace { Del, Bs };         /* what Backspace sends: 0x7F (most systems) or 0x08 */

struct Frame {
    int data = 8;       /* 5..8 */
    char parity = 'N';  /* N E O */
    int stop = 1;       /* 1 2 */
    std::string text() const; /* "8N1" */
};

bool parse_frame(const std::string &text, Frame &out);
const std::vector<std::string> &frame_list();  /* 8N1 7E1 8E1 8O1 7O1 8N2 */
const std::vector<int> &speed_list();          /* 1200 ... 921600, 74880 included (ESP8266 boot ROM) */
bool valid_baud(int baud);

const char *ending_name(Ending ending);        /* "CR" "LF" "CR LF" */
std::string ending_bytes(Ending ending);
const char *backspace_name(Backspace bs);      /* "DEL (0x7F)" "BS (0x08)" */
char backspace_byte(Backspace bs);

struct PortSettings {
    int baud = 0;               /* 0: the recommended speed of the device */
    std::string frame = "8N1";
    bool echo = false;
    Ending ending = Ending::CR;
    Backspace backspace = Backspace::Del;
    bool log = false;
    bool detect_enter = false;  /* Find the speed: a second pass that sends Enter at each speed */
    bool operator==(const PortSettings &o) const;
    bool operator!=(const PortSettings &o) const { return !(*this == o); }
};

/* The speed to use: the saved one, else the recommended one. */
inline int effective_baud(const PortSettings &s, int recommended) { return s.baud > 0 ? s.baud : recommended; }

struct Config {
    bool show_onboard = false;
    std::string last_port;
    std::map<std::string, PortSettings> ports;

    PortSettings settings(const std::string &id) const; /* the defaults for a port never saved */
    void set(const std::string &id, const PortSettings &settings) { ports[id] = settings; }

    std::string serialize() const;
    /* migrated: the text was a version 0.1.0 file */
    static Config parse(const std::string &text, bool *migrated = nullptr);
};

/* ${XDG_CONFIG_HOME:-$HOME/.config}/<launcher config folder>/serial-console.conf ("" without HOME). */
std::string config_path();
Config load_config(const std::string &path, bool *migrated = nullptr);
bool save_config(const std::string &path, const Config &config); /* creates the folder */

/* $HOME/serial-logs and "<tty>-<YYYYmmdd>-<HHMMSS>.log" (local time, as 0.1.0 named them). */
std::string log_dir();
std::string log_file_name(const std::string &tty, std::time_t when);

} // namespace sc
