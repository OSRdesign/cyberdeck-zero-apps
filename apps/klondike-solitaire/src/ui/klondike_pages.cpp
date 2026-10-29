/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike Solitaire: the menu, Settings, Statistics and Help views and the dialogs (see klondike_app.hpp). Lists are the
 * toolkit's CompactList (Comfortable rows: every row is a finger target) and follow the row-affordance rule: Open for a
 * page, Edit for a value that changes (its value, then the chevron), Action for a thing done right there, Info for a
 * read-out.
 */
#include "klondike_app.hpp"

#include "app.hpp"
#include "cardart.hpp"

#include "cp0_ui_list.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace klondike;
using cp0::ui::ListRow;
using cp0::ui::RowAffordance;
using cp0::ui::ValueTone;

namespace kui {

namespace {

std::string mmss(std::uint32_t s)
{
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(b, sizeof b, "%u:%02u", s / 60, s % 60);
    return b;
}

std::string passes_name(int p)
{
    return p == 0 ? "Unlimited" : (p == 1 ? "1 pass" : std::to_string(p) + " passes");
}

const char *scoring_name(Scoring s)
{
    return s == Scoring::Standard ? "Standard" : (s == Scoring::Vegas ? "Vegas" : "None");
}

const char *on_off(bool v)
{
    return v ? "On" : "Off";
}

const char *anim_name(int a)
{
    return a == 0 ? "On" : (a == 1 ? "Reduced" : "Off");
}

} // namespace

/* ------------------------------------------------------------------------------------------------ views */

void KlondikeApp::show_title()
{
    std::vector<std::string> path{"Klondike"};
    switch (view_) {
    case View::Menu: path.push_back("Menu"); break;
    case View::Settings: path.push_back("Settings"); break;
    case View::Stats: path.push_back("Statistics"); break;
    case View::Help: path.push_back("Help"); break;
    default: break;
    }
    page_->set_title(path);
}

void KlondikeApp::open_view(View v)
{
    if (view_ == v) return;
    stop_autoplay();
    clear_selection();
    clear_hint();
    if (view_ != View::Game) {
        list_.reset();
        text_.reset();
    } else {
        show_game_widgets(false);
    }
    view_ = v;
    cp0_ui_list_layout_t ll{};
    cp0_ui_list_page_layout(cp0_ui_metrics_get(), &layout_, CP0_UI_LIST_COMFORTABLE, &ll);
    ll.separator_px = 1;
    cp0::ui::CompactListEvents ev;
    ev.back = [this] { post([this] { back(); }); };
    switch (v) {
    case View::Menu:
        ev.activate = [this](int, std::intptr_t id) { post([this, id] { menu_activate(id); }); };
        break;
    case View::Settings:
        ev.activate = [this](int, std::intptr_t id) { post([this, id] { settings_activate(id); }); };
        break;
    case View::Stats:
        stat_draw_ = game().rules().draw;
        stat_passes_ = game().rules().passes;
        stat_scoring_ = static_cast<int>(game().rules().scoring);
        ev.activate = [this](int, std::intptr_t id) { post([this, id] { stats_activate(id); }); };
        break;
    default: break;
    }
    if (v == View::Help) {
        text_ = std::make_unique<cp0::ui::TextBlock>(page_->root(), layout_, help_text(), nullptr, [this] { post([this] { back(); }); });
        focus(text_->focus_object());
    } else {
        list_ = std::make_unique<cp0::ui::CompactList>(page_->root(), ll, std::move(ev));
        list_->reset_rows(v == View::Menu ? menu_rows() : (v == View::Settings ? settings_rows() : stats_rows()), v == View::Menu ? menu_sel_ : 0);
        focus(list_->focus_object());
    }
    show_title();
}

void KlondikeApp::close_view()
{
    if (view_ == View::Game) return;
    const View was = view_;
    list_.reset();
    text_.reset();
    view_ = View::Game;
    show_game_widgets(true);
    show_title();
    last_tick_ = lv_tick_get();
    focus(board_->surface());
    if (was == View::Settings) leave_settings();
    after_change(false);
}

void KlondikeApp::back()
{
    if (!alive_) return;
    if (win_ && win_->running()) {
        win_->skip();
        return;
    }
    if (dialog_) {
        close_dialog();
        return;
    }
    if (view_ == View::Settings || view_ == View::Stats || view_ == View::Help) {
        /* back to the menu when it was opened from there: the menu is one level up */
        const bool to_menu = from_menu_;
        from_menu_ = false;
        close_view();
        if (to_menu && !dialog_) open_view(View::Menu);
        return;
    }
    if (view_ == View::Menu) {
        close_view();
        return;
    }
    if (autoplay_) return;
    if (sel_.valid()) {
        clear_selection();
        show_cursor();
        return;
    }
    session_->save_game();
    cdk::request_quit(0);
}

/* ------------------------------------------------------------------------------------------------ rows */

std::vector<ListRow> KlondikeApp::menu_rows() const
{
    const bool done = session_->finished();
    auto row = [](const char *label, const std::string &value, std::intptr_t id, RowAffordance a, ValueTone t = ValueTone::Normal) {
        return ListRow::item(label, value, t, id, a);
    };
    std::vector<ListRow> r;
    r.push_back(row("New game", "", kActNew, RowAffordance::Action));
    r.push_back(row("New game by deal number", "", kActNumber, RowAffordance::Edit));
    r.push_back(row("Winnable deals", on_off(session_->prefs().winnable), kActWinnable, RowAffordance::Edit));
    r.push_back(row("Restart this deal", "", kActRestart, RowAffordance::Action));
    r.push_back(row("Undo", game().can_undo() && !done ? "" : "none", kActUndo, RowAffordance::Action, ValueTone::Muted));
    r.push_back(row("Redo", game().can_redo() && !done ? "" : "none", kActRedo, RowAffordance::Action, ValueTone::Muted));
    r.push_back(row("Hint", "", kActHint, RowAffordance::Action));
    r.push_back(row("Settings", "", kActSettings, RowAffordance::Open));
    r.push_back(row("Statistics", "", kActStats, RowAffordance::Open));
    r.push_back(row("Help and rules", "", kActHelp, RowAffordance::Open));
    return r;
}

std::vector<ListRow> KlondikeApp::settings_rows() const
{
    const Rules &next = session_->rules();
    const Rules &cur = game().rules();
    const UiPrefs &p = session_->prefs();
    auto edit = [](const char *label, const std::string &value, std::intptr_t id, bool pending = false) {
        return ListRow::item(label, value, pending ? ValueTone::Warn : ValueTone::Normal, id, RowAffordance::Edit);
    };
    std::vector<ListRow> r;
    r.push_back(ListRow::section(session_->in_progress() ? "Rules (from the next game)" : "Rules"));
    r.push_back(edit("Draw", next.draw == 3 ? "3 cards" : "1 card", kSetDraw, next.draw != cur.draw));
    r.push_back(edit("Stock passes", passes_name(next.passes), kSetPasses, next.passes != cur.passes));
    r.push_back(edit("Scoring", scoring_name(next.scoring), kSetScoring, next.scoring != cur.scoring));
    r.push_back(edit("Timed", on_off(next.timed), kSetTimed, next.timed != cur.timed));
    r.push_back(edit("Auto-move up", on_off(next.auto_foundation), kSetAuto, next.auto_foundation != cur.auto_foundation));
    r.push_back(edit("Foundation back", next.foundation_back ? "Allowed" : "Not allowed", kSetFback, next.foundation_back != cur.foundation_back));
    r.push_back(edit("Empty column", next.fill == TableauFill::KingOnly ? "King only" : "Any card", kSetFill, next.fill != cur.fill));
    r.push_back(ListRow::section("Look"));
    r.push_back(edit("Left-handed", on_off(p.left_handed), kSetLeft));
    r.push_back(edit("Four-colour", on_off(p.four_colour), kSetFour));
    r.push_back(edit("Felt", art::felt_name(p.felt), kSetFelt));
    r.push_back(edit("Card back", art::back_name(p.back), kSetBack));
    r.push_back(edit("Animation", anim_name(p.anim), kSetAnim));
    return r;
}

std::string KlondikeApp::stats_key() const
{
    Rules r;
    r.draw = stat_draw_;
    r.passes = stat_passes_;
    r.scoring = static_cast<Scoring>(stat_scoring_);
    return r.variation_key();
}

std::vector<ListRow> KlondikeApp::stats_rows() const
{
    const VariationStats v = session_->stats().get(stats_key());
    const Scoring sc = static_cast<Scoring>(stat_scoring_);
    auto info = [](const char *label, const std::string &value) {
        return ListRow::item(label, value, ValueTone::Normal, kStInfo, RowAffordance::Info);
    };
    auto edit = [](const char *label, const std::string &value, std::intptr_t id) {
        return ListRow::item(label, value, ValueTone::Normal, id, RowAffordance::Edit);
    };
    std::vector<ListRow> r;
    r.push_back(ListRow::section("Variation"));
    r.push_back(edit("Draw", stat_draw_ == 3 ? "3 cards" : "1 card", kStDraw));
    r.push_back(edit("Stock passes", passes_name(stat_passes_), kStPasses));
    r.push_back(edit("Scoring", scoring_name(sc), kStScoring));
    r.push_back(ListRow::section("Results"));
    r.push_back(info("Played", std::to_string(v.played)));
    r.push_back(info("Won", std::to_string(v.won)));
    r.push_back(info("Win rate", v.played ? std::to_string(v.won * 100 / v.played) + "%" : "-"));
    if (sc != Scoring::None) r.push_back(info("Best score", v.has_best_score ? std::to_string(v.best_score) : "-"));
    r.push_back(info("Best time", v.best_time_s ? mmss(v.best_time_s) : "-"));
    r.push_back(info("Fewest moves", v.fewest_moves ? std::to_string(v.fewest_moves) : "-"));
    r.push_back(info("Current streak", std::to_string(v.streak)));
    r.push_back(info("Best streak", std::to_string(v.best_streak)));
    r.push_back(info("Total time played", mmss(static_cast<std::uint32_t>(v.total_time_s))));
    if (sc == Scoring::Vegas) r.push_back(info("Vegas bank", std::to_string(v.vegas_bank)));
    r.push_back(ListRow::section("Reset"));
    r.push_back(ListRow::item("Reset this variation", "", ValueTone::Normal, kStResetOne, RowAffordance::Action));
    r.push_back(ListRow::item("Reset all statistics", "", ValueTone::Normal, kStResetAll, RowAffordance::Action));
    return r;
}

std::string KlondikeApp::help_text() const
{
    return "KLONDIKE\n"
           "Build the four foundations from Ace to King in each suit. In the seven columns, build down in alternating "
           "colours; a run of cards moves together. Only a King (or any card, if you chose that) goes on an empty column. "
           "Tap the stock to draw; when it is empty, tap it to go through the waste again.\n\n"
           "TOUCH\n"
           "Drag a card or a run to where it goes (a card dropped near a pile that takes it snaps there, any other drop "
           "flies back). Or tap a card, then tap where it goes. Double-tap a card to send it to its foundation. Undo and "
           "Redo are always on the bar; Hint shows a move; when every card is face up, Auto finishes the game.\n\n"
           "KEYS\n"
           "Arrows move the cursor. Enter or Space picks up the card under the cursor, then puts it down on the pile the "
           "cursor is on; Enter again on the picked card sends it to its foundation. On the stock it draws.\n"
           "U undo, R redo, H hint, N new game, A auto-finish, M or Ctrl+O the menu. Esc drops a pick, then leaves "
           "(the header's Esc button is the same).\n\n"
           "SCORING\n"
           "Standard: 5 for a card from the waste to a column, 10 to a foundation, 5 for turning a card over, -15 for a "
           "card back from a foundation, -100 for going through the stock again in draw 1; timed games lose 2 every 10 "
           "seconds and win a bonus. Vegas: the game costs 52, each foundation card pays 5; the bank carries over. None: "
           "no score.\n\n"
           "GAMES\n"
           "A game you leave after a move counts as a lost game in the statistics of its variation (draw, passes, scoring). "
           "Winnable deals are proven to be solvable by a solver on the deck. The game in progress is saved after every "
           "move and comes back when you open the app. Changed rules apply from the next game.";
}

/* ------------------------------------------------------------------------------------------------ activation */

void KlondikeApp::menu_activate(std::intptr_t id)
{
    if (view_ != View::Menu || dialog_) return;
    switch (id) {
    case kActNew:
        close_view();
        request_new_game();
        break;
    case kActNumber: open_number_dialog(); break;
    case kActWinnable: {
        session_->prefs().winnable = !session_->prefs().winnable;
        session_->save_prefs();
        if (list_) list_->set_rows(menu_rows());
        break;
    }
    case kActRestart:
        close_view();
        request_restart();
        break;
    case kActUndo:
        close_view();
        do_undo();
        break;
    case kActRedo:
        close_view();
        do_redo();
        break;
    case kActHint:
        close_view();
        do_hint();
        break;
    case kActSettings:
        menu_sel_ = list_ ? list_->selected() : 0;
        view_ = View::Game;
        list_.reset();
        from_menu_ = true;
        open_view(View::Settings);
        break;
    case kActStats:
        menu_sel_ = list_ ? list_->selected() : 0;
        view_ = View::Game;
        list_.reset();
        from_menu_ = true;
        open_view(View::Stats);
        break;
    case kActHelp:
        menu_sel_ = list_ ? list_->selected() : 0;
        view_ = View::Game;
        list_.reset();
        from_menu_ = true;
        open_view(View::Help);
        break;
    default: break;
    }
}

void KlondikeApp::settings_activate(std::intptr_t id)
{
    if (view_ != View::Settings || dialog_) return;
    Rules r = session_->rules();
    UiPrefs &p = session_->prefs();
    bool rules_changed = true, relayout = false, prefs_changed = false;
    switch (id) {
    case kSetDraw: r.draw = r.draw == 1 ? 3 : 1; break;
    case kSetPasses: r.passes = r.passes == 0 ? 3 : (r.passes == 3 ? 1 : 0); break;
    case kSetScoring:
        r.scoring = r.scoring == Scoring::Standard ? Scoring::Vegas : (r.scoring == Scoring::Vegas ? Scoring::None : Scoring::Standard);
        break;
    case kSetTimed: r.timed = !r.timed; break;
    case kSetAuto: r.auto_foundation = !r.auto_foundation; break;
    case kSetFback: r.foundation_back = !r.foundation_back; break;
    case kSetFill: r.fill = r.fill == TableauFill::KingOnly ? TableauFill::AnyCard : TableauFill::KingOnly; break;
    default: rules_changed = false; break;
    }
    switch (id) {
    case kSetLeft: p.left_handed = !p.left_handed; relayout = prefs_changed = true; break;
    case kSetFour: p.four_colour = !p.four_colour; prefs_changed = true; break;
    case kSetFelt: p.felt = (p.felt + 1) % art::kFelts; prefs_changed = true; break;
    case kSetBack: p.back = (p.back + 1) % art::kBacks; prefs_changed = true; break;
    case kSetAnim: p.anim = (p.anim + 1) % 3; prefs_changed = true; break;
    default: break;
    }
    if (rules_changed) session_->set_rules(r);
    if (prefs_changed) apply_prefs_change(relayout);
    if (list_) list_->set_rows(settings_rows());
}

/* leaving Settings: new rules never touch a game in progress */
void KlondikeApp::leave_settings()
{
    if (!session_->rules_pending()) return;
    if (!session_->in_progress()) {
        const std::uint32_t deal = game().deal_number(), user = game().user_number();
        const bool w = game().winnable();
        session_->begin_deal(deal, user, w);
        lay_ = compute_layout();
        board_->set_layout(lay_);
        apply_layout();
        after_change(false);
        notice("New rules are in force for this deal", 2200);
        return;
    }
    confirm("New rules", "They start with the next game. Start one now? The game in progress counts as a lost game.", "New game",
            [this] { start_new_game(session_->random_deal(), session_->prefs().winnable); }, "Later");
}

void KlondikeApp::stats_activate(std::intptr_t id)
{
    if (view_ != View::Stats || dialog_) return;
    switch (id) {
    case kStDraw: stat_draw_ = stat_draw_ == 1 ? 3 : 1; break;
    case kStPasses: stat_passes_ = stat_passes_ == 0 ? 3 : (stat_passes_ == 3 ? 1 : 0); break;
    case kStScoring: stat_scoring_ = (stat_scoring_ + 1) % 3; break;
    case kStResetOne: {
        const std::string key = stats_key();
        confirm("Reset this variation?", "Its results are removed for good.", "Reset", [this, key] {
            session_->reset_stats(key);
            if (list_) list_->set_rows(stats_rows());
        });
        return;
    }
    case kStResetAll:
        confirm("Reset all statistics?", "The results of every variation are removed for good.", "Reset all", [this] {
            session_->reset_all_stats();
            if (list_) list_->set_rows(stats_rows());
        });
        return;
    default: return;
    }
    if (list_) list_->set_rows(stats_rows());
}

/* ------------------------------------------------------------------------------------------------ dialogs */

void KlondikeApp::close_dialog()
{
    if (!dialog_) return;
    field_.reset();
    dialog_.reset();
    stop_win(); /* after the result box: the cascade's last picture goes, the table is put back */
    if (view_ == View::Game) {
        focus(board_->surface());
        last_tick_ = lv_tick_get();
    } else if (list_) {
        list_->set_hidden(false);
        focus(list_->focus_object());
    } else if (text_) {
        focus(text_->focus_object());
    }
}

void KlondikeApp::confirm(const std::string &title, const std::string &text, const std::string &ok, std::function<void()> on_ok,
                          const std::string &cancel)
{
    if (dialog_) return;
    cp0::ui::DialogSpec spec;
    spec.title = title;
    spec.page = page_.get();
    spec.buttons.push_back({cancel, cp0::ui::ButtonRole::Cancel, [this] { post([this] { close_dialog(); }); }});
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

void KlondikeApp::open_number_dialog()
{
    if (dialog_) return;
    if (list_) list_->set_hidden(true); /* nothing of the page may lie under the keyboard */
    cp0::ui::DialogSpec spec;
    spec.title = "Deal number";
    spec.page = page_.get();
    spec.message_line = true;
    spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] { post([this] { close_dialog(); }); }});
    spec.buttons.push_back({"Deal", cp0::ui::ButtonRole::Primary, [this] {
                                post([this] {
                                    if (!field_) return;
                                    const std::string t = field_->text();
                                    const unsigned long n = t.empty() ? 0 : std::strtoul(t.c_str(), nullptr, 10);
                                    if (n == 0 || n > 999999999ul) {
                                        if (dialog_) dialog_->set_message("Type a number from 1 to 999999999", true);
                                        return;
                                    }
                                    close_dialog();
                                    close_view();
                                    auto go = [this, n] { start_new_game(static_cast<std::uint32_t>(n), session_->prefs().winnable); };
                                    if (session_->in_progress())
                                        confirm("Start a new game?", "The game in progress counts as a lost game.", "New game", go);
                                    else go();
                                });
                            }});
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    cp0::ui::TextFieldSpec fs;
    fs.label = "Number";
    fs.type = cp0::ui::TextType::Number;
    fs.placeholder = "1 - 999999999";
    fs.max_bytes = 9;
    cp0::ui::TextFieldEvents fe;
    fe.submit = [this](const std::string &) { if (dialog_) dialog_->primary(); };
    fe.escape = [this] { post([this] { close_dialog(); }); };
    fe.back = [this] { post([this] { close_dialog(); }); };
    field_ = std::make_unique<cp0::ui::TextField>(dialog_->body(), dialog_->field_layout(), std::move(fs), std::move(fe));
    dialog_->relayout();
    focus(field_->focus_object());
}

} // namespace kui
