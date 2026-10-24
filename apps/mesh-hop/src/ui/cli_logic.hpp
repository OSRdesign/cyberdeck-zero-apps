/*
 * SPDX-License-Identifier: MIT
 *
 * The CLI page (report 033, phase P5, the line console T1): the parts without LVGL, unit tested (tests/unit/test_cli_ui.inc).
 *
 * The page runs the bundled meshcore-cli (/opt/mesh-hop/cli/mccli, phase P4) on a pty with TERM=dumb and -c off, connected to
 * Mesh Hop's loopback relay (phase P3). In that mode prompt_toolkit prints a plain prompt ("SimNode> ", "SimNode/Hilltop|0> ")
 * and echoes every character it reads itself, then "\r\n" when the line is accepted; the pty's own echo is off.
 *
 *   CliScreen   the output: a TermBuffer (core/term.hpp, Serial Console's line view) fed with what meshcore-cli writes, and the
 *               password rule. meshcore-cli asks a password with "Password for <name>: " (login on a repeater or room, the
 *               first time) or "Password: " (login <name> $); in dumb mode prompt_toolkit echoes the typed password in clear.
 *               While the line being received is such a prompt, password_prompt() is true (the entry shows '*'); the line
 *               sent then (sent_line(true)) is never shown: what meshcore-cli writes up to the next line end (its echo) is
 *               dropped. The prompt texts are upstream's (meshcore_cli.py v1.6.6 lines 1867 and 3388): a pin bump checks them.
 *   CliHistory  the lines typed in this run of the app (never on disk): Up / Down, the line being written kept as a draft.
 *   the rest    the bytes of a typed line, the texts of how a session ended, the curated Help page (every command checked
 *               against meshcore-cli v1.6.6 and the firmware's CommonCLI.cpp, MeshCore 81dbae5), inserting a command.
 */

#pragma once

#include "term.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace meshhop {

constexpr size_t kCliMaxLines = 1500;        // the lines of output kept (the scrollback)
constexpr size_t kCliMaxCols = 512;          // a longer line is cut into two
constexpr size_t kCliMaxEntry = 300;         // the entry line, in bytes
constexpr size_t kCliHistory = 100;          // the lines Up / Down go through
constexpr size_t kCliMaskMax = 1024;         // the most echo bytes dropped after a password (then the view shows again)
constexpr const char *kCliInterrupt = "\x03";
constexpr const char *kCliDefaultPath = "/opt/mesh-hop/cli/mccli";

class CliScreen {
public:
    explicit CliScreen(size_t max_lines = kCliMaxLines, size_t max_cols = kCliMaxCols);

    /* What meshcore-cli wrote (the pty's output). */
    void feed(const std::string &bytes);
    /* A line of Mesh Hop's own ("-- meshcore-cli ended"), on a line of its own. */
    void note(const std::string &text);
    void clear();
    /* A line went to meshcore-cli. password: it answered a password prompt (its echo is dropped up to the line end). */
    void sent_line(bool password);
    /* 0.7.2: a line Mesh Hop sent by itself (Open in CLI's `to <64 hex key>`): its echo is dropped up to the line end as after a
     * password, and `shown` ("to Hilltop repeater") stands in its place. Never used for what the user types. */
    void sent_line_shown(const std::string &shown);

    /* The line being received is a password prompt that was not answered yet. */
    bool password_prompt() const;
    /* The line being received is meshcore-cli's prompt ("...> "): it waits for a line. */
    bool at_prompt() const;
    bool masking() const { return masking_; }
    size_t masked_bytes() const { return masked_; }
    /* The line being received (the prompt, or what is printed after it). */
    std::string current_line() const;
    /* Every line kept, joined by '\n' (tests and the scripted flows). */
    std::string text() const;
    const meshzero::term::TermBuffer &buffer() const { return buf_; }

private:
    meshzero::term::TermBuffer buf_;
    bool masking_ = false;
    size_t mask_run_ = 0, masked_ = 0;
    std::string shown_;
};

/* True for "Password for <name>: " and "Password: " (meshcore-cli's two password prompts). */
bool is_password_prompt(const std::string &line);

class CliHistory {
public:
    explicit CliHistory(size_t cap = kCliHistory) : cap_(cap) {}
    /* A line that was sent (not an empty one, not the same as the last; never a password). The cursor goes back to the end. */
    void add(const std::string &line);
    /* Up: the older line in `out` (true), the entry's text `current` kept as the draft when the cursor leaves the end. */
    bool up(const std::string &current, std::string &out);
    /* Down: the newer line, or the draft back at the end (true); false at the end already. */
    bool down(std::string &out);
    void reset() { at_ = entries_.size(); }
    size_t size() const { return entries_.size(); }
    bool at_end() const { return at_ == entries_.size(); }

private:
    size_t cap_;
    std::vector<std::string> entries_;
    size_t at_ = 0;
    std::string draft_;
};

/* The bytes for a typed line: no control character (a pasted CR, LF, ESC or TAB would act as a key), then "\n", which
 * prompt_toolkit reads as Enter. */
std::string cli_line_bytes(const std::string &typed);

/* The command to insert at `cursor` (bytes) of the entry: a space before it when the text before the cursor does not end with
 * one. *cursor_after: where the cursor goes (after the insert). */
std::string cli_insert(const std::string &entry, size_t cursor, const std::string &what, size_t *cursor_after);

/* How meshcore-cli ended, for the output and the caption line. python_found: /usr/bin/python3 is there (an exit 127 without it
 * is the missing python3). */
std::string cli_exit_text(int exit_code, int term_signal, bool python_found);
/* Why the relay ended the session (Client::relay_end_reason()), in the page's words. */
std::string cli_relay_end_text(const std::string &reason);

/* The page's phases (the caption line says them). */
enum class CliPhase { Off, Waiting, Starting, Running, Ended };
const char *cli_phase_name(CliPhase p);

/* ---- the Help page */
enum class HelpKind { Section, Insert, Info, Warning, Usage };
struct HelpItem {
    HelpKind kind = HelpKind::Info;
    std::string label;                   // the command (or the section's title)
    std::string text;                    // what it does
    std::string insert;                  // Insert: what a tap (or Enter) puts into the entry
};
/* The curated list: How it works, Your node, Repeaters and rooms, Careful (the dangerous commands, no confirmation), and the
 * row of All commands (meshcore-cli -h). */
const std::vector<HelpItem> &cli_help_items();

} // namespace meshhop
