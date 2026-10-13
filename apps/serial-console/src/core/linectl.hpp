/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the modem control lines (DTR, RTS) behind an interface, and the reset sequences of the boards
 * (no LVGL; tests/test_bootproto.cpp checks the sequences on the recording fake, tests/test_probe_pty.cpp drives
 * simulated boards through it: a pseudo-terminal has no modem lines).
 *
 * "Asserted" (true) is the TIOCM bit set, the pin LOW on the adapter (the lines are active low). Linux asserts both
 * when it opens a port and drops both when it closes it.
 *
 * The usual wiring on development boards:
 *   ESP32 / ESP8266, two cross-wired transistors: EN (reset) is held LOW while RTS is asserted and DTR is not; IO0
 *       (boot mode) is held LOW while DTR is asserted and RTS is not; both asserted or both released is neutral.
 *       Running reset: EN low with IO0 high, then EN released with IO0 still high (the board boots its program).
 *       Bootloader entry: EN low, then EN released while IO0 is low (the ROM download mode), then IO0 released.
 *   ESP32-S3 / C3 / C6 USB-Serial/JTAG (303a:1001): the chip reads the same two lines from USB and acts the same way.
 *   Arduino (Uno, Mega, Nano and their clones): DTR goes through a capacitor to RESET: asserting DTR (its falling edge)
 *       pulses RESET; the bootloader then listens for about a second before the sketch starts.
 *   RP2040 / RP2350, Leonardo, Teensy, CircuitPython boards (the microcontroller's own USB): no reset line. Their only
 *       USB "reset" is the 1200-baud touch, which reboots them into their bootloader: this app never does it.
 *
 * A sequence is plain data (LineStep: set the lines, or wait) so the tests can read it; run_steps() plays it on a
 * LineControl, waiting through a callback (the prober reads the port while it waits: boot output starts at once).
 * LineGuard reads the lines first and puts them back as they were whatever happens (cancel, timeout, error).
 */
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sc {

struct Lines {
    bool dtr = false;
    bool rts = false;
    bool operator==(const Lines &o) const { return dtr == o.dtr && rts == o.rts; }
    bool operator!=(const Lines &o) const { return !(*this == o); }
};

class LineControl {
public:
    virtual ~LineControl() = default;
    /* false: the port has no modem lines (a pseudo-terminal, some USB devices) */
    virtual bool get(Lines &out) = 0;
    virtual bool set(const Lines &lines) = 0;
    /* run_steps() tells it each wait (the recording fake writes it in its trace) */
    virtual void note_wait(int ms) { (void)ms; }
};

/* The real lines of an open serial port: TIOCMGET / TIOCMSET (both lines in one call, never an in-between state). */
class TtyLines : public LineControl {
public:
    explicit TtyLines(int fd) : fd_(fd) {}
    bool get(Lines &out) override;
    bool set(const Lines &lines) override;

private:
    int fd_;
};

/* The recording fake: remembers every change ("DTR=0 RTS=1"), tells a listener (a simulated board's reset circuit),
 * and can play a port without modem lines. Thread-safe. */
class FakeLines : public LineControl {
public:
    explicit FakeLines(Lines initial = Lines{true, true}, bool supported = true)
        : state_(initial), supported_(supported) {}
    bool get(Lines &out) override;
    bool set(const Lines &lines) override;

    void set_listener(std::function<void(const Lines &)> listener);
    Lines state() const;
    std::vector<Lines> history() const; /* every set(), in order */
    /* The history as text, with the waits run_steps() recorded: "D0R1 w100 D0R0 w150 D1R1" */
    std::string trace() const;
    void note_wait(int ms) override;    /* the waits in the trace */

private:
    mutable std::mutex mutex_;
    Lines state_;
    bool supported_;
    std::vector<Lines> history_;
    std::string trace_;
    std::function<void(const Lines &)> listener_;
};

/* What a port's reset wiring most likely is (from its USB identity). */
enum class ResetStyle {
    AutoReset, /* a USB-serial chip: the ESP auto-reset circuit or the Arduino DTR capacitor (both sequences combined) */
    UsbJtag,   /* Espressif USB-Serial/JTAG (303a:1001) */
    None,      /* the microcontroller's own USB: no reset line (RP2040, Leonardo, Teensy, CircuitPython boards) */
};

struct LineStep {
    enum Kind { Set, Wait } kind = Set;
    Lines lines;
    int ms = 0;
};

/* Level 2's reset: ESP auto-reset (RTS asserted with DTR released: EN low, IO0 high, 100 ms; then both released: the
 * ESP boots its program, IO0 never low, 150 ms), then both asserted (neutral for the ESP circuit; DTR's assertion is
 * the Arduino reset edge), 50 ms, then `original` when it was not that. Both kinds of board restart normally; a board
 * with neither circuit is not touched. None: no steps. */
std::vector<LineStep> run_reset_steps(const Lines &original, ResetStyle style);
/* An ESP into its ROM download mode (esptool's "tight" reset for USB-serial chips, its USB-JTAG reset for 303a:1001):
 * ends with both lines released. */
std::vector<LineStep> esp_bootloader_steps(ResetStyle style, int reset_delay_ms = 50);
/* An ESP out of the ROM download mode into its program (esptool's hard reset): RTS pulse with DTR released. */
std::vector<LineStep> esp_hard_reset_steps(ResetStyle style);
/* An Arduino into its bootloader: both released 250 ms, then both asserted (the DTR edge), 50 ms (avrdude's). */
std::vector<LineStep> arduino_reset_steps();

/* Plays the steps; wait(ms) is called for each Wait (it may read the port; it returns false to stop: cancelled).
 * Returns false when a set() failed (no modem lines) or wait() stopped it. */
bool run_steps(LineControl &lines, const std::vector<LineStep> &steps, const std::function<bool(int ms)> &wait);
/* "D0R1 w100 D0R0 w150 D1R1" */
std::string steps_text(const std::vector<LineStep> &steps);

/* Reads the lines when made; restore() (and the destructor) sets them back exactly. */
class LineGuard {
public:
    explicit LineGuard(LineControl &lines);
    ~LineGuard() { restore(); }
    LineGuard(const LineGuard &) = delete;
    LineGuard &operator=(const LineGuard &) = delete;

    bool supported() const { return supported_; }
    const Lines &original() const { return original_; }
    void restore();

private:
    LineControl &lines_;
    Lines original_;
    bool supported_ = false;
    bool restored_ = false;
};

/* Makes the LineControl for an open port (tests and the demonstration world give a fake). */
using LinesFactory = std::function<std::unique_ptr<LineControl>(int fd)>;

} // namespace sc
