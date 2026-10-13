/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: finding the speed of a port by listening (no LVGL, no I/O; tests/test_baud.cpp). The I/O side is
 * serial_io.hpp SpeedFinder.
 *
 * At the right speed a board's output is text: printable ASCII, spaces, line ends, maybe UTF-8. At a wrong speed the
 * UART cuts the bits into other bytes: NUL and 0xFF (framing errors arrive as NUL), bytes over 0x7F that are not
 * UTF-8, control codes. score_text() gives the share of the bytes that look like text, with those penalised, from 0
 * (noise) to 1 (clean text).
 *
 * Detector walks the candidate speeds (the recommended one first, then the common ones), listening at each (the
 * caller listens about 700 ms and records what came). It stops at the first speed whose text is clearly clean. A
 * second pass that sends one Enter (CR) at each speed runs only when asked (a silent board that answers a key, such as
 * a router's login prompt) and only when the first pass found nothing.
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sc {

struct TextScore {
    double score = 0.0;      /* 0 .. 1 */
    std::size_t bytes = 0;
    std::size_t good = 0;    /* printable, space, CR, LF, TAB, valid UTF-8 */
    std::size_t noise = 0;   /* NUL and 0xFF */
    std::size_t bad = 0;     /* other controls, bytes that are not UTF-8 */
    std::size_t line_ends = 0;
};

TextScore score_text(const std::string &bytes);

/* Enough bytes to judge, and clearly text. */
constexpr std::size_t kMinJudgeBytes = 8;
constexpr double kCleanScore = 0.90;
bool looks_clean(const TextScore &s);

/* The speeds tried, in order: the recommended one, then 115200, 9600, 57600, 38400, 19200, 230400, 460800, 921600,
 * 4800, 2400, 1200 (each once). */
std::vector<int> detect_order(int recommended);

struct DetectResult {
    bool found = false;
    int baud = 0;           /* the speed found (found), else the best one with any data (0: silence) */
    double score = 0.0;
    std::string message;    /* the sentence for the user */
};

class Detector {
public:
    Detector(int recommended, bool enter_pass);

    bool done() const { return done_; }
    int rate() const;                 /* the speed to listen at now */
    int index() const { return index_; }
    int count() const { return static_cast<int>(rates_.size()); }
    int pass() const { return pass_; }
    bool send_enter() const { return pass_ == 2; } /* send one CR at the start of this listen */
    /* What came at rate() during the listen; moves to the next speed (or ends). */
    void record(const std::string &bytes);
    void cancel() { done_ = true; cancelled_ = true; }
    bool cancelled() const { return cancelled_; }
    DetectResult result() const;
    /* "Trying 9600... (2 of 12)", "Trying 9600 with Enter... (2 of 12)" */
    std::string progress_text() const;

private:
    std::vector<int> rates_;
    bool enter_pass_;
    int index_ = 0, pass_ = 1;
    bool done_ = false, cancelled_ = false;
    bool found_ = false;
    int best_rate_ = 0;
    TextScore best_;
};

} // namespace sc
