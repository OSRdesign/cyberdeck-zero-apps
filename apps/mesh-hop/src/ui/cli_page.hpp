/*
 * SPDX-License-Identifier: MIT
 *
 * The CLI page (report 033, phase P5, the line console T1): what App keeps for it (cli_page.cpp has App's methods for it).
 *
 *   Tools > CLI     (0.6.0: Home > CLI) connected to a node: the page, and a session starts; not connected: a box says why
 *                   (nothing starts).
 *   a session       Client::start_relay() (Mesh Hop's own commands wait), then, once relay_ready(), the bundled meshcore-cli
 *                   on a pty: `mccli -c off -t 127.0.0.1 -p <relay port>`, TERM=dumb, MESHHOP_DATA = Mesh Hop's data folder
 *                   (meshcore-cli keeps the repeaters' passwords in its cli/ folder). The caption says Starting... until its
 *                   first prompt. It ends by itself when meshcore-cli exits (the reason: python3 missing, the wrapper does not
 *                   match upstream (exit 3), an exit code, a signal) or when the relay ends (the link to the node lost...): the
 *                   reason shows in the output and the caption. Leaving the page (Esc) ends it: meshcore-cli's process group gets
 *                   SIGHUP + SIGTERM (SIGKILL after 1.5 s), stop_relay(), and Mesh Hop resumes and reads the node again (P3).
 *   the page        the output (Serial Console's line view: the newest lines, a drag or PgUp / PgDn scrolls back, the bottom
 *                   follows again) in the launcher's monospace face; one entry line and Help at the bottom. Enter sends the
 *                   line + "\n", Up / Down go through the lines typed (this run only, never a password), Ctrl+C sends the
 *                   interrupt (at its prompt meshcore-cli then ends: prompt_toolkit's Ctrl+C), Ctrl+H opens Help, Tab does
 *                   nothing (no completion in T1), Esc leaves. A password prompt turns the entry to '*' and its echo never
 *                   shows. Touch only: a tap on the entry brings the on-screen keyboard by the shared rule (with it up at
 *                   640x480 only the entry shows: the caption line then shows the line meshcore-cli is on); Enter sends and the
 *                   keyboard goes, as after a Send in Chats (the answer shows); Esc puts it away first.
 *   Help            the curated list (cli_logic.cpp): a command row puts its text into the entry (a tap on the selected row, or
 *                   Enter); All commands shows `mccli -h` of the installed bundle (read once, in the background).
 *   warm-up         `mccli -v` once per run in the background, at nice 19 / ionice idle, when the CLI page first opens (0.7.1;
 *                   0.6.0 and 0.7.0 ran it when Mesh Hop first reached a node, with the connection); the first session waits
 *                   for it (Starting meshcore-cli...): Python compiles the bundle's .pyc files (the first start on a Pi Zero 2 W
 *                   takes 10-20 s more) and the version is known for the caption. MESHHOP_NO_WARMUP=1 skips it.
 *   0.7.0           the page opens from Tools > CLI, or from a Repeater page's More > Open in CLI (then `to <its 64 hex key>` goes
 *                   to meshcore-cli once, at its first prompt: a full key matches one contact only, whatever the names, report
 *                   036 section 4.2); its Esc goes back where it was opened from.
 *   0.7.2           the To... button, its picker and Ctrl+T are gone (a repeater is picked in Contacts > it > More > Open in
 *                   CLI); the entry takes the room. Opened from Tools, the caption says how to pick one until a line is sent
 *                   (cli_pick_hint). The echo of the automatic `to <64 hex>` line shows as "to <name> <909192" (output only).
 */

#pragma once

#include "cli_logic.hpp"
#include "mesh_app.hpp"
#include "pty_process.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace meshhop {

struct CliState {
    CliPhase phase = CliPhase::Off;
    std::string end_text;
    bool end_error = false;
    uint64_t phase_ms = 0;               // when the phase began
    uint64_t relay_gone_ms = 0;          // the relay ended by itself (the console closed it): the process end is awaited until +3 s
    int relay_port = 0;
    uint32_t relay_seen = 0;             // Client::relay_sessions_ended() when this session's relay started
    std::unique_ptr<meshzero::PtyProcess> proc;
    std::vector<std::unique_ptr<meshzero::PtyProcess>> dying;   // sessions left: reaped by the tick
    CliScreen screen;
    CliHistory history;
    bool typing = false;                 // touch: the entry was tapped (the on-screen keyboard may show)
    bool password = false;               // the entry is in password mode
    int help = 0;                        // 0 the console, 1 Help, 2 All commands
    bool sent_any = false;               // a line went to meshcore-cli in this session (the Tools hint goes)
    int help_sel = -1;
    std::vector<HelpItem> help_rows;     // the rows of the Help list, in order (sections included)
    uint32_t help_sig = 0;

    // the warm-up (mccli -v) and All commands (mccli -h): background commands of the shared runner
    bool warm_started = false, warm_done = false;
    int warm_exit = -1;
    std::string version;                 // "v1.6.6" from mccli -v
    int usage_state = 0;                 // 0 not read, 1 reading, 2 read, 3 failed
    std::string usage_text;

    // the output view
    const lv_font_t *mono = nullptr;
    int char_w = 1, line_h = 1, cols = 80, rows = 1;
    bool follow = true;
    meshzero::term::ViewPos top;
    uint64_t drawn_version = ~0ull, last_draw_ms = 0;
    std::string shown;
    int drag = 0;
    bool out_visible = true;

    // widgets (in the CLI page, page_[Tab::Cli]; Help and All commands are panels over it)
    lv_obj_t *out = nullptr, *text = nullptr, *waiting = nullptr, *entry = nullptr, *help_btn = nullptr;
    lv_obj_t *help_panel = nullptr, *usage_panel = nullptr, *usage_box = nullptr, *usage_label = nullptr;
    RowList help_list;
};

} // namespace meshhop
