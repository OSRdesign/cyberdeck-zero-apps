/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl sleep timer (P2a, the Menu's Sleep timer page; report 060 3g): a pure state machine on the caller's monotonic
 * clock (ms), so the tests run it with a fake clock.
 *
 *   Off
 *   15, 30, 60 minutes   counted from the choice on the monotonic clock, playing or paused; over its last 10 s the music
 *                        fades out (linear in dB down to -60 dB, fade_gain); at its end the music pauses and the timer
 *                        is Off again
 *   This track           playback stops at the end of the track that plays (whichever it is: a skip moves the end);
 *                        the last 10 s of that track fade out (the engine fades the end of its last decoded track)
 *   This album or queue  playback stops before the next track of another album (the album tag, else the folder), or at
 *                        the end of the queue; for a playlist queue at its end only; the last 10 s fade out
 *
 * The timer keeps running when the pages change (the controller owns it); it does not survive a restart (it is not
 * saved). When it ends it pauses the music and nothing else (no quit, no screen change, the queue kept), and the
 * controller shows a notice. LVGL-free.
 */
#pragma once

#include <cstdint>
#include <string>

namespace vinyl {

enum class SleepMode { Off, Min15, Min30, Min60, Track, Album };
constexpr uint64_t kSleepFadeMs = 10000;   /* the fade over the last 10 s */
constexpr double kSleepFadeFloorDb = -60;  /* the fade's end */

/* The page's words: "Off", "15 minutes", "30 minutes", "60 minutes", "This track", "This album or queue". */
const char *sleep_mode_word(SleepMode m);
/* The six choices in the page's order. */
constexpr SleepMode kSleepModes[] = {SleepMode::Off, SleepMode::Min15, SleepMode::Min30, SleepMode::Min60, SleepMode::Track,
                                     SleepMode::Album};

/* The fade's amplitude factor `remaining_ms` before the end: 1 above 10 s, then linear in dB to -60 dB at the end, 0 at
 * or past it. */
float sleep_fade_gain(uint64_t remaining_ms);

class SleepTimer {
public:
    void set(SleepMode m, uint64_t now_ms);
    SleepMode mode() const { return mode_; }
    bool active() const { return mode_ != SleepMode::Off; }
    /* a minute mode (15, 30, 60) */
    bool timed() const { return mode_ == SleepMode::Min15 || mode_ == SleepMode::Min30 || mode_ == SleepMode::Min60; }
    /* a minute mode's time left (0 for the other modes) */
    uint64_t remaining_ms(uint64_t now_ms) const;
    /* a minute mode's fade (1 for the other modes and before the last 10 s) */
    float fade_gain(uint64_t now_ms) const;
    /* a minute mode reached its end: pause, then finish() */
    bool expired(uint64_t now_ms) const { return timed() && now_ms >= end_ms_; }
    /* the track modes: does playback stop at the end of the playing track? next_exists: the queue has a next track;
     * next_same_album: it is of the same album; playlist: the queue is a playlist's */
    bool stop_after_track(bool next_exists, bool next_same_album, bool playlist) const;
    /* the timer did its job (or was cancelled): Off */
    void finish() { mode_ = SleepMode::Off; }

private:
    SleepMode mode_ = SleepMode::Off;
    uint64_t end_ms_ = 0;
};

/* "29:12" (m:ss, the minute modes' time left; 60:00 at most) */
std::string sleep_left_text(uint64_t remaining_ms);
/* The page's status line while the timer runs ("" when Off): "Stops in 29:12. The music fades out for the last 10
 * seconds." (the drawn text), "Stops at the end of this track. ...", "Stops at the end of this album or queue. ..." */
std::string sleep_status_text(SleepMode m, uint64_t remaining_ms);
/* The Menu row's value: "Off", "29:12", "This track", "This album". */
std::string sleep_menu_value(SleepMode m, uint64_t remaining_ms);
/* The notice when the timer pauses the music. */
constexpr const char *kSleepEndedNotice = "Sleep timer ended. Paused.";

}  // namespace vinyl
