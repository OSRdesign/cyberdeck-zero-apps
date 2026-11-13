/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl positions in long files (P2a, the Playback setting "Remember position in long files", On by default; report 060
 * 3g): a file longer than 20 minutes (a mix, an audiobook, a podcast) starts again where it was left, the next time it
 * is played from the browser, the queue or the transport keys; a shorter track always starts at 0:00.
 *
 *   ~/.config/vinyl/positions.json   path -> position in ms, the 500 most recently played long files (a small file of
 *                                    its own, never inside state.json); written by the controller through a
 *                                    WritePolicy: on pause, on a track change, at exit, and at most once a minute while
 *                                    a long file plays; never while a short track plays.
 *
 * A long file that was played to its end (the last 15 s) forgets its position; a position in the first 5 s is no
 * position. LVGL-free.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vinyl {

class FileOps;

constexpr uint64_t kLongFileMs = 20ull * 60 * 1000; /* "longer than 20 minutes" */
constexpr size_t kMaxPositions = 500;
constexpr uint64_t kPositionMinMs = 5000;      /* under it: start at 0:00 */
constexpr uint64_t kPositionEndMarginMs = 15000; /* the last 15 s: played to the end */

/* Does a file of this length keep its position? */
inline bool is_long_file(uint64_t duration_ms) { return duration_ms > kLongFileMs; }

class Positions {
public:
    /* the saved position of `path` (0: none) */
    uint64_t get(const std::string &path) const;
    /* Notes where a long file is (pos_ms of duration_ms): kept (and made the most recent), or forgotten in its first 5 s
     * and its last 15 s. true when what the file must hold changed. */
    bool note(const std::string &path, uint64_t pos_ms, uint64_t duration_ms);
    bool erase(const std::string &path);
    size_t size() const { return items_.size(); }

    std::string to_json() const;
    bool from_json(const std::string &text);

private:
    std::vector<std::pair<std::string, uint64_t>> items_; /* oldest first */
};

/* ~/.config/vinyl/positions.json */
std::string positions_path();
/* 0 or -errno */
int save_positions(FileOps &ops, const std::string &path, const Positions &p);
bool load_positions(FileOps &ops, const std::string &path, Positions &p);

}  // namespace vinyl
