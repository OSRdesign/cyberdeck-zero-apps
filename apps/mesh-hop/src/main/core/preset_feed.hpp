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
