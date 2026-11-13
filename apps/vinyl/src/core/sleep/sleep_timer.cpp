/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl sleep timer (see sleep_timer.hpp).
 */
#include "sleep_timer.hpp"

#include <cmath>
#include <cstdio>

namespace vinyl {

const char *sleep_mode_word(SleepMode m) {
    switch (m) {
    case SleepMode::Min15: return "15 minutes";
    case SleepMode::Min30: return "30 minutes";
    case SleepMode::Min60: return "60 minutes";
    case SleepMode::Track: return "This track";
    case SleepMode::Album: return "This album or queue";
    default: return "Off";
    }
}

float sleep_fade_gain(uint64_t remaining_ms) {
    if (remaining_ms >= kSleepFadeMs) return 1.0f;
    if (remaining_ms == 0) return 0.0f;
    const double db = kSleepFadeFloorDb * (1.0 - static_cast<double>(remaining_ms) / kSleepFadeMs);
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

void SleepTimer::set(SleepMode m, uint64_t now_ms) {
    mode_ = m;
    const uint64_t min = m == SleepMode::Min15 ? 15 : m == SleepMode::Min30 ? 30 : m == SleepMode::Min60 ? 60 : 0;
    end_ms_ = now_ms + min * 60000;
}

uint64_t SleepTimer::remaining_ms(uint64_t now_ms) const {
    if (!timed()) return 0;
    return now_ms >= end_ms_ ? 0 : end_ms_ - now_ms;
}

float SleepTimer::fade_gain(uint64_t now_ms) const {
    if (!timed()) return 1.0f;
    return sleep_fade_gain(remaining_ms(now_ms));
}

bool SleepTimer::stop_after_track(bool next_exists, bool next_same_album, bool playlist) const {
    if (mode_ == SleepMode::Track) return true;
    if (mode_ == SleepMode::Album) return !next_exists || (!playlist && !next_same_album);
    return false;
}

std::string sleep_left_text(uint64_t remaining_ms) {
    const uint64_t s = (remaining_ms + 999) / 1000; /* 29:12 shows until the second is over: never 0:00 while it runs */
    char b[24];
    std::snprintf(b, sizeof b, "%u:%02u", unsigned(s / 60), unsigned(s % 60));
    return b;
}

std::string sleep_status_text(SleepMode m, uint64_t remaining_ms) {
    switch (m) {
    case SleepMode::Min15:
    case SleepMode::Min30:
    case SleepMode::Min60:
        return "Stops in " + sleep_left_text(remaining_ms) + ". The music fades out for the last 10 seconds.";
    case SleepMode::Track: return "Stops at the end of this track. The music fades out for its last 10 seconds.";
    case SleepMode::Album: return "Stops at the end of this album or queue. The music fades out for its last 10 seconds.";
    default: return "";
    }
}

std::string sleep_menu_value(SleepMode m, uint64_t remaining_ms) {
    switch (m) {
    case SleepMode::Min15:
    case SleepMode::Min30:
    case SleepMode::Min60: return sleep_left_text(remaining_ms);
    case SleepMode::Track: return "This track";
    case SleepMode::Album: return "This album";
    default: return "Off";
    }
}

}  // namespace vinyl
