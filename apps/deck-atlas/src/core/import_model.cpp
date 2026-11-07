/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the import page's model (see import_model.hpp).
 */
#include "import_model.hpp"

#include "format.hpp"

#include <algorithm>

namespace atlas {

void ImportProgress::start(ImportKind kind, const std::string &name, std::int64_t now_ms)
{
    *this = ImportProgress();
    kind_ = kind;
    name_ = name;
    started_ms_ = phase_ms_ = now_ms;
    phase_ = ImportPhase::Space;
}

void ImportProgress::set_phase(ImportPhase p, std::int64_t now_ms)
{
    if (p == phase_) return;
    if (phase_ == ImportPhase::Convert && p != ImportPhase::Convert) {
        convert_end_ms_ = now_ms;
        const double secs = (now_ms - phase_ms_) / 1000.0;
        if (secs > 0 && done_ > phase_done0_) convert_rate_ = static_cast<double>(done_ - phase_done0_) / secs;
    }
    phase_ = p;
    phase_ms_ = now_ms;
    phase_done0_ = 0;
    if (p == ImportPhase::Scan || p == ImportPhase::Copy || p == ImportPhase::Convert) {
        done_ = total_ = 0;
    }
}

void ImportProgress::update(int engine_phase, std::uint64_t done, std::uint64_t total, std::uint64_t skipped,
                            std::uint64_t resumed, std::int64_t now_ms)
{
    if (!running()) return;
    ImportPhase p = phase_;
    switch (engine_phase) {
    case 1: p = ImportPhase::Scan; break;
    case 2: p = ImportPhase::Copy; break;
    case 3: p = ImportPhase::Convert; break;
    case 4: p = ImportPhase::Check; break;
    default: return;
    }
    const bool new_phase = p != phase_;
    set_phase(p, now_ms);
    done_ = done;
    total_ = total;
    skipped_ = skipped;
    resumed_ = resumed;
    if (new_phase) phase_done0_ = std::max<std::uint64_t>(done, resumed);
}

void ImportProgress::finish_ok(std::uint64_t tiles, std::uint64_t bytes, std::int64_t now_ms)
{
    set_phase(ImportPhase::Done, now_ms);
    result_tiles_ = tiles;
    result_bytes_ = bytes;
}

void ImportProgress::finish_failed(const std::string &reason, std::int64_t now_ms)
{
    set_phase(ImportPhase::Failed, now_ms);
    reason_ = reason;
}

void ImportProgress::finish_cancelled(std::int64_t now_ms) { set_phase(ImportPhase::Cancelled, now_ms); }

double ImportProgress::fraction() const
{
    switch (phase_) {
    case ImportPhase::Space:
    case ImportPhase::Scan: return 0.0;
    case ImportPhase::Copy:
    case ImportPhase::Convert:
        if (total_ == 0) return 0.0;
        return std::min(1.0, static_cast<double>(done_) / static_cast<double>(total_));
    case ImportPhase::Check:
    case ImportPhase::Done: return 1.0;
    default: return total_ ? std::min(1.0, static_cast<double>(done_) / static_cast<double>(total_)) : 0.0;
    }
}

double ImportProgress::rate(std::int64_t now_ms) const
{
    if (phase_ != ImportPhase::Convert && phase_ != ImportPhase::Copy) return 0.0;
    const double secs = (now_ms - phase_ms_) / 1000.0;
    if (secs < 1.0 || done_ <= phase_done0_) return 0.0;
    return static_cast<double>(done_ - phase_done0_) / secs;
}

std::string ImportProgress::title() const
{
    switch (phase_) {
    case ImportPhase::Space: return "Checking free space";
    case ImportPhase::Scan: return "Counting tiles";
    case ImportPhase::Copy: return "Copying the file";
    case ImportPhase::Convert: return "Converting tiles";
    case ImportPhase::Check: return "Checking the result";
    case ImportPhase::Done: return "Imported";
    case ImportPhase::Failed: return "Import failed";
    case ImportPhase::Cancelled: return "Import stopped";
    }
    return std::string();
}

std::string ImportProgress::count_line() const
{
    switch (phase_) {
    case ImportPhase::Scan: return count_label(done_) + (done_ == 1 ? " tile found" : " tiles found");
    case ImportPhase::Copy:
        return total_ ? bytes_label(done_) + " of " + bytes_label(total_) : bytes_label(done_);
    case ImportPhase::Convert: {
        std::string s = total_ ? count_label(done_) + " of " + count_label(total_) + " tiles" : count_label(done_) + " tiles";
        if (resumed_) s += " (" + count_label(resumed_) + " from before)";
        return s;
    }
    default: return std::string();
    }
}

std::string ImportProgress::rate_line(std::int64_t now_ms) const
{
    const double r = rate(now_ms);
    if (r <= 0) return std::string();
    std::string s = phase_ == ImportPhase::Copy ? byte_rate_label(r) : tile_rate_label(r);
    if (total_ > done_) {
        const std::string eta = eta_label(static_cast<double>(total_ - done_) / r);
        if (!eta.empty()) s += ", " + eta + " left";
    }
    return s;
}

std::string ImportProgress::result_line() const
{
    switch (phase_) {
    case ImportPhase::Done: {
        std::string s;
        if (result_tiles_) s = count_label(result_tiles_) + (result_tiles_ == 1 ? " tile" : " tiles");
        if (result_bytes_) s += (s.empty() ? "" : ", ") + bytes_label(result_bytes_);
        if (convert_rate_ > 0) s += (s.empty() ? "" : ", ") + tile_rate_label(convert_rate_);
        if (skipped_) s += (s.empty() ? "" : ", ") + count_label(skipped_) + " other files left out";
        return s;
    }
    case ImportPhase::Failed: return reason_;
    case ImportPhase::Cancelled:
        return kind_ == ImportKind::Folder ? "Stopped. Import the same folder again to go on where it stopped."
                                           : "Stopped. Nothing was added.";
    default: return std::string();
    }
}

std::uint64_t space_needed(ImportKind kind, std::uint64_t source_bytes)
{
    const std::uint64_t margin = 16ull << 20;
    switch (kind) {
    case ImportKind::Folder: return source_bytes + source_bytes / 20 + margin;
    case ImportKind::FileCopy: return source_bytes + margin;
    default: return 0;
    }
}

std::string space_error(std::uint64_t needed, std::uint64_t free_bytes)
{
    if (needed <= free_bytes) return std::string();
    return "Not enough free space: about " + bytes_label(needed) + " needed, " + bytes_label(free_bytes) + " free";
}

} // namespace atlas
