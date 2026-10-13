/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: speed detection logic (see baud.hpp).
 */
#include "baud.hpp"

#include <algorithm>

namespace sc {
namespace {

constexpr double kNoiseWeight = 3.0; /* NUL / 0xFF: what a wrong speed and framing errors give most */
constexpr double kBadWeight = 1.5;   /* other control codes, bytes that are not UTF-8 */
constexpr std::size_t kMinVisible = 4; /* some characters that are not spaces or line ends */

std::size_t visible_count(const std::string &bytes)
{
    std::size_t n = 0;
    for (const char ch : bytes) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c > 0x20 && c < 0x7F) || c >= 0xC2) ++n;
    }
    return n;
}

} // namespace

TextScore score_text(const std::string &bytes)
{
    TextScore s;
    s.bytes = bytes.size();
    for (std::size_t i = 0; i < bytes.size();) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == 0x00 || c == 0xFF) {
            ++s.noise;
            ++i;
            continue;
        }
        if (c == '\r' || c == '\n') {
            ++s.good;
            ++s.line_ends;
            ++i;
            continue;
        }
        if (c == '\t' || (c >= 0x20 && c < 0x7F)) {
            ++s.good;
            ++i;
            continue;
        }
        if (c == 0x1B) { /* an escape sequence (colours of a log): text, not noise */
            ++s.good;
            ++i;
            continue;
        }
        // a valid UTF-8 sequence counts as text
        std::size_t n = 0;
        if (c >= 0xC2 && c <= 0xDF) n = 2;
        else if (c >= 0xE0 && c <= 0xEF) n = 3;
        else if (c >= 0xF0 && c <= 0xF4) n = 4;
        bool valid = n > 0 && i + n <= bytes.size();
        for (std::size_t k = 1; valid && k < n; ++k)
            valid = (static_cast<unsigned char>(bytes[i + k]) & 0xC0) == 0x80;
        if (valid) {
            s.good += n;
            i += n;
            continue;
        }
        ++s.bad;
        ++i;
    }
    if (s.bytes > 0) {
        const double raw = (static_cast<double>(s.good) - kNoiseWeight * static_cast<double>(s.noise) -
                            kBadWeight * static_cast<double>(s.bad)) /
                           static_cast<double>(s.bytes);
        s.score = std::max(0.0, std::min(1.0, raw));
    }
    if (visible_count(bytes) < kMinVisible) s.score = std::min(s.score, kCleanScore - 0.01);
    return s;
}

bool looks_clean(const TextScore &s)
{
    return s.bytes >= kMinJudgeBytes && s.score >= kCleanScore;
}

std::vector<int> detect_order(int recommended)
{
    std::vector<int> order;
    const int common[] = {115200, 9600, 57600, 38400, 19200, 230400, 460800, 921600, 4800, 2400, 1200};
    if (recommended > 0) order.push_back(recommended);
    for (const int rate : common)
        if (std::find(order.begin(), order.end(), rate) == order.end()) order.push_back(rate);
    return order;
}

Detector::Detector(int recommended, bool enter_pass) : rates_(detect_order(recommended)), enter_pass_(enter_pass)
{
}

int Detector::rate() const
{
    return index_ < count() ? rates_[static_cast<std::size_t>(index_)] : 0;
}

void Detector::record(const std::string &bytes)
{
    if (done_) return;
    const TextScore s = score_text(bytes);
    if (looks_clean(s)) {
        found_ = true;
        best_rate_ = rate();
        best_ = s;
        done_ = true;
        return;
    }
    if (s.bytes > 0 && (best_rate_ == 0 || s.score > best_.score || (s.score == best_.score && s.bytes > best_.bytes))) {
        best_rate_ = rate();
        best_ = s;
    }
    if (++index_ < count()) return;
    if (pass_ == 1 && enter_pass_) {
        pass_ = 2;
        index_ = 0;
        return;
    }
    done_ = true;
}

DetectResult Detector::result() const
{
    DetectResult r;
    r.found = found_;
    r.baud = best_rate_;
    r.score = best_.score;
    if (found_) r.message = "Found " + std::to_string(best_rate_) + " - text looks clean";
    else if (cancelled_) r.message = "Stopped";
    else r.message = "No readable text at any speed; the board may be silent: try Connect and press its reset button";
    return r;
}

std::string Detector::progress_text() const
{
    if (done_) return result().message;
    return "Trying " + std::to_string(rate()) + (send_enter() ? " with Enter" : "") + "... (" +
           std::to_string(index_ + 1) + " of " + std::to_string(count()) + ")";
}

} // namespace sc
