/*
 * SPDX-License-Identifier: MIT
 *
 * The CLI page (report 033, phase P5): App's methods for it (see cli_page.hpp). Sizes from the layout service only, as the
 * other pages (Geo: the page column, the ROW token for the entry and Help, the CAPTION size for the monospace output).
 */

#include "cli_page.hpp"

#include "app.hpp"
#include "async_run.hpp"
#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cctype>
#include <unistd.h>

using namespace meshzero;

namespace meshhop {

namespace {

constexpr uint32_t kCliText = CP0_UI_LIST_TEXT_RGB;
constexpr uint32_t kCliMuted = CP0_UI_LIST_MUTED_RGB;
constexpr uint32_t kCliWarn = CP0_UI_LIST_WARN_RGB;
constexpr uint32_t kCliButton = CP0_UI_LIST_BAR_RGB;
constexpr uint32_t kCliPressed = CP0_UI_OSK_KEY_PRESSED_RGB;
constexpr int kCliDrawMs = 100;            // the output is drawn at most 10 times a second
constexpr uint32_t kCliConsoleGoneMs = 3000;   // the console closed the relay: its exit status is awaited this long
constexpr const char *kPython = "/usr/bin/python3";   // what the mccli wrapper runs

enum CliWhat { kCliTapEntry = 1, kCliTapHelp = 2, kCliPress = 3, kCliDrag = 4 };

void hide(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return b;
}

/* An opaque sub-page of the full width (Help, All commands): it covers the console and takes the taps meant for it. */
lv_obj_t *panel(lv_obj_t *parent)
{
    lv_obj_t *p = box(parent);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    return p;
}

int lh(const lv_font_t *f)
{
    return static_cast<int>(lv_font_get_line_height(f));
}

int tw(const char *text, const lv_font_t *font)
{
    lv_point_t sz{};
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

int lines_in(const std::string &text, const lv_font_t *font, int w)
{
    if (text.empty()) return 1;
    lv_point_t sz{};
    lv_text_get_size(&sz, text.c_str(), font, 0, 0, std::max(1, w), LV_TEXT_FLAG_NONE);
    return std::max(1, (static_cast<int>(sz.y) + lh(font) - 1) / lh(font));
}

uint32_t sig_of(uint32_t h, const std::string &s)
{
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return (h ^ 0xFF) * 16777619u;
}

bool python_found()
{
    return ::access(kPython, X_OK) == 0;
}

/* The byte offset of the n-th character of a UTF-8 text (LVGL's textarea counts characters). */
size_t byte_of_char(const std::string &s, uint32_t n)
{
    size_t i = 0;
    for (uint32_t k = 0; k < n && i < s.size(); ++k) {
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    }
    return i;
}

uint32_t chars_in(const std::string &s, size_t bytes)
{
    uint32_t n = 0;
    for (size_t i = 0; i < std::min(bytes, s.size()); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++n;
    return n;
}

void cli_cb(lv_event_t *e)
{
    App *app = static_cast<App *>(lv_event_get_user_data(e));
    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    if (!app || !obj) return;
    const int what = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj)));
    switch (lv_event_get_code(e)) {
    case LV_EVENT_CLICKED:
        if (what == kCliTapEntry || what == kCliTapHelp) app->cli_event(what, 0);
        break;
    case LV_EVENT_PRESSED:
        if (what == kCliDrag) app->cli_event(kCliPress, 0);
        break;
    case LV_EVENT_PRESSING:
        if (what == kCliDrag) {
            lv_indev_t *indev = lv_indev_active();
            if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) break;
            lv_point_t v{};
            lv_indev_get_vect(indev, &v);
            app->cli_event(kCliDrag, v.y);
        }
        break;
    default: break;
    }
}

} // namespace

/* ------------------------------------------------------------------ construction and layout */

void App::build_cli()
{
    cli_ = std::make_unique<CliState>();
    CliState &c = *cli_;
    lv_obj_t *p = page_[static_cast<int>(Tab::Cli)];
    const Geo g = geo();
    // the launcher's monospace face at the CAPTION size, as Serial Console's terminal
    const int px = cp0_ui_metrics_get()->text_px[CP0_TEXT_CAPTION];
    c.mono = cp0_fonts().get("JetBrainsMono-Regular.ttf", static_cast<uint16_t>(px));
    if (!c.mono) c.mono = cp0_ui_font_px(px);
    c.char_w = std::max(1, static_cast<int>(lv_font_get_glyph_width(c.mono, 'M', 0)));
    c.line_h = std::max(1, lh(c.mono));

    // the output: a drag scrolls it (the rows of the text view, not LVGL's scrolling)
    c.out = box(p);
    lv_obj_add_flag(c.out, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(c.out, reinterpret_cast<void *>(static_cast<intptr_t>(kCliDrag)));
    lv_obj_add_event_cb(c.out, cli_cb, LV_EVENT_ALL, this);
    c.text = lv_label_create(c.out);
    lv_label_set_long_mode(c.text, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(c.text, c.mono, 0);
    lv_obj_set_style_text_color(c.text, lv_color_hex(kCliText), 0);
    lv_obj_set_style_text_line_space(c.text, 0, 0);
    lv_obj_remove_flag(c.text, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(c.text, "");
    c.waiting = lv_label_create(c.out);
    lv_label_set_long_mode(c.waiting, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(c.waiting, f_caption_, 0);
    lv_obj_set_style_text_color(c.waiting, lv_color_hex(kCliMuted), 0);
    lv_obj_remove_flag(c.waiting, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(c.waiting, "Starting meshcore-cli... (the first start after an install or an update takes longer)");

    // the entry line (the toolkit's TextField look, typed into by the app) and Help
    c.entry = make_text_field(p, 0, kCliMaxEntry, g.row);
    lv_obj_add_flag(c.entry, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(c.entry, reinterpret_cast<void *>(static_cast<intptr_t>(kCliTapEntry)));
    lv_obj_add_event_cb(c.entry, cli_cb, LV_EVENT_CLICKED, this);
    lv_textarea_set_placeholder_text(c.entry, "Type a command, Enter sends");
    lv_textarea_set_password_bullet(c.entry, "*");
    lv_textarea_set_password_show_time(c.entry, 0);
    c.help_btn = box(p);
    lv_obj_add_flag(c.help_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c.help_btn, lv_color_hex(kCliButton), 0);
    lv_obj_set_style_bg_opa(c.help_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c.help_btn, layout_.esc_radius, 0);
    lv_obj_set_style_bg_color(c.help_btn, lv_color_hex(kCliPressed), LV_STATE_PRESSED);
    lv_obj_set_user_data(c.help_btn, reinterpret_cast<void *>(static_cast<intptr_t>(kCliTapHelp)));
    lv_obj_add_event_cb(c.help_btn, cli_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *hl = lv_label_create(c.help_btn);
    lv_label_set_text(hl, "Help");
    lv_obj_set_style_text_font(hl, f_body_, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_remove_flag(hl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(hl);


    // Help: the curated list, in the look of the other lists (the markers of the row kinds)
    c.help_panel = panel(wide_);
    RowList::Config cfg;
    cfg.max_cells = 2;
    cfg.max_boxes = 0;
    cfg.flat = true;
    cfg.marks = true;
    cfg.mark_layout = settings_list_layout(g.s / 2, std::max(1, g.h - g.s / 2));
    cfg.mark_layout.text_px = cp0_ui_metrics_get()->text_px[CP0_TEXT_BODY];
    cfg.min_row_h = g.list_row;
    c.help_list.create(c.help_panel, g.lx, g.s / 2, g.lw, std::max(1, g.h - g.s / 2), cfg);
    hide(c.help_panel, true);

    // All commands: the text of mccli -h, scrolled by a drag or the keys
    c.usage_panel = panel(wide_);
    c.usage_box = lv_obj_create(c.usage_panel);
    lv_obj_remove_style_all(c.usage_box);
    lv_obj_set_scroll_dir(c.usage_box, LV_DIR_VER);
    lv_obj_remove_flag(c.usage_box, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(c.usage_box, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_scrollbar_mode(c.usage_box, LV_SCROLLBAR_MODE_AUTO);
    c.usage_label = lv_label_create(c.usage_box);
    lv_label_set_long_mode(c.usage_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(c.usage_label, c.mono, 0);
    lv_obj_set_style_text_color(c.usage_label, lv_color_hex(kCliText), 0);
    lv_obj_remove_flag(c.usage_label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(c.usage_label, "");
    hide(c.usage_panel, true);

    layout_cli();
}

/* The output down to the entry line; the entry and Help at the bottom (Help at the right, ROW high: 7 mm). With the on-screen
 * keyboard up the room is what is left above it: at 640x480 only the entry (the output comes back when the keyboard goes). */
void App::layout_cli()
{
    if (!cli_ || !cli_->out) return;
    CliState &c = *cli_;
    const Geo g = geo();
    const int h = panel_h_;
    const int help_w = std::max(g.row, tw("Help", f_body_) + 2 * g.m);
    const int ey = std::max(0, h - g.row - g.s / 2);
    lv_obj_set_pos(c.entry, g.x, ey);
    lv_obj_set_size(c.entry, std::max(g.row, g.cw - help_w - g.s), g.row);     // 0.7.2: To... is gone, the entry has its room
    lv_obj_set_pos(c.help_btn, g.x + g.cw - help_w, ey);
    lv_obj_set_size(c.help_btn, help_w, g.row);
    const int top = g.s / 2;
    const int oh = ey - g.s / 2 - top;
    c.out_visible = oh >= c.line_h;
    hide(c.out, !c.out_visible);
    lv_obj_set_pos(c.out, g.x, top);
    lv_obj_set_size(c.out, g.cw, std::max(1, oh));
    lv_obj_set_pos(c.text, 0, 0);
    lv_obj_set_size(c.text, g.cw, std::max(1, oh));
    lv_obj_set_pos(c.waiting, 0, 0);
    lv_obj_set_width(c.waiting, g.cw);
    const int cols = std::max(20, g.cw / c.char_w), rows = std::max(1, oh / c.line_h);
    if (cols != c.cols || rows != c.rows) {
        c.cols = cols;
        c.rows = rows;
        c.drawn_version = ~0ull;
        if (c.proc) c.proc->resize(cols, std::max(rows, 5));
    }
    lv_obj_set_size(c.help_panel, layout_.w, h);
    c.help_list.resize(std::max(1, h - g.s / 2));
    lv_obj_set_size(c.usage_panel, layout_.w, h);
    lv_obj_set_pos(c.usage_box, g.x, g.s / 2);
    lv_obj_set_size(c.usage_box, g.cw, std::max(1, h - g.s));
    lv_obj_set_width(c.usage_label, std::max(1, g.cw - 8));
    c.help_sig = 0;                                          // the Help rows are measured again for the room
    cli_draw(true);
}

/* ------------------------------------------------------------------ Tools > CLI (0.6.0: Home > CLI), the session */

std::string App::cli_path() const
{
    if (const char *e = std::getenv("MESHHOP_MESHCLI"); e && *e) return e;
    return kCliDefaultPath;
}

std::string App::cli_home_value(cp0::ui::ValueTone &tone) const
{
    tone = cp0::ui::ValueTone::Muted;
    if (!client_->ready()) return "not connected";
    if (cli_ && cli_->warm_done && cli_->warm_exit == 127 && !python_found()) {
        tone = cp0::ui::ValueTone::Warn;
        return "needs python3";
    }
    return "";
}

void App::open_cli()
{
    if (!cli_ || nav_.popup_open || editor_open_ || view_pending_) return;
    if (!client_->ready()) {
        open_info("CLI", "Connect to a node first (Home > Node).",
                  "The CLI runs meshcore-cli through Mesh Hop's connection to the node: your commands and its answers pass through Mesh Hop.");
        return;
    }
    CliState &c = *cli_;
    cli_to_pending_.clear();                                 // (Open in CLI sets both after this)
    cli_back_rep_.clear();
    open_page(Tab::Cli);
    c.screen.clear();
    c.history.reset();
    c.help = 0;
    c.typing = false;
    c.follow = true;
    c.end_text.clear();
    c.end_error = false;
    c.sent_any = false;
    c.relay_port = 0;
    c.relay_gone_ms = 0;
    c.phase = CliPhase::Waiting;
    c.phase_ms = mono_ms();
    cli_set_entry("");
    if (log_) log_->line("cli: the page opened, a session starts");
    invalidate();
}

void App::cli_spawn()
{
    CliState &c = *cli_;
    const std::string path = cli_path();
    if (::access(path.c_str(), X_OK) != 0) {
        cli_end("Mesh Hop's meshcore-cli is missing (" + path + "): reinstall Mesh Hop", true);
        return;
    }
    PtyProcess::Spec s;
    s.argv = {path, "-c", "off", "-t", "127.0.0.1", "-p", std::to_string(c.relay_port)};
    s.env = {"TERM=dumb", "MESHHOP_DATA=" + store_->dir()};
    s.unset = {"COLUMNS", "LINES"};
    s.cols = c.cols;
    s.rows = std::max(c.rows, 5);
    c.proc = std::make_unique<PtyProcess>();
    if (!c.proc->start(s)) {
        cli_end("meshcore-cli could not start: " + c.proc->error(), true);
        return;
    }
    c.phase = CliPhase::Starting;
    c.phase_ms = mono_ms();
    if (log_) log_->line("cli: meshcore-cli started (pid " + std::to_string(c.proc->pid()) + ") on the relay port " + std::to_string(c.relay_port));
    invalidate();
}

void App::cli_end(const std::string &text, bool error)
{
    CliState &c = *cli_;
    const uint64_t now = mono_ms();
    c.phase = CliPhase::Ended;
    c.phase_ms = now;
    c.end_text = text;
    c.end_error = error;
    c.screen.note("-- " + text);
    if (c.proc) {
        if (c.proc->running()) c.proc->stop(now);
        c.dying.push_back(std::move(c.proc));
    }
    client_->stop_relay();                                   // (nothing when the relay ended already)
    c.relay_port = 0;
    c.follow = true;
    if (error) notice(text, false);
    if (log_) log_->line("cli: the session ended: " + text);
    invalidate();
}

void App::cli_stop()
{
    if (!cli_) return;
    CliState &c = *cli_;
    if (c.proc) {
        if (c.proc->running()) c.proc->stop(mono_ms());
        c.dying.push_back(std::move(c.proc));
    }
    if (c.phase == CliPhase::Waiting || c.phase == CliPhase::Starting || c.phase == CliPhase::Running) {
        if (client_) client_->stop_relay();
        if (log_) log_->line("cli: left, meshcore-cli stopped, Mesh Hop resumes");
    }
    c.phase = CliPhase::Off;
    c.relay_port = 0;
    c.help = 0;
    c.typing = false;
    invalidate();
}

void App::cli_shutdown()
{
    if (!cli_) return;
    CliState &c = *cli_;
    if (c.proc) c.proc->kill_now();
    for (auto &p : c.dying) p->kill_now();
    c.dying.clear();
    if (client_ && (c.phase == CliPhase::Waiting || c.phase == CliPhase::Starting || c.phase == CliPhase::Running)) client_->stop_relay();
    c.phase = CliPhase::Off;
}

/* Once per run, when the CLI page first opens (0.7.1; 0.6.0 and 0.7.0 ran it when Mesh Hop first reached a node, at the same
 * moment as the connection: the deck reset there, report of 2026-10-24): `mccli -v` in the background at the lowest CPU and
 * I/O priority (nice 19, ionice idle, when the deck has them), and the session waits for it (cli_tick, Waiting: the caption
 * says Starting meshcore-cli...). Python compiles the bundle's .pyc files there, so the session's start is not the slow one,
 * and the version is shown in the caption. MESHHOP_NO_WARMUP=1 skips it (diagnostics: the session then compiles by itself).
 * No bundle (a PC build): nothing. */
void App::cli_warmup()
{
    CliState &c = *cli_;
    c.warm_started = true;
    const std::string path = cli_path();
    if (::access(path.c_str(), X_OK) != 0) {
        c.warm_done = true;
        return;
    }
    if (const char *e = std::getenv("MESHHOP_NO_WARMUP"); e && *e && *e != '0') {
        c.warm_done = true;
        if (log_) log_->line("cli: warm-up skipped (MESHHOP_NO_WARMUP)");
        return;
    }
    cdk::async_run::Spec s;
    if (::access("/usr/bin/nice", X_OK) == 0) s.argv = {"/usr/bin/nice", "-n", "19"};
    if (::access("/usr/bin/ionice", X_OK) == 0) s.argv.insert(s.argv.end(), {"/usr/bin/ionice", "-c", "3"});
    s.argv.insert(s.argv.end(), {path, "-v"});
    s.env = {"MESHHOP_DATA=" + store_->dir()};
    s.timeout_ms = 120000;
    s.on_line = [this](const std::string &line) {
        if (!cli_ || !cli_->version.empty() || line.find("meshcore-cli") == std::string::npos) return;
        const size_t v = line.rfind(" v");
        if (v != std::string::npos && v + 2 < line.size()) cli_->version = line.substr(v + 1);
    };
    s.on_done = [this](const cdk::async_run::Result &r) {
        if (!cli_) return;
        cli_->warm_done = true;
        cli_->warm_exit = r.exit_code;
        if (log_) log_->line("cli: warm-up mccli -v: exit " + std::to_string(r.exit_code) + " in " + std::to_string(r.elapsed_ms) + " ms" +
                             (cli_->version.empty() ? std::string() : ", meshcore-cli " + cli_->version));
        invalidate();
    };
    std::string cmd;
    for (const std::string &a : s.argv) cmd += (cmd.empty() ? "" : " ") + a;
    if (log_) log_->line("cli: warm-up " + cmd);
    cdk::async_run::run(std::move(s));
}

void App::cli_tick(uint64_t now)
{
    if (!cli_) return;
    CliState &c = *cli_;
    for (auto it = c.dying.begin(); it != c.dying.end();) {        // the processes of sessions that ended: reaped, their pty drained
        (*it)->poll(now);
        std::string drop;
        (*it)->read(drop);
        if ((*it)->exited()) it = c.dying.erase(it);
        else ++it;
    }
    if (c.phase == CliPhase::Off || c.phase == CliPhase::Ended) return;
    if (c.proc) {
        c.proc->poll(now);
        std::string chunk;
        if (c.proc->read(chunk) > 0) {
            const bool was_pw = c.screen.password_prompt();
            c.screen.feed(chunk);
            if (c.screen.password_prompt() != was_pw) invalidate();
        }
    }
    const bool relay_gone = c.relay_port != 0 && client_->relay_sessions_ended() != c.relay_seen;
    switch (c.phase) {
    case CliPhase::Waiting:
        if (!client_->ready()) {
            cli_end("Not connected to a node", true);
            return;
        }
        if (c.relay_port == 0) {
            if (!c.warm_started) cli_warmup();               // 0.7.1: the first session of the run waits for `mccli -v`
            if (!c.warm_done) break;                         // (Mesh Hop's own commands go on meanwhile: no relay yet)
            if (client_->relay_active()) break;              // the last session still settles (a few hundred ms)
            c.relay_seen = client_->relay_sessions_ended();
            c.relay_port = client_->start_relay();
            if (c.relay_port == 0) cli_end(client_->notice().text.empty() ? std::string("The relay could not start") : client_->notice().text, true);
            return;
        }
        if (relay_gone) {
            cli_end(cli_relay_end_text(client_->relay_end_reason()), true);
            return;
        }
        if (client_->relay_ready()) cli_spawn();
        return;
    case CliPhase::Starting:
    case CliPhase::Running:
        if (c.phase == CliPhase::Starting && c.screen.at_prompt()) {
            c.phase = CliPhase::Running;
            if (log_) log_->line("cli: meshcore-cli is ready after " + std::to_string(now - c.phase_ms) + " ms");
            c.phase_ms = now;
            if (!cli_to_pending_.empty()) {                  // 0.7.0: Open in CLI before the prompt: `to <key>`, once
                const std::string key = cli_to_pending_;
                cli_to_pending_.clear();
                cli_send_to(key);
            }
            invalidate();
        }
        if (c.proc && c.proc->exited()) {
            std::string rest;
            while (c.proc->read(rest) > 0) {}
            c.screen.feed(rest);
            const int code = c.proc->exit_code(), sig = c.proc->term_signal();
            cli_end(cli_exit_text(code, sig, python_found()), code != 0 || sig != 0);
            return;
        }
        if (relay_gone) {
            const std::string why = client_->relay_end_reason();
            if (why == "the console closed its connection") {   // meshcore-cli is ending: its exit status says how
                if (!c.relay_gone_ms) c.relay_gone_ms = now;
                if (now - c.relay_gone_ms < kCliConsoleGoneMs) break;
            }
            cli_end(cli_relay_end_text(why), true);
            return;
        }
        break;
    default: break;
    }
    if (!at_home_ && !at_nodes_ && nav_.tab == Tab::Cli && !cdk::hidden()) cli_draw(false);
}

/* ------------------------------------------------------------------ the output */

void App::cli_draw(bool force)
{
    if (!cli_ || !cli_->text) return;
    CliState &c = *cli_;
    const uint64_t now = mono_ms();
    const term::TermBuffer &buf = c.screen.buffer();
    if (!force && (buf.version() == c.drawn_version || now - c.last_draw_ms < static_cast<uint64_t>(kCliDrawMs))) return;
    c.last_draw_ms = now;
    c.drawn_version = buf.version();
    const std::vector<std::string> rows = c.follow ? term::view_bottom(buf, c.cols, c.rows, &c.top) : term::view_at(buf, c.cols, c.rows, &c.top);
    std::string text;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i) text += '\n';
        text += rows[i];
    }
    if (text != c.shown) {
        c.shown = text;
        lv_label_set_text(c.text, c.shown.c_str());
    }
    const bool empty = buf.end() - buf.first() <= 1 && c.screen.current_line().empty();
    hide(c.waiting, !(empty && (c.phase == CliPhase::Waiting || c.phase == CliPhase::Starting)));
}

void App::cli_scroll(int delta)
{
    if (!cli_ || delta == 0) return;
    CliState &c = *cli_;
    c.follow = term::scroll_view(c.screen.buffer(), c.cols, c.rows, &c.top, delta);
    cli_draw(true);
}

std::string App::cli_text() const
{
    return cli_ ? cli_->screen.text() : std::string();
}

/* ------------------------------------------------------------------ rendering, the caption */

void App::render_cli()
{
    if (!cli_) return;
    CliState &c = *cli_;
    hide(c.help_panel, c.help != 1);
    hide(c.usage_panel, c.help != 2);
    const bool pw = c.screen.password_prompt();
    if (pw != c.password) {
        c.password = pw;
        lv_textarea_set_password_mode(c.entry, pw);
        lv_textarea_set_placeholder_text(c.entry, pw ? "Password, Enter sends (hidden)" : "Type a command, Enter sends");
    }
    const bool live = c.phase == CliPhase::Waiting || c.phase == CliPhase::Starting || c.phase == CliPhase::Running;
    style_field(c.entry, live && c.help == 0 && (kbd_ok_ || c.typing));
    if (c.help == 1) cli_render_help();
    if (c.help == 2) cli_render_usage();
    cli_draw(true);
}

bool App::cli_caption(std::string &text) const
{
    if (!cli_) return false;
    const CliState &c = *cli_;
    if (c.help == 1) {
        text = "Pick a command: it goes into the entry line";
        return true;
    }
    if (c.help == 2) {
        text = c.usage_state == 1 ? "Reading meshcore-cli -h..." : "meshcore-cli" + (c.version.empty() ? std::string() : " " + c.version) + " -h";
        return true;
    }
    switch (c.phase) {
    case CliPhase::Waiting:
    case CliPhase::Starting: text = "Starting meshcore-cli..."; return true;
    case CliPhase::Running: {
        std::string line = c.screen.current_line();
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (!c.out_visible && !line.empty()) text = line;     // the keyboard took the output's room: the line meshcore-cli is on
        else if (!c.sent_any && cli_back_rep_.empty()) text = cli_pick_hint();   // 0.7.2: opened from Tools, nothing sent yet
        else text = "meshcore-cli" + (c.version.empty() ? std::string() : " " + c.version) + " running  \xC2\xB7  Esc ends it";
        return true;
    }
    case CliPhase::Ended: text = c.end_text; return true;
    case CliPhase::Off: return false;
    }
    return false;
}

/* ------------------------------------------------------------------ keys and taps */

bool App::cli_keys_to_entry() const
{
    return cli_ && cli_->help == 0;
}

void App::cli_set_entry(const std::string &text)
{
    if (!cli_ || !cli_->entry) return;
    lv_textarea_set_text(cli_->entry, text.c_str());
    lv_textarea_set_cursor_pos(cli_->entry, LV_TEXTAREA_CURSOR_LAST);
}

void App::cli_entry_key(const KeyEvent &e)
{
    lv_obj_t *ta = cli_->entry;
    switch (e.key) {
    case Key::Backspace: lv_textarea_delete_char(ta); break;
    case Key::Delete: lv_textarea_delete_char_forward(ta); break;
    case Key::Left: lv_textarea_cursor_left(ta); break;
    case Key::Right: lv_textarea_cursor_right(ta); break;
    case Key::Home: lv_textarea_set_cursor_pos(ta, 0); break;
    case Key::End: lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); break;
    case Key::Char: {
        if (e.ctrl || e.alt) break;
        const char *cur = lv_textarea_get_text(ta);
        const std::string add = filter_typed(EditMode::Free, cur ? cur : "", e.text, kCliMaxEntry);
        if (!add.empty()) lv_textarea_add_text(ta, add.c_str());
        break;
    }
    default: break;
    }
}

void App::cli_send_line()
{
    CliState &c = *cli_;
    const bool live = (c.phase == CliPhase::Starting || c.phase == CliPhase::Running) && c.proc && c.proc->running();
    if (!live) {
        notice(c.phase == CliPhase::Ended ? "meshcore-cli is not running: Esc, then CLI again" : "meshcore-cli is starting: wait for its prompt", false);
        return;
    }
    const char *t = lv_textarea_get_text(c.entry);
    const std::string line = t ? t : "";
    const bool pw = c.screen.password_prompt();
    c.proc->write(cli_line_bytes(line));
    c.screen.sent_line(pw);
    c.sent_any = true;
    if (pw) c.history.reset();                               // a password never goes into the history
    else c.history.add(line);
    cli_set_entry("");
    c.follow = true;
    // a physical keyboard keeps typing; the on-screen keyboard goes after a line (as after a Send in Chats): the answer shows
    if (osk_inset_ > 0) c.typing = false;
    invalidate();
}

void App::key_cli(const KeyEvent &e)
{
    if (!cli_) return;
    CliState &c = *cli_;
    if (c.help == 1) {
        std::vector<bool> ok;
        for (const HelpItem &h : c.help_rows) ok.push_back(h.kind == HelpKind::Insert || h.kind == HelpKind::Usage);
        switch (e.key) {
        case Key::Up:
        case Key::Down:
        case Key::PageUp:
        case Key::PageDown:
        case Key::Home:
        case Key::End: {
            const int d = e.key == Key::Up ? -1 : e.key == Key::Down ? 1 : e.key == Key::PageUp ? -6 : e.key == Key::PageDown ? 6
                          : e.key == Key::Home ? -1000 : 1000;
            // nothing selected yet (the list opens without a highlight: a first tap selects): Up / Down take the first command,
            // End and PgDn go on from it
            const int from = c.help_sel < 0 && std::abs(d) > 1 ? step_selectable(ok, -1, 1) : c.help_sel;
            const int next = c.help_sel < 0 && std::abs(d) == 1 ? step_selectable(ok, -1, 1) : step_selectable(ok, from, d);
            if (next >= 0) {
                c.help_sel = next;
                c.help_list.set_selected(next, true, true);
            }
            break;
        }
        case Key::Enter:
        case Key::Right:
            if (!e.repeat) cli_help_activate(c.help_sel);
            break;
        default: break;
        }
        return;
    }
    if (c.help == 2) {
        const int page = std::max(c.line_h, lv_obj_get_height(c.usage_box) - c.line_h);
        switch (e.key) {
        case Key::Up: lv_obj_scroll_by_bounded(c.usage_box, 0, 3 * c.line_h, LV_ANIM_OFF); break;
        case Key::Down: lv_obj_scroll_by_bounded(c.usage_box, 0, -3 * c.line_h, LV_ANIM_OFF); break;
        case Key::PageUp: lv_obj_scroll_by_bounded(c.usage_box, 0, page, LV_ANIM_OFF); break;
        case Key::PageDown: lv_obj_scroll_by_bounded(c.usage_box, 0, -page, LV_ANIM_OFF); break;
        case Key::Home: lv_obj_scroll_to_y(c.usage_box, 0, LV_ANIM_OFF); break;
        default: break;
        }
        return;
    }
    if (e.key == Key::Char && e.ctrl && !e.alt && e.text.size() == 1) {
        const char k = static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0])));
        if (e.repeat) return;
        if (k == 'c') {                                      // the interrupt: meshcore-cli decides (at its prompt it ends)
            if (c.proc && c.proc->running()) c.proc->write(kCliInterrupt);
            cli_set_entry("");
            c.history.reset();
        } else if (k == 'h') {
            cli_open_help();
        }
        return;
    }
    switch (e.key) {
    case Key::Enter:
        if (!e.repeat) cli_send_line();
        return;
    case Key::Up:
    case Key::Down: {
        if (c.screen.password_prompt()) return;              // no history in a password
        std::string line;
        const char *cur = lv_textarea_get_text(c.entry);
        if (e.key == Key::Up ? c.history.up(cur ? cur : "", line) : c.history.down(line)) cli_set_entry(line);
        return;
    }
    case Key::PageUp: cli_scroll(-std::max(1, c.rows - 1)); return;
    case Key::PageDown: cli_scroll(std::max(1, c.rows - 1)); return;
    case Key::Tab:
    case Key::BackTab: return;                               // no completion in the line console (T1), and no page change
    default: cli_entry_key(e); return;
    }
}

void App::cli_back()
{
    CliState &c = *cli_;
    if (c.help == 2) {
        c.help = 1;
        invalidate();
        return;
    }
    if (c.help == 1) {
        c.help = 0;
        invalidate();
        return;
    }
    if (c.typing && osk_inset_ > 0) {                        // the on-screen keyboard goes first, the session stays
        c.typing = false;
        invalidate();
        return;
    }
    cli_leave();                                             // the session ends; back to Tools or the Repeater page (0.7.0)
}

/* The CLI's Esc at its root (0.7.0): the session ends (select_tab: cli_stop) and the page it was opened from comes back: the
 * Repeater page of More > Open in CLI, else Tools (the CLI is one of its rows). */
void App::cli_leave()
{
    const std::string rep = cli_back_rep_;
    cli_back_rep_.clear();
    cli_to_pending_.clear();
    at_home_ = false;
    if (!rep.empty() && model_.find_contact(rep)) {
        select_tab(Tab::Contacts);
        open_repeater(rep, false);
    } else {
        select_tab(Tab::Tools);
    }
    invalidate();
}

/* Repeater page > More > Open in CLI: the CLI page and a new session; `to <its full key>` goes at the first prompt. */
void App::open_cli_for(const std::string &key_hex)
{
    if (!cli_) return;
    if (!client_->ready()) {
        notice("Connect to a node first (Home > Node)", false);
        return;
    }
    open_cli();
    if (nav_.tab != Tab::Cli) return;                        // (refused: a box or an editor is open)
    cli_back_rep_ = key_hex;
    cli_to_pending_ = key_hex;
}

/* `to <64 hex key>` + Enter into meshcore-cli now (at its prompt), or at its first prompt when it is still starting. */
void App::cli_send_to(const std::string &key_hex)
{
    if (!cli_) return;
    CliState &c = *cli_;
    const std::string line = cli_to_line(key_hex);
    if (line.empty()) return;
    const bool live = c.phase == CliPhase::Running && c.proc && c.proc->running();
    if (!live) {
        if (c.phase == CliPhase::Waiting || c.phase == CliPhase::Starting) cli_to_pending_ = key_hex;
        else notice("meshcore-cli is not running: Esc, then CLI again", false);
        return;
    }
    c.proc->write(cli_line_bytes(line));
    // 0.7.2: the echo of the 64 hex digits (two lines at 640x480) is replaced by "to <name> <909192" in the output only
    const ContactRec *r = model_.find_contact(key_hex);
    c.screen.sent_line_shown(cli_to_shown(r ? r->c.name : std::string(), key_hex));
    c.history.add(line);
    c.follow = true;
    if (log_) log_->line("cli: to " + key_hex.substr(0, 8) + "... typed for meshcore-cli");
    invalidate();
}

void App::cli_event(int what, int arg)
{
    if (!cli_ || at_home_ || nav_.tab != Tab::Cli || nav_.popup_open || editor_open_) return;
    CliState &c = *cli_;
    switch (what) {
    case kCliTapEntry:
        if (c.help == 0 && !c.typing) {
            c.typing = true;
            invalidate();
        }
        break;
    case kCliTapHelp:
        if (c.help == 0) post([this] { cli_open_help(); });
        break;
    case kCliPress: c.drag = 0; break;
    case kCliDrag: {
        c.drag += arg;
        int steps = 0;
        while (c.drag >= c.line_h) {
            c.drag -= c.line_h;
            --steps;                                         // the finger goes down: older text
        }
        while (c.drag <= -c.line_h) {
            c.drag += c.line_h;
            ++steps;
        }
        cli_scroll(steps);
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------ Help and All commands */

void App::cli_open_help()
{
    CliState &c = *cli_;
    c.help = 1;
    c.typing = false;
    c.help_sig = 0;
    c.help_sel = -1;
    invalidate();
}

/* The rows: a section in grey capitals; a command, its text under it (wrapped), the action dot (a tap on the selected row, or Enter,
 * puts it into the entry); a warning in amber with its text, no marker (nothing to do there); All commands with the chevron. */
void App::cli_render_help()
{
    CliState &c = *cli_;
    const Geo g = geo();
    const cp0_ui_list_layout_t l = settings_list_layout(0, g.h);
    const lv_font_t *sf = cp0_ui_font_px(l.section_px);
    const int lhb = lh(f_body_), lhc = lh(f_caption_), lhs = lh(sf);
    const int tw_all = g.lw - 2 * g.inset, pad = std::max(4, g.s / 2);
    const int trail = cp0_ui_list_trail_w(&l, CP0_UI_LIST_AFFORDANCE_ACTION);
    uint32_t sig = sig_of(0, std::to_string(tw_all));
    for (const HelpItem &h : cli_help_items()) sig = sig_of(sig, h.label);
    if (sig == c.help_sig && c.help_list.count() == static_cast<int>(c.help_rows.size())) {
        c.help_list.set_selected(c.help_sel, true, false);
        return;
    }
    c.help_sig = sig;
    c.help_rows = cli_help_items();
    std::vector<RowSpec> specs;
    for (size_t i = 0; i < c.help_rows.size(); ++i) {
        const HelpItem &h = c.help_rows[i];
        RowSpec s;
        s.id = std::to_string(i);
        if (h.kind == HelpKind::Section) {
            s.h = l.section_h;
            s.selectable = false;
            std::string up = h.label;
            for (char &ch : up)
                if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
            s.cells.push_back({up, CP0_UI_LIST_SECTION_RGB, g.inset, tw_all, LV_TEXT_ALIGN_LEFT, sf, (s.h - lhs) / 2});
        } else {
            const bool marked = h.kind == HelpKind::Insert || h.kind == HelpKind::Usage;
            const int w = std::max(1, tw_all - (marked ? trail : 0));
            const int n = lines_in(h.text, f_caption_, w);
            s.h = std::max(l.row_h, pad + lhb + n * lhc + pad);
            s.selectable = marked;
            s.aff = h.kind == HelpKind::Insert ? Aff::Action : h.kind == HelpKind::Usage ? Aff::Open : Aff::Info;
            const uint32_t label_rgb = h.kind == HelpKind::Warning ? kCliWarn : kCliText;
            s.cells.push_back({h.label, label_rgb, g.inset, w, LV_TEXT_ALIGN_LEFT, f_body_, pad});
            Cell d{h.text, kCliMuted, g.inset, w, LV_TEXT_ALIGN_LEFT, f_caption_, pad + lhb};
            d.lines = n;
            s.cells.push_back(d);
        }
        specs.push_back(std::move(s));
    }
    c.help_list.set(std::move(specs), c.help_sel, true);    // (opened with no highlight: the first tap on a row selects it)
    c.help_list.scroll_to_top();
}

void App::cli_help_activate(int index)
{
    CliState &c = *cli_;
    if (index < 0 || index >= static_cast<int>(c.help_rows.size())) return;
    const HelpItem &h = c.help_rows[static_cast<size_t>(index)];
    if (h.kind == HelpKind::Usage) {
        cli_open_usage();
        return;
    }
    if (h.kind != HelpKind::Insert || h.insert.empty()) return;
    const char *t = lv_textarea_get_text(c.entry);
    const std::string cur = t ? t : "";
    const size_t at = byte_of_char(cur, lv_textarea_get_cursor_pos(c.entry));
    size_t after = 0;
    const std::string next = cli_insert(cur, at, h.insert, &after);
    if (next.size() <= kCliMaxEntry) {
        lv_textarea_set_text(c.entry, next.c_str());
        lv_textarea_set_cursor_pos(c.entry, static_cast<int32_t>(chars_in(next, after)));
    }
    c.help = 0;                                              // back to the console, the command in the entry
    invalidate();
}

/* All commands: `mccli -h` of the installed bundle, read once in the background (a few seconds on a Pi Zero 2 W), then kept. */
void App::cli_open_usage()
{
    CliState &c = *cli_;
    c.help = 2;
    lv_obj_scroll_to_y(c.usage_box, 0, LV_ANIM_OFF);
    if (c.usage_state == 0 || c.usage_state == 3) {
        const std::string path = cli_path();
        if (::access(path.c_str(), X_OK) != 0) {
            c.usage_state = 3;
            c.usage_text = "Mesh Hop's meshcore-cli is missing (" + path + "): reinstall Mesh Hop.";
        } else {
            c.usage_state = 1;
            c.usage_text.clear();
            cdk::async_run::Spec s;
            s.argv = {path, "-h"};
            s.env = {"MESHHOP_DATA=" + store_->dir()};
            s.timeout_ms = 60000;
            s.on_line = [this](const std::string &line) {
                if (cli_) cli_->usage_text += line + "\n";
            };
            s.on_done = [this](const cdk::async_run::Result &r) {
                if (!cli_) return;
                if (r.ok() && !cli_->usage_text.empty()) {
                    cli_->usage_state = 2;
                } else {
                    cli_->usage_state = 3;
                    cli_->usage_text += "\n" + cli_exit_text(r.exit_code, r.term_signal, python_found()) + "\n";
                }
                invalidate();
            };
            cdk::async_run::run(std::move(s));
        }
    }
    invalidate();
}

void App::cli_render_usage()
{
    CliState &c = *cli_;
    const std::string text = c.usage_state == 1 ? std::string("Reading meshcore-cli -h...") : c.usage_text;
    const char *now = lv_label_get_text(c.usage_label);
    if (!now || text != now) lv_label_set_text(c.usage_label, text.c_str());
}

} // namespace meshhop
