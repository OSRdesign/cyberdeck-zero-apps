/*
 * SPDX-License-Identifier: MIT
 *
 * The radio preset list kept up to date from https://api.meshcore.nz/api/v1/config (feature A of phase 1b).
 *
 *   parse_preset_feed   strict parser / validator of the real JSON shape (config.suggested_radio_settings.entries[]: title,
 *                       frequency, bandwidth, spreading_factor, coding_rate, all four numbers as strings). One odd value and the
 *                       whole list is refused.
 *   save_preset_cache / load_preset_cache   <data dir>/presets.jsonl: the validated fields only, with the fetch date.
 *   PresetFetcher       spawns /usr/bin/curl in the background (posix_spawn, never waits), polled from the UI tick.
 *   route_table_has_default / network_online   "is the deck online": a default route exists.
 *
 * No LVGL. The network never touches the UI thread: the only things done there are a waitpid(WNOHANG) and a file read.
 */

#pragma once

#include "presets.hpp"

#include <string>
#include <sys/types.h>
#include <vector>

namespace meshzero {

constexpr size_t kPresetFeedMaxBytes = 200 * 1024;
constexpr size_t kPresetMinEntries = 5;
constexpr size_t kPresetMaxEntries = 120;

/* Parses the JSON text of the feed. On success fills `out` (in the order of the feed) and returns true. On any problem returns
 * false, leaves `out` empty and puts the reason in `why`. */
bool parse_preset_feed(const std::string &json, std::vector<RadioPreset> &out, std::string &why);

/* The same checks on a list that is already parsed (also used for the cache file): names, ranges, duplicates, counts. */
bool validate_preset_list(const std::vector<RadioPreset> &list, std::string &why);

/* presets.jsonl in `dir`: a header line {"date":..,"src":..} then one line per preset. Written through a temporary file. */
bool save_preset_cache(const std::string &dir, const std::vector<RadioPreset> &list, const std::string &date, const std::string &source);
/* False when the file is missing or anything in it is odd (nothing is returned then). */
bool load_preset_cache(const std::string &dir, std::vector<RadioPreset> &list, std::string &date, std::string &source);

/* "YYYY-MM-DD" in the deck's local time from a UNIX time; "" while the clock is not plausible. */
std::string fetch_date_text(uint32_t unix_time);

/* /proc/net/route text: is there a default route on an interface other than lo? */
bool route_table_has_default(const std::string &text);
/* $MESHHOP_ONLINE (1 / 0) when set, else the table of /proc/net/route. */
bool network_online();

/* When the silent download runs (the policy the app asks, pure so it is unit tested). One attempt 4 s after the start, once the deck is
 * online (offline: the check repeats every 30 s); if that attempt FAILED, ONE retry about 60 s later, again only while the deck is online.
 * Never more than two attempts per launch, and no retry after a success or when curl is missing / cannot start. */
class PresetSchedule {
public:
    static constexpr uint64_t kFirstDelayMs = 4000, kOfflineCheckMs = 30000, kRetryDelayMs = 60000;
    static constexpr int kMaxAttempts = 2;
    /* start_ms: the monotonic time of the app start. */
    explicit PresetSchedule(uint64_t start_ms) : start_(start_ms) {}
    /* True when a download should be started now. `online` is called only when the time is right (it reads /proc/net/route). */
    template <typename OnlineFn> bool due(uint64_t now, OnlineFn online)
    {
        if (running_ || done_) return false;
        if (attempts_ == 0) {
            if (now - start_ < kFirstDelayMs) return false;
        } else if (!retry_armed_ || now < retry_at_) {
            return false;
        }
        if (checked_ && now - last_check_ < kOfflineCheckMs) return false;
        checked_ = true;
        last_check_ = now;
        return online();
    }
    void started() { running_ = true; checked_ = false; ++attempts_; }
    /* retryable: the download was really tried (curl ran); false when curl is missing or could not start. */
    void failed(uint64_t now, bool retryable)
    {
        running_ = false;
        if (retryable && attempts_ < kMaxAttempts) {
            retry_armed_ = true;
            retry_at_ = now + kRetryDelayMs;
        } else {
            done_ = true;
        }
    }
    void succeeded() { running_ = false; done_ = true; }
    int attempts() const { return attempts_; }
    bool finished() const { return done_; }

private:
    uint64_t start_ = 0, last_check_ = 0, retry_at_ = 0;
    int attempts_ = 0;
    bool running_ = false, done_ = false, checked_ = false, retry_armed_ = false;
};

class PresetFetcher {
public:
    enum class State { Idle, Running, Done, Failed };

    /* curl: the program ($MESHHOP_CURL, else /usr/bin/curl). url: $MESHHOP_PRESETS_URL, else the official one. */
    explicit PresetFetcher(std::string data_dir);
    ~PresetFetcher();
    PresetFetcher(const PresetFetcher &) = delete;
    PresetFetcher &operator=(const PresetFetcher &) = delete;

    /* Starts the download and returns at once. False (state Failed, reason in message()) when curl is missing or cannot start. */
    bool start(uint64_t now_ms);
    /* Non-blocking. Running until curl exits or the deadline passes (then it is killed). Done: list() holds the validated list. */
    State poll(uint64_t now_ms);
    State state() const { return state_; }
    const std::string &message() const { return message_; }
    const std::vector<RadioPreset> &list() const { return list_; }
    const std::string &url() const { return url_; }
    const std::string &curl_path() const { return curl_; }
    void abort();

    static constexpr uint64_t kDeadlineMs = 25000;       // curl has its own --max-time 15; this is the safety net

private:
    void finish(State s, const std::string &msg);

    std::string dir_, curl_, url_, tmp_;
    pid_t pid_ = 0;
    uint64_t started_ms_ = 0;
    State state_ = State::Idle;
    std::string message_;
    std::vector<RadioPreset> list_;
};

} // namespace meshzero
