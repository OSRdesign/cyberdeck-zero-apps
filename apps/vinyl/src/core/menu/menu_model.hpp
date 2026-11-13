/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl Menu (P2a; the full-UX canvas Menu-480x320 / Menu-320x480, SleepTimer-480x320, Playback-480x320, Keys-480x320 /
 * Keys-320x480, About-480x320; report 060 with the owner's decisions of 2026-11-13): what the Menu pages list, pure and
 * tested (tests/test_menu.cpp); the view draws it (ui/menu_pages.cpp).
 *
 *   Menu        TOOLS: Sleep timer (Off or the time left); SETTINGS: Playback, Output (the output's name), Keys, About.
 *               Only built features (owner decision D32, "OK"): no Equalizer or Library row until their phases
 *   Sleep timer STOP PLAYING AFTER: Off, 15 minutes, 30 minutes, 60 minutes, This track, This album or queue (the chosen
 *               one with the blue dot); the status line while it runs ("Stops in 29:12. ...")
 *   Playback    Gapless (On / Off: Enter or a tap toggles it), ReplayGain (Off / Track / Album: opens its pick list),
 *               Remember position in long files (On / Off: toggles); no Speed row (D32); the strip "ReplayGain evens out
 *               the loudness between tracks."
 *   ReplayGain  the pick list: Off, Track, Album (the chosen one with the blue dot)
 *   Keys        two columns, the key and what it does: the app's real key map (key_help(), checked against
 *               shortcut_command() by the tests); the rows that act on Now playing only say so
 *   About       name and version, a line, the formats, THIS OUTPUT (the playing output's levels, from its volume model),
 *               the libraries, where the licences are
 *
 * Row kinds follow the toolkit's affordances (design QA D-45): Open (a chevron), Edit (the value, then a chevron),
 * Choice (Action: a white dot, ActionRunning: the blue dot of the chosen one), Heading (a section title, never selected).
 * LVGL-free.
 */
#pragma once

#include "../settings/settings.hpp"
#include "../sleep/sleep_timer.hpp"
#include "../volume/output_volume.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vinyl {
namespace menu {

enum class Page { Menu, Sleep, Playback, ReplayGain, Keys, About };
const char *page_title(Page p); /* the header's title: "Menu", "Sleep timer", "Playback", "ReplayGain", "Keys", "About" */

struct Row {
    enum class Kind { Heading, Open, Edit, Choice };
    Kind kind = Kind::Open;
    std::string id;    /* what a tap or Enter does: "sleep", "playback", "output", "keys", "about"; "gapless",
                          "replaygain", "remember"; a choice's value ("off", "15" ... "album") */
    std::string text;
    std::string value; /* Edit: its value; Open: an optional value (the Output row's name) */
    bool chosen = false; /* Choice: the blue dot */
    bool operator==(const Row &o) const {
        return kind == o.kind && id == o.id && text == o.text && value == o.value && chosen == o.chosen;
    }
};

struct State {
    SleepMode sleep = SleepMode::Off;
    uint64_t sleep_left_ms = 0;
    std::string output; /* the output's name as now playing shows it ("" when none is known) */
    Settings settings;
};

std::vector<Row> menu_rows(const State &s);
std::vector<Row> sleep_rows(SleepMode chosen);
std::vector<Row> playback_rows(const Settings &s);
std::vector<Row> replaygain_rows(ReplayGainMode chosen);
/* the rows of a list page (Menu, Sleep, Playback, ReplayGain) */
std::vector<Row> rows_of(Page p, const State &s);
/* the heading line over a list page's rows ("STOP PLAYING AFTER", "REPLAYGAIN"), "" for none */
std::string page_heading(Page p);
/* the strip under a list page's rows ("" for none): the sleep timer's status while it runs, Playback's line, the
 * ReplayGain pick list's line */
std::string page_hint(Page p, const State &s);

/* Keys: the first item (-1: none), the item d items on from i (headings skipped, no wrap). */
int first_item(const std::vector<Row> &rows);
int step_item(const std::vector<Row> &rows, int i, int d);
/* the row to select when the page opens: the chosen choice, else the first item */
int initial_item(const std::vector<Row> &rows);
/* a sleep choice's id ("off", "15", "30", "60", "track", "album") and back */
std::string sleep_id(SleepMode m);
bool sleep_from_id(const std::string &id, SleepMode &out);

/* ------------------------------------------------------------------------------------------ the key map */

/* What a key does outside the lists (evdev codes: the physical key, design QA D-32). The Keys page and the controller
 * both read this table, the tests check one against the other. */
enum class KeyCmd {
    None, Toggle, Shuffle, Repeat, Queue, Browse, Output, Menu, Sleep, VolumeDown, VolumeUp, Prev, Next,
    QueueUp, QueueDown, QueueRemove, QueueClear,
};
/* evdev codes used */
enum : uint32_t {
    kKeyMinus = 12, kKeyEqual = 13, kKeyQ = 16, kKeyR = 19, kKeyT = 20, kKeyY = 21, kKeyU = 22, kKeyO = 24, kKeyS = 31,
    kKeyD = 32, kKeyX = 45, kKeyC = 46, kKeyB = 48, kKeyM = 50, kKeyComma = 51, kKeyDot = 52, kKeySpace = 57,
    kKeyDelete = 111,
};
/* A letter or punctuation key's command; queue_page: the queue page's editing letters (U, D, X, C) act there only. */
KeyCmd letter_command(uint32_t evdev, bool queue_page);
/* With the modifiers (KBD_MOD_* bits; Ctrl is bit 1): Ctrl+O is the options key (design QA D-42, AC 10b: the pointer's
 * held click sends it), so it opens the Menu; plain O stays Output. Any other Ctrl or Alt chord does nothing. */
constexpr uint32_t kModCtrl = 1u << 1, kModAlt = 1u << 2;
KeyCmd shortcut_command(uint32_t evdev, uint32_t mods, bool queue_page);

struct KeyHelp {
    std::string keys;   /* the key's name: "Space", "Q, B, O, M" */
    std::string action; /* "Play or pause" */
    std::vector<uint32_t> codes;  /* the letter keys it names (evdev), checked against shortcut_command */
    std::vector<KeyCmd> cmds;     /* what each of them does (same order) */
    bool queue_page = false;      /* the letters act on the queue page */
    uint32_t mods = 0;            /* the modifiers held with them (Ctrl+O) */
};
std::vector<KeyHelp> key_help();

/* ------------------------------------------------------------------------------------------ About */

/* THIS OUTPUT: "Sharkoon Mobile DAC: -45.0 to 0.0 dB, 23 levels of 2.05 dB." from the output's volume model; a control
 * without dB data: "... 23 steps (no dB scale)."; software: Vinyl's own volume (a card without a playback mixer);
 * known false: "...: the level is not known yet." */
std::string about_output_line(const std::string &label, const OutputVolumeModel &m, bool known, bool software);

struct About {
    std::string name;        /* "Vinyl 0.2.0" */
    std::string line;        /* "A music player for CyberdeckZero Launcher boards. MIT licence." */
    std::string formats, output, libraries, licences;
};
About about(const std::string &version, const std::string &output_line);

}  // namespace menu
}  // namespace vinyl
