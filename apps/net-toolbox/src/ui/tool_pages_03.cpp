/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the new tool pages (see toolbox_app.hpp): Speed test, Wake-on-LAN (the list and a machine's page) and the
 * Bluetooth radar. Like the 0.2 pages each builds from the layout service's tokens, takes the snapshots of its runner on the
 * 100 ms timer and hands them to view_model_03.hpp for the words; the blocks are widgets_03.hpp's. The speed test, the wake
 * watch and the scan run on the core sessions' own worker threads: nothing here waits on them, nothing starts until the user
 * presses Start, Wake now or Start on the radar, and leaving the page (or the app) stops them (stop_tools).
 */
#include "toolbox_app.hpp"

#include "app.hpp"
#include "style.hpp"
#include "toolbox.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include "netaddr.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace nt {

namespace {

ui::RichRow action_row(const std::string &label, const std::string &value, std::intptr_t id)
{
    return ui::RichRow::item(label, value, ui::Tone::Normal, static_cast<int>(id));
}

constexpr int kBleExpiryMs = 20000; /* the core's default: a device not heard for this long leaves the list */

} // namespace

/* ---------------------------------------------------------------- shared */

std::int64_t ToolboxApp::clock_ms() const
{
    return backend_.clock_ms ? backend_.clock_ms() : now_ms();
}

void ToolboxApp::build_rich_at(int x, int y, int w, int h)
{
    ui::RichListEvents events;
    events.activate = [this](int index, std::intptr_t id) { post([this, index, id] { activate(index, id); }); };
    events.back = [this] { post([this] { back(); }); };
    rich_ = std::make_unique<ui::RichList>(page_->root(), x, y, std::max(1, w), std::max(1, h), std::move(events));
    if (backend_.demo)
        std::fprintf(stderr, "[net-toolbox] list at %d,%d, %d x %d, rows %d px, screen %dx%d\n", x, y, w, h, rich_->layout().row_h,
                     layout_.w, layout_.h);
}

/* A small wrapped note under the cells (CAPTION text, or smaller when it needs more than max_lines lines). */
lv_obj_t *ToolboxApp::build_info(int y, const std::string &text, int max_lines, int *height)
{
    const Geo g = geo();
    const int w = std::max(1, g.w - 2 * g.inset);
    int px = ui::caption_px();
    const lv_font_t *font = ui::font_medium(px);
    lv_point_t size{};
    for (;; px -= 2) {
        font = ui::font_medium(px);
        lv_text_get_size(&size, text.c_str(), font, 0, 0, w, LV_TEXT_FLAG_NONE);
        if (size.y <= max_lines * ui::line_height(font) || px <= 12) break;
    }
    const int lh = ui::line_height(font);
    const bool cut = size.y > max_lines * lh;
    if (cut && backend_.demo) {
        std::fprintf(stderr, "[net-toolbox] the note does not fit %d lines: %s\n", max_lines, text.c_str());
        layout_cut_ = true;
    }
    const int lines = std::max(1, std::min(max_lines, (static_cast<int>(size.y) + lh - 1) / lh));
    lv_obj_t *l = ui::make_label(page_->root(), font, ui::kLabel);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, g.x + g.inset, y);
    lv_obj_set_size(l, w, lines * lh);
    ui::set_text_if(l, text);
    if (height) *height = lines * lh;
    return l;
}

void ToolboxApp::log_rows(const char *page) const
{
    if (!backend_.demo || !rich_) return;
    lv_obj_update_layout(rich_->focus_object());
    const int list_y = static_cast<int>(lv_obj_get_y(rich_->focus_object()));
    std::string out;
    for (int i = 0; i < rich_->count(); ++i) {
        const int cy = list_y + rich_->row_top(i) + rich_->row_height(i) / 2 - rich_->scroll_y();
        char buf[48];
        std::snprintf(buf, sizeof(buf), " %d:%.3f", i, static_cast<double>(cy) / std::max(1, layout_.h));
        out += buf;
    }
    std::fprintf(stderr, "[net-toolbox] rows %s %dx%d:%s\n", page, layout_.w, layout_.h, out.c_str());
    if (const char *path = std::getenv("NT_ROWS_LOG")) { /* the harness keeps no stderr: a file for the scene author */
        if (FILE *f = std::fopen(path, "a")) {
            std::fprintf(f, "rows %s %dx%d:%s\n", page, layout_.w, layout_.h, out.c_str());
            std::fclose(f);
        }
    }
}

std::string ToolboxApp::fit_label(const std::string &text, const lv_font_t *font, int max_w) const
{
    if (ui::text_width(text.c_str(), font) <= max_w) return text;
    std::string s = text;
    while (!s.empty() && ui::text_width((s + "...").c_str(), font) > max_w) {
        std::size_t n = s.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
        s.erase(n);
    }
    return s + "...";
}

/* ---------------------------------------------------------------- Speed test */

void ToolboxApp::build_speed()
{
    const Geo g = geo();
    read_net();
    band_check_ = true;
    int y = build_band();
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    ui::ActionBarEvents bar_events;
    bar_events.activate = [this](int id) { post([this, id] { activate_speed(id); }); };
    bar_events.back = [this] { post([this] { back(); }); };
    bar_events.down = [this] {
        post([this] {
            if (rich_ && rich_->selected() >= 0) focus(rich_->focus_object());
        });
    };
    const std::vector<ui::ActionCell> cells = {{kRun, "Start", ""}, {kSaveLog, "Save log", ""}};
    bar_ = std::make_unique<ui::ActionBar>(page_->root(), g.x, y, g.w, g.row, ui::font_medium(m->text_px[CP0_TEXT_BODY]),
                                           ui::font_medium(std::max(10, ui::caption_px() - 4)), cells, std::move(bar_events));
    y += g.row + g.s;
    const SpeedConfig cfg;
    int info_h = 0;
    info_ = build_info(y, ui::speed_note(cfg.max_bytes), 2, &info_h);
    y += info_h + g.s;
    build_rich(y, g.bottom - y);
    rich_->set_up_out([this] {
        post([this] {
            if (bar_) focus(bar_->cell_object(0));
        });
    });
    rich_->set_tab([this] {
        post([this] {
            if (bar_) focus(bar_->cell_object(0));
        });
    });
    speed_target_s_ = cfg.target_ms / 1000.0;
    const int inset = rich_->layout().inset, w = rich_->layout().w;
    const int big_px = std::clamp(2 * ui::caption_px(), 24, 56);
    speed_live_ = std::make_shared<ui::SpeedLiveBlock>(w, rich_->view_height(), inset, ui::caption_font(),
                                                       ui::font_medium(std::max(10, ui::caption_px() - 3)), ui::font_bold(big_px));
    speed_big_ = std::make_shared<ui::SpeedResultsBlock>(w, inset, ui::caption_font(), ui::font_bold(std::max(24, m->text_px[CP0_TEXT_TITLE])));
    speed_grade_ = std::make_shared<ui::GradeBlock>(w, inset, ui::caption_font(), ui::font_bold(m->text_px[CP0_TEXT_TITLE]));
    speed_ended_ = false;
    std::vector<lv_obj_t *> order = bar_->focus_objects();
    order.push_back(rich_->focus_object());
    focus_group(order);
    poll_speed(true);
}

std::vector<ui::RichRow> ToolboxApp::speed_list()
{
    const std::shared_ptr<const SpeedSnapshot> s = fresh_ ? backend_.speed->latest() : nullptr;
    std::vector<ui::RichRow> rows;
    const bool ended = s && (s->state == SpeedSnapshot::State::Done || s->state == SpeedSnapshot::State::Cancelled ||
                             s->state == SpeedSnapshot::State::Failed);
    /* the gauge gives way to the card when a run has ended (a half-hidden dial above it would only be in the way) */
    if (!ended) rows.push_back(ui::RichRow::custom(speed_live_, speed_live_->height(), false));
    if (!ended) return rows;
    const SpeedResult &r = s->result;
    if (s->state == SpeedSnapshot::State::Failed) rows.push_back(ui::RichRow::text(s->error, ui::Tone::Bad, true));
    const bool any = r.down.measured || r.up.measured;
    if (s->state == SpeedSnapshot::State::Cancelled)
        rows.push_back(ui::RichRow::text(any ? "Stopped before the end: these are the results so far."
                                             : "Stopped before a speed was measured.",
                                         ui::Tone::Warn, true));
    if (any) {
        speed_big_->set(r.down.measured ? ui::speed_value_text(r.down.mbps) : std::string("--"),
                        r.up.measured ? ui::speed_value_text(r.up.mbps) : std::string("--"));
        rows.push_back(ui::RichRow::custom(speed_big_, speed_big_->height(), true, kSpeedBig));
    }
    /* the grade is the verdict: it sits right under the two big numbers, not below the detail rows */
    if (s->state == SpeedSnapshot::State::Done) {
        speed_grade_->set(ui::speed_grade(r));
        rows.push_back(ui::RichRow::custom(speed_grade_, speed_grade_->height(), true, kSpeedGrade));
    }
    for (const ui::RowDef &d : ui::speed_rows(*s)) rows.push_back(rich_row(d));
    for (const std::string &n : ui::speed_notes(*s)) rows.push_back(ui::RichRow::text(n, ui::Tone::Normal, true, true));
    return rows;
}

void ToolboxApp::poll_speed(bool force)
{
    const std::shared_ptr<const SpeedSnapshot> snap = fresh_ ? backend_.speed->latest() : nullptr;
    const std::uint64_t seq = backend_.speed->seq();
    const SpeedSnapshot idle;
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        const bool running = snap && snap->running();
        speed_live_->update(snap ? *snap : idle, speed_target_s_);
        bar_->set_cell(0, running ? "Stop" : "Start", "");
        const std::vector<ui::RichRow> rows = speed_list();
        const bool ended = snap && !running && snap->state != SpeedSnapshot::State::Idle;
        if (rich_->count() == 0 || ended != speed_ended_) rich_->reset_rows(rows, 0); /* the card, or the gauge, from the top */
        else rich_->set_rows(rows);
        speed_ended_ = ended;
        if (backend_.demo) {
            const std::string problem = speed_live_->layout_problem();
            if (!problem.empty()) {
                std::fprintf(stderr, "[net-toolbox] the gauge's texts do not fit: %s\n", problem.c_str());
                layout_cut_ = true;
            }
            if (!speed_grade_->fits()) {
                std::fprintf(stderr, "[net-toolbox] the grade's line does not fit its block\n");
                layout_cut_ = true;
            }
        }
    }
    show_band(ui::speed_band(snap ? *snap : idle, speed_target_s_));
}

void ToolboxApp::start_speed()
{
    SpeedConfig cfg;
    const std::string target = settings_.speed_ping;
    if (target == "off") cfg.ping_target.clear();
    else if (target == "gateway") cfg.ping_target = gateway();
    else cfg.ping_target = target.empty() ? std::string("1.1.1.1") : target;
    cfg.resolve.server = pick_resolver("", dns_servers());
    speed_target_s_ = cfg.target_ms / 1000.0;
    backend_.speed->start(cfg);
    fresh_ = true;
    shown_seq_ = 0;
    speed_ended_ = false;
}

void ToolboxApp::activate_speed(std::intptr_t id)
{
    if (id == kRun) {
        const std::shared_ptr<const SpeedSnapshot> s = fresh_ ? backend_.speed->latest() : nullptr;
        if (s && s->running()) backend_.speed->stop();
        else start_speed();
    } else if (id == kSaveLog) {
        save_result();
    }
    poll_speed(true);
}

/* ---------------------------------------------------------------- Wake-on-LAN: the list */

void ToolboxApp::build_wol(int select)
{
    read_net();
    band_check_ = true;
    const Geo g = geo();
    const int y = build_band();
    build_rich(y, g.bottom - y);
    rich_->reset_rows(wol_list(), 0);
    if (select > 0) rich_->select(rich_->index_of_id(select));
    focus(rich_->focus_object());
    log_rows("wol");
    ui::Band band;
    const std::size_t n = settings_.wol_machines.size();
    band.text = n == 0 ? "No machine saved yet" : std::to_string(n) + (n == 1 ? " machine saved" : " machines saved");
    show_band(band);
    next_state_ms_ = now_ms() + 2000;
}

std::vector<ui::RichRow> ToolboxApp::wol_list()
{
    const std::vector<WolMachine> &machines = settings_.wol_machines;
    const bool full = machines.size() >= kMaxWolMachines;
    std::vector<ui::RichRow> rows;
    rows.push_back(action_row("Add a machine", full ? "list full" : "", kWolAdd));
    const int inset = rich_->layout().inset, w = rich_->layout().w;
    if (machine_views_.size() > machines.size()) machine_views_.resize(machines.size());
    while (machine_views_.size() < machines.size())
        machine_views_.push_back(std::make_shared<ui::MachineRowView>(
            w, inset, ui::font_medium(rich_->layout().text_px), ui::caption_font(), rich_->layout().row_h));
    for (std::size_t i = 0; i < machines.size(); ++i) {
        const WolMachine &m = machines[i];
        const auto it = wol_last_.find(lower(m.name));
        const ui::WakeLast last = it == wol_last_.end() ? ui::WakeLast() : it->second;
        machine_views_[i]->set(m.name, m.host.empty() ? m.mac : m.mac + "  " + m.host, ui::wake_last_text(last), ui::wake_last_tone(last));
        rows.push_back(ui::RichRow::custom(machine_views_[i], machine_views_[i]->height(), true, kMachine + static_cast<int>(i)));
    }
    if (machines.empty())
        rows.push_back(ui::RichRow::text("No machine saved yet. Add one to wake it from here.", ui::Tone::Normal, false));
    if (full) rows.push_back(ui::RichRow::text(ui::kWolFullText, ui::Tone::Warn, false, true));
    rows.push_back(ui::RichRow::text(ui::kWolNote, ui::Tone::Normal, false, true));
    return rows;
}

void ToolboxApp::refresh_wol()
{
    if (rich_) rich_->set_rows(wol_list());
}

void ToolboxApp::activate_wol(std::intptr_t id)
{
    if (id == kWolAdd) {
        if (settings_.wol_machines.size() >= kMaxWolMachines) {
            note({ui::kWolFullText, ui::Tone::Warn});
            show_band(ui::Band());
            return;
        }
        open_wol_edit("", 0);
        return;
    }
    const int index = static_cast<int>(id) - kMachine;
    if (index >= 0 && index < static_cast<int>(settings_.wol_machines.size())) {
        wol_current_ = settings_.wol_machines[static_cast<std::size_t>(index)].name;
        open(View::Machine);
    }
}

/* ---------------------------------------------------------------- Wake-on-LAN: adding and editing */

void ToolboxApp::open_wol_edit(const std::string &old_name, int mode)
{
    wol_draft_ = WolDraft();
    wol_draft_.old_name = old_name;
    wol_draft_.mode = mode;
    if (!old_name.empty()) {
        if (const WolMachine *m = settings_.find_machine(old_name)) wol_draft_.machine = *m;
    }
    open_wol_field(mode == 0 ? 1 : mode);
}

/* One typed value at a time: 1 the name, 2 the MAC address, 3 the address to watch (optional). Adding goes on to the next
 * one; editing one field saves it. The core's parser says what is wrong with a value, under the field. */
void ToolboxApp::open_wol_field(int step)
{
    Typed t;
    const bool chain = wol_draft_.mode == 0;
    if (step == 1) {
        t.title = "Name";
        t.label = "Name";
        t.text = wol_draft_.machine.name;
        t.placeholder = "Desk PC";
        t.max_bytes = 96;
        t.check = [this](const std::string &text) { return ui::wol_name_problem(text, settings_, wol_draft_.old_name); };
        t.done = [this, chain](const std::string &text) {
            wol_draft_.machine.name = text;
            if (chain) open_wol_field(2);
            else commit_wol_draft();
        };
    } else if (step == 2) {
        t.title = "MAC address";
        t.label = "MAC";
        t.text = wol_draft_.machine.mac;
        t.placeholder = "aa:bb:cc:dd:ee:ff";
        t.max_bytes = 40;
        t.check = [](const std::string &text) { return ui::wol_mac_problem(text); };
        t.done = [this, chain](const std::string &text) {
            MacAddr mac;
            if (parse_mac(text, mac)) wol_draft_.machine.mac = mac.text();
            if (chain) open_wol_field(3);
            else commit_wol_draft();
        };
    } else {
        t.title = "Watch address";
        t.label = "Address";
        t.text = wol_draft_.machine.host;
        t.placeholder = "192.168.68.20 (optional)";
        t.max_bytes = 253;
        t.check = [](const std::string &text) { return ui::wol_host_problem(text); };
        t.done = [this](const std::string &text) {
            wol_draft_.machine.host = text;
            commit_wol_draft();
        };
    }
    open_typed(std::move(t));
}

void ToolboxApp::commit_wol_draft()
{
    WolMachine m = wol_draft_.machine;
    std::string why;
    if (!normalize_machine(m, &why)) {
        note({why.empty() ? std::string("This machine cannot be saved") : why, ui::Tone::Bad});
        return;
    }
    const std::string old = wol_draft_.old_name;
    WolMachine previous;
    bool had_previous = false;
    if (!old.empty()) {
        if (const WolMachine *p = settings_.find_machine(old)) {
            previous = *p;
            had_previous = true;
        }
        settings_.remove_machine(old);
    }
    if (!settings_.save_machine(m, &why)) {
        if (had_previous) settings_.save_machine(previous);
        note({why.empty() ? std::string("This machine cannot be saved") : why, ui::Tone::Bad});
        return;
    }
    if (!old.empty() && lower(old) != lower(m.name)) {
        const auto it = wol_last_.find(lower(old));
        if (it != wol_last_.end()) {
            wol_last_[lower(m.name)] = it->second;
            wol_last_.erase(it);
        }
    }
    wol_current_ = m.name;
    fresh_ = false;
    save_settings_now();
    note({"Saved " + m.name, ui::Tone::Ok});
}

/* ---------------------------------------------------------------- Wake-on-LAN: a machine */

void ToolboxApp::build_machine(int select)
{
    const WolMachine *m = settings_.find_machine(wol_current_);
    if (!m) {
        post([this] { back(); });
        return;
    }
    read_net();
    band_check_ = true;
    const Geo g = geo();
    const int y = build_band();
    build_rich(y, g.bottom - y);
    const cp0_ui_metrics_t *mt = cp0_ui_metrics_get();
    wake_block_ = std::make_shared<ui::WakeBlock>(rich_->layout().w, rich_->layout().inset, ui::caption_font(),
                                                  ui::font_bold(mt->text_px[CP0_TEXT_TITLE]));
    rich_->reset_rows(machine_list(), 0);
    if (select > 0) rich_->select(rich_->index_of_id(select));
    focus(rich_->focus_object());
    poll_machine(true);
    log_rows("machine");
}

std::vector<ui::RichRow> ToolboxApp::machine_list()
{
    const WolMachine *m = settings_.find_machine(wol_current_);
    const std::shared_ptr<const WolSnapshot> s = fresh_ ? backend_.wol->latest() : nullptr;
    const bool running = s && s->running();
    std::vector<ui::RichRow> rows;
    rows.push_back(ui::RichRow::custom(wake_block_, wake_block_->height(), false));
    rows.push_back(action_row(running ? "Stop" : "Wake now", running ? (s->state == WolSnapshot::State::Sending ? "sending" : "watching") : "", kRun));
    if (!m) return rows;
    rows.push_back(action_row("Name", m->name, kWolName));
    rows.push_back(action_row("MAC address", m->mac, kWolMac));
    rows.push_back(action_row("Watch address", m->host.empty() ? "none" : m->host, kWolHost));
    rows.push_back(action_row("Watch after waking", m->host.empty() ? "needs an address" : (settings_.wol_watch ? "On" : "Off"), kWolWatch));
    rows.push_back(action_row("Save log", "", kSaveLog));
    ui::RichRow del = ui::RichRow::item(wol_delete_armed_ ? "Press again to delete" : "Delete", wol_delete_armed_ ? "this cannot be undone" : "",
                                        wol_delete_armed_ ? ui::Tone::Bad : ui::Tone::Normal, kWolDelete);
    rows.push_back(del);
    rows.push_back(ui::RichRow::text(ui::kWolNote, ui::Tone::Normal, true, true));
    return rows;
}

void ToolboxApp::poll_machine(bool force)
{
    const std::shared_ptr<const WolSnapshot> snap = fresh_ ? backend_.wol->latest() : nullptr;
    const std::uint64_t seq = backend_.wol->seq();
    const WolMachine *m = settings_.find_machine(wol_current_);
    const WolSnapshot idle;
    if ((force || seq != shown_seq_) && m && wake_block_) {
        shown_seq_ = seq;
        wake_block_->set(snap ? *snap : idle, !m->host.empty());
        if (snap && snap->state != WolSnapshot::State::Idle) wol_last_[lower(wol_current_)] = ui::wake_last_of(*snap, wol_sent_at_);
        rich_->set_rows(machine_list());
        if (backend_.demo && !wake_block_->detail_fits()) {
            std::fprintf(stderr, "[net-toolbox] the wake detail does not fit its block\n");
            layout_cut_ = true;
        }
    }
    show_band(ui::wol_band(snap ? *snap : idle, wol_current_));
}

void ToolboxApp::start_wake()
{
    const WolMachine *m = settings_.find_machine(wol_current_);
    if (!m) return;
    read_net();
    WolRequest req;
    req.machine = *m;
    req.net = net_;
    req.iface = scope_.iface; /* the chosen network: "" = every interface that is up */
    req.watch = settings_.wol_watch;
    req.resolve.server = pick_resolver("", dns_servers());
    wol_sent_at_ = backend_.wall_clock ? backend_.wall_clock() : std::time(nullptr);
    backend_.wol->start(req);
    fresh_ = true;
    shown_seq_ = 0;
}

void ToolboxApp::activate_machine(std::intptr_t id)
{
    if (id != kWolDelete) wol_delete_armed_ = false;
    const WolMachine *m = settings_.find_machine(wol_current_);
    if (!m) return;
    if (id == kRun) {
        const std::shared_ptr<const WolSnapshot> s = fresh_ ? backend_.wol->latest() : nullptr;
        if (s && s->running()) backend_.wol->stop();
        else start_wake();
    } else if (id == kWolName) {
        open_wol_edit(m->name, 1);
        return;
    } else if (id == kWolMac) {
        open_wol_edit(m->name, 2);
        return;
    } else if (id == kWolHost) {
        open_wol_edit(m->name, 3);
        return;
    } else if (id == kWolWatch) {
        if (m->host.empty()) {
            note({"Add an address to watch first", ui::Tone::Warn});
        } else {
            settings_.wol_watch = !settings_.wol_watch;
            save_settings_now();
        }
    } else if (id == kSaveLog) {
        save_result();
    } else if (id == kWolDelete) {
        if (!wol_delete_armed_) {
            wol_delete_armed_ = true;
        } else {
            const std::string name = m->name;
            settings_.remove_machine(name);
            wol_last_.erase(lower(name));
            wol_delete_armed_ = false;
            save_settings_now();
            note({"Deleted " + name, ui::Tone::Ok});
            back(); /* the list again */
            return;
        }
    }
    poll_machine(true);
}

/* ---------------------------------------------------------------- Bluetooth radar */

void ToolboxApp::build_ble()
{
    const Geo g = geo();
    band_check_ = true;
    int y = build_band();
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    ui::ActionBarEvents bar_events;
    bar_events.activate = [this](int id) { post([this, id] { activate_ble(id); }); };
    bar_events.back = [this] { post([this] { back(); }); };
    bar_events.down = [this] { post([this] { focus(rich_ ? rich_->focus_object() : nullptr); }); };
    const std::vector<ui::ActionCell> cells = {{kRun, "Start", ""}, {kSaveLog, "Save log", ""}};
    bar_ = std::make_unique<ui::ActionBar>(page_->root(), g.x, y, g.w, g.row, ui::font_medium(m->text_px[CP0_TEXT_BODY]),
                                           ui::font_medium(std::max(10, ui::caption_px() - 4)), cells, std::move(bar_events));
    y += g.row + g.s;
    int info_h = 0;
    info_ = build_info(y, ui::kBleWarnText, 2, &info_h);
    y += info_h + g.s;
    ble_box_ = ui::ble_layout(g.w, g.bottom - y, g.crow, g.s);
    const ui::Box &rb = ble_box_.radar;
    const ui::Box &lb = ble_box_.list;
    radar_ = std::make_unique<ui::RadarView>(page_->root(), ui::Box{g.x + rb.x, y + rb.y, rb.w, rb.h},
                                             ui::font_medium(std::max(10, ui::caption_px() - 4)));
    build_rich_at(g.x + lb.x, y + lb.y, lb.w, lb.h);
    rich_->set_up_out([this] {
        post([this] {
            if (bar_) focus(bar_->cell_object(0));
        });
    });
    rich_->set_tab([this] {
        post([this] {
            if (bar_) focus(bar_->cell_object(0));
        });
    });
    ble_selected_.clear();
    ble_addrs_.clear();
    ble_detail_ = ble_detail_shown_ = false;
    std::vector<lv_obj_t *> order = bar_->focus_objects();
    order.push_back(rich_->focus_object());
    focus_group(order);
    poll_ble(true);
}

std::vector<ui::RichRow> ToolboxApp::ble_list()
{
    const std::shared_ptr<const BleSnapshot> s = fresh_ ? backend_.ble->latest() : nullptr;
    std::vector<ui::RichRow> rows;
    ble_addrs_.clear();
    if (ble_detail_) {
        const BleDevice *dev = nullptr;
        if (s)
            for (const BleDevice &d : s->devices)
                if (d.address == ble_detail_addr_) dev = &d;
        if (!dev) {
            rows.push_back(ui::RichRow::text("This device is not in the list any more.", ui::Tone::Normal, true));
            return rows;
        }
        rows.push_back(ui::RichRow::section(ui::ble_row_label(*dev)));
        for (const ui::RowDef &d : ui::ble_detail_rows(*dev, clock_ms())) rows.push_back(rich_row(d));
        rows.push_back(ui::RichRow::text("Esc closes the details.", ui::Tone::Normal, true, true));
        return rows;
    }
    if (s && !s->devices.empty()) {
        rows.push_back(ui::RichRow::text(ui::kBleLegendLine, ui::Tone::Normal, false, true));
        const int inset = rich_->layout().inset, w = rich_->layout().w;
        const std::size_t n = s->devices.size();
        if (machine_views_.size() > n) machine_views_.resize(n);
        while (machine_views_.size() < n)
            machine_views_.push_back(std::make_shared<ui::MachineRowView>(w, inset, ui::font_medium(rich_->layout().text_px),
                                                                         ui::caption_font(), rich_->layout().row_h));
        for (std::size_t i = 0; i < n; ++i) {
            const BleDevice &d = s->devices[i];
            ble_addrs_.push_back(d.address);
            machine_views_[i]->set(ui::ble_row_label(d), ui::ble_row_value(d), "", ui::Tone::Normal);
            rows.push_back(ui::RichRow::custom(machine_views_[i], machine_views_[i]->height(), true, kBleDev + static_cast<int>(i)));
        }
        return rows;
    }
    if (s && s->state == BleSnapshot::State::Failed) {
        const ui::BlePlain p = ui::ble_problem(s->error);
        if (!p.fix.empty()) rows.push_back(ui::RichRow::text(p.fix, ui::Tone::Warn, true));
        rows.push_back(ui::RichRow::text(p.words, ui::Tone::Normal, true, true));
    } else if (s && s->state == BleSnapshot::State::Checking) {
        rows.push_back(ui::RichRow::text("Checking the Bluetooth adapter.", ui::Tone::Normal, true));
    } else if (s && s->state == BleSnapshot::State::Scanning) {
        rows.push_back(ui::RichRow::text("No device heard yet.", ui::Tone::Normal, true));
    } else if (s && (s->state == BleSnapshot::State::Done || s->state == BleSnapshot::State::Cancelled)) {
        rows.push_back(ui::RichRow::text("No device was heard.", ui::Tone::Normal, true));
    } else {
        rows.push_back(ui::RichRow::text(ui::kBleFirstUse, ui::Tone::Normal, true));
    }
    return rows;
}

void ToolboxApp::update_ble_selection()
{
    if (!rich_ || !radar_) return;
    const int id = rich_->selected_id();
    std::string addr = ble_selected_;
    if (!ble_detail_) {
        if (id >= kBleDev && id - kBleDev < static_cast<int>(ble_addrs_.size())) addr = ble_addrs_[static_cast<std::size_t>(id - kBleDev)];
        else if (ble_addrs_.empty()) addr.clear();
    }
    ble_selected_ = addr;
    radar_->set_selected(addr);
}

void ToolboxApp::poll_ble(bool force)
{
    const std::shared_ptr<const BleSnapshot> snap = fresh_ ? backend_.ble->latest() : nullptr;
    const std::uint64_t seq = backend_.ble->seq();
    const BleSnapshot idle;
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        const bool running = snap && snap->running();
        bar_->set_cell(0, running ? "Stop" : "Start", "");
        const std::vector<ui::RichRow> rows = ble_list();
        if (rich_->count() == 0 || ble_detail_ != ble_detail_shown_) rich_->reset_rows(rows, ble_detail_ ? 1 : 0);
        else rich_->set_rows(rows);
        ble_detail_shown_ = ble_detail_;
        if (!ble_detail_ && !ble_selected_.empty()) { /* the highlight follows the device, not the row */
            for (std::size_t i = 0; i < ble_addrs_.size(); ++i)
                if (ble_addrs_[i] == ble_selected_) rich_->select(rich_->index_of_id(kBleDev + static_cast<int>(i)));
        }
        const BleSnapshot &ref = snap ? *snap : idle;
        radar_->set_devices(ref.devices, clock_ms(), ref.state == BleSnapshot::State::Scanning, kBleExpiryMs);
        if (force) log_rows("ble");
        if (backend_.demo && !radar_->inside_box()) {
            std::fprintf(stderr, "[net-toolbox] the radar leaves its box\n");
            layout_cut_ = true;
        }
    }
    if (radar_) radar_->tick(clock_ms());
    update_ble_selection();
    show_band(ui::ble_band(snap ? *snap : idle), false);
}

void ToolboxApp::start_ble()
{
    BleConfig cfg;
    cfg.seconds = settings_.ble_seconds;
    backend_.ble->start(cfg);
    fresh_ = true;
    shown_seq_ = 0;
    ble_selected_.clear();
    ble_detail_ = false;
}

void ToolboxApp::activate_ble(std::intptr_t id)
{
    if (id == kRun) {
        const std::shared_ptr<const BleSnapshot> s = fresh_ ? backend_.ble->latest() : nullptr;
        if (s && s->running()) backend_.ble->stop();
        else start_ble();
    } else if (id == kSaveLog) {
        save_result();
    } else if (ble_detail_) { /* a tap or Enter in the details: back to the list */
        ble_detail_ = false;
    } else if (id >= kBleDev && id - kBleDev < static_cast<int>(ble_addrs_.size())) {
        ble_detail_addr_ = ble_addrs_[static_cast<std::size_t>(id - kBleDev)];
        ble_selected_ = ble_detail_addr_;
        ble_detail_ = true;
    }
    poll_ble(true);
}

} // namespace nt
