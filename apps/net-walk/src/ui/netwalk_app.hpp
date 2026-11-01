/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk: the screen. The launcher's shared page header (cp0::ui::Page: the Esc button, the title "Net Walk" and the
 * status strip, as in every app) over one LVGL object, the body, that draws everything under it from a logical canvas
 * scaled by a whole number (core/layout.hpp): the Palm OS 4 colour look, the board and its devices (core/sprites.hpp), the
 * panel and the modal windows (Size, Scores, "Network up!"). The header's Esc button and the Esc key do the same: close
 * the open window, else end the app.
 *
 * Input. Touch: a tap on a tile turns it clockwise, a press of 0.35 s or more counter-clockwise, a hold of 0.6 s toggles
 * its padlock (a locked tile does not turn). Keys: arrows move the cursor, Enter or Space turn clockwise, C counter-
 * clockwise, L the padlock, N a new game, Esc closes a window or ends the app. LVGL thread only.
 */
#pragma once

#include "game.hpp"
#include "layout.hpp"
#include "painter.hpp"
#include "scores.hpp"
#include "sprites.hpp"

#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"
#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct key_item;

namespace nwui {

constexpr int kMinW = 480, kMinH = 320;
constexpr int kPortraitW = 480, kPortraitH = 640;
constexpr const char *kVersion = "0.1.0";

class NetWalkApp {
public:
    explicit NetWalkApp(lv_obj_t *screen);
    ~NetWalkApp();
    NetWalkApp(const NetWalkApp &) = delete;
    NetWalkApp &operator=(const NetWalkApp &) = delete;

private:
    enum class Mode { Play, Size, Scores, Flash, Win };
    enum Id {
        kNone = 0,
        kBtnNew, kBtnSize, kBtnScores,
        kSizeEasy, kSizeNormal, kSizeHard, kSizeExpert,
        kOk, kCancel
    };
    enum class Aff { None, Button, Action };
    struct Item {
        netwalk::Rect r;
        Id id = kNone;
        std::string label;
        Aff aff = Aff::Button;
    };

    static void event_cb(lv_event_t *e);
    static void key_cb(lv_event_t *e);
    static void keyboard_cb(lv_event_t *e);
    static void tick_cb(lv_timer_t *t);
    static void flash_cb(lv_timer_t *t);
    static void posted_cb(void *data);
    void post(std::function<void()> fn);

    /* layout, items */
    void relayout();
    void rebuild_items();
    void invalidate();
    void make_probes();
    Item *item_at(int lx, int ly);
    const Item *find_item(Id id) const;
    std::string new_label() const;

    /* drawing */
    void draw(lv_event_t *e);
    void draw_board(const Painter &p) const;
    void draw_tile(const Painter &p, int index) const;
    void draw_device(const Painter &p, const netwalk::Rect &tile, netwalk::SpriteKind kind, bool lit) const;
    void draw_panel(const Painter &p) const;
    void draw_button(const Painter &p, const netwalk::Rect &r, const std::string &label, bool pressed, bool tri, bool dflt) const;
    void draw_window(const Painter &p, const std::string &title) const;
    void draw_modal(const Painter &p) const;
    void draw_marker(const Painter &p, const Item &it, bool selected) const;

    /* game */
    void new_game(netwalk::Level level, bool pick_seed = true);
    void turn_tile(int i, bool cw);
    void lock_tile(int i);
    void after_move();
    void finish_flash();
    void activate(Id id);
    void open_mode(Mode m);
    void close_modal();
    void quit();
    void back();
    void run_setup(const std::string &ops);

    /* input */
    void on_pointer(lv_event_code_t code);
    void on_key(uint32_t key);
    void on_keyboard(const struct key_item &item);
    void use_keys();
    void cursor_move(int dx, int dy);
    void list_move(int d);

    lv_obj_t *screen_ = nullptr;
    cp0_ui_roller_page_t page_layout_{};
    std::unique_ptr<cp0::ui::Page> page_;
    lv_obj_t *surface_ = nullptr; /* the body: under the header, takes the pointer and the keys */
    lv_obj_t *probes_ = nullptr;
    int sw_ = 0, sh_ = 0;         /* the body, in pixels */
    netwalk::Layout layout_;
    netwalk::Game game_;
    netwalk::Scores scores_;
    netwalk::ScoreStore store_;
    netwalk::Level level_ = netwalk::Level::Normal;

    Mode mode_ = Mode::Play;
    std::vector<Item> items_;
    netwalk::Rect dlg_;           /* the modal window (logical) */
    std::string dlg_title_;
    std::vector<std::string> dlg_lines_;
    int sel_ = 0;                 /* the highlighted row of the size list (keys) */
    bool keys_used_ = false;
    int cursor_ = -1;
    bool flash_on_ = false;
    int flash_n_ = 0;
    int win_moves_ = 0, win_seconds_ = 0;
    netwalk::Record win_record_;

    /* the pointer */
    bool pressed_ = false, moved_ = false, fired_ = false;
    int press_lx_ = 0, press_ly_ = 0;
    std::uint32_t press_ms_ = 0;
    int press_tile_ = -1;
    Id press_item_ = kNone;

    lv_timer_t *tick_timer_ = nullptr;
    lv_timer_t *flash_timer_ = nullptr;
    std::uint32_t last_tick_ = 0;
    int shown_seconds_ = -1;
    bool probes_on_ = false;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;
    bool alive_ = true;
};

} // namespace nwui
