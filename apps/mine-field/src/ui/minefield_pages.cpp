/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field: the menu, difficulty, best times, statistics and settings pages (see minefield_app.hpp).
 * Lists are the toolkit's CompactList (Comfortable rows: every row is a finger target) and follow the row-affordance rule:
 * Open for a page, Edit for a value that changes (its value, then the chevron), Action for a thing done right there (the
 * white dot), Info for a read-out (grey).
 */
#include "minefield_app.hpp"

#include "palette.hpp"

#include "app.hpp"

#include "cp0_ui_list.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace minefield;
using cp0::ui::ListRow;
using cp0::ui::RowAffordance;
using cp0::ui::ValueTone;

namespace mfui {

namespace {

std::string mmss_ms(std::uint32_t ms)
{
    const std::uint32_t s = ms / 1000;
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(b, sizeof b, "%u:%02u", s / 60, s % 60);
    return b;
}

std::string dims_text(const Dims &d)
{
    return std::to_string(d.w) + " x " + std::to_string(d.h) + ", " + std::to_string(d.mines) + " mines";
}

std::string dims_short(const Dims &d)
{
    return std::to_string(d.w) + "x" + std::to_string(d.h) + ", " + std::to_string(d.mines);
}

int text_w(const char *text, const lv_font_t *font)
{
    lv_point_t sz{};
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

const char *on_off(bool v)
{
    return v ? "On" : "Off";
}

const char *anim_name(Animations a)
{
    return a == Animations::On ? "On" : (a == Animations::Reduced ? "Reduced" : "Off");
}

std::string cap_name(Preset p)
{
    switch (p) {
    case Preset::Beginner: return "Beginner";
    case Preset::Intermediate: return "Intermediate";
    case Preset::Expert: return "Expert";
    default: return "Beginner";
    }
}

std::string seed_short(std::uint64_t seed)
{
    char b[16];
    std::snprintf(b, sizeof b, "#%06llX", static_cast<unsigned long long>(seed & 0xFFFFFF));
    return b;
}

} // namespace

/* ------------------------------------------------------------------------------------------------ views */

void MineFieldApp::show_title()
{
    std::vector<std::string> path{"Mine Field"};
    switch (view_) {
    case View::Menu: path.push_back("Menu"); break;
    case View::Difficulty: path.push_back("Difficulty"); break;
    case View::Best: path.push_back("Best times"); break;
    case View::Stats: path.push_back("Statistics"); break;
    case View::Settings: path.push_back("Settings"); break;
    default: break;
    }
    page_->set_title(path);
}

void MineFieldApp::open_view(View v)
{
    if (view_ == v) return;
    clear_hint();
    if (view_ == View::Game) {
        show_game_widgets(false);
    } else {
        list_.reset();
        drop_hint();
    }
    view_ = v;
    show_title();
    cp0_ui_list_layout_t ll{};
    cp0_ui_list_page_layout(cp0_ui_metrics_get(), &layout_, CP0_UI_LIST_COMFORTABLE, &ll);
    ll.separator_px = 1;
    int hint_h = 0;
    if (v == View::Settings) { /* a two-line explanation of the highlighted row under the list */
        const lv_font_t *cf = cp0_ui_font(CP0_TEXT_CAPTION);
        hint_h = 2 * lv_font_get_line_height(cf) + std::max(4, cp0_ui_metrics_get()->tok[CP0_TOK_SPACE_M]);
        ll.h -= hint_h;
    }
    cp0::ui::CompactListEvents ev;
    ev.back = [this] { post([this] { back(); }); };
    std::vector<ListRow> rows;
    int sel = 0;
    switch (v) {
    case View::Menu:
        ev.activate = [this](int, std::intptr_t id) { post([this, id] { menu_activate(id); }); };
        rows = menu_rows();
        sel = menu_sel_;
        break;
    case View::Difficulty:
        ev.activate = [this](int, std::intptr_t id) { post([this, id] { difficulty_activate(id); }); };
        rows = difficulty_rows();
        break;
    case View::Settings:
        ev.activate = [this](int, std::intptr_t id) { post([this, id] { settings_activate(id); }); };
        ev.selected = [this](int index) {
            if (view_ == View::Settings && hint_) lv_label_set_text(hint_, settings_hint(index).c_str());
        };
        rows = settings_rows();
        break;
    case View::Best: rows = best_rows(); break;
    case View::Stats: rows = stats_rows(); break;
    default: break;
    }
    list_ = std::make_unique<cp0::ui::CompactList>(page_->root(), ll, std::move(ev));
    list_->reset_rows(rows, sel);
    if (v == View::Settings) {
        hint_ = lv_label_create(page_->root());
        lv_obj_set_style_text_font(hint_, cp0_ui_font(CP0_TEXT_CAPTION), 0);
        lv_obj_set_style_text_color(hint_, lv_color_hex(pal::kMuted), 0);
        lv_label_set_long_mode(hint_, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(hint_, ll.x + ll.inset, ll.y + ll.h + std::max(2, cp0_ui_metrics_get()->tok[CP0_TOK_SPACE_S]));
        lv_obj_set_size(hint_, ll.w - 2 * ll.inset, hint_h);
        lv_label_set_text(hint_, settings_hint(list_->selected()).c_str());
    }
    focus(list_->focus_object());
}

void MineFieldApp::close_view()
{
    if (view_ == View::Game) return;
    const View was = view_;
    list_.reset();
    drop_hint();
    view_ = View::Game;
    show_game_widgets(true);
    show_title();
    last_tick_ = lv_tick_get();
    focus(board_->surface());
    if (was == View::Settings) apply_pending_settings();
    after_change(false);
    announce_pending();
}

void MineFieldApp::back()
{
    if (!alive_) return;
    if (animating_) {
        skip_anim();
        return;
    }
    if (dialog_) {
        close_dialog();
        return;
    }
    switch (view_) {
    case View::Menu: close_view(); return;
    case View::Difficulty:
    case View::Best:
    case View::Stats:
    case View::Settings: {
        const bool was_settings = view_ == View::Settings;
        open_view(View::Menu);
        if (was_settings) apply_pending_settings();
        return;
    }
    default: break;
    }
    cancel_prepare();
    session_->save_game();
    cdk::request_quit(0);
}

void MineFieldApp::drop_hint()
{
    if (hint_ && lv_obj_is_valid(hint_)) lv_obj_delete(hint_);
    hint_ = nullptr;
}

/* the next game's board or fair flag differs from this game's: a game not yet dug (or finished) takes it now */
void MineFieldApp::apply_pending_settings()
{
    if (!session_->settings_pending() || session_->in_progress()) return;
    cancel_prepare();
    session_->new_game();
    supplied_ = false;
    finish_handled_ = result_shown_ = false;
    loss_order_.clear();
    board_->bind(&session_->game());
    apply_layout();
}

void MineFieldApp::announce_pending()
{
    if (!session_->settings_pending() || !session_->in_progress()) {
        pending_announced_ = false;
        return;
    }
    if (pending_announced_) return;
    pending_announced_ = true;
    notice("New board settings start with the next game", 3000);
}

/* ------------------------------------------------------------------------------------------------ rows */

std::vector<ListRow> MineFieldApp::menu_rows() const
{
    const Settings &s = session_->settings();
    std::vector<ListRow> r;
    r.push_back(ListRow::item("Resume", "", ValueTone::Normal, kMenuResume, RowAffordance::Action));
    r.push_back(ListRow::item("New game", "", ValueTone::Normal, kMenuNew, RowAffordance::Action));
    r.push_back(ListRow::item("Difficulty", cap_name(s.preset), ValueTone::Normal, kMenuDifficulty, RowAffordance::Open));
    r.push_back(ListRow::item("Best times", "", ValueTone::Normal, kMenuBest, RowAffordance::Open));
    r.push_back(ListRow::item("Statistics", "", ValueTone::Normal, kMenuStats, RowAffordance::Open));
    r.push_back(ListRow::item("Settings", "", ValueTone::Normal, kMenuSettings, RowAffordance::Open));
    r.push_back(ListRow::section("About"));
    r.push_back(ListRow::item("Mine Field", kVersion, ValueTone::Normal, kMenuVersion, RowAffordance::Info));
    return r;
}

std::vector<ListRow> MineFieldApp::difficulty_rows() const
{
    const Settings &s = session_->settings();
    /* the board of each level on THIS screen; the long form when it fits next to the label, else columns x rows, mines */
    cp0_ui_list_layout_t ll{};
    cp0_ui_list_page_layout(cp0_ui_metrics_get(), &layout_, CP0_UI_LIST_COMFORTABLE, &ll);
    bool long_form = true;
    for (Preset p : {Preset::Beginner, Preset::Intermediate, Preset::Expert}) {
        const int need = text_w("Intermediate", cp0_ui_font_px(ll.text_px)) + ll.gap + text_w(dims_text(session_->dims_for(p)).c_str(), cp0_ui_font_px(ll.value_px));
        if (need > ll.w - 2 * ll.inset - 2 * ll.text_px) long_form = false;
    }
    std::vector<ListRow> r;
    r.push_back(ListRow::section("On this screen"));
    for (Preset p : {Preset::Beginner, Preset::Intermediate, Preset::Expert}) {
        const Dims d = session_->dims_for(p);
        r.push_back(ListRow::item(cap_name(p), long_form ? dims_text(d) : dims_short(d),
                                  p == s.preset ? ValueTone::Ok : ValueTone::Normal,
                                  p == Preset::Beginner ? kDiffBeginner : (p == Preset::Intermediate ? kDiffIntermediate : kDiffExpert),
                                  RowAffordance::Action));
    }
    return r;
}

std::vector<ListRow> MineFieldApp::best_rows() const
{
    std::vector<ListRow> r;
    for (Preset p : {Preset::Beginner, Preset::Intermediate, Preset::Expert}) {
        r.push_back(ListRow::section(cap_name(p) + " (now " + dims_text(session_->dims_for(p)) + ")"));
        const PresetStats st = session_->stats().get(p);
        if (st.best.empty()) {
            r.push_back(ListRow::item("No best time yet", "", ValueTone::Muted, kInfo, RowAffordance::Info));
            continue;
        }
        int n = 0;
        for (const BestEntry &e : st.best)
            r.push_back(ListRow::item(std::to_string(++n) + ".  " + mmss_ms(e.time_ms),
                                      std::to_string(e.w) + "x" + std::to_string(e.h) + (e.fair ? " fair " : " ") + seed_short(e.seed), ValueTone::Normal, kInfo, RowAffordance::Info));
    }
    return r;
}

std::vector<ListRow> MineFieldApp::stats_rows() const
{
    std::vector<ListRow> r;
    auto info = [&](const char *label, const std::string &value) {
        r.push_back(ListRow::item(label, value, ValueTone::Normal, kInfo, RowAffordance::Info));
    };
    for (Preset p : {Preset::Beginner, Preset::Intermediate, Preset::Expert}) {
        const PresetStats v = session_->stats().get(p);
        r.push_back(ListRow::section(cap_name(p)));
        info("Played", std::to_string(v.played));
        info("Won", std::to_string(v.won));
        info("Win rate", v.played ? std::to_string(v.won * 100 / v.played) + "%" : "-");
        info("Current streak", std::to_string(v.streak));
        info("Best streak", std::to_string(v.best_streak));
        info("Total time", mmss_ms(static_cast<std::uint32_t>(std::min<std::uint64_t>(v.total_time_ms, 0xFFFFFFFFull))));
    }
    return r;
}

std::vector<ListRow> MineFieldApp::settings_rows() const
{
    const Settings &s = session_->settings();
    const bool pending_fair = s.fair != game().config().fair;
    auto edit = [](const char *label, const std::string &value, std::intptr_t id, bool pending = false) {
        return ListRow::item(label, value, pending ? ValueTone::Warn : ValueTone::Normal, id, RowAffordance::Edit);
    };
    std::vector<ListRow> r;
    r.push_back(ListRow::section("Board"));
    r.push_back(edit("Fair boards", on_off(s.fair), kSetFair, pending_fair && session_->in_progress()));
    r.push_back(ListRow::section("Look"));
    r.push_back(edit("Animations", anim_name(s.animations), kSetAnim));
    r.push_back(edit("Show timer", on_off(s.show_timer), kSetTimer));
    r.push_back(edit("Flag mode on start", on_off(s.flag_mode), kSetFlagStart));
    r.push_back(ListRow::section("Data"));
    r.push_back(ListRow::item("Reset statistics", "", ValueTone::Normal, kSetReset, RowAffordance::Action));
    return r;
}

/* the one-line explanation of the row under the highlight, shown in the page's status line */
std::string MineFieldApp::settings_hint(int index) const
{
    const std::vector<ListRow> rows = settings_rows();
    if (index < 0 || index >= static_cast<int>(rows.size())) return "";
    switch (rows[static_cast<size_t>(index)].id) {
    case kSetFair: return "Never needs a guess; may take a moment to prepare";
    case kSetAnim: return "Reduced and Off show wins, losses and hints without motion";
    case kSetTimer: return "The clock always runs; this only draws it";
    case kSetFlagStart: return "Start the app with taps flagging instead of digging";
    case kSetReset: return "Removes the results and best times of every difficulty";
    default: return "";
    }
}

/* ------------------------------------------------------------------------------------------------ activation */

void MineFieldApp::menu_activate(std::intptr_t id)
{
    if (view_ != View::Menu || dialog_) return;
    menu_sel_ = list_ ? list_->selected() : 0;
    switch (id) {
    case kMenuResume: close_view(); break;
    case kMenuNew:
        close_view();
        request_new_game();
        break;
    case kMenuDifficulty: open_view(View::Difficulty); break;
    case kMenuBest: open_view(View::Best); break;
    case kMenuStats: open_view(View::Stats); break;
    case kMenuSettings: open_view(View::Settings); break;
    default: break;
    }
}

void MineFieldApp::apply_difficulty(Preset p)
{
    Settings s = session_->settings();
    s.preset = p;
    session_->set_settings(s);
    auto go = [this] { start_new_game(); };
    if (session_->in_progress()) {
        confirm("Start a new game?", "The game in progress counts as a lost game.", "New game", go);
    } else {
        go();
    }
}

void MineFieldApp::difficulty_activate(std::intptr_t id)
{
    if (view_ != View::Difficulty || dialog_) return;
    switch (id) {
    case kDiffBeginner: apply_difficulty(Preset::Beginner); break;
    case kDiffIntermediate: apply_difficulty(Preset::Intermediate); break;
    case kDiffExpert: apply_difficulty(Preset::Expert); break;
    default: break;
    }
}

void MineFieldApp::settings_activate(std::intptr_t id)
{
    if (view_ != View::Settings || dialog_) return;
    Settings s = session_->settings();
    switch (id) {
    case kSetFair: s.fair = !s.fair; break;
    case kSetAnim:
        s.animations = s.animations == Animations::On ? Animations::Reduced : (s.animations == Animations::Reduced ? Animations::Off : Animations::On);
        break;
    case kSetTimer: s.show_timer = !s.show_timer; break;
    case kSetFlagStart: s.flag_mode = !s.flag_mode; break;
    case kSetReset:
        confirm("Reset statistics?", "The results and the best times of every difficulty are removed for good.", "Reset", [this] {
            session_->reset_all_stats();
            notice("Statistics reset", 1800);
        });
        return;
    default: return;
    }
    session_->set_settings(s);
    if (list_) list_->set_rows(settings_rows());
}

} // namespace mfui
