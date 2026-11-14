/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl queue: the tracks to play (a folder's tracks or a playlist's entries, report 059 P1), the play order (linear, or
 * shuffled with the current track first), repeat (off / all / one), and the editing the queue page offers without any
 * dragging (owner rule): move up, move down, remove, clear.
 *
 * Positions are indices into the PLAY ORDER (what the queue page lists and "3 OF 20" counts); items() keeps the tracks as
 * they were added. With shuffle off the two are the same. LVGL-free, no locking (the controller's thread only).
 */
#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace vinyl {

enum class Repeat { Off, All, One };
const char *repeat_name(Repeat r); /* "off", "all", "one" */
Repeat repeat_from(const std::string &s);

class Queue {
public:
    /* New contents; `start` is an index into `paths` (-1: none current). Shuffle stays as it was (a new order). */
    void set(std::vector<std::string> paths, int start, const std::string &source);
    void clear();

    int size() const { return static_cast<int>(items_.size()); }
    bool empty() const { return items_.empty(); }
    const std::string &source() const { return source_; }

    /* the track at play-order position `pos` */
    const std::string &at(int pos) const;
    /* the current play-order position, -1 when none */
    int current() const { return cur_; }
    const std::string *current_path() const;
    void set_current(int pos);

    bool shuffle() const { return shuffle_; }
    void set_shuffle(bool on);
    Repeat repeat() const { return repeat_; }
    void set_repeat(Repeat r) { repeat_ = r; }
    Repeat cycle_repeat(); /* off -> all -> one -> off */

    /* The position after the current one: `user` true for the Next key (repeat one moves on), false at the end of a track
     * (repeat one plays it again). -1 at the end of the queue (repeat off). */
    int next_pos(bool user) const { return next_after(cur_, user); }
    /* The same from another position (the engine's decoder may already be one track ahead of what is heard). */
    int next_after(int pos, bool user) const;
    /* The position the Previous key goes to when the track is near its start (-1: none; the caller restarts). */
    int prev_pos() const;

    /* Editing by play-order position. false when the move is not possible. */
    bool move_up(int pos);
    bool move_down(int pos);
    /* Removes the track; the current position follows (the current track itself may be removed: then current() becomes
     * the track that took its place, or -1). */
    void remove(int pos);
    /* The tracks that lie under any of `dirs` (a drive that went away, owner ruling 2026-11-14): all of them are removed at once.
     * The current track stays current when it survives; when it was one of them the track that took its place becomes current
     * (the first one when none follows), -1 when the queue is empty. */
    struct RemoveResult {
        int removed = 0;
        bool current_removed = false;
    };
    RemoveResult remove_under(const std::vector<std::string> &dirs);
    /* Removes every track but the current one (all of them when none is current). */
    void clear_except_current();

    /* The Library's track actions (P2b): "Play next" puts the tracks right after the current one (first when none is
     * current), "Add to queue" at the end of the play order. With shuffle on they go into the play order at that place,
     * in their own order (not shuffled in), and into the list order after the current track (or at its end), so turning
     * shuffle off keeps them there. The current track stays current and the source stays. Returns the play-order
     * position of the first track added (-1 when `paths` is empty). */
    int insert_next(const std::vector<std::string> &paths);
    int append(const std::vector<std::string> &paths);

    /* For the resume file: the items in their own order and the play order as item indices. */
    const std::vector<std::string> &items() const { return items_; }
    const std::vector<int> &order() const { return order_; }
    /* Restores a saved state; a bad order (not a permutation) is replaced by the linear one. */
    void restore(std::vector<std::string> items, std::vector<int> order, int current, bool shuffle, Repeat r,
                 const std::string &source);

    void seed(uint32_t s) { rng_.seed(s); }

private:
    void linear();
    void reshuffle(); /* keeps the current track first */
    /* puts paths into items_ at item index item_at and into the play order at position pos */
    int insert_at(int item_at, int pos, const std::vector<std::string> &paths);

    std::vector<std::string> items_;
    std::vector<int> order_; /* play order: indices into items_ */
    int cur_ = -1;           /* position in order_ */
    bool shuffle_ = false;
    Repeat repeat_ = Repeat::Off;
    std::string source_;
    std::mt19937 rng_{20261112u};
};

}  // namespace vinyl
