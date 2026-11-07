/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the import page's model. An import runs on a worker thread (the engine's cp0_map_store_import_ex); its
 * progress callback hands over phase, done and total, and this model turns them into what the page shows: a title, a
 * bar (0..1), "4,000 of 10,326 tiles", the rate and the time left, and the result line. Plus the free-space check made
 * before an import starts. Pure: times are passed in (monotonic ms), no LVGL, no engine.
 */
#pragma once

#include <cstdint>
#include <string>

namespace atlas {

/* What is being imported. */
enum class ImportKind { Folder, FileCopy, FileInPlace };

/* The phases, in order (the engine's cp0_map_phase_t plus the app's own space check and the end states). */
enum class ImportPhase { Space = 0, Scan, Copy, Convert, Check, Done, Failed, Cancelled };

class ImportProgress {
public:
    void start(ImportKind kind, const std::string &name, std::int64_t now_ms);
    /* engine progress (phase as the engine's numbers: 1 scan, 2 copy, 3 convert, 4 check) */
    void update(int engine_phase, std::uint64_t done, std::uint64_t total, std::uint64_t skipped, std::uint64_t resumed,
                std::int64_t now_ms);
    void set_phase(ImportPhase p, std::int64_t now_ms);
    /* the end: result 0 = done (tiles imported, bytes of the archive), else failed / cancelled with the reason */
    void finish_ok(std::uint64_t tiles, std::uint64_t bytes, std::int64_t now_ms);
    void finish_failed(const std::string &reason, std::int64_t now_ms);
    void finish_cancelled(std::int64_t now_ms);

    ImportKind kind() const { return kind_; }
    ImportPhase phase() const { return phase_; }
    bool running() const { return phase_ < ImportPhase::Done; }
    std::uint64_t done() const { return done_; }
    std::uint64_t total() const { return total_; }
    std::uint64_t skipped() const { return skipped_; }
    std::uint64_t resumed() const { return resumed_; }

    /* the bar, 0..1: a folder is counted (scan, indeterminate: 0), then converted (0..1), then checked (1) */
    double fraction() const;
    /* tiles (convert) or bytes (copy) per second since the phase began; 0 before a second has passed */
    double rate(std::int64_t now_ms) const;
    /* the convert phase's average once it ended (what the result line and the log give) */
    double convert_rate() const { return convert_rate_; }

    std::string title() const;              /* "Counting tiles", "Converting tiles", "Copying the file", ... */
    std::string count_line() const;         /* "4,000 of 10,326 tiles", "120 MB of 276 MB", "1,234 tiles found" */
    std::string rate_line(std::int64_t now_ms) const; /* "812 tiles/s, about 8 s left" */
    std::string result_line() const;        /* "10,326 tiles, 276 MB, 812 tiles/s" or the failure / stop */
    const std::string &name() const { return name_; }

private:
    ImportKind kind_ = ImportKind::Folder;
    ImportPhase phase_ = ImportPhase::Space;
    std::string name_, reason_;
    std::uint64_t done_ = 0, total_ = 0, skipped_ = 0, resumed_ = 0;
    std::uint64_t phase_done0_ = 0;         /* done when the phase began (a resumed import starts above 0) */
    std::int64_t started_ms_ = 0, phase_ms_ = 0, convert_end_ms_ = 0;
    std::uint64_t result_tiles_ = 0, result_bytes_ = 0;
    double convert_rate_ = 0;
};

/* Free space: what an import needs on the store's file system (a folder: its bytes + 5 % + 16 MB of SQLite pages and
 * journal; a copied file: its size + 16 MB; read in place: nothing). */
std::uint64_t space_needed(ImportKind kind, std::uint64_t source_bytes);
/* "" when it fits, else the message for the page ("Not enough free space: about 290 MB needed, 120 MB free"). */
std::string space_error(std::uint64_t needed, std::uint64_t free_bytes);

} // namespace atlas
