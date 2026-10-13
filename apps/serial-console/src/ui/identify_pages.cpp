/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the pages of "What is on the other end?" (see serial_app.hpp): the probes' rows and confirmations,
 * the Ask page, the result card and its detail pages. The probes run in core/probe.hpp (a worker thread); this file
 * only shows them. Layout from the layout service only (the roller page, the widgets, the text sizes).
 */
#include "serial_app.hpp"

#include "app.hpp"
#include "fsutil.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>

namespace sc {
namespace {

constexpr int kErrorRgb = 0xFF6666; /* the shell's error colour (failed values) */

int level_index(Probe p)
{
    return probe_level(p) - 1; /* 0 Listen, 1 Reset */
}

} // namespace

Identity SerialApp::port_identity() const
{
    return config_.identity(port_.id);
}

/* ------------------------------------------------------------------ starting a probe */

void SerialApp::show_confirm(const std::string &title, const std::string &text, const std::string &ok,
                             std::function<void()> on_ok)
{
    dialog_.reset();
    cp0::ui::DialogSpec spec;
    spec.title = title;
    spec.page = page_.get();
    spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] { post([this] { close_dialog(); }); }});
    spec.buttons.push_back({ok, cp0::ui::ButtonRole::Primary, [this, on_ok] {
                                post([this, on_ok] {
                                    close_dialog();
                                    on_ok();
                                });
                            }});
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    dialog_->add_text(text);
    dialog_->relayout();
    focus(dialog_->focus_object());
}

void SerialApp::start_probe(Probe probe, bool confirmed)
{
    if (prober_ || finder_) return;
    if (!port_present_) {
        show_problem(open_problem(ENOENT, groups(), port_.tty));
        return;
    }
    const ResetStyle style = reset_style(port_);
    const int baud = effective_baud(settings_, recommended());
    if (probe_level(probe) >= 2 && !confirmed) { /* never without the user's yes */
        show_confirm(confirm_title(probe, style), confirm_text(probe, style, baud), confirm_button(probe, style),
                     [this, probe] { start_probe(probe, true); });
        return;
    }
    Frame frame;
    parse_frame(settings_.frame, frame);
    ProbeRequest req;
    req.probe = probe;
    req.target = probe_target(port_, baud, frame, roots_);
    req.confirmed = confirmed;
    if (world_) req.lines = world_->lines_for(port_.tty); /* the demonstration's boards see the lines */
    prober_ = std::make_unique<Prober>(std::move(req));
    prober_->start();
    probe_ = probe;
    probe_shown_ = prober_->progress().text;
    card_note_.clear();
    std::fprintf(stderr, "[serial-console] identify %s: %s on %s\n", port_.tty.c_str(), probe_key(probe),
                 port_.path.c_str());
    if (probe_level(probe) <= 2) {
        probe_value_[level_index(probe)].clear();
        show_port_rows(port_row_index(probe == Probe::Listen ? PortRow::IdListen : PortRow::IdReset));
        update_port_caption(true);
    } else {
        ask_probe_ = probe;
        ask_value_.clear();
        const auto it = std::find(ask_map_.begin(), ask_map_.end(), probe);
        open_ask(it == ask_map_.end() ? 0 : static_cast<int>(it - ask_map_.begin()));
    }
}

void SerialApp::check_probe()
{
    if (!prober_) return;
    const ProbeProgress p = prober_->progress();
    if (!p.done) {
        if (p.text == probe_shown_) return;
        probe_shown_ = p.text;
        cp0::ui::RowSpec spec;
        spec.kind = cp0::ui::RowKind::Busy;
        spec.label = probe_shown_;
        if (probe_level(probe_) <= 2 && nav_.current() == View::Port)
            roller_->update_row(port_row_index(probe_ == Probe::Listen ? PortRow::IdListen : PortRow::IdReset), spec);
        if (probe_level(probe_) == 3 && nav_.current() == View::Ask) {
            const auto it = std::find(ask_map_.begin(), ask_map_.end(), probe_);
            if (it != ask_map_.end()) roller_->update_row(static_cast<int>(it - ask_map_.begin()), spec);
        }
        return;
    }
    const Probe probe = probe_;
    prober_.reset();
    probe_done(p, probe);
}

void SerialApp::probe_done(const ProbeProgress &p, Probe probe)
{
    std::string outcome;
    Problem notice;
    bool show = false;
    Identity shown;
    if (p.error && p.error != ENODEV) {
        outcome = "Failed";
        notice = open_problem(p.error, groups(), port_.tty);
    } else if (p.error == ENODEV) {
        outcome = "Port went away";
        notice.title = port_.tty + " went away";
        notice.text = port_.tty + " went away during the probe. Plug it in again.";
    } else if (p.cancelled) {
        outcome = "Stopped";
    } else {
        const Identity now = p.result;
        const Identity keep = remembered_identity(port_identity(), now);
        config_.set_identity(port_.id, keep);
        save_port();
        port_.rec = identified_recommendation(recommend(port_.known, port_.onboard), keep);
        shown = now.found() ? keep : now;
        if (!now.found()) outcome = now.kind == Kind::Silent ? "Nothing heard" : "Not identified";
        card_note_ = p.note;
        show = true;
        std::fprintf(stderr, "[serial-console] identify %s: %s -> %s (%d)\n", port_.tty.c_str(), probe_key(probe),
                     kind_key(now.kind), now.confidence);
    }
    if (probe_level(probe) <= 2) probe_value_[level_index(probe)] = outcome;
    else ask_value_ = outcome;
    // the probe's page comes back with its row as it is now
    if (nav_.current() == View::Port) {
        show_port_rows(port_row_index(probe == Probe::Listen ? PortRow::IdListen
                                      : probe == Probe::Reset ? PortRow::IdReset
                                                              : PortRow::IdAsk));
        update_port_caption(true);
    } else if (nav_.current() == View::Ask) {
        const auto it = std::find(ask_map_.begin(), ask_map_.end(), probe);
        open_ask(it == ask_map_.end() ? 0 : static_cast<int>(it - ask_map_.begin()));
    }
    if (show) open_card(shown, probe, -1);
    if (!notice.title.empty()) show_problem(notice);
}

/* ------------------------------------------------------------------ the Ask page */

std::vector<cp0::ui::RowSpec> SerialApp::ask_rows()
{
    ask_map_ = ask_probes();
    std::vector<cp0::ui::RowSpec> rows;
    for (const Probe p : ask_map_) {
        cp0::ui::RowSpec spec;
        if (prober_ && probe_ == p) {
            spec.kind = cp0::ui::RowKind::Busy;
            spec.label = probe_shown_;
        } else {
            spec.kind = cp0::ui::RowKind::Nav;
            spec.label = probe_name(p);
            spec.value = p == ask_probe_ && !ask_value_.empty() ? ask_value_ : ask_value(p);
            if (p == ask_probe_ && (ask_value_ == "Stopped" || ask_value_ == "Failed")) spec.value_rgb = kErrorRgb;
        }
        rows.push_back(spec);
    }
    return rows;
}

void SerialApp::open_ask(int select)
{
    if (nav_.current() != View::Ask) nav_.open(View::Ask);
    const std::vector<cp0::ui::RowSpec> rows = ask_rows();
    roller_->set_rows(rows, std::max(0, std::min(select, static_cast<int>(rows.size()) - 1)), true);
    roller_->set_hidden(false);
    caption_row_ = -2;
    show_title();
    update_ask_caption(true);
}

void SerialApp::activate_ask(int row)
{
    if (row < 0 || row >= static_cast<int>(ask_map_.size())) return;
    const Probe p = ask_map_[static_cast<std::size_t>(row)];
    if (prober_) { /* while one runs, only its own row (stop) */
        if (p == probe_) prober_->cancel();
        return;
    }
    start_probe(p, false);
}

void SerialApp::update_ask_caption(bool force)
{
    const int sel = roller_->selected();
    if (!force && sel == caption_row_) return;
    caption_row_ = sel;
    if (prober_) page_->set_caption("Esc stops", cp0::ui::CaptionTone::Normal);
    else if (sel >= 0 && sel < static_cast<int>(ask_map_.size()))
        page_->set_caption(ask_caption(ask_map_[static_cast<std::size_t>(sel)]), cp0::ui::CaptionTone::Normal);
    else page_->set_caption("", cp0::ui::CaptionTone::Normal);
}

/* ------------------------------------------------------------------ the result card */

void SerialApp::open_card(const Identity &id, Probe probe, int select)
{
    card_id_ = id;
    card_probe_ = probe;
    if (nav_.current() != View::Card) nav_.open(View::Card);
    show_card_rows(select);
    show_title();
    update_card_caption(true);
}

void SerialApp::show_card_rows(int select)
{
    card_rows_ = card_rows(card_id_, port_, card_probe_, effective_baud(settings_, recommended()));
    if (!card_note_.empty()) { /* what the probe has to say (no reset line, the board left at its prompt...) */
        const auto behind = std::find_if(card_rows_.begin(), card_rows_.end(),
                                         [](const CardRow &r) { return r.key == "Behind it"; });
        card_rows_.insert(behind == card_rows_.end() ? card_rows_.end() : behind + 1,
                          CardRow{card_note_, std::string(), CardAction::None});
    }
    std::vector<cp0::ui::RowSpec> rows;
    int behind_row = 0;
    for (std::size_t i = 0; i < card_rows_.size(); ++i) {
        const CardRow &r = card_rows_[i];
        cp0::ui::RowSpec spec;
        spec.kind = r.action == CardAction::None ? cp0::ui::RowKind::Info : cp0::ui::RowKind::Nav;
        spec.label = r.key;
        spec.value = r.value;
        if (r.key == "Behind it") behind_row = static_cast<int>(i);
        rows.push_back(spec);
    }
    if (select < 0 && card_id_.kind == Kind::Silent) { /* heard nothing: the highlight is on the next step, Enter asks */
        for (std::size_t i = 0; i < card_rows_.size(); ++i)
            if (card_rows_[i].action == CardAction::Ask) {
                select = static_cast<int>(i);
                break;
            }
    }
    if (select < 0 || select >= static_cast<int>(rows.size())) select = behind_row;
    roller_->set_rows(rows, select, false); /* a table stops at its ends */
    roller_->set_hidden(false);
    caption_row_ = -2;
}

void SerialApp::update_card_caption(bool force)
{
    const int sel = roller_->selected();
    if (!force && sel == caption_row_) return;
    caption_row_ = sel;
    if (sel < 0 || sel >= static_cast<int>(card_rows_.size())) {
        page_->set_caption("", cp0::ui::CaptionTone::Normal);
        return;
    }
    const CardRow &r = card_rows_[static_cast<std::size_t>(sel)];
    std::string text;
    switch (r.action) {
    case CardAction::None: break; /* over the table it would cover the rows above */
    case CardAction::Boot: text = "The first lines it printed"; break;
    case CardAction::Evidence: text = "The lines that told what it is"; break;
    case CardAction::Apply: text = "Sets this speed for " + port_.tty + "; Connect uses it"; break;
    case CardAction::Again:
        text = probe_level(card_probe_) <= 2 ? probe_caption(card_probe_, reset_style(port_)) : ask_caption(card_probe_);
        break;
    case CardAction::Copy: text = "Writes this card to a text file in ~/serial-logs"; break;
    case CardAction::Reset: text = probe_caption(Probe::Reset, reset_style(port_)); break;
    case CardAction::Ask: text = probe_caption(Probe::At, reset_style(port_)); break;
    }
    page_->set_caption(text, cp0::ui::CaptionTone::Normal);
}

void SerialApp::close_card()
{
    nav_.back();
    if (nav_.current() == View::Ask) {
        const auto it = std::find(ask_map_.begin(), ask_map_.end(), card_probe_);
        open_ask(it == ask_map_.end() ? 0 : static_cast<int>(it - ask_map_.begin()));
        return;
    }
    PortRow row = PortRow::Behind;
    if (card_probe_ == Probe::Listen && !port_identity().found()) row = PortRow::IdListen;
    if (card_probe_ == Probe::Reset) row = PortRow::IdReset;
    show_port_rows(port_row_index(row));
    show_title();
    update_port_caption(true);
}

void SerialApp::activate_card(int row)
{
    if (row < 0 || row >= static_cast<int>(card_rows_.size())) return;
    const CardRow r = card_rows_[static_cast<std::size_t>(row)];
    switch (r.action) {
    case CardAction::None: break;
    case CardAction::Boot: open_detail("Boot output", card_id_.boot); break;
    case CardAction::Evidence: open_detail("Evidence", card_id_.evidence); break;
    case CardAction::Apply: {
        int baud = 0;
        std::string reason;
        if (!identity_recommendation(card_id_, &baud, &reason)) break;
        settings_.baud = baud == recommended() ? 0 : baud; /* the recommended speed follows the device */
        settings_.frame = "8N1";
        save_port();
        show_card_rows(row);
        page_->set_caption("Set: " + std::to_string(baud) + " 8N1 for " + port_.tty, cp0::ui::CaptionTone::Normal);
        caption_row_ = row;
        break;
    }
    case CardAction::Copy: {
        const std::string name = port_.tty + "-identify-" + log_file_name("x", std::time(nullptr)).substr(2);
        const std::string path = log_dir_ + "/" + name.substr(0, name.size() - 4) + ".txt";
        const bool ok = fs::make_dirs(log_dir_) &&
                        fs::write_atomic(path, identity_summary(card_id_, port_.title, port_.tty));
        page_->set_caption(ok ? "Saved " + fs::base_name(path) : "Could not write " + log_dir_,
                           ok ? cp0::ui::CaptionTone::Normal : cp0::ui::CaptionTone::Error);
        caption_row_ = row;
        break;
    }
    case CardAction::Again: {
        const Probe p = card_probe_;
        if (probe_level(p) <= 2) {
            while (nav_.current() != View::Port && nav_.back() == Back::Popped) {
            }
            show_port_rows(port_row_index(p == Probe::Listen ? PortRow::IdListen : PortRow::IdReset));
            show_title();
            update_port_caption(true);
        } else {
            close_card();
        }
        start_probe(p, false);
        break;
    }
    case CardAction::Reset:
        while (nav_.current() != View::Port && nav_.back() == Back::Popped) {
        }
        show_port_rows(port_row_index(PortRow::IdReset));
        show_title();
        update_port_caption(true);
        start_probe(Probe::Reset, false);
        break;
    case CardAction::Ask:
        while (nav_.current() != View::Port && nav_.back() == Back::Popped) {
        }
        open_ask(0);
        break;
    }
}

/* ------------------------------------------------------------------ detail pages (boot output, evidence) */

void SerialApp::open_detail(const std::string &title, const std::vector<std::string> &lines)
{
    std::string text;
    for (const std::string &l : lines) text += l + "\n";
    if (!text.empty()) text.pop_back();
    const int px = cp0_ui_metrics_get()->text_px[CP0_TEXT_CAPTION];
    const lv_font_t *mono = cp0_fonts().get("JetBrainsMono-Regular.ttf", static_cast<uint16_t>(px));
    if (!mono) mono = cp0_ui_font_px(px);
    nav_.open(View::Detail);
    roller_->set_hidden(true);
    page_->set_caption("", cp0::ui::CaptionTone::Normal);
    detail_ = std::make_unique<cp0::ui::TextBlock>(page_->root(), layout_, text, mono, [this] { post([this] { back(); }); });
    page_->set_title({kAppTitle, port_.tty, title});
    focus(detail_->focus_object());
}

void SerialApp::close_detail()
{
    detail_.reset();
    nav_.back();
    roller_->set_hidden(false);
    show_title();
    focus(roller_->focus_object());
    caption_row_ = -2;
    update_card_caption(true);
}

} // namespace sc
