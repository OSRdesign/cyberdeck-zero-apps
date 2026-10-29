/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike UI: the table. 52 card objects and 13 pile slots are created once and only moved and restyled afterwards
 * (nothing is allocated per frame); each card draws itself (rounded face, index corner, pips or emblem, or the back) from
 * masks made once per card size (core/cardart.hpp). One surface under the cards takes the pointer: it tells a tap, a
 * double tap and a drag apart, picks the card under the finger from the layout (never from the objects, which may be
 * mid-animation) and moves the dragged cards itself, so a drag costs a few set_pos calls per pointer event.
 *
 *   sync(game, animate)   lays every card where the board says (animated by the animation preference); hidden cards (deep
 *                         in the stock, the foundations or the waste) sit at their pile's slot
 *   marks                 cursor (keyboard), selection (tap) and hint are frame objects over the cards
 *   drag                  a pick of a card or a run, the cards follow the pointer lifted (shadow), then either one of the
 *                         legal piles near the dropped card takes it (events.move) or the cards fly back
 *   win animation hooks   card_object(card), card_pos(card), set_card_visible(), set_input_enabled(), hide_marks(),
 *                         felt object: everything a win_animation module needs to take over the sprites
 * LVGL thread only.
 */
#pragma once

#include "cardart.hpp"
#include "game.hpp"
#include "view_layout.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kui {

struct Hit {
    int pile = -1;
    int idx = -1; /* card index in the pile (the board's order), -1 on an empty slot */
    bool valid() const { return pile >= 0; }
    bool operator==(const Hit &o) const { return pile == o.pile && idx == o.idx; }
    bool operator!=(const Hit &o) const { return !(*this == o); }
};

struct BoardEvents {
    std::function<bool(int from, int to, int count)> can_move;
    std::function<void(const klondike::Move &)> move; /* a drop on a legal pile */
    std::function<void(Hit)> tap;
    std::function<void(Hit)> double_tap;
    std::function<void()> drag_started;
};

struct BoardStyle {
    bool four_colour = false;
    int felt = 0;
    int back = 0;
    int anim = 0; /* 0 on, 1 reduced, 2 off */
};

class BoardView {
public:
    BoardView(lv_obj_t *page_root, const klondike::Layout &layout, BoardEvents events, const BoardStyle &style);
    ~BoardView();
    BoardView(const BoardView &) = delete;
    BoardView &operator=(const BoardView &) = delete;

    void set_layout(const klondike::Layout &layout);
    void set_style(const BoardStyle &style);
    void sync(const klondike::Game &game, bool animate);
    void set_hidden(bool hidden);
    void set_input_enabled(bool on) { input_ = on; }
    void finish_animations();

    /* a card (or run from it) may be picked up; count = how many cards that is */
    bool pickable(const Hit &h, int *count) const;
    /* what a tap on (x, y) of the screen would hit */
    Hit hit_test(int x, int y) const;
    /* the card (idx of the pile's order) as laid out now (not mid-animation) */
    klondike::Rect card_rect(int pile, int idx) const;
    /* the part of a tableau card that shows (the whole card for the top one) */
    klondike::Rect visible_rect(int pile, int idx) const;
    /* the rectangle of cards idx..top of a pile */
    klondike::Rect run_rect(int pile, int idx) const;

    void show_cursor(const Hit &h, bool run_from_cursor);
    void hide_cursor();
    void show_selection(int pile, int idx);
    void hide_selection();
    void show_hint(int src_pile, int src_idx, int dst_pile);
    void hide_hint();
    void hide_marks();

    lv_obj_t *surface() const { return surface_; }
    lv_obj_t *felt() const { return felt_; }
    lv_obj_t *card_object(klondike::Card c) const { return cards_[c].obj; }
    void card_pos(klondike::Card c, int *x, int *y) const;
    void set_card_visible(klondike::Card c, bool visible);
    void set_card_face(klondike::Card c, bool face_up);
    /* the animation preference as the style gave it: 0 on, 1 reduced, 2 off */
    int anim_mode() const { return style_.anim; }
    /* draws card c face up, as the table shows it, into dst (ARGB8888, card_w x card_h, made by the caller): the win
     * cascade stamps these into its trail. false when the art is not ready or dst does not fit. */
    bool render_card(klondike::Card c, lv_draw_buf_t *dst);
    /* the felt and the empty slots drawn or not: the cascade's opaque trail covers them, so they need not be drawn at all */
    void set_backdrop_visible(bool visible);
    const klondike::Layout &layout() const { return layout_; }
    const klondike::Game *game() const { return game_; }
    bool dragging() const { return drag_.active; }
    /* test hook: the geometry of every hit target as "name cx cy" lines on stderr */
    void dump_probes();
    /* test hook (KLONDIKE_TEST_ZCHECK): is every pile stacked in its own order, and are the flying and lifted cards on top?
     * false with a reason in *why otherwise. Also run after each sync / animation end / drop when the env var is set: a
     * violation is printed to stderr and kept as a "ZFAIL ..." label on the table (scenes: expect notext "ZFAIL"). */
    bool stacking_ok(std::string *why) const;
    void check_stacking(const char *where);
    /* test hook: a real drag of the run idx.. of a pile (begin, move onto the target pile, drop); to < 0: a drop on nothing */
    bool test_drag(int pile, int idx, int to);

private:
    struct CardCtx {
        BoardView *view = nullptr;
        int id = 0;
    };
    struct CardView {
        lv_obj_t *obj = nullptr;
        bool face_up = false;
        bool lifted = false;
        bool visible = false;
        int x = 0, y = 0;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0; /* animation */
        bool moving = false;
        CardCtx ctx;
    };
    struct Drag {
        bool active = false;
        bool pressed = false;
        Hit hit;
        int count = 0;
        int press_x = 0, press_y = 0;
        std::vector<int> ids;
        std::vector<int> ox, oy;
    };
    struct Art;

    static void surface_event_cb(lv_event_t *e);
    static void card_event_cb(lv_event_t *e);
    static void slot_event_cb(lv_event_t *e);
    static void anim_exec_cb(void *var, int32_t v);
    static void anim_done_cb(lv_anim_t *a);

    void restack();
    void build_art();
    void apply_felt();
    void place_slots();
    void on_pointer(lv_event_code_t code);
    void begin_drag(int px, int py);
    void move_drag(int px, int py);
    void end_drag(int px, int py, bool cancelled);
    void draw_card(lv_event_t *e, int id);
    void draw_card_into(lv_layer_t *layer, const lv_area_t &a, int id, bool face_up, bool lifted);
    void draw_slot(lv_event_t *e, int pile);
    void layout_pile_targets(std::vector<int> &tx, std::vector<int> &ty, std::vector<char> &vis, std::vector<char> &face) const;
    void animate_to(const std::vector<int> &tx, const std::vector<int> &ty, int duration);
    void set_frame(lv_obj_t *frame, const klondike::Rect &r, uint32_t rgb);
    klondike::Rect slot_rect(int pile) const { return layout_.pile_rect(pile); }
    int anim_ms() const { return style_.anim == 0 ? 190 : (style_.anim == 1 ? 80 : 0); }

    klondike::Layout layout_;
    BoardEvents events_;
    BoardStyle style_;
    const klondike::Game *game_ = nullptr;
    lv_obj_t *parent_ = nullptr;
    lv_obj_t *felt_ = nullptr;
    lv_obj_t *slots_[klondike::kPiles] = {};
    lv_obj_t *surface_ = nullptr;
    lv_obj_t *probes_ = nullptr;
    lv_obj_t *frame_cursor_ = nullptr;
    lv_obj_t *frame_select_ = nullptr;
    lv_obj_t *frame_hint_src_ = nullptr;
    lv_obj_t *frame_hint_dst_ = nullptr;
    CardView cards_[klondike::kCards];
    CardCtx slot_ctx_[klondike::kPiles];
    Drag drag_;
    std::unique_ptr<Art> art_;
    bool input_ = true;
    bool anim_running_ = false;
    std::uint32_t last_tap_ms_ = 0;
    Hit last_tap_;
    bool have_last_tap_ = false;
    int card_w_made_ = 0, back_made_ = -1;
    bool test_probes_ = false;
    bool test_zcheck_ = false;
    lv_obj_t *zfail_ = nullptr;
};

} // namespace kui
