/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field: the screen. One cp0::ui::Page (title, status strip, the 9 mm Esc) and under it either the game or one of the list
 * pages (menu, difficulty, best times, statistics, settings). The board ALWAYS fits the screen: its size comes from the
 * viewport (core/view_layout.hpp), nothing is panned, zoomed or scrolled.
 *
 * Landscape (wider than tall): no strip; a dock column at the right edge holds the readouts (mines left, the clock) and the four
 * buttons New game, Dig / Flag, Hint, Menu; the board takes the rest. Portrait (X-Portrait = 480x640): a strip over the board
 * (mines left, the new-game button, the clock) and three buttons (Dig / Flag, Hint, Menu) along the bottom.
 *
 * Input: touch (tap, long press) and keys (arrows move a cursor ring, Enter or Space dig, F flag, C chord, H hint, T the tap
 * mode, N new game, M or Ctrl+O the menu, Esc leaves). The first dig lays the mines on a worker thread (a fair board can take
 * a moment on a slow board); the board is inert meanwhile. LVGL thread only.
 */
#pragma once

#include "board_view.hpp"
#include "session.hpp"

#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mfui {

constexpr int kMinW = 640, kMinH = 480;
constexpr int kPortraitW = 480, kPortraitH = 640;
constexpr const char *kVersion = "0.1.0";

class MineFieldApp;
/* what a button's click handler gets (the app and the button's number) */
struct ButtonCtx {
    MineFieldApp *app;
    int index;
};

class MineFieldApp {
public:
    explicit MineFieldApp(lv_obj_t *screen);
    ~MineFieldApp();
    MineFieldApp(const MineFieldApp &) = delete;
    MineFieldApp &operator=(const MineFieldApp &) = delete;

private:
    enum class View { Game, Menu, Difficulty, Best, Stats, Settings };
    enum Action {
        kNone = 0,
        kMenuResume, kMenuNew, kMenuDifficulty, kMenuBest, kMenuStats, kMenuSettings, kMenuVersion,
        kDiffBeginner, kDiffIntermediate, kDiffExpert,
        kSetFair, kSetAnim, kSetTimer, kSetFlagStart, kSetReset, kSetInfo,
        kInfo
    };
    struct Prep {
        minefield::GameConfig cfg;
        int cell = -1;
        std::atomic<bool> done{false};
        int delay_ms = 0;
        minefield::Game::Prepared result;
    };
    struct Rect {
        int x = 0, y = 0, w = 0, h = 0;
    };
    /* where everything but the board goes (compute_chrome) */
    struct Chrome {
        bool side = false;                 /* landscape: a dock column at the right, no strip */
        bool strip = false;                /* portrait: the strip over the board and the three-button dock under it */
        bool small_font = false;
        Rect readout[2];                   /* mines left, the clock */
        Rect btn[4];                       /* new game, Dig / Flag, Hint, Menu */
        lv_area_t viewport{0, 0, 0, 0};    /* the board's area */
        int thickness_px = 0;              /* the dock: its width (landscape) or height (portrait) */
        int strip_px = 0;
        int button_h_px = 0;
        int label_need_px = 0;             /* the width the labels need (landscape) */
        double ppmm = 8.5;
    };

    static void posted_cb(void *data);
    static void tick_cb(lv_timer_t *t);
    static void prep_cb(lv_timer_t *t);
    static void anim_cb(lv_timer_t *t);
    static void notice_cb(lv_timer_t *t);
    static void hint_cb(lv_timer_t *t);
    static void keyboard_cb(lv_event_t *e);
    static void surface_key_cb(lv_event_t *e);
    static void button_cb(lv_event_t *e);
    void post(std::function<void()> fn);
    void focus(lv_obj_t *obj);

    /* building and layout */
    void build_game_widgets();
    Chrome compute_chrome() const;
    void apply_layout();
    void dump_layout() const;
    void raise_chrome();
    void show_game_widgets(bool shown);
    void refresh_hud();
    void refresh_buttons();
    void notice(const std::string &text, int ms, const std::string &extra = std::string(), int avoid = -1);
    void clear_notice();
    void make_probes();

    /* the game */
    const minefield::Game &game() const { return session_->game(); }
    bool can_act() const;
    void after_change(bool animate);
    void dig(int cell, bool announce);
    void flag(int cell);
    void chord(int cell);
    void on_tap(int cell);
    void on_long_press(int cell);
    void start_prepare(int cell);
    void finish_prepare();
    void cancel_prepare();
    void do_hint();
    void clear_hint();
    void toggle_mode();
    void request_new_game();
    void start_new_game();
    void replay_same();
    void on_finish(bool animate);
    void anim_step();
    void skip_anim();
    void show_result();
    void close_dialog();
    void confirm(const std::string &title, const std::string &text, const std::string &ok, std::function<void()> on_ok,
                 const std::string &cancel = "Cancel");
    void run_setup(const std::string &ops);
    void load_test_map(const std::string &spec);
    int board_cells() const { return game().width() * game().height(); }

    /* keys */
    void on_key(uint32_t key);
    void on_keyboard(const struct key_item &item);
    void use_keys();
    void cursor_move(int dx, int dy);
    void cursor_activate();
    void show_cursor();

    /* pages */
    void back();
    void open_view(View v);
    void close_view();
    void show_title();
    std::vector<cp0::ui::ListRow> menu_rows() const;
    std::vector<cp0::ui::ListRow> difficulty_rows() const;
    std::vector<cp0::ui::ListRow> best_rows() const;
    std::vector<cp0::ui::ListRow> stats_rows() const;
    std::vector<cp0::ui::ListRow> settings_rows() const;
    std::string settings_hint(int index) const;
    void menu_activate(std::intptr_t id);
    void difficulty_activate(std::intptr_t id);
    void settings_activate(std::intptr_t id);
    void apply_difficulty(minefield::Preset p);
    void apply_pending_settings();
    void announce_pending();
    void drop_hint();

    lv_obj_t *screen_ = nullptr;
    cp0_ui_roller_page_t layout_{};
    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<minefield::Session> session_;
    std::unique_ptr<BoardView> board_;
    bool side_dock_ = true;

    lv_obj_t *readout_[2] = {};           /* mines left, the clock */
    lv_obj_t *btn_[4] = {};               /* new game, Dig / Flag, Hint, Menu */
    lv_obj_t *btn_label_[4] = {};
    lv_obj_t *banner_ = nullptr;
    lv_obj_t *banner_label_ = nullptr;
    lv_obj_t *banner_extra_ = nullptr;
    lv_obj_t *probes_ = nullptr;
    Chrome chrome_;
    Rect banner_room_;
    std::string mines_shown_, time_shown_;

    std::unique_ptr<cp0::ui::CompactList> list_;
    std::unique_ptr<cp0::ui::Dialog> dialog_;
    lv_obj_t *hint_ = nullptr;            /* the settings page's explanation line */
    View view_ = View::Game;
    View return_to_ = View::Game;         /* where Esc from a sub-page goes (the menu, or the game) */
    int menu_sel_ = 0;

    bool flag_mode_ = false;
    bool keys_used_ = false;
    int cursor_ = -1;
    int hint_cell_ = -1;
    bool finish_handled_ = false;
    bool result_shown_ = false;
    bool animating_ = false;
    int anim_kind_ = 0;                   /* 1 sweep (win), 2 mine reveal (loss) */
    int anim_tick_ = 0;
    std::vector<int> loss_order_;
    std::shared_ptr<Prep> prep_;
    bool preparing_ = false;
    bool pending_announced_ = false;
    bool supplied_ = false;               /* the next first dig has its layout already (same board, try again) */

    lv_timer_t *tick_timer_ = nullptr;
    lv_timer_t *prep_timer_ = nullptr;
    lv_timer_t *anim_timer_ = nullptr;
    lv_timer_t *notice_timer_ = nullptr;
    lv_timer_t *hint_timer_ = nullptr;
    std::uint32_t last_tick_ = 0;
    std::uint32_t last_save_ms_ = 0;
    int gen_delay_ms_ = 0;                /* test only */
    bool probes_on_ = false;              /* test only */

    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;
    bool alive_ = true;
};

} // namespace mfui
