/*
 * SPDX-License-Identifier: MIT
 *
 * The win cascade (see win_animation.hpp; the physics is core/win_sim.hpp).
 *
 * What is on the screen while it plays, bottom to top: the felt, the trail (one RGB565 canvas over the felt, filled with
 * the felt's colour, which the stamps are written into: no redraw of it, only blits and an invalidated rectangle per
 * stamp), the card objects of the table (the foundations still holding their cards; the flying cards are those very
 * objects, hidden as they leave), the flying cards (kWinMaxActive image objects showing the card's picture), then the app's
 * HUD and buttons. A card's picture (ARGB8888, what the image shows and what is stamped into the trail) is drawn once,
 * a few launches ahead, one per tick.
 *
 * Time: one timer (16 ms) turns the elapsed time into whole fixed steps of the simulation (33 ms each), at most
 * kMaxCatchUp per call; a slower board therefore plays the cascade slower, never in a burst, and the trail never has
 * gaps in a step it did run. Nothing is allocated per frame: the trail, the sprites and the card objects exist from
 * start() to stop().
 *
 * Memory: trail = width x (height - header) x 2 bytes (1280x800 landscape: 2.0 MB, 800x1280: 2.0 MB, 640x480: 0.6 MB,
 * 480x800: 0.7 MB), sprites 8 x card_w x card_h x 4 (about 0.2 MB at the largest card).
 *
 * Test hooks (env, off on a board): KLONDIKE_TEST_WINFRAME=N plays N steps at once and holds that picture (no timer
 * steps) for the scenes; KLONDIKE_TEST_WINSTATS=/abs/file appends the cost of the steps and of the refresh (1 = stderr) at the end;
 * with KLONDIKE_TEST_WINFORCE each frame is also painted at once and timed (the refresh the timer does then has nothing left).
 */
#include "win_animation.hpp"

#include "app.hpp"
#include "board_view.hpp"
#include "win_sim.hpp"

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

using namespace klondike;

namespace kui {

namespace {

constexpr int kTimerMs = 16;
constexpr int kMaxCatchUp = 4;

std::int64_t now_us()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

class Cascade : public WinAnimation {
public:
    ~Cascade() override { teardown(); }

    void start(BoardView &board, std::function<void()> done) override
    {
        teardown();
        done_ = std::move(done);
        board_ = &board;
        if (!setup(board)) {
            teardown(); /* keeps done_: the quick path below calls it */
            quick_ = true;
        }
        if (quick_) {
            timer_ = lv_timer_create(quick_cb, 40, this);
            lv_timer_set_repeat_count(timer_, 1);
            return;
        }
        state_ = State::Running;
        last_us_ = now_us();
        timer_ = lv_timer_create(tick_cb, kTimerMs, this);
        if (stats_) {
            if (lv_display_t *d = lv_display_get_default()) {
                lv_display_add_event_cb(d, refr_cb, LV_EVENT_REFR_START, this);
                lv_display_add_event_cb(d, refr_cb, LV_EVENT_REFR_READY, this);
                refr_hooked_ = true;
            }
        }
        if (pre_steps_ > 0) {
            begin_batch();
            for (int i = 0; i < pre_steps_ && !sim_.done(); ++i) step_once();
            place_flyers();
            end_batch();
            lv_obj_invalidate(canvas_);
            frozen_ = true;
        }
    }

    void stop() override
    {
        done_ = nullptr;
        teardown();
    }

    /* true until the skip has been carried out too: the keys of that moment are swallowed, not acted on */
    bool running() const override { return state_ == State::Running || state_ == State::Skipping; }

    void skip() override
    {
        if (state_ != State::Running) return;
        state_ = State::Skipping;
        lv_async_call(skip_cb, this);
    }

private:
    enum class State { Idle, Running, Skipping, Held };

    /* a card in flight: its picture (also what the trail is stamped with) shown by one image object, a single blit per
     * dirty area instead of the table card's ten draw calls */
    struct Sprite {
        lv_draw_buf_t *buf = nullptr;
        lv_obj_t *img = nullptr;
        Card card = kNoCard;
        int px = 0, py = 0;     /* where the image is now */
        bool placed = false;
        lv_area_t dirty{0, 0, -1, -1}; /* this frame's changes of this card (its old and new place, its stamps) */
    };

    static void join(lv_area_t &a, const lv_area_t &b)
    {
        if (a.x2 < a.x1) {
            a = b;
            return;
        }
        a.x1 = std::min(a.x1, b.x1);
        a.y1 = std::min(a.y1, b.y1);
        a.x2 = std::max(a.x2, b.x2);
        a.y2 = std::max(a.y2, b.y2);
    }
    void add_extra(int x, int y)
    {
        if (nextra_ < 24) extra_[nextra_++] = lv_area_t{x, y, x + card_w_ - 1, y + card_h_ - 1};
    }

    /* LVGL keeps 32 invalid areas per frame and then repaints the whole screen: 8 cards moving, stamped and shown or hidden
     * make about 45 single events. So all of a frame's changes are made with the invalidation off and then reported as one
     * rectangle per card (the union of its old and new place and its stamps) and one per launch or exit. */
    void begin_batch()
    {
        for (Sprite &s : sprite_) s.dirty = lv_area_t{0, 0, -1, -1};
        nextra_ = 0;
        if (disp_) lv_display_enable_invalidation(disp_, false);
    }
    void end_batch()
    {
        if (disp_) lv_display_enable_invalidation(disp_, true);
        for (Sprite &s : sprite_)
            if (s.dirty.x2 >= s.dirty.x1) lv_obj_invalidate_area(canvas_, &s.dirty);
        for (int i = 0; i < nextra_; ++i) lv_obj_invalidate_area(canvas_, &extra_[i]);
    }

    /* ----------------------------------------------------------------------------------------------- setup */

    bool setup(BoardView &board)
    {
        const Game *g = board.game();
        const Layout &l = board.layout();
        if (board.anim_mode() == 2 || !g || l.card_w <= 0 || !board.felt()) return false;
        board.finish_animations();
        board.hide_marks();

        WinParams p;
        p.x0 = l.board.x;
        p.x1 = l.board.right();
        p.top = l.board.y;
        p.floor = l.board.bottom();
        p.card_w = l.card_w;
        p.card_h = l.card_h;
        p.seed = g->deal_number();
        p.reduced = board.anim_mode() == 1;
        const Board &b = g->board();
        std::memset(below_, 0xFF, sizeof below_);
        int n = 0;
        for (int rank = kRanks; rank >= 1; --rank) {
            for (int f = 0; f < kFoundations; ++f) {
                const int pile = foundation_pile(f);
                if (b.size(pile) < rank) continue;
                const Card c = b.card(pile, rank - 1);
                p.launch[n].card = c;
                p.launch[n].x = l.pile_x(pile);
                p.launch[n].y = l.pile_y(pile);
                if (rank > 1) below_[c] = b.card(pile, rank - 2);
                ++n;
            }
        }
        p.count = n;
        if (n == 0) return false;

        lv_obj_t *felt = board.felt();
        parent_ = lv_obj_get_parent(felt);
        cx_ = lv_obj_get_x(felt);
        cy_ = lv_obj_get_y(felt);
        const int tw = lv_obj_get_width(felt), th = lv_obj_get_height(felt);
        if (tw <= 0 || th <= 0) return false;
        trail_ = lv_draw_buf_create(static_cast<uint32_t>(tw), static_cast<uint32_t>(th), LV_COLOR_FORMAT_RGB565, 0);
        if (!trail_) return false;
        std::fill(face_, face_ + kCards, nullptr);
        norder_ = n;
        for (int i = 0; i < n; ++i) order_[i] = p.launch[i].card;
        next_render_ = 0;
        /* (the felt colour is read before the felt is hidden below) */
        const std::uint16_t felt565 = lv_color_to_u16(lv_obj_get_style_bg_color(felt, LV_PART_MAIN));
        for (int y = 0; y < th; ++y) {
            auto *row = reinterpret_cast<std::uint16_t *>(static_cast<std::uint8_t *>(trail_->data) + static_cast<size_t>(y) * trail_->header.stride);
            std::fill(row, row + tw, felt565);
        }

        canvas_ = lv_canvas_create(parent_);
        lv_canvas_set_draw_buf(canvas_, trail_);
        lv_obj_set_pos(canvas_, cx_, cy_);
        lv_obj_set_size(canvas_, tw, th);
        lv_obj_remove_flag(canvas_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE));
        lv_obj_add_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(canvas_, press_cb, LV_EVENT_PRESSED, this);
        /* under every card object, over the felt, the pointer surface and the empty slots */
        int low = lv_obj_get_child_count(parent_);
        for (int id = 0; id < kCards; ++id) low = std::min<int>(low, lv_obj_get_index(board.card_object(static_cast<Card>(id))));
        lv_obj_move_to_index(canvas_, low);
        board.set_backdrop_visible(false);
        int top = 0;
        for (int id = 0; id < kCards; ++id) top = std::max<int>(top, lv_obj_get_index(board.card_object(static_cast<Card>(id))));
        int k = 0;
        for (Sprite &s : sprite_) {
            s.img = lv_image_create(parent_);
            lv_obj_remove_flag(s.img, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(s.img, l.card_w, l.card_h);
            lv_obj_move_to_index(s.img, top + 1 + k++); /* above the table's cards, under the HUD and buttons */
        }

        card_w_ = l.card_w;
        card_h_ = l.card_h;
        disp_ = lv_obj_get_display(parent_);
        clip_x0_ = p.x0;
        clip_x1_ = p.x1;
        clip_y0_ = p.top;
        sim_.init(p);
        for (int i = 0; i < 3; ++i) render_ahead();
        std::fill(slot_of_, slot_of_ + kCards, -1);
        state_ = State::Running;
        stats_ = env_set("KLONDIKE_TEST_WINSTATS");
        if (const char *f = std::getenv("KLONDIKE_TEST_WINFRAME")) pre_steps_ = std::atoi(f);
        return true;
    }

    static bool env_set(const char *name)
    {
        const char *v = std::getenv(name);
        return v && *v && std::strcmp(v, "0") != 0;
    }

    void teardown()
    {
        if (timer_) lv_timer_delete(timer_);
        timer_ = nullptr;
        lv_async_call_cancel(skip_cb, this);
        if (refr_hooked_) {
            if (lv_display_t *d = lv_display_get_default()) lv_display_remove_event_cb_with_user_data(d, refr_cb, this);
            refr_hooked_ = false;
        }
        const bool had_canvas = canvas_ != nullptr;
        if (canvas_ && lv_obj_is_valid(canvas_)) {
            lv_obj_remove_event_cb(canvas_, press_cb);
            lv_obj_delete(canvas_);
        }
        canvas_ = nullptr;
        if (trail_) lv_draw_buf_destroy(trail_);
        trail_ = nullptr;
        for (Sprite &s : sprite_) {
            if (s.img && lv_obj_is_valid(s.img)) lv_obj_delete(s.img);
            s = Sprite();
        }
        for (lv_draw_buf_t *&f : face_)
            if (f) {
                lv_image_cache_drop(f);
                lv_draw_buf_destroy(f);
                f = nullptr;
            }
        if (had_canvas && board_ && parent_ && lv_obj_is_valid(parent_)) board_->set_backdrop_visible(true);
        /* the cards go back where the game says (they were hidden, one by one, as they left) */
        if (had_canvas && board_ && board_->game() && parent_ && lv_obj_is_valid(parent_)) board_->sync(*board_->game(), false);
        board_ = nullptr;
        parent_ = nullptr;
        disp_ = nullptr;
        state_ = State::Idle;
        quick_ = false;
        frozen_ = false;
        pre_steps_ = 0;
    }

    /* ----------------------------------------------------------------------------------------------- one step */

    /* A card's picture: drawn once, ahead of its launch (one per tick, so that no frame pays for more than one: the
     * cascade's whole set of 52 is 52 x card_w x card_h x 4 bytes, 1.2 MB at 480x800), kept to the end. */
    lv_draw_buf_t *ensure_face(Card c)
    {
        if (face_[c]) return face_[c];
        const Layout &l = board_->layout();
        lv_draw_buf_t *b = lv_draw_buf_create(static_cast<uint32_t>(l.card_w), static_cast<uint32_t>(l.card_h), LV_COLOR_FORMAT_ARGB8888, 0);
        if (!b) return nullptr;
        if (!board_->render_card(c, b)) {
            lv_draw_buf_destroy(b);
            return nullptr;
        }
        face_[c] = b;
        return b;
    }

    void render_ahead()
    {
        while (next_render_ < norder_) {
            const Card c = order_[next_render_++];
            if (face_[c]) continue;
            ensure_face(c);
            break;
        }
    }

    void begin_card(Card c)
    {
        int slot = -1;
        for (int i = 0; i < kWinMaxActive; ++i)
            if (sprite_[i].card == kNoCard) {
                slot = i;
                break;
            }
        if (slot < 0) return; /* cannot happen: the sim never has more than kWinMaxActive in flight */
        sprite_[slot].card = c;
        slot_of_[c] = slot;
        ensure_face(c);
        sprite_[slot].buf = face_[c];
        if (face_[c]) lv_image_set_src(sprite_[slot].img, face_[c]);
        lv_obj_remove_flag(sprite_[slot].img, LV_OBJ_FLAG_HIDDEN);
        sprite_[slot].placed = false;
        const Layout &l = board_->layout();
        int lx = 0, ly = 0;
        board_->card_pos(c, &lx, &ly);
        add_extra(lx, ly); /* the table's card leaves its place ... */
        if (below_[c] != kNoCard) add_extra(lx, ly); /* ... and the one under it shows */
        (void)l;
        board_->set_card_visible(c, false); /* the table's own card leaves its place, the image takes over */
        if (below_[c] != kNoCard) board_->set_card_visible(below_[c], true); /* the next card of that foundation shows */
    }

    void end_card(Card c)
    {
        const int slot = slot_of_[c];
        if (slot >= 0) {
            sprite_[slot].card = kNoCard;
            if (sprite_[slot].placed) add_extra(sprite_[slot].px, sprite_[slot].py);
            lv_obj_add_flag(sprite_[slot].img, LV_OBJ_FLAG_HIDDEN);
        }
        slot_of_[c] = -1;
    }

    void stamp(const Flyer &f)
    {
        const int slot = slot_of_[f.card];
        if (slot < 0) return;
        const lv_draw_buf_t *spr = sprite_[slot].buf;
        if (!spr) return;
        const int sw = static_cast<int>(spr->header.w), sh = static_cast<int>(spr->header.h);
        const int tw = static_cast<int>(trail_->header.w), th = static_cast<int>(trail_->header.h);
        const int x = f.x - cx_, y = f.y - cy_;
        /* only the play area takes the trail: the dock and the HUD stay clean */
        const int x0 = std::max(std::max(0, x), clip_x0_ - cx_), x1 = std::min(std::min(tw, x + sw), clip_x1_ - cx_);
        const int y0 = std::max(std::max(0, y), clip_y0_ - cy_), y1 = std::min(th, y + sh);
        if (x0 >= x1 || y0 >= y1) return;
        for (int yy = y0; yy < y1; ++yy) {
            const auto *s = reinterpret_cast<const lv_color32_t *>(static_cast<const std::uint8_t *>(spr->data) + static_cast<size_t>(yy - y) * spr->header.stride) + (x0 - x);
            auto *d = reinterpret_cast<std::uint16_t *>(static_cast<std::uint8_t *>(trail_->data) + static_cast<size_t>(yy) * trail_->header.stride) + x0;
            for (int xx = x0; xx < x1; ++xx, ++s, ++d) {
                const unsigned a = s->alpha;
                if (a == 0) continue;
                unsigned r = s->red, gr = s->green, bl = s->blue;
                if (a != 255) {
                    const unsigned v = *d;
                    const unsigned dr = (v >> 8) & 0xF8, dg = (v >> 3) & 0xFC, db = (v << 3) & 0xF8;
                    r = (r * a + dr * (255 - a)) / 255;
                    gr = (gr * a + dg * (255 - a)) / 255;
                    bl = (bl * a + db * (255 - a)) / 255;
                }
                *d = static_cast<std::uint16_t>(((r & 0xF8) << 8) | ((gr & 0xFC) << 3) | (bl >> 3));
            }
        }
        join(sprite_[slot].dirty, lv_area_t{cx_ + x0, cy_ + y0, cx_ + x1 - 1, cy_ + y1 - 1});
    }

    void step_once()
    {
        sim_.step();
        for (int i = 0; i < sim_.active(); ++i) {
            const Flyer &f = sim_.flyer(i);
            if (f.fresh) begin_card(f.card);
            if (f.stamp) stamp(f);
        }
        for (int i = 0; i < sim_.left_count(); ++i) end_card(sim_.left_card(i));
    }

    void place_flyers()
    {
        for (int i = 0; i < sim_.active(); ++i) {
            const Flyer &f = sim_.flyer(i);
            const int slot = slot_of_[f.card];
            if (slot < 0) continue;
            Sprite &sp = sprite_[slot];
            if (sp.placed) join(sp.dirty, lv_area_t{sp.px, sp.py, sp.px + card_w_ - 1, sp.py + card_h_ - 1});
            sp.px = f.x;
            sp.py = f.y;
            sp.placed = true;
            join(sp.dirty, lv_area_t{f.x, f.y, f.x + card_w_ - 1, f.y + card_h_ - 1});
            lv_obj_set_pos(sp.img, f.x, f.y);
        }
    }

    /* ----------------------------------------------------------------------------------------------- callbacks */

    static void tick_cb(lv_timer_t *t) { static_cast<Cascade *>(lv_timer_get_user_data(t))->tick(); }

    void tick()
    {
        if (state_ != State::Running) return;
        if (cdk::hidden()) {
            /* nothing is drawn while hidden: wait slowly, and do not run the time away */
            if (!was_hidden_) lv_timer_set_period(timer_, 250);
            was_hidden_ = true;
            last_us_ = now_us();
            acc_us_ = 0;
            return;
        }
        if (was_hidden_) {
            was_hidden_ = false;
            lv_timer_set_period(timer_, kTimerMs);
            last_us_ = now_us();
            acc_us_ = 0;
            return;
        }
        if (frozen_) {
            if (sim_.done()) finish();
            return;
        }
        const std::int64_t t0 = now_us();
        std::int64_t dt = t0 - last_us_;
        last_us_ = t0;
        if (dt > 500000) dt = static_cast<std::int64_t>(sim_.step_ms()) * 1000; /* a long gap (the process was frozen): not played */
        acc_us_ += dt;
        const std::int64_t step_us = static_cast<std::int64_t>(sim_.step_ms()) * 1000;
        int steps = static_cast<int>(acc_us_ / step_us);
        acc_us_ -= static_cast<std::int64_t>(steps) * step_us;
        if (steps > kMaxCatchUp) {
            steps = kMaxCatchUp; /* too slow: the cascade plays slower, the lost time is not made up */
            acc_us_ = 0;
        }
        begin_batch();
        for (int i = 0; i < steps && !sim_.done(); ++i) step_once();
        if (steps > 0) {
            place_flyers();
            ++frames_;
            steps_ += steps;
            if (steps > 1) skipped_ += steps - 1;
        }
        end_batch();
        render_ahead();
        if (stats_) {
            if (std::getenv("KLONDIKE_TEST_WINFORCE") && disp_) { /* paint now, to time the paint of exactly this frame's changes */
                const std::int64_t r0 = now_us();
                lv_refr_now(disp_);
                forced_us_ += now_us() - r0;
            }
        }
        const std::int64_t cost = now_us() - t0;
        tick_us_ += cost;
        tick_max_us_ = std::max(tick_max_us_, cost);
        if (sim_.done()) finish();
    }

    /* the last card has left: keep the picture, hand over to the result dialog */
    void finish()
    {
        if (timer_) lv_timer_delete(timer_);
        timer_ = nullptr;
        state_ = State::Held;
        if (stats_) print_stats();
        auto done = std::move(done_);
        done_ = nullptr;
        if (done) done();
    }

    static void skip_cb(void *p)
    {
        auto *self = static_cast<Cascade *>(p);
        if (self->state_ != State::Skipping) return;
        auto done = std::move(self->done_);
        self->done_ = nullptr;
        self->teardown();
        if (done) done();
    }

    static void quick_cb(lv_timer_t *t)
    {
        auto *self = static_cast<Cascade *>(lv_timer_get_user_data(t));
        self->timer_ = nullptr; /* a repeat-count-1 timer deletes itself */
        auto done = std::move(self->done_);
        self->done_ = nullptr;
        self->quick_ = false;
        if (done) done();
    }

    static void press_cb(lv_event_t *e) { static_cast<Cascade *>(lv_event_get_user_data(e))->skip(); }

    static void refr_cb(lv_event_t *e)
    {
        auto *self = static_cast<Cascade *>(lv_event_get_user_data(e));
        if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
            self->refr_start_us_ = now_us();
        } else if (self->refr_start_us_ > 0) {
            const std::int64_t c = now_us() - self->refr_start_us_;
            self->refr_start_us_ = 0;
            if (self->state_ != State::Running) return;
            ++self->refr_n_;
            self->refr_us_ += c;
            self->refr_max_us_ = std::max(self->refr_max_us_, c);
        }
    }

    void print_stats()
    {
        const char *path = std::getenv("KLONDIKE_TEST_WINSTATS");
        FILE *out = path && path[0] == '/' ? std::fopen(path, "a") : stderr;
        if (!out) out = stderr;
        std::fprintf(out,
                     "[winstats] %dx%d trail %.2f MB, steps %d in %d ticks (%d steps lost to catch-up cap), step+stamp avg %.0f us max %lld us, "
                     "refresh avg %.0f us max %lld us over %d refreshes, paint of the frame itself (WINFORCE) avg %.0f us\n",
                     trail_ ? static_cast<int>(trail_->header.w) : 0, trail_ ? static_cast<int>(trail_->header.h) : 0,
                     trail_ ? static_cast<double>(trail_->data_size) / 1048576.0 : 0.0, sim_.step_count(), frames_, skipped_,
                     frames_ ? static_cast<double>(tick_us_) / frames_ : 0.0, static_cast<long long>(tick_max_us_),
                     refr_n_ ? static_cast<double>(refr_us_) / refr_n_ : 0.0, static_cast<long long>(refr_max_us_), refr_n_,
                     frames_ ? static_cast<double>(forced_us_) / frames_ : 0.0);
        if (out != stderr) std::fclose(out);
    }

    BoardView *board_ = nullptr;
    lv_obj_t *parent_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    lv_draw_buf_t *trail_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::function<void()> done_;
    State state_ = State::Idle;
    bool quick_ = false, frozen_ = false, was_hidden_ = false, stats_ = false, refr_hooked_ = false;
    int pre_steps_ = 0;
    lv_display_t *disp_ = nullptr;
    int card_w_ = 0, card_h_ = 0, nextra_ = 0;
    lv_area_t extra_[24];
    int cx_ = 0, cy_ = 0, clip_x0_ = 0, clip_x1_ = 0, clip_y0_ = 0;
    WinSim sim_;
    Sprite sprite_[kWinMaxActive];
    lv_draw_buf_t *face_[kCards] = {};
    Card order_[kCards] = {};
    int norder_ = 0, next_render_ = 0;
    int slot_of_[kCards] = {};
    Card below_[kCards] = {};
    std::int64_t last_us_ = 0, acc_us_ = 0;
    int frames_ = 0, steps_ = 0, skipped_ = 0;
    std::int64_t tick_us_ = 0, tick_max_us_ = 0, refr_start_us_ = 0, refr_us_ = 0, refr_max_us_ = 0;
    int refr_n_ = 0;
    std::int64_t forced_us_ = 0;
};

} // namespace

std::unique_ptr<WinAnimation> make_win_animation()
{
    return std::make_unique<Cascade>();
}

} // namespace kui
