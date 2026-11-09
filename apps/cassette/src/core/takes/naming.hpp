/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette takes: file names, the take counter and the edit-suffix numbers. LVGL-free.
 *
 * Grammar of a take file name (D15, amended for edits):
 *   <prefix>_<take>_<when>[_p<part>][-<edit>].wav
 *   prefix  1..24 of [A-Za-z0-9_-], not starting with '.' (so never "." or ".."), no path separators
 *   take    decimal, zero-padded to at least 3 digits ("006", "999", "1000")
 *   when    YYYYMMDD-HHMM (local time) or "nodate" while the clock has not been set
 *   part    auto-split part 2, 3, ... (part 1, the first file of a take, has no suffix)
 *   edit    1, 2, 3 ... : results of trim/split, never reused for the same take (and part)
 * The name is parsed from the right, so a prefix may itself contain '_' and '-'.
 *
 * The take counter lives in <recordings>/.take-counter (the last take number handed out, decimal) and is replaced
 * atomically. A new number is counter + 1, moved forward past any number the same prefix already uses in the folder's
 * file names and journals (so a take number is never duplicated for a prefix); with no usable counter file it is the
 * highest take found + 1, so it does not repeat after deletes, a crash, or a lost counter file. The Take number setting
 * (set_next_take_number) may move the counter back or forward; the same guard applies. The edit numbers used per take are kept in
 * <recordings>/.edits/<take>[_p<part>].used (one number per line) so a deleted "-2" is never handed out again.
 */
#pragma once

#include "../fileops.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cassette {

constexpr size_t kMaxPrefix = 24;
constexpr const char *kDefaultPrefix = "take";
constexpr const char *kTimesyncMarker = "/run/systemd/timesync/synchronized";

struct DateTime {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    bool operator==(const DateTime &o) const {
        return year == o.year && month == o.month && day == o.day && hour == o.hour && minute == o.minute;
    }
};

/* Local time (or UTC) of a wall-clock second. False when the conversion fails. */
bool datetime_from_wall(int64_t wall_s, DateTime &out, bool utc = false);

/* Has the system clock been set since boot? The boards have no RTC, so a clock that was never synchronised says
 * 1970 or the last shutdown time. */
class ClockState {
public:
    virtual ~ClockState() = default;
    virtual bool synchronized() = 0;
};

/* Production: the clock counts as set when systemd-timesyncd's marker file exists (it is created on the first
 * successful NTP sync after boot and lives in /run, so it is gone after every reboot). A missing marker means "not
 * set", also on a system without timesyncd: a wrong date in a file name is worse than "nodate". */
class TimesyncClockState final : public ClockState {
public:
    explicit TimesyncClockState(FileOps &ops, std::string marker = kTimesyncMarker)
        : ops_(ops), marker_(std::move(marker)) {}
    bool synchronized() override;

private:
    FileOps &ops_;
    std::string marker_;
};

/* Prefix rule: 1..kMaxPrefix characters of letters, digits, '-' and '_'; never starting with '.' (implied). */
bool valid_prefix(const std::string &prefix);

struct TakeName {
    std::string prefix;
    uint64_t take = 0;
    bool has_date = false; /* false: "nodate" */
    DateTime when;
    uint32_t part = 1;     /* auto-split part, 1 = the first file (no suffix) */
    uint32_t edit = 0;     /* 0 = not an edit result */
};

/* "006", "1000". */
std::string take_number_text(uint64_t take);
/* The file name with ".wav". The prefix is not validated here (see valid_prefix). */
std::string format_take_name(const TakeName &t);
/* Parses a file name (no directory). False when it does not follow the grammar. */
bool parse_take_name(const std::string &file_name, TakeName &out);
/* The list label: "006", "006-1", and for later auto-split parts "006 p2", "006 p2-1". */
std::string take_label(const TakeName &t);
/* The name of a new take: date and time from the clock when it is synchronized, else "nodate". */
TakeName new_take_name(const std::string &prefix, uint64_t take, ClockState &cs, Clock &clock);
/* File name without a trailing ".wav" / ".WAV". */
std::string file_stem(const std::string &file_name);
bool has_wav_extension(const std::string &file_name);

/* ---- take counter ---- */
std::string take_counter_path(const std::string &recordings_dir);
/* Highest take number in the file names of the folder and in its journals (take names, paths, parts); 0 if none. */
uint64_t highest_take_found(FileOps &ops, const std::string &recordings_dir);
constexpr uint64_t kTakeNumberMin = 1, kTakeNumberMax = 9999; /* what the Take number setting accepts */
/* The first number >= from that no take of this prefix uses (file names and journals; the take number plus the prefix,
 * not the timestamped name). from 0 counts as 1. */
uint64_t first_free_take_number(FileOps &ops, const std::string &recordings_dir, const std::string &prefix,
                                uint64_t from);
/* The number the next take gets, without reserving it: the counter (the last number handed out) + 1, moved forward past
 * numbers this prefix already uses. A missing or corrupt counter: the highest take in the folder + 1. */
uint64_t peek_take_number(FileOps &ops, const std::string &recordings_dir, const std::string &prefix);
/* Hands out the number peek_take_number() gives and stores it before returning (creates the folder). 0 or -errno; on an
 * error no number is handed out. */
int reserve_take_number(FileOps &ops, const std::string &recordings_dir, uint64_t &take,
                        const std::string &prefix = kDefaultPrefix);
/* The Take number setting: the next take is `wanted` (kTakeNumberMin..kTakeNumberMax, else -EINVAL), moved forward past
 * numbers this prefix already uses; `landed` is the number it ended on. Stores the counter atomically (wanted - 1 or
 * the landed number - 1). Existing files are never touched. 0 or -errno; on an error nothing changes. */
int set_next_take_number(FileOps &ops, const std::string &recordings_dir, const std::string &prefix, uint64_t wanted,
                         uint64_t &landed);

/* ---- edit suffixes (for trim and split) ----
 * next_edit_names(): the names for count results cut from source_name. They are the source's name without its own
 * edit suffix plus -N, -N+1, ... where N is one more than the highest number already used for that take and part:
 * in `existing` (file names in the folder, any prefix, since a rename changes only the prefix) and in `used` (the
 * take's record). Cutting "take_006_20261108-1105-1.wav" again therefore continues the numbering of take 006. The
 * take number and the take counter never change. A source that does not follow the grammar gets "<stem>-N.wav". */
std::vector<std::string> next_edit_names(const std::string &source_name, unsigned count,
                                         const std::vector<std::string> &existing,
                                         const std::vector<uint32_t> &used = {});
/* The record key of a source: "006", "006_p2", or "x_<stem>" for a name outside the grammar. */
std::string edit_record_key(const std::string &source_name);
std::string edit_record_path(const std::string &recordings_dir, const std::string &source_name);
/* Reads the used numbers of the take (missing record: none). */
std::vector<uint32_t> read_edit_record(FileOps &ops, const std::string &recordings_dir, const std::string &source_name);
/* The call T5b makes before writing results: lists the folder, reads the record, picks the names with
 * next_edit_names(), and stores the new numbers in the record atomically before returning them, so a crash during the
 * cut cannot hand the same number out twice. 0 or -errno; out holds file names (no directory). */
int reserve_edit_names(FileOps &ops, const std::string &recordings_dir, const std::string &source_name, unsigned count,
                       std::vector<std::string> &out);

}  // namespace cassette
