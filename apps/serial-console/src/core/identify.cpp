/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the classifier of "What is on the other end?" (see identify.hpp).
 *
 * The rule table below is the knowledge: one line per sign a firmware leaves in its serial output. The texts are the
 * formats the projects print (their sources and documentation: ESP-IDF's log and boot loader, the ESP32 / ESP8266
 * ROMs, Arduino-ESP32's debug report, Meshtastic's log, Marlin, Prusa, GRBL / grblHAL / FluidNC, U-Boot, the Linux
 * kernel and systemd, OpenWrt, MikroTik RouterOS, Cisco IOS, MicroPython, CircuitPython, SIMCom / Quectel / ESP-AT
 * modems, u-blox / MediaTek / SiRF / CASIC GNSS receivers). Add a firmware by adding lines; tests/test_identify.cpp
 * runs every sample of tests/fixtures through it.
 */
#include "identify.hpp"

#include "baud.hpp"
#include "term.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace sc {
namespace {

/* ------------------------------------------------------------------ the rule table */

struct Rule {
    Kind kind;
    const char *pattern;
    int weight;
    bool weak;        /* counts only next to a strong rule of the same family */
    const char *sets; /* "key=value;key=value" given when it matches */
};

constexpr bool W = true;  /* weak */
constexpr bool S = false; /* strong */

const Rule kRules[] = {
    /* ---- Meshtastic (on ESP32 / nRF52 / RP2040 boards) */
    {Kind::Meshtastic, "E S H T /\\\\ S T / C", 60, S, "firmware=Meshtastic"},
    {Kind::Meshtastic, "S:B:{hw},{version},{board},", 60, S, "firmware=Meshtastic"},
    {Kind::Meshtastic, "Meshtastic hwvendor={hw}, swver={version}", 60, S, "firmware=Meshtastic"},
    {Kind::Meshtastic, "Meshtastic", 25, S, "firmware=Meshtastic"},

    /* ---- Arduino-ESP32 (core 3.x debug report before setup()) */
    {Kind::ArduinoEsp32, "Before Setup Start", 50, S, "firmware=Arduino-ESP32"},
    {Kind::ArduinoEsp32, "^ *Model*: {chip*}$", 20, W, ""},
    {Kind::ArduinoEsp32, "^ *Arduino Board*: {board*}$", 20, W, ""},

    /* ---- ESP-IDF programs: the 2nd stage boot loader and the application's start */
    {Kind::EspIdf, "^I (#) boot: ESP-IDF {idf} 2nd stage bootloader", 45, S, ""},
    {Kind::EspIdf, "^I (#) cpu_start: ", 30, S, ""},
    {Kind::EspIdf, "^I (#) app_init: ", 30, S, ""},
    {Kind::EspIdf, "ESP-IDF: {idf}", 30, S, ""},
    {Kind::EspIdf, "Project name: {project*}$", 25, S, ""},
    {Kind::EspIdf, "^I (#) boot.{chip}: ", 20, S, ""},
    {Kind::EspIdf, "App version: {appver*}$", 10, W, ""},
    {Kind::EspIdf, "chip revision: v{rev}", 10, W, ""},

    /* ---- the ESP8266 ROM (prints at 74880) */
    {Kind::Esp8266, "ets Jan  8 2013,rst cause:{cause}, boot mode:({mode*})", 60, S, "chip=ESP8266"},
    {Kind::Esp8266, "ets Jan  8 2013", 45, S, "chip=ESP8266"},
    {Kind::Esp8266, "rst cause:#, boot mode:(#,#)", 35, S, "chip=ESP8266"},

    /* ---- the ESP32 family's ROM */
    {Kind::EspRom, "^ets Jun  8 2016 00:22:57", 45, S, "chip=ESP32"},
    {Kind::EspRom, "^ESP-ROM:{rom}", 45, S, ""},
    {Kind::EspRom, "^rst:0x{rst} ({reason*}),boot:0x{bootsel} ({mode*})", 30, S, ""},
    {Kind::EspRom, "waiting for download", 25, S, "download=1"},
    {Kind::EspRom, "^configsip: #, SPIWP:0x", 15, S, ""},
    {Kind::EspRom, "^entry 0x4", 15, S, ""},
    {Kind::EspRom, "^load:0x{addr},len:#", 15, S, ""},
    {Kind::EspRom, "^Build:{rombuild*}$", 10, W, ""},

    /* ---- MicroPython and CircuitPython */
    {Kind::MicroPython, "MicroPython {version} on {date}; {board*} with {mcu*}$", 65, S, "firmware=MicroPython"},
    {Kind::MicroPython, "^MicroPython {version}", 45, S, "firmware=MicroPython"},
    {Kind::MicroPython, "Type \"help()\" for more information.", 30, S, ""},
    {Kind::MicroPython, "^MPY: soft reboot", 35, S, ""},
    {Kind::MicroPython, "^>>>", 15, S, ""},
    {Kind::MicroPython, "^Traceback (most recent call last):", 10, W, ""},
    {Kind::MicroPython, "^KeyboardInterrupt", 10, W, ""},
    {Kind::CircuitPython, "CircuitPython {version} on {date}; {board*} with {mcu*}$", 65, S, "firmware=CircuitPython"},
    {Kind::CircuitPython, "Auto-reload is on.", 40, S, ""},
    {Kind::CircuitPython, "Press any key to enter the REPL. Use CTRL-D to reload.", 40, S, ""},
    {Kind::CircuitPython, "^Code done running.", 30, S, ""},
    {Kind::CircuitPython, "^code.py output:", 30, S, ""},

    /* ---- 3D printers and CNC */
    {Kind::Marlin, "FIRMWARE_NAME:Marlin {version}", 60, S, "firmware=Marlin"},
    {Kind::Marlin, "FIRMWARE_NAME:Prusa-Firmware {version}", 60, S, "firmware=Prusa-Firmware"},
    {Kind::Marlin, "^echo:Marlin {version}", 50, S, "firmware=Marlin"},
    {Kind::Marlin, "^echo: Last Updated: ", 20, S, ""},
    {Kind::Marlin, "^echo:Compiled: ", 10, S, ""},
    {Kind::Marlin, "T:#.# /#.# B:#.# /#.#", 25, S, ""},
    {Kind::Marlin, "MACHINE_TYPE:{machine*} EXTRUDER_COUNT:", 15, W, ""},
    {Kind::Marlin, "^start$", 10, W, ""},
    {Kind::Marlin, "^echo:", 10, W, ""},
    {Kind::Marlin, "^ok$", 5, W, ""},
    {Kind::Klipper, "Klipper", 20, S, "firmware=Klipper"},
    {Kind::Klipper, "klippy", 20, S, "firmware=Klipper"},
    {Kind::Grbl, "[FluidNC {version}", 60, S, "firmware=FluidNC"},
    {Kind::Grbl, "^GrblHAL {version} [", 60, S, "firmware=grblHAL"},
    {Kind::Grbl, "^Grbl {version} [", 55, S, "firmware=Grbl"},
    {Kind::Grbl, "^[VER:{version*}:", 45, S, "firmware=Grbl"},
    {Kind::Grbl, "^<Idle|", 30, S, ""},
    {Kind::Grbl, "^<Alarm|", 30, S, ""},
    {Kind::Grbl, "^ALARM:#", 20, S, ""},
    {Kind::Grbl, "^[MSG:", 15, S, ""},
    {Kind::Grbl, "^[OPT:", 15, W, ""},
    {Kind::Grbl, "^error:#", 10, W, ""},

    /* ---- routers and network devices */
    {Kind::OpenWrt, "W I R E L E S S   F R E E D O M", 60, S, "firmware=OpenWrt"},
    {Kind::OpenWrt, "OpenWrt {version}, r", 55, S, "firmware=OpenWrt"},
    {Kind::OpenWrt, "root@OpenWrt:", 40, S, "firmware=OpenWrt"},
    {Kind::OpenWrt, "Please press Enter to activate this console.", 25, S, ""},
    {Kind::OpenWrt, "procd: ", 20, S, ""},
    {Kind::MikroTik, "MikroTik RouterOS {version} ", 60, S, "firmware=RouterOS"},
    {Kind::MikroTik, "MikroTik Login:", 55, S, "firmware=RouterOS"},
    {Kind::MikroTik, "@MikroTik] >", 45, S, "firmware=RouterOS"},
    {Kind::MikroTik, "RouterBOOT booter {bootver}", 40, S, ""},
    {Kind::Cisco, "Cisco IOS Software, {platform*} Software (", 60, S, "firmware=Cisco IOS"},
    {Kind::Cisco, "Press RETURN to get started", 45, S, ""},
    {Kind::Cisco, "User Access Verification", 35, S, ""},
    {Kind::Cisco, "System Bootstrap, Version {bootver*},", 35, S, ""},
    {Kind::Cisco, "^rommon # >", 35, S, ""},
    {Kind::Cisco, "^Switch>$", 25, S, ""},
    {Kind::Cisco, "^Router>$", 25, S, ""},
    {Kind::Cisco, "^Switch\\#$", 25, S, ""},
    {Kind::Cisco, "Cisco IOS Software*, Version {version*},", 20, W, ""},

    /* ---- boot loaders and Linux */
    {Kind::UBoot, "^U-Boot SPL {version} ", 45, S, "firmware=U-Boot"},
    {Kind::UBoot, "^U-Boot {version} ", 55, S, "firmware=U-Boot"},
    {Kind::UBoot, "Hit any key to stop autoboot", 45, S, "firmware=U-Boot"},
    {Kind::UBoot, "^Starting kernel ...", 30, S, ""},
    {Kind::UBoot, "^Model: {board*}$", 15, W, ""},
    {Kind::UBoot, "^DRAM: {mem*}$", 15, W, ""},
    {Kind::UBoot, "^CPU: {chip*}$", 10, W, ""},
    {Kind::UBoot, "^=>$", 10, W, ""},
    {Kind::Linux, "Linux version {version} ", 50, S, "firmware=Linux"},
    {Kind::Linux, "Booting Linux on physical CPU", 45, S, "firmware=Linux"},
    {Kind::Linux, "^{host} login:", 40, S, ""},
    {Kind::Linux, "^Welcome to {distro*}!", 35, S, ""},
    {Kind::Linux, "GNU/Linux {osver} {host} {tty}$", 30, S, ""},
    {Kind::Linux, "systemd[1]: ", 30, S, ""},
    {Kind::Linux, "BusyBox v{version} ", 30, S, "firmware=BusyBox"},
    {Kind::Linux, "^[ *#.#] ", 25, S, ""},
    {Kind::Linux, "^root@{host}:", 20, W, ""},
    {Kind::Linux, "^Password:", 10, W, ""},

    /* ---- AT command modems (cellular, Wi-Fi, Bluetooth) */
    {Kind::AtModem, "^AT version:{version*}(", 50, S, "firmware=ESP-AT"},
    {Kind::AtModem, "^+CME ERROR: {err*}$", 35, S, ""},
    {Kind::AtModem, "^Quectel$", 35, S, "firmware=Quectel"},
    {Kind::AtModem, "^+CMS ERROR:", 30, S, ""},
    {Kind::AtModem, "^Call Ready$", 30, S, ""},
    {Kind::AtModem, "^OK$", 25, S, ""},
    {Kind::AtModem, "^RDY$", 25, S, ""},
    {Kind::AtModem, "^+CPIN: {pin}", 25, S, ""},
    {Kind::AtModem, "^SMS Ready$", 25, S, ""},
    {Kind::AtModem, "^Revision: {version*}$", 25, W, ""},
    {Kind::AtModem, "^{model} R{version#}$", 25, W, ""},
    {Kind::AtModem, "^Model: {model*}$", 20, W, ""},
    {Kind::AtModem, "^Manufacturer: {maker*}$", 15, W, ""},
    {Kind::AtModem, "^SDK version:{sdk*}$", 15, W, ""},
    {Kind::AtModem, "^ERROR$", 10, W, ""},
    {Kind::AtModem, "^AT$", 5, W, ""},
    {Kind::AtModem, "^ATI$", 5, W, ""},

    /* ---- GNSS receivers: their text sentences (the NMEA sentences themselves are checked in code) */
    {Kind::Gnss, "TXT,#,#,#,u-blox ag", 30, S, "maker=u-blox"},
    {Kind::Gnss, "TXT,#,#,#,u-blox AG", 30, S, "maker=u-blox"},
    {Kind::Gnss, "^$PUBX,", 25, S, "maker=u-blox"},
    {Kind::Gnss, "^$PMTK", 25, S, "maker=MediaTek"},
    {Kind::Gnss, "^$PSRF", 25, S, "maker=SiRF"},
    {Kind::Gnss, "TXT,#,#,#,MA=CASIC", 25, S, "maker=CASIC"},
    {Kind::Gnss, "^$PQTM", 25, S, "maker=Quectel"},
    {Kind::Gnss, "TXT,#,#,#,MOD={model}", 25, W, ""},
    {Kind::Gnss, "TXT,#,#,#,HW {hw}", 15, W, ""},
    {Kind::Gnss, "TXT,#,#,#,FWVER={version*}\\*", 15, W, ""},
    {Kind::Gnss, "TXT,#,#,#,PROTVER={protver}", 10, W, ""},
};

constexpr int kRuleCount = static_cast<int>(sizeof(kRules) / sizeof(kRules[0]));

/* the families whose findings complete another's (the chip of the ROM banner for an ESP-IDF program...) */
std::vector<Kind> related(Kind k)
{
    switch (k) {
    case Kind::Meshtastic: return {Kind::EspRom, Kind::EspIdf, Kind::ArduinoEsp32, Kind::Esp8266};
    case Kind::ArduinoEsp32: return {Kind::EspRom, Kind::EspIdf};
    case Kind::EspIdf: return {Kind::EspRom};
    case Kind::EspRom: return {Kind::EspIdf};
    case Kind::AtModem: return {Kind::EspRom, Kind::Esp8266};
    case Kind::OpenWrt: return {Kind::Linux, Kind::UBoot};
    case Kind::Linux: return {Kind::UBoot};
    case Kind::Marlin: return {Kind::ArduinoBoot};
    default: return {};
    }
}

/* a boot stage: it loses to the program it starts once that program is clearly there */
bool base_layer(Kind k)
{
    return k == Kind::EspRom || k == Kind::UBoot || k == Kind::Esp8266;
}

/* ------------------------------------------------------------------ the pattern matcher */

struct Tok {
    enum T { Lit, Any1, Star, Digits, Word, Text } t = Lit;
    char c = 0;
    bool digit = false; /* Word: it must start with a digit */
    std::string name;
};

bool word_stop(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) || std::strchr(",;:()[]\"'*", c) != nullptr;
}

void parse_pattern(const std::string &p, std::vector<Tok> &toks, bool &start, bool &end)
{
    std::size_t i = 0, n = p.size();
    start = n > 0 && p[0] == '^';
    if (start) ++i;
    end = n > i && p[n - 1] == '$' && (n < 2 || p[n - 2] != '\\');
    if (end) --n;
    while (i < n) {
        Tok t;
        const char c = p[i];
        if (c == '\\' && i + 1 < n) {
            t.c = p[i + 1];
            i += 2;
        } else if (c == '*') {
            t.t = Tok::Star;
            ++i;
        } else if (c == '?') {
            t.t = Tok::Any1;
            ++i;
        } else if (c == '#') {
            t.t = Tok::Digits;
            ++i;
        } else if (c == '{') {
            const std::size_t close = p.find('}', i);
            if (close == std::string::npos || close >= n) {
                t.c = c;
                ++i;
            } else {
                std::string name = p.substr(i + 1, close - i - 1);
                t.t = Tok::Word;
                if (!name.empty() && name.back() == '*') {
                    name.pop_back();
                    t.t = Tok::Text;
                } else if (!name.empty() && name.back() == '#') {
                    name.pop_back();
                    t.digit = true;
                }
                t.name = name;
                i = close + 1;
            }
        } else {
            t.c = c;
            ++i;
        }
        toks.push_back(t);
    }
}

std::string trim(const std::string &s)
{
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

class Matcher {
public:
    Matcher(const std::vector<Tok> &toks, bool end, const std::string &line, std::map<std::string, std::string> *caps)
        : toks_(toks), end_(end), line_(line), caps_(caps), fail_((toks.size() + 1) * (line.size() + 1), 0)
    {
    }

    bool at(std::size_t ti, std::size_t pos)
    {
        const std::size_t len = line_.size();
        if (ti == toks_.size()) return !end_ || pos == len;
        char &failed = fail_[ti * (len + 1) + pos];
        if (failed) return false;
        const Tok &t = toks_[ti];
        bool ok = false;
        switch (t.t) {
        case Tok::Lit: ok = pos < len && line_[pos] == t.c && at(ti + 1, pos + 1); break;
        case Tok::Any1: ok = pos < len && at(ti + 1, pos + 1); break;
        case Tok::Digits: {
            std::size_t k = pos;
            while (k < len && std::isdigit(static_cast<unsigned char>(line_[k]))) ++k;
            ok = k > pos && at(ti + 1, k);
            break;
        }
        case Tok::Word: {
            std::size_t k = pos;
            while (k < len && line_[k] == ' ') ++k;
            const std::size_t s = k;
            while (k < len && !word_stop(line_[k])) ++k;
            if (k > s && (!t.digit || std::isdigit(static_cast<unsigned char>(line_[s])))) {
                set(t.name, line_.substr(s, k - s));
                ok = at(ti + 1, k);
            }
            break;
        }
        case Tok::Star:
            for (std::size_t k = pos; k <= len && !ok; ++k) ok = at(ti + 1, k);
            break;
        case Tok::Text:
            if (ti + 1 == toks_.size()) { /* last: the rest of the line */
                const std::string rest = trim(line_.substr(pos));
                if (!rest.empty()) {
                    set(t.name, rest);
                    ok = at(ti + 1, len);
                }
            } else {
                for (std::size_t k = pos + 1; k <= len && !ok; ++k) {
                    const std::string part = trim(line_.substr(pos, k - pos));
                    if (part.empty()) continue;
                    set(t.name, part);
                    ok = at(ti + 1, k);
                }
            }
            break;
        }
        if (!ok) failed = 1;
        return ok;
    }

private:
    void set(const std::string &name, const std::string &value)
    {
        if (caps_ && !name.empty()) (*caps_)[name] = value;
    }

    const std::vector<Tok> &toks_;
    bool end_;
    const std::string &line_;
    std::map<std::string, std::string> *caps_;
    std::vector<char> fail_;
};

/* ------------------------------------------------------------------ helpers */

std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/* "esp32s3" / "ESP32S3" / "esp32s3-20210327" -> "ESP32-S3"; "rp2040" -> "RP2040" */
std::string normalize_chip(const std::string &raw)
{
    const std::string s = trim(raw);
    const std::string l = lower(s);
    if (l.compare(0, 7, "esp8266") == 0) return "ESP8266";
    if (l.compare(0, 5, "esp32") == 0) {
        std::size_t i = 5;
        if (i < l.size() && (l[i] == '-' || l[i] == '_')) ++i;
        std::string rest;
        for (; i < l.size() && std::isalnum(static_cast<unsigned char>(l[i])); ++i) rest += l[i];
        return rest.empty() ? "ESP32" : "ESP32-" + upper(rest);
    }
    if (l.compare(0, 2, "rp") == 0 && l.size() >= 6 && std::isdigit(static_cast<unsigned char>(l[2]))) return upper(s);
    return s;
}

/* cut to max bytes without splitting a UTF-8 sequence; control bytes become dots */
std::string safe_text(const std::string &s, std::size_t max)
{
    std::string out;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        out += (c < 0x20 || c == 0x7F) ? '.' : ch;
    }
    if (out.size() > max) {
        std::size_t cut = max;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out = out.substr(0, cut);
        if (max >= 4) {
            while (out.size() > max - 3) out.pop_back();
            out += "...";
        }
    }
    return out;
}

bool hexval(char c, int &v)
{
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else return false;
    return true;
}

/* A capture that ends with readable text after a garbled start (an ESP8266's ROM at 74880, then its program at the
 * speed listened at): the length of that text, when it is long enough to count. */
std::size_t clean_tail(const std::string &b)
{
    std::size_t n = 0;
    for (std::size_t i = b.size(); i > 0; --i, ++n) {
        const unsigned char c = static_cast<unsigned char>(b[i - 1]);
        const bool text = (c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n' || c == '\t' || c == 0x1B;
        if (!text) break;
    }
    return n >= 32 && n * 100 >= b.size() * 15 ? n : 0;
}

bool idf_log_line(const std::string &l)
{
    if (l.size() < 8 || std::strchr("IWEDV", l[0]) == nullptr || l[1] != ' ' || l[2] != '(') return false;
    std::size_t i = 3;
    while (i < l.size() && std::isdigit(static_cast<unsigned char>(l[i]))) ++i;
    if (i == 3 || i + 2 >= l.size() || l[i] != ')' || l[i + 1] != ' ') return false;
    const std::size_t colon = l.find(": ", i + 2);
    return colon != std::string::npos && colon > i + 2 && l.find(' ', i + 2) >= colon;
}

bool meshtastic_log_line(const std::string &l)
{
    static const char *kHeads[] = {"DEBUG | ", "INFO  | ", "WARN  | ", "ERROR | ", "TRACE | ", "CRIT  | "};
    for (const char *h : kHeads)
        if (l.compare(0, std::strlen(h), h) == 0) return true;
    return false;
}

std::string talker_name(const std::string &t)
{
    if (t == "GP") return "GPS";
    if (t == "GN") return "multi-GNSS";
    if (t == "GL") return "GLONASS";
    if (t == "GA") return "Galileo";
    if (t == "GB" || t == "BD") return "BeiDou";
    if (t == "GQ" || t == "QZ") return "QZSS";
    return t;
}

std::uint8_t at(const std::string &s, std::size_t i)
{
    return static_cast<std::uint8_t>(s[i]);
}

std::string hex_bytes(const std::string &s, std::size_t from, std::size_t n)
{
    std::string out;
    char buf[4];
    for (std::size_t i = from; i < s.size() && i < from + n; ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", at(s, i));
        if (!out.empty()) out += ' ';
        out += buf;
    }
    return out;
}

/* ------------------------------------------------------------------ binary framing */

int count_ubx(const std::string &b, std::string *first)
{
    int n = 0;
    for (std::size_t i = 0; i + 8 <= b.size(); ++i) {
        if (at(b, i) != 0xB5 || at(b, i + 1) != 0x62) continue;
        const std::size_t len = at(b, i + 4) | (static_cast<std::size_t>(at(b, i + 5)) << 8);
        if (len > 1024 || i + 8 + len > b.size()) continue;
        std::uint8_t ca = 0, cb = 0;
        for (std::size_t k = i + 2; k < i + 6 + len; ++k) {
            ca = static_cast<std::uint8_t>(ca + at(b, k));
            cb = static_cast<std::uint8_t>(cb + ca);
        }
        if (ca != at(b, i + 6 + len) || cb != at(b, i + 7 + len)) continue;
        if (n == 0 && first) *first = hex_bytes(b, i, 8);
        ++n;
        i += 7 + len;
    }
    return n;
}

int count_rtcm(const std::string &b, std::string *first)
{
    int n = 0;
    for (std::size_t i = 0; i + 6 <= b.size(); ++i) {
        if (at(b, i) != 0xD3 || (at(b, i + 1) & 0xFC) != 0) continue;
        const std::size_t len = ((at(b, i + 1) & 3u) << 8) | at(b, i + 2);
        if (i + 3 + len + 3 > b.size()) continue;
        const std::uint32_t crc = crc24q(b.substr(i, 3 + len));
        const std::uint32_t got = (static_cast<std::uint32_t>(at(b, i + 3 + len)) << 16) |
                                  (static_cast<std::uint32_t>(at(b, i + 4 + len)) << 8) | at(b, i + 5 + len);
        if (crc != got) continue;
        if (n == 0 && first) *first = hex_bytes(b, i, 8);
        ++n;
        i += 5 + len;
    }
    return n;
}

/* MAVLink HEARTBEAT (message 0, CRC extra 50), version 1 (FE) and 2 (FD) */
int count_mavlink(const std::string &b, std::string *first)
{
    int n = 0;
    for (std::size_t i = 0; i + 8 <= b.size(); ++i) {
        const std::uint8_t stx = at(b, i);
        if (stx != 0xFE && stx != 0xFD) continue;
        const std::size_t len = at(b, i + 1);
        std::size_t head = stx == 0xFE ? 6 : 10;
        if (i + head + len + 2 > b.size()) continue;
        std::uint32_t msgid = stx == 0xFE ? at(b, i + 5)
                                          : (at(b, i + 7) | (static_cast<std::uint32_t>(at(b, i + 8)) << 8) |
                                             (static_cast<std::uint32_t>(at(b, i + 9)) << 16));
        if (msgid != 0 || len > 9 || (stx == 0xFE && len != 9)) continue;
        std::uint16_t crc = crc_x25(b.substr(i + 1, head - 1 + len));
        crc = crc_x25(std::string(1, static_cast<char>(50)), crc);
        const std::uint16_t got = static_cast<std::uint16_t>(at(b, i + head + len) | (at(b, i + head + len + 1) << 8));
        if (crc != got) continue;
        if (n == 0 && first) *first = hex_bytes(b, i, 8);
        ++n;
        i += head + len + 1;
    }
    return n;
}

/* Klipper: <len> <seq 0x1n> <payload> <crc hi> <crc lo> <0x7E>, len = the whole block (5..64) */
int count_klipper(const std::string &b, std::string *first)
{
    int n = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (at(b, i) != 0x7E) continue;
        const std::size_t len = i + 1 - start;
        if (len >= 5 && len <= 64 && at(b, start) == len && (at(b, start + 1) & 0xF0) == 0x10) {
            const std::uint16_t crc = crc16_klipper(b.substr(start, len - 3));
            if (at(b, i - 2) == (crc >> 8) && at(b, i - 1) == (crc & 0xFF)) {
                if (n == 0 && first) *first = hex_bytes(b, start, 8);
                ++n;
            }
        }
        start = i + 1;
    }
    return n;
}

/* Meshtastic's serial API: 94 C3 <len hi> <len lo> <a FromRadio protobuf> */
int count_meshtastic(const std::string &b, std::string *first)
{
    int n = 0;
    for (std::size_t i = 0; i + 5 <= b.size(); ++i) {
        if (at(b, i) != 0x94 || at(b, i + 1) != 0xC3) continue;
        const std::size_t len = (static_cast<std::size_t>(at(b, i + 2)) << 8) | at(b, i + 3);
        if (len == 0 || len > 512 || i + 4 + len > b.size()) continue;
        const std::uint8_t tag = at(b, i + 4);
        const int wire = tag & 7, field = tag >> 3;
        if (field < 1 || field > 17 || (wire != 0 && wire != 2)) continue;
        if (n == 0 && first) *first = hex_bytes(b, i, 8);
        ++n;
        i += 3 + len;
    }
    return n;
}

int count_slip(const std::string &b, std::string *first)
{
    int n = 0;
    std::size_t inside = 0, start = std::string::npos;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (at(b, i) != 0xC0) continue;
        if (start != std::string::npos && i - start - 1 >= 4) {
            bool ok = true;
            for (std::size_t k = start + 1; k < i && ok; ++k)
                if (at(b, k) == 0xDB) ok = k + 1 < i && (at(b, k + 1) == 0xDC || at(b, k + 1) == 0xDD);
            if (ok) {
                if (n == 0 && first) *first = hex_bytes(b, start, 8);
                ++n;
                inside += i - start + 1;
            }
        }
        start = i;
    }
    return (n >= 2 && inside * 10 >= b.size() * 8) ? n : 0;
}

bool read_varint(const std::string &b, std::size_t &i, std::uint64_t &v)
{
    v = 0;
    for (int shift = 0; shift < 64 && i < b.size(); shift += 7) {
        const std::uint8_t c = at(b, i++);
        v |= static_cast<std::uint64_t>(c & 0x7F) << shift;
        if (!(c & 0x80)) return true;
    }
    return false;
}

/* a stream that parses as protobuf fields (tag, wire type, value) for most of its length */
int count_protobuf(const std::string &b)
{
    int best = 0;
    for (std::size_t off = 0; off < 4 && off < b.size(); ++off) {
        std::size_t i = off;
        int fields = 0;
        while (i < b.size()) {
            const std::size_t mark = i;
            std::uint64_t tag = 0, v = 0;
            if (!read_varint(b, i, tag)) break;
            const std::uint64_t field = tag >> 3, wire = tag & 7;
            if (field < 1 || field > 2047) { i = mark; break; }
            bool ok = true;
            if (wire == 0) ok = read_varint(b, i, v);
            else if (wire == 1) { ok = i + 8 <= b.size(); i += 8; }
            else if (wire == 5) { ok = i + 4 <= b.size(); i += 4; }
            else if (wire == 2) { ok = read_varint(b, i, v) && v <= 4096 && i + v <= b.size(); i += static_cast<std::size_t>(v); }
            else ok = false;
            if (!ok) { i = mark; break; }
            ++fields;
        }
        if (fields >= 6 && (i - off) * 10 >= b.size() * 7) best = std::max(best, fields);
    }
    return best;
}

/* ------------------------------------------------------------------ the classification state */

struct FamilyScore {
    int strong = 0, weak = 0;
    std::set<int> rules;
    std::map<std::string, std::string> fields;
    std::vector<std::string> evidence;
    std::map<int, int> by_capture; /* capture index -> weight found there */
    int score() const { return strong > 0 ? strong + weak : 0; }
};

void add_evidence(std::vector<std::string> &ev, const std::string &line)
{
    const std::string e = safe_text(trim(line), kEvidenceChars);
    if (e.empty() || ev.size() >= kEvidenceMax) return;
    if (std::find(ev.begin(), ev.end(), e) == ev.end()) ev.push_back(e);
}

void apply_sets(FamilyScore &f, const char *sets)
{
    std::string s = sets ? sets : "";
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t semi = s.find(';', pos);
        if (semi == std::string::npos) semi = s.size();
        const std::string kv = s.substr(pos, semi - pos);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos && !f.fields.count(kv.substr(0, eq))) f.fields[kv.substr(0, eq)] = kv.substr(eq + 1);
        pos = semi + 1;
    }
}

/* The rules parsed once, each with its longest literal run: a line without it cannot match (a quick test first). */
struct Compiled {
    std::vector<std::vector<Tok>> toks;
    std::vector<char> starts, ends;
    std::vector<std::string> needs;
};

const Compiled &compiled()
{
    static const Compiled c = [] {
        Compiled k;
        k.toks.resize(kRuleCount);
        k.starts.resize(kRuleCount);
        k.ends.resize(kRuleCount);
        k.needs.resize(kRuleCount);
        for (int r = 0; r < kRuleCount; ++r) {
            const std::size_t i = static_cast<std::size_t>(r);
            bool a = false, b = false;
            parse_pattern(kRules[r].pattern, k.toks[i], a, b);
            k.starts[i] = a;
            k.ends[i] = b;
            std::string run;
            for (const Tok &t : k.toks[i]) {
                if (t.t == Tok::Lit) {
                    run += t.c;
                    if (run.size() > k.needs[i].size()) k.needs[i] = run;
                } else {
                    run.clear();
                }
            }
        }
        return k;
    }();
    return c;
}

bool run_match(const std::vector<Tok> &toks, bool start, bool end, const std::string &line,
               std::map<std::string, std::string> *caps)
{
    Matcher m(toks, end, line, caps);
    if (start) return m.at(0, 0);
    for (std::size_t s = 0; s <= line.size(); ++s)
        if (m.at(0, s)) return true;
    return false;
}

std::string field(const std::map<std::string, std::string> &f, const char *key)
{
    const auto it = f.find(key);
    return it == f.end() ? std::string() : it->second;
}

const char *firmware_default(Kind k)
{
    switch (k) {
    case Kind::Meshtastic: return "Meshtastic";
    case Kind::ArduinoEsp32: return "Arduino-ESP32";
    case Kind::EspIdf: return "ESP-IDF";
    case Kind::MicroPython: return "MicroPython";
    case Kind::CircuitPython: return "CircuitPython";
    case Kind::Marlin: return "Marlin";
    case Kind::Klipper: return "Klipper";
    case Kind::Grbl: return "Grbl";
    case Kind::OpenWrt: return "OpenWrt";
    case Kind::MikroTik: return "RouterOS";
    case Kind::Cisco: return "Cisco IOS";
    case Kind::UBoot: return "U-Boot";
    case Kind::Linux: return "Linux";
    case Kind::Gnss: return "NMEA";
    default: return "";
    }
}

} // namespace

/* ------------------------------------------------------------------ public helpers */

const char *kind_key(Kind kind)
{
    switch (kind) {
    case Kind::Meshtastic: return "meshtastic";
    case Kind::ArduinoEsp32: return "arduino-esp32";
    case Kind::EspIdf: return "esp-idf";
    case Kind::Esp8266: return "esp8266";
    case Kind::EspRom: return "esp-rom";
    case Kind::MicroPython: return "micropython";
    case Kind::CircuitPython: return "circuitpython";
    case Kind::Marlin: return "marlin";
    case Kind::Klipper: return "klipper";
    case Kind::Grbl: return "grbl";
    case Kind::OpenWrt: return "openwrt";
    case Kind::MikroTik: return "mikrotik";
    case Kind::Cisco: return "cisco";
    case Kind::UBoot: return "u-boot";
    case Kind::Linux: return "linux";
    case Kind::AtModem: return "at-modem";
    case Kind::Gnss: return "gnss";
    case Kind::ArduinoBoot: return "arduino-boot";
    case Kind::Sketch: return "sketch";
    case Kind::Text: return "text";
    case Kind::Binary: return "binary";
    case Kind::Silent: return "silent";
    case Kind::None: break;
    }
    return "none";
}

Kind kind_from_key(const std::string &key)
{
    for (int k = 0; k <= static_cast<int>(Kind::None); ++k)
        if (key == kind_key(static_cast<Kind>(k))) return static_cast<Kind>(k);
    return Kind::None;
}

const char *kind_title(Kind kind)
{
    switch (kind) {
    case Kind::Meshtastic: return "Meshtastic node";
    case Kind::ArduinoEsp32: return "ESP32 (Arduino sketch)";
    case Kind::EspIdf: return "ESP32 (ESP-IDF app)";
    case Kind::Esp8266: return "ESP8266";
    case Kind::EspRom: return "ESP32 boot ROM";
    case Kind::MicroPython: return "MicroPython board";
    case Kind::CircuitPython: return "CircuitPython board";
    case Kind::Marlin: return "3D printer (Marlin)";
    case Kind::Klipper: return "Klipper MCU";
    case Kind::Grbl: return "CNC controller (GRBL)";
    case Kind::OpenWrt: return "OpenWrt router";
    case Kind::MikroTik: return "MikroTik router";
    case Kind::Cisco: return "Cisco console";
    case Kind::UBoot: return "U-Boot boot loader";
    case Kind::Linux: return "Linux console";
    case Kind::AtModem: return "AT modem";
    case Kind::Gnss: return "GPS / GNSS receiver";
    case Kind::ArduinoBoot: return "Arduino boot loader";
    case Kind::Sketch: return "Arduino sketch (text)";
    case Kind::Text: return "Text, not recognised";
    case Kind::Binary: return "Binary data";
    case Kind::Silent: return "Silent";
    case Kind::None: break;
    }
    return "Not identified";
}

bool match_pattern(const std::string &pattern, const std::string &line, std::map<std::string, std::string> *captures)
{
    std::vector<Tok> toks;
    bool start = false, end = false;
    parse_pattern(pattern, toks, start, end);
    return run_match(toks, start, end, line, captures);
}

std::vector<std::string> clean_lines(const std::string &bytes, std::size_t max)
{
    TermBuffer term(5000, 1024); /* the first lines matter (a boot): keep them all */
    term.feed(bytes);
    std::vector<std::string> out;
    for (std::uint64_t i = term.first(); i < term.end(); ++i) {
        std::string l = term.line(i);
        while (!l.empty() && l.back() == ' ') l.pop_back();
        if (i + 1 == term.end() && l.empty()) break; /* the cursor on a new line */
        out.push_back(l);
    }
    if (out.size() > max) out.resize(max);
    return out;
}

bool nmea_valid(const std::string &line, std::string *talker)
{
    const std::size_t d = line.find('$');
    if (d == std::string::npos) return false;
    const std::size_t star = line.find('*', d);
    if (star == std::string::npos || star + 2 >= line.size() + 0 || star + 3 > line.size()) return false;
    if (star < d + 7 || line[d + 6] != ',') return false;
    for (std::size_t i = d + 1; i < d + 6; ++i)
        if (!std::isupper(static_cast<unsigned char>(line[i])) && !std::isdigit(static_cast<unsigned char>(line[i])))
            return false;
    std::uint8_t sum = 0;
    for (std::size_t i = d + 1; i < star; ++i) sum ^= static_cast<std::uint8_t>(line[i]);
    int hi = 0, lo = 0;
    if (!hexval(line[star + 1], hi) || !hexval(line[star + 2], lo)) return false;
    if (sum != static_cast<std::uint8_t>(hi * 16 + lo)) return false;
    if (talker) *talker = line.substr(d + 1, 2);
    return true;
}

std::uint16_t crc_x25(const std::string &bytes, std::uint16_t crc)
{
    for (const char ch : bytes) {
        std::uint8_t tmp = static_cast<std::uint8_t>(static_cast<std::uint8_t>(ch) ^ (crc & 0xFF));
        tmp = static_cast<std::uint8_t>(tmp ^ (tmp << 4));
        crc = static_cast<std::uint16_t>((crc >> 8) ^ (static_cast<std::uint16_t>(tmp) << 8) ^
                                         (static_cast<std::uint16_t>(tmp) << 3) ^ (tmp >> 4));
    }
    return crc;
}

std::uint16_t crc16_klipper(const std::string &bytes)
{
    /* klipper/src/generic/crc16_ccitt.c: the same arithmetic as MAVLink's (CRC-16/MCRF4XX), sent high byte first */
    return crc_x25(bytes, 0xFFFF);
}

std::uint32_t crc24q(const std::string &bytes)
{
    std::uint32_t crc = 0;
    for (const char ch : bytes) {
        crc ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(ch)) << 16;
        for (int i = 0; i < 8; ++i) {
            crc <<= 1;
            if (crc & 0x1000000u) crc ^= 0x1864CFBu;
        }
    }
    return crc & 0xFFFFFFu;
}

std::string binary_framing(const std::string &bytes, int *frames, Kind *kind)
{
    struct Try {
        const char *name;
        Kind kind;
        int count;
        std::string first;
    };
    std::vector<Try> tries(6);
    tries[0] = {"UBX (B5 62)", Kind::Gnss, 0, ""};
    tries[0].count = count_ubx(bytes, &tries[0].first);
    tries[1] = {"RTCM 3 (D3)", Kind::Gnss, 0, ""};
    tries[1].count = count_rtcm(bytes, &tries[1].first);
    tries[2] = {"MAVLink (drone autopilot)", Kind::Binary, 0, ""};
    tries[2].count = count_mavlink(bytes, &tries[2].first);
    tries[3] = {"Klipper (7E frames)", Kind::Klipper, 0, ""};
    tries[3].count = count_klipper(bytes, &tries[3].first);
    tries[4] = {"Meshtastic API (94 C3)", Kind::Meshtastic, 0, ""};
    tries[4].count = count_meshtastic(bytes, &tries[4].first);
    tries[5] = {"SLIP (C0 frames)", Kind::Binary, 0, ""};
    tries[5].count = count_slip(bytes, &tries[5].first);
    const Try *best = nullptr;
    for (const Try &t : tries) {
        const int need = (t.kind == Kind::Meshtastic || t.kind == Kind::Binary) ? 2 : 1;
        if (t.count >= need && (!best || t.count > best->count)) best = &t;
    }
    if (best) {
        if (frames) *frames = best->count;
        if (kind) *kind = best->kind;
        return std::string(best->name) + ": " + best->first;
    }
    const int pb = count_protobuf(bytes);
    if (pb) {
        if (frames) *frames = pb;
        if (kind) *kind = Kind::Binary;
        return "protobuf-like, no framing found";
    }
    if (frames) *frames = 0;
    if (kind) *kind = Kind::None;
    return std::string();
}

/* ------------------------------------------------------------------ classify */

Identity classify(const std::vector<Capture> &captures, const Context &ctx)
{
    const Compiled &rules = compiled();
    std::map<Kind, FamilyScore> fam;
    std::set<std::string> talkers;
    int nmea = 0;
    std::size_t total = 0;
    int best_clean = -1;          /* the capture that reads best as text */
    double best_clean_score = 0.0;
    std::vector<std::vector<std::string>> lines_of(captures.size());
    std::string framing;
    Kind framing_kind = Kind::None;
    int framing_frames = 0, framing_cap = -1;

    for (std::size_t ci = 0; ci < captures.size(); ++ci) {
        const Capture &cap = captures[ci];
        total += cap.bytes.size();
        if (cap.bytes.empty()) continue;
        TextScore ts = score_text(cap.bytes);
        if (!looks_clean(ts)) { /* a garbled start, then text: judge the text */
            const std::size_t tail = clean_tail(cap.bytes);
            if (tail) ts = score_text(cap.bytes.substr(cap.bytes.size() - tail));
        }
        if (looks_clean(ts) && (best_clean < 0 || ts.score > best_clean_score)) {
            best_clean = static_cast<int>(ci);
            best_clean_score = ts.score;
        }
        lines_of[ci] = clean_lines(cap.bytes);
        int idf_lines = 0, mesh_lines = 0;
        std::string idf_first, mesh_first;
        for (const std::string &line : lines_of[ci]) {
            if (line.empty()) continue;
            for (int r = 0; r < kRuleCount; ++r) {
                const Rule &rule = kRules[r];
                const std::size_t ri = static_cast<std::size_t>(r);
                if (!rules.needs[ri].empty() && line.find(rules.needs[ri]) == std::string::npos) continue;
                std::map<std::string, std::string> caps;
                if (!run_match(rules.toks[ri], rules.starts[ri] != 0, rules.ends[ri] != 0, line, &caps)) continue;
                FamilyScore &f = fam[rule.kind];
                if (f.rules.insert(r).second) {
                    (rule.weak ? f.weak : f.strong) += rule.weight;
                    f.by_capture[static_cast<int>(ci)] += rule.weight;
                    add_evidence(f.evidence, line);
                    apply_sets(f, rule.sets);
                }
                for (const auto &kv : caps)
                    if (!kv.second.empty() && !f.fields.count(kv.first)) f.fields[kv.first] = kv.second;
            }
            std::string talker;
            if (nmea_valid(line, &talker)) {
                ++nmea;
                talkers.insert(talker);
                FamilyScore &g = fam[Kind::Gnss];
                if (nmea <= 3) {
                    g.strong += 20;
                    g.by_capture[static_cast<int>(ci)] += 20;
                    add_evidence(g.evidence, line);
                }
            }
            if (idf_log_line(line) && ++idf_lines == 1) idf_first = line;
            if (meshtastic_log_line(line) && ++mesh_lines == 1) mesh_first = line;
        }
        if (idf_lines) {
            FamilyScore &f = fam[Kind::EspIdf];
            const int w = idf_lines >= 3 ? 45 : 15; /* the log format alone: likely an ESP-IDF program */
            f.strong += w;
            f.by_capture[static_cast<int>(ci)] += w;
            add_evidence(f.evidence, idf_first);
        }
        if (mesh_lines >= 2) {
            FamilyScore &f = fam[Kind::Meshtastic];
            f.strong += 35;
            f.by_capture[static_cast<int>(ci)] += 35;
            add_evidence(f.evidence, mesh_first);
            if (!f.fields.count("firmware")) f.fields["firmware"] = "Meshtastic";
        }
        int frames = 0;
        Kind fk = Kind::None;
        const std::string fr = binary_framing(cap.bytes, &frames, &fk);
        if (!fr.empty() && frames > framing_frames) {
            framing = fr;
            framing_kind = fk;
            framing_frames = frames;
            framing_cap = static_cast<int>(ci);
        }
    }
    if (framing_kind == Kind::Gnss || framing_kind == Kind::Klipper || framing_kind == Kind::Meshtastic) {
        FamilyScore &f = fam[framing_kind];
        const int w = std::min(70, 25 * framing_frames);
        f.strong += w;
        f.by_capture[framing_cap] += w;
        add_evidence(f.evidence, std::to_string(framing_frames) + " frame(s) " + framing);
    }

    Identity id;
    // the winner: the highest score; a boot stage loses to a program found clearly (30 and more)
    Kind win = Kind::None;
    int win_score = 0;
    bool app_clear = false;
    for (const auto &kv : fam)
        if (!base_layer(kv.first) && kv.second.score() >= 30) app_clear = true;
    for (const auto &kv : fam) {
        const int s = kv.second.score();
        if (s <= 0) continue;
        if (app_clear && base_layer(kv.first)) continue;
        if (win == Kind::None || s > win_score || (s == win_score && kv.first < win)) {
            win = kv.first;
            win_score = s;
        }
    }

    int baud_cap = -1;
    if (win != Kind::None) {
        const FamilyScore &f = fam[win];
        std::map<std::string, std::string> fields = f.fields;
        int conf = f.score();
        std::vector<std::string> evidence = f.evidence;
        for (const Kind r : related(win)) {
            const auto it = fam.find(r);
            if (it == fam.end() || it->second.score() <= 0) continue;
            conf += it->second.score() / 2;
            for (const char *key : {"chip", "rom", "board", "mcu", "download", "reason", "mode", "model", "maker"})
                if (!fields.count(key) && it->second.fields.count(key)) fields[key] = it->second.fields.at(key);
            for (const std::string &e : it->second.evidence) add_evidence(evidence, e);
        }
        id.kind = win;
        id.confidence = std::min(100, conf);
        id.evidence = evidence;
        // the capture that showed it best gives the speed
        int best_w = -1;
        for (const auto &kv : f.by_capture)
            if (kv.second > best_w) best_w = kv.second, baud_cap = kv.first;
        // an ESP's boot ROM prints at its own speed: the program's speed is the one of the clean text, if any
        if ((win == Kind::Esp8266 || win == Kind::EspRom) && best_clean >= 0 && best_clean != baud_cap &&
            captures[static_cast<std::size_t>(best_clean)].baud != 74880)
            baud_cap = best_clean;

        std::string chip = field(fields, "chip");
        if (!field(fields, "rom").empty()) chip = field(fields, "rom");
        if (chip.empty()) chip = field(fields, "mcu");
        id.chip = normalize_chip(chip);
        id.firmware = field(fields, "firmware");
        if (id.firmware.empty()) id.firmware = firmware_default(win);
        id.version = field(fields, "version");
        if ((win == Kind::EspIdf || win == Kind::ArduinoEsp32) && id.version.empty()) id.version = field(fields, "idf");
        id.board = field(fields, "board");
        if (id.board.empty()) id.board = field(fields, "machine");
        if (id.board.empty()) id.board = field(fields, "platform");
        switch (win) {
        case Kind::Gnss: {
            const std::string maker = field(fields, "maker"), model = field(fields, "model");
            id.chip = maker.empty() ? model : (model.empty() ? maker : maker + " " + model);
            std::string t;
            for (const std::string &k : talkers) t += (t.empty() ? "" : ", ") + talker_name(k);
            if (!t.empty()) id.detail = t;
            if (!field(fields, "protver").empty())
                id.detail += (id.detail.empty() ? "" : "; ") + std::string("protocol ") + field(fields, "protver");
            if (!field(fields, "hw").empty() && id.chip.find(field(fields, "hw")) == std::string::npos)
                id.detail += (id.detail.empty() ? "" : "; ") + field(fields, "hw");
            break;
        }
        case Kind::EspIdf:
            if (!field(fields, "project").empty())
                id.detail = "project " + field(fields, "project") +
                            (field(fields, "appver").empty() ? "" : " " + field(fields, "appver"));
            break;
        case Kind::AtModem:
            if (!field(fields, "model").empty()) id.chip = field(fields, "model");
            if (!field(fields, "maker").empty()) id.detail = field(fields, "maker");
            break;
        case Kind::Linux:
            if (!field(fields, "distro").empty()) id.detail = field(fields, "distro");
            else if (!field(fields, "host").empty()) id.detail = "host " + field(fields, "host");
            break;
        case Kind::Esp8266:
            if (!field(fields, "cause").empty())
                id.detail = "reset cause " + field(fields, "cause") + ", boot mode (" + field(fields, "mode") + ")";
            break;
        default: break;
        }
        if (id.detail.empty() && field(fields, "download") == "1") id.detail = "in download mode (BOOT / IO0 held low)";
        else if (id.detail.empty() && !field(fields, "reason").empty()) id.detail = "reset: " + field(fields, "reason");
    } else if (best_clean >= 0) {
        id.kind = ctx.usb == Family::Arduino ? Kind::Sketch : Kind::Text;
        id.confidence = id.kind == Kind::Sketch ? 45 : 30;
        baud_cap = best_clean;
        for (const std::string &l : lines_of[static_cast<std::size_t>(best_clean)]) {
            if (trim(l).empty()) continue;
            add_evidence(id.evidence, l);
            if (id.evidence.size() >= 3) break;
        }
    } else if (!framing.empty()) {
        id.kind = Kind::Binary;
        id.confidence = framing_kind == Kind::Binary && framing.find("protobuf") != std::string::npos ? 20 : 60;
        baud_cap = framing_cap;
        add_evidence(id.evidence, std::to_string(framing_frames) + " frame(s) " + framing);
    } else if (total > 0) {
        id.kind = Kind::Binary;
        id.confidence = 10;
        id.detail = ctx.native_usb ? "bytes that are not text" : "noise at every speed tried: binary, or another speed";
        for (std::size_t ci = 0; ci < captures.size(); ++ci)
            if (!captures[ci].bytes.empty()) {
                add_evidence(id.evidence, "at " + std::to_string(captures[ci].baud) + ": " +
                                              hex_bytes(captures[ci].bytes, 0, 12));
                if (baud_cap < 0) baud_cap = static_cast<int>(ci);
            }
        baud_cap = -1; /* no speed to recommend */
    } else {
        id.kind = Kind::Silent;
        id.confidence = 0;
    }
    if (!framing.empty() && id.framing.empty()) id.framing = framing;
    id.title = kind_title(id.kind);
    if (baud_cap >= 0) {
        id.baud = captures[static_cast<std::size_t>(baud_cap)].baud;
        for (const std::string &l : lines_of[static_cast<std::size_t>(baud_cap)]) {
            if (id.boot.size() >= kBootLinesMax) break;
            if (id.boot.empty() && trim(l).empty()) continue; /* the output starts at its first line of text */
            id.boot.push_back(safe_text(l, 160));
        }
    }
    // a boot ROM at 74880 adds its lines too (level 2's second listen)
    for (std::size_t ci = 0; ci < captures.size(); ++ci) {
        if (static_cast<int>(ci) == baud_cap || captures[ci].baud != 74880 || id.boot.size() >= kBootLinesMax) continue;
        const auto it = fam.find(Kind::Esp8266);
        if (it == fam.end() || !it->second.by_capture.count(static_cast<int>(ci))) continue;
        id.boot.push_back("--- at 74880 (the ESP8266 boot ROM) ---");
        for (const std::string &l : lines_of[ci]) {
            if (id.boot.size() >= kBootLinesMax) break;
            id.boot.push_back(safe_text(l, 160));
        }
    }
    while (!id.boot.empty() && trim(id.boot.back()).empty()) id.boot.pop_back();
    return id;
}

Identity merge(const Identity &a, const Identity &b)
{
    if (!b.found() && b.kind != Kind::Text) return a.kind == Kind::None ? b : a;
    if (!a.found() && a.kind != Kind::Text) return b;
    Identity w = a.confidence >= b.confidence ? a : b;
    const Identity &o = a.confidence >= b.confidence ? b : a;
    if (w.chip.empty()) w.chip = o.chip;
    if (w.board.empty()) w.board = o.board;
    if (w.detail.empty()) w.detail = o.detail;
    if (w.framing.empty()) w.framing = o.framing;
    if (w.firmware.empty() && o.kind == w.kind) w.firmware = o.firmware;
    if (w.version.empty() && o.kind == w.kind) w.version = o.version;
    for (const std::string &e : o.evidence) add_evidence(w.evidence, e);
    for (const std::string &l : o.boot) {
        if (w.boot.size() >= kBootLinesMax) break;
        w.boot.push_back(l);
    }
    if (!w.baud) w.baud = o.baud;
    w.confidence = std::min(100, w.confidence + (o.kind == w.kind ? o.confidence / 2 : 0));
    return w;
}

const char *confidence_word(int confidence)
{
    if (confidence >= 70) return "Sure";
    if (confidence >= 40) return "Likely";
    return "A guess";
}

std::string behind_text(const Identity &id)
{
    switch (id.kind) {
    case Kind::Silent: return "Nothing heard";
    case Kind::None: return "Not identified";
    case Kind::EspIdf:
    case Kind::ArduinoEsp32:
    case Kind::EspRom: return id.chip.empty() || id.chip.size() > 16 ? "An ESP32 board" : id.chip;
    case Kind::Esp8266: return "ESP8266";
    case Kind::MicroPython:
    case Kind::CircuitPython: return id.chip.empty() || id.chip.size() > 16 ? "Python board" : id.chip;
    case Kind::ArduinoBoot: return id.chip.empty() || id.chip.size() > 16 ? "Arduino board" : id.chip;
    case Kind::Meshtastic: return "Meshtastic node";
    case Kind::Marlin: return "Marlin printer";
    case Kind::Klipper: return "Klipper MCU";
    case Kind::Grbl: return "GRBL controller";
    case Kind::OpenWrt: return "OpenWrt router";
    case Kind::MikroTik: return "MikroTik router";
    case Kind::Cisco: return "Cisco console";
    case Kind::UBoot: return "U-Boot";
    case Kind::Linux: return "Linux console";
    case Kind::AtModem: return "AT modem";
    case Kind::Gnss: return "GPS receiver";
    case Kind::Sketch: return "Arduino sketch";
    case Kind::Text: return "Text, unknown";
    case Kind::Binary: return "Binary data";
    }
    return kind_title(id.kind);
}

std::string firmware_text(const Identity &id)
{
    if (id.firmware.empty()) return id.version;
    return id.version.empty() ? id.firmware : id.firmware + " " + id.version;
}

bool identity_recommendation(const Identity &id, int *baud, std::string *reason)
{
    if (!id.baud || id.kind == Kind::Silent || id.kind == Kind::None || id.kind == Kind::ArduinoBoot) return false;
    if (id.kind == Kind::Binary && id.framing.empty()) return false;
    if (baud) *baud = id.baud;
    if (reason) {
        const std::string at = std::to_string(id.baud);
        switch (id.kind) {
        case Kind::Gnss: *reason = "GPS found: NMEA at " + at; break;
        case Kind::Esp8266:
            *reason = id.baud == 74880 ? "ESP8266 ROM at 74880; sketches mostly 115200" : "ESP8266 found: program at " + at;
            break;
        case Kind::EspIdf:
        case Kind::ArduinoEsp32:
        case Kind::EspRom: *reason = (id.chip.empty() ? "ESP32" : id.chip) + " found: its log at " + at; break;
        case Kind::Text:
        case Kind::Sketch: *reason = "Readable text found at " + at; break;
        case Kind::Binary: *reason = "Binary frames found at " + at; break;
        default: *reason = std::string(kind_title(id.kind)) + " found at " + at; break;
        }
        if (reason->size() > 48) *reason = std::string(kind_title(id.kind)).substr(0, 30) + " at " + at;
    }
    return true;
}

std::string identity_summary(const Identity &id, const std::string &port_title, const std::string &tty)
{
    std::string s = "Serial Console - what is on the other end of " + tty + "\n";
    char when[32] = "";
    if (id.when) {
        struct tm tm;
        localtime_r(&id.when, &tm);
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
    }
    const char *how = id.level == 3 ? "asked" : id.level == 2 ? "reset and listened" : "listened";
    s += "Device:      " + port_title + "\n";
    s += "Behind it:   " + std::string(kind_title(id.kind)) + "\n";
    if (!id.chip.empty()) s += "Chip:        " + id.chip + "\n";
    if (!firmware_text(id).empty()) s += "Firmware:    " + firmware_text(id) + "\n";
    if (!id.board.empty()) s += "Board:       " + id.board + "\n";
    if (!id.detail.empty()) s += "Details:     " + id.detail + "\n";
    if (!id.framing.empty()) s += "Framing:     " + id.framing + "\n";
    s += "Confidence:  " + std::string(confidence_word(id.confidence)) + " (" + std::to_string(id.confidence) + ")\n";
    s += "How:         " + std::string(how) + (when[0] ? std::string(", ") + when : std::string()) + "\n";
    int baud = 0;
    std::string reason;
    if (identity_recommendation(id, &baud, &reason)) s += "Recommended: " + std::to_string(baud) + " 8N1 (" + reason + ")\n";
    if (!id.evidence.empty()) {
        s += "Evidence:\n";
        for (const std::string &e : id.evidence) s += "  " + e + "\n";
    }
    if (!id.boot.empty()) {
        s += "Output:\n";
        for (const std::string &l : id.boot) s += "  " + l + "\n";
    }
    return s;
}

std::vector<std::pair<std::string, std::string>> identity_to_keys(const Identity &id)
{
    std::vector<std::pair<std::string, std::string>> kv;
    if (id.kind == Kind::None) return kv;
    const auto add = [&kv](const char *key, const std::string &value) {
        if (!value.empty()) kv.emplace_back(key, safe_text(value, 120));
    };
    add("id_kind", kind_key(id.kind));
    add("id_chip", id.chip);
    add("id_firmware", id.firmware);
    add("id_version", id.version);
    add("id_board", id.board);
    add("id_detail", id.detail);
    add("id_framing", id.framing);
    add("id_conf", std::to_string(id.confidence));
    add("id_baud", std::to_string(id.baud));
    add("id_level", std::to_string(id.level));
    add("id_probe", id.probe);
    add("id_when", std::to_string(static_cast<long long>(id.when)));
    for (const std::string &e : id.evidence) add("id_ev", e);
    std::size_t n = 0;
    for (const std::string &l : id.boot) {
        if (++n > 16) break;
        kv.emplace_back("id_boot", safe_text(l, 120)); /* empty lines too: the output keeps its shape */
    }
    return kv;
}

bool identity_apply_key(Identity &id, const std::string &key, const std::string &value)
{
    if (key.compare(0, 3, "id_") != 0) return false;
    const auto num = [&value](long long lo, long long hi) {
        if (value.empty() || value.size() > 12) return lo - 1;
        for (const char c : value)
            if (c < '0' || c > '9') return lo - 1;
        const long long v = std::atoll(value.c_str());
        return v >= lo && v <= hi ? v : lo - 1;
    };
    const std::string v = safe_text(value, 120);
    if (key == "id_kind") {
        id.kind = kind_from_key(value);
        id.title = kind_title(id.kind);
    } else if (key == "id_chip") id.chip = v;
    else if (key == "id_firmware") id.firmware = v;
    else if (key == "id_version") id.version = v;
    else if (key == "id_board") id.board = v;
    else if (key == "id_detail") id.detail = v;
    else if (key == "id_framing") id.framing = v;
    else if (key == "id_probe") id.probe = v;
    else if (key == "id_conf") { const long long n = num(0, 100); if (n >= 0) id.confidence = static_cast<int>(n); }
    else if (key == "id_baud") { const long long n = num(0, 4000000); if (n >= 0) id.baud = static_cast<int>(n); }
    else if (key == "id_level") { const long long n = num(1, 3); if (n >= 1) id.level = static_cast<int>(n); }
    else if (key == "id_when") { const long long n = num(0, 99999999999LL); if (n >= 0) id.when = static_cast<std::time_t>(n); }
    else if (key == "id_ev") { if (id.evidence.size() < kEvidenceMax && !v.empty()) id.evidence.push_back(v); }
    else if (key == "id_boot") { if (id.boot.size() < kBootLinesMax) id.boot.push_back(v); }
    return true;
}

} // namespace sc
