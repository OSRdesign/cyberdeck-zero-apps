/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: "What is on the other end?" - the classifier (no LVGL, no I/O; tests/test_identify.cpp runs it on
 * the text samples of tests/fixtures).
 *
 * Input: what came from the port, one Capture per speed listened at (the bytes and the speed), plus what the USB
 * identity says (Context). Each capture is cut into clean lines (core/term.hpp: colour codes removed, bytes that are
 * not text shown as dots) when it reads as text at its speed, and scanned for binary frames otherwise.
 *
 * The text rules are a table (kRules in identify.cpp): a family, a line pattern, a weight, and fixed values. A pattern
 * is a small glob, matched anywhere in a line unless anchored:
 *   ^ $        anchor at the start / the end of the line
 *   *          any run of characters (as few as possible)
 *   #          one or more digits
 *   ?          any one character
 *   {name}     a word, captured: leading spaces skipped, then characters up to a space or one of , ; : ( ) [ ] " ' *
 *   {name#}    a word that starts with a digit (a version number)
 *   {name*}    a text, captured: as few characters as possible up to what follows (the rest of the line at the end)
 *   \x         the character x itself (\* \# \? \{ \^ \$ \\)
 * Captures fill the result (chip, version, board, model ...). Code adds what a table cannot say: NMEA sentences with a
 * valid checksum, the ESP-IDF log format ("I (123) tag: ..."), Meshtastic's log format, and binary framing with valid
 * checksums (UBX, RTCM 3, MAVLink heartbeats, Klipper, Meshtastic API frames, SLIP), or a protobuf-like stream.
 *
 * Each family adds up the weights of its distinct rules that matched (a weak rule counts only next to a strong one of
 * the same family); the highest wins (ties: the more specific family, the order of Kind). The confidence is that
 * total, capped at 100: 70 and more "Sure", 40 and more "Likely", under 40 "A guess".
 */
#pragma once

#include "devices.hpp"

#include <cstdint>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace sc {

/* What is behind the port. The order is the tie-break: the more specific first. */
enum class Kind {
    Meshtastic,
    ArduinoEsp32,
    EspIdf,
    Esp8266,
    EspRom,
    MicroPython,
    CircuitPython,
    Marlin,
    Klipper,
    Grbl,
    OpenWrt,
    MikroTik,
    Cisco,
    UBoot,
    Linux,
    AtModem,
    Gnss,
    ArduinoBoot,
    Sketch,  /* plain text from an Arduino board (its USB id says so) */
    Text,    /* readable text, not recognised */
    Binary,  /* bytes that are not text */
    Silent,  /* nothing came */
    None,    /* not identified yet */
};

/* "esp-idf", "gnss"... (the settings file) and back */
const char *kind_key(Kind kind);
Kind kind_from_key(const std::string &key);
/* "ESP32 (ESP-IDF app)", "GPS / GNSS receiver"... */
const char *kind_title(Kind kind);

struct Capture {
    std::string bytes;
    int baud = 0;
};

struct Context {
    Family usb = Family::Unknown; /* from the device table */
    bool native_usb = false;      /* the microcontroller's own USB: the speed does not matter */
};

struct Identity {
    Kind kind = Kind::None;
    std::string title;       /* kind_title() */
    std::string chip;        /* "ESP32-S3", "ATmega328P", "RP2040", "u-blox NEO-M8N" */
    std::string firmware;    /* "ESP-IDF", "Marlin", "MicroPython", "NMEA" */
    std::string version;     /* "v5.1.2", "2.1.2.1" */
    std::string board;       /* "Raspberry Pi Pico", "Ender-3 V2", "heltec-v3" */
    std::string detail;      /* "project weather-station 1.4.2", "MAC 24:0a:c4:...", "in download mode" */
    std::string framing;     /* binary: "UBX (B5 62)" */
    std::vector<std::string> evidence; /* the lines (or frames) that matched, cut to kEvidenceChars */
    std::vector<std::string> boot;     /* the first clean lines it printed */
    int confidence = 0;      /* 0..100 */
    int baud = 0;            /* the speed it was found at (0: none) */
    int level = 0;           /* 1 listened, 2 reset and listened, 3 asked */
    std::string probe;       /* the probe that found it (probe.hpp probe_key()) */
    std::time_t when = 0;    /* wall clock (the boards have no RTC: it may be off before NTP) */

    bool found() const { return kind != Kind::None && kind != Kind::Silent; }
};

constexpr std::size_t kEvidenceChars = 72;
constexpr std::size_t kEvidenceMax = 8;
constexpr std::size_t kBootLinesMax = 40;

/* Classifies what came. The captures in the order they were made; ctx from the USB identity. */
Identity classify(const std::vector<Capture> &captures, const Context &ctx);
/* The better of two results of the same board (level 2 at two speeds): the stronger family, then the fields, the
 * evidence and the boot lines of both. */
Identity merge(const Identity &a, const Identity &b);

/* "Sure", "Likely", "A guess" */
const char *confidence_word(int confidence);
/* "ESP32-S3", "ESP-IDF v5.1.2" ...: the card's short texts */
/* "Behind it": short, it is a value beside its key on a 640x480 row (at most 16 characters): the chip when it names
 * the board ("ESP32-S3", "RP2040", "ATmega328P"), else what it is ("GPS receiver", "Marlin printer"); "Nothing
 * heard" when silent. The Firmware row says the rest. */
std::string behind_text(const Identity &id);
std::string firmware_text(const Identity &id); /* "ESP-IDF v5.1.2" */
/* The recommendation the identity makes: the speed and one short sentence for the status line (<= 48 characters):
 * "GPS found: NMEA at 9600". false when it recommends nothing (silent, nothing found). */
bool identity_recommendation(const Identity &id, int *baud, std::string *reason);
/* The summary written by "Copy summary to the log file". */
std::string identity_summary(const Identity &id, const std::string &port_title, const std::string &tty);

/* For the settings file: key=value pairs (repeated keys for the evidence and the boot lines) and back. Values are
 * made safe (printable ASCII, no line ends, capped). */
std::vector<std::pair<std::string, std::string>> identity_to_keys(const Identity &id);
/* Applies one key of a [port] section; false when the key is not an identity key. */
bool identity_apply_key(Identity &id, const std::string &key, const std::string &value);

/* ---------------------------------------------------------------- the pieces, for the tests */

/* The glob above on one line; captures by name. */
bool match_pattern(const std::string &pattern, const std::string &line, std::map<std::string, std::string> *captures);
/* The lines of a capture as text (colour codes and line editing applied, other bytes as dots), at most max lines. */
std::vector<std::string> clean_lines(const std::string &bytes, std::size_t max = 400);
/* "$GPGGA,...*hh": a sentence with a valid checksum; *talker gets "GP", "GN"... */
bool nmea_valid(const std::string &line, std::string *talker);
/* The framing of a binary capture ("UBX (B5 62)"; "" when none validates) and how many frames validated. */
std::string binary_framing(const std::string &bytes, int *frames, Kind *kind);
/* Klipper's CRC-16 (CCITT, as klipper/src/generic/crc16_ccitt.c) and RTCM 3's CRC-24Q, MAVLink's X.25. */
std::uint16_t crc16_klipper(const std::string &bytes);
std::uint32_t crc24q(const std::string &bytes);
std::uint16_t crc_x25(const std::string &bytes, std::uint16_t crc = 0xFFFF);

} // namespace sc
