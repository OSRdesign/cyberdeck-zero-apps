/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike Solitaire: the screen. One cp0::ui::Page (title, status strip, the 9 mm Esc) and, under it, either the table
 * (BoardView + HUD line + button dock + a notice banner) or one of the list / text views (menu, settings, statistics,
 * help). All sizes come from the layout service and core/view_layout.hpp; the game itself is core/session.hpp.
 *
 * Input: touch (tap the stock to draw, drag a card or a run, tap a card then its destination, double tap = to the
 * foundation) and keys (arrows move a cursor, Enter / Space pick and place or draw, U undo, R redo, H hint, N new game,
 * A auto-complete, M or Ctrl+O the menu, Esc cancels a selection, then leaves / quits).
 * LVGL thread only.
 */
#pragma once

#include "board_view.hpp"
#include "session.hpp"
#include "win_animation.hpp"

#include "async_run.hpp"
#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kui {

class KlondikeApp {
public:
    explicit KlondikeApp(lv_obj_t *screen);
    ~KlondikeApp();
    KlondikeApp(const KlondikeApp &) = delete;
    KlondikeApp &operator=(const KlondikeApp &) = delete;

private:
    enum class View { Game, Menu, Settings, Stats, Help };
    enum Action {
        kActNone = 0, kActBack, kActNew, kActNumber, kActWinnable, kActRestart, kActUndo, kActRedo, kActHint, kActSettings,
        kActStats, kActHelp,
        kSetDraw, kSetPasses, kSetScoring, kSetTimed, kSetAuto, kSetFback, kSetFill, kSetLeft, kSetFour, kSetFelt, kSetBack,
        kSetAnim,
        kStDraw, kStPasses, kStScoring, kStResetOne, kStResetAll, kStInfo
    };

    static void posted_cb(void *data);
    static void tick_cb(lv_timer_t *t);
    static void keyboard_cb(lv_event_t *e);
    static void surface_key_cb(lv_event_t *e);
    static void button_cb(lv_event_t *e);
    static void banner_timer_cb(lv_timer_t *t);
    static void hint_timer_cb(lv_timer_t *t);
    static void deadend_timer_cb(lv_timer_t *t);
    static void autoplay_cb(lv_timer_t *t);
    static void zfuzz_cb(lv_timer_t *t);
    void zfuzz_step();
    void post(std::function<void()> fn);
    void focus(lv_obj_t *obj);

    /* building */
    void build_game_widgets();
    klondike::Layout compute_layout() const;
    void raise_chrome();
    void stop_win();
    void set_dock_backing(bool on);
    void apply_layout();
    void show_game_widgets(bool shown);
    BoardStyle board_style() const;

    /* the game */
    const klondike::Game &game() const { return session_->game(); }
    void after_change(bool animate);
    void user_move(const klondike::Move &m);
    void draw_from_stock();
    void on_tap(const Hit &h);
    void on_double_tap(const Hit &h);
    void select(const Hit &h);
    void clear_selection();
    void do_undo();
    void do_redo();
    void do_hint();
    void do_auto_complete();
    bool auto_plan(std::vector<klondike::Move> &plan) const;
    void autoplay_step();
    void stop_autoplay();
    void request_new_game();
    void start_new_game(std::uint32_t user, bool want_winnable);
    void search_winnable(std::uint32_t user);
    void request_restart();
    void on_win();
    void show_win_dialog();
    void clear_hint();
    void notice(const std::string &text, int ms);
    void clear_banner();
    void schedule_deadend();
    void run_deadend();
    void cancel_jobs();

    /* HUD, dock */
    void refresh_hud();
    void refresh_buttons();
    void fit_button_fonts();
    void check_labels();
    std::vector<std::string> hud_items(bool with_deal) const;
    void button_action(int index);

    /* keys */
    void on_key(uint32_t key);
    void on_keyboard(const struct key_item &item);
    void cursor_fix();
    void cursor_move(int dx, int dy);
    void cursor_activate();
    void show_cursor();
    void use_keys();

    /* pages and dialogs */
    void back();
    void open_view(View v);
    void close_view();
    void show_title();
    void menu_activate(std::intptr_t id);
    void settings_activate(std::intptr_t id);
    void stats_activate(std::intptr_t id);
    std::vector<cp0::ui::ListRow> menu_rows() const;
    std::vector<cp0::ui::ListRow> settings_rows() const;
    std::vector<cp0::ui::ListRow> stats_rows() const;
    std::string help_text() const;
    void leave_settings();
    void confirm(const std::string &title, const std::string &text, const std::string &ok, std::function<void()> on_ok,
                 const std::string &cancel = "Cancel");
    void open_number_dialog();
    void close_dialog();
    void apply_prefs_change(bool relayout);
    std::string stats_key() const;

    lv_obj_t *screen_ = nullptr;
    cp0_ui_roller_page_t layout_{};
    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<klondike::Session> session_;
    std::unique_ptr<BoardView> board_;
    std::unique_ptr<WinAnimation> win_;
    klondike::Layout lay_;
    int hud_h_ = 20;

    lv_obj_t *hud_ = nullptr;
    lv_obj_t *hud_item_[5] = {};
    lv_obj_t *dock_ = nullptr;
    lv_obj_t *btn_[klondike::kDockButtons] = {};
    lv_obj_t *btn_label_[klondike::kDockButtons] = {};
    lv_obj_t *clip_mark_ = nullptr; /* test only */
    lv_obj_t *banner_ = nullptr;
    lv_obj_t *banner_label_ = nullptr;
    std::string hud_shown_;

    std::unique_ptr<cp0::ui::CompactList> list_;
    std::unique_ptr<cp0::ui::TextBlock> text_;
    std::unique_ptr<cp0::ui::Dialog> dialog_;
    std::unique_ptr<cp0::ui::TextField> field_;
    View view_ = View::Game;
    int stat_draw_ = 1, stat_passes_ = 0, stat_scoring_ = 0;

    Hit cur_;
    Hit sel_;
    bool keys_used_ = false;
    bool won_shown_ = false;
    bool auto_available_ = false;
    std::vector<klondike::Move> auto_plan_;
    lv_timer_t *autoplay_ = nullptr;
    size_t auto_step_ = 0;

    lv_timer_t *tick_timer_ = nullptr;
    lv_timer_t *banner_timer_ = nullptr;
    lv_timer_t *hint_timer_ = nullptr;
    lv_timer_t *deadend_timer_ = nullptr;
    lv_timer_t *zfuzz_timer_ = nullptr; /* test only: KLONDIKE_TEST_ZFUZZ */
    std::uint32_t zfuzz_rng_ = 1;
    int zfuzz_left_ = 0;
    std::uint32_t last_tick_ = 0;
    std::uint32_t last_save_ms_ = 0;
    bool was_hidden_ = false;
    bool dead_banner_ = false;
    std::uint32_t version_ = 0;
    cdk::async_run::JobId deadend_job_ = 0;
    cdk::async_run::JobId winnable_job_ = 0;
    std::uint32_t winnable_user_ = 0;

    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;
    bool alive_ = true;
    bool from_menu_ = false;
    int menu_sel_ = 0;
    std::string exe_;
};

} // namespace kui
