/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: modem control lines and reset sequences (see linectl.hpp).
 */
#include "linectl.hpp"

#include <sys/ioctl.h>
#include <termios.h>

namespace sc {
namespace {

std::string lines_text(const Lines &l)
{
    return std::string("D") + (l.dtr ? "1" : "0") + "R" + (l.rts ? "1" : "0");
}

LineStep set_step(bool dtr, bool rts)
{
    LineStep s;
    s.kind = LineStep::Set;
    s.lines.dtr = dtr;
    s.lines.rts = rts;
    return s;
}

LineStep wait_step(int ms)
{
    LineStep s;
    s.kind = LineStep::Wait;
    s.ms = ms;
    return s;
}

} // namespace

/* ---------------------------------------------------------------- the real lines */

bool TtyLines::get(Lines &out)
{
    int bits = 0;
    if (fd_ < 0 || ioctl(fd_, TIOCMGET, &bits) != 0) return false;
    out.dtr = (bits & TIOCM_DTR) != 0;
    out.rts = (bits & TIOCM_RTS) != 0;
    return true;
}

bool TtyLines::set(const Lines &lines)
{
    int bits = 0;
    if (fd_ < 0 || ioctl(fd_, TIOCMGET, &bits) != 0) return false;
    bits &= ~(TIOCM_DTR | TIOCM_RTS);
    if (lines.dtr) bits |= TIOCM_DTR;
    if (lines.rts) bits |= TIOCM_RTS;
    return ioctl(fd_, TIOCMSET, &bits) == 0;
}

/* ---------------------------------------------------------------- the recording fake */

bool FakeLines::get(Lines &out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!supported_) return false;
    out = state_;
    return true;
}

bool FakeLines::set(const Lines &lines)
{
    std::function<void(const Lines &)> listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!supported_) return false;
        state_ = lines;
        history_.push_back(lines);
        if (!trace_.empty()) trace_ += ' ';
        trace_ += lines_text(lines);
        listener = listener_;
    }
    if (listener) listener(lines);
    return true;
}

void FakeLines::note_wait(int ms)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!trace_.empty()) trace_ += ' ';
    trace_ += "w" + std::to_string(ms);
}

void FakeLines::set_listener(std::function<void(const Lines &)> listener)
{
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
}

Lines FakeLines::state() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::vector<Lines> FakeLines::history() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return history_;
}

std::string FakeLines::trace() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return trace_;
}

/* ---------------------------------------------------------------- sequences */

std::vector<LineStep> run_reset_steps(const Lines &original, ResetStyle style)
{
    if (style == ResetStyle::None) return {};
    std::vector<LineStep> steps = {
        set_step(false, true), /* ESP: EN low, IO0 high (RTS asserted, DTR released) */
        wait_step(100),
        set_step(false, false), /* ESP: EN released with IO0 high: it boots its program */
        wait_step(150),
        set_step(true, true),   /* neutral for the ESP circuit; DTR asserted: the Arduino reset edge */
        wait_step(50),
    };
    if (original != Lines{true, true}) steps.push_back(set_step(original.dtr, original.rts));
    return steps;
}

std::vector<LineStep> esp_bootloader_steps(ResetStyle style, int reset_delay_ms)
{
    if (style == ResetStyle::UsbJtag) /* esptool USBJTAGSerialReset */
        return {set_step(false, false), wait_step(100), set_step(true, false), wait_step(100), set_step(true, true),
                set_step(false, true), wait_step(100), set_step(false, false)};
    if (style == ResetStyle::None) return {};
    /* esptool UnixTightReset: both lines in one call, so IO0 and EN never pass through a wrong state */
    return {set_step(false, false), set_step(true, true), set_step(false, true), /* IO0 high, EN low: in reset */
            wait_step(100),
            set_step(true, false), /* IO0 low, EN high: out of reset into the ROM download mode */
            wait_step(reset_delay_ms),
            set_step(false, false)}; /* IO0 released */
}

std::vector<LineStep> esp_hard_reset_steps(ResetStyle style)
{
    if (style == ResetStyle::UsbJtag)
        return {set_step(false, true), wait_step(200), set_step(false, false), wait_step(200)};
    if (style == ResetStyle::None) return {};
    return {set_step(false, true), wait_step(100), set_step(false, false)};
}

std::vector<LineStep> arduino_reset_steps()
{
    return {set_step(false, false), wait_step(250), set_step(true, true), wait_step(50)};
}

bool run_steps(LineControl &lines, const std::vector<LineStep> &steps, const std::function<bool(int ms)> &wait)
{
    for (const LineStep &s : steps) {
        if (s.kind == LineStep::Set) {
            if (!lines.set(s.lines)) return false;
        } else {
            lines.note_wait(s.ms);
            if (wait && !wait(s.ms)) return false;
        }
    }
    return true;
}

std::string steps_text(const std::vector<LineStep> &steps)
{
    std::string out;
    for (const LineStep &s : steps) {
        if (!out.empty()) out += ' ';
        out += s.kind == LineStep::Set ? lines_text(s.lines) : "w" + std::to_string(s.ms);
    }
    return out;
}

/* ---------------------------------------------------------------- the guard */

LineGuard::LineGuard(LineControl &lines) : lines_(lines)
{
    supported_ = lines_.get(original_);
}

void LineGuard::restore()
{
    if (restored_ || !supported_) return;
    restored_ = true;
    Lines now;
    if (lines_.get(now) && now == original_) return; /* already as it was: no extra edge */
    lines_.set(original_);
}

} // namespace sc
