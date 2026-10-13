/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: "What is on the other end?" - the probes, on a worker thread (no LVGL; tests/test_probe_pty.cpp runs
 * them against simulated boards on pseudo-terminals, core/sim.hpp).
 *
 *   Level 1  Listen          nothing is sent, the lines are not touched: it listens at the candidate speeds (the
 *                            port's speed first, then the common ones, 74880 included; one speed on a
 *                            microcontroller's own USB, where the speed does not matter; never 1200, the speed that
 *                            reboots some USB boards into their boot loader) and stops at the first conclusive one.
 *   Level 2  Reset + listen  only once confirmed: the DTR/RTS reset of linectl.hpp (ESP auto-reset with IO0 high, then
 *                            the Arduino DTR edge), then about 3 s of boot output at the port's speed; an ESP8266 (a
 *                            garbled start on an ESP-style adapter) gets a second reset and 1 s at 74880 for its ROM.
 *                            No reset line (RP2040, CircuitPython boards, a port without modem lines): it says so and
 *                            listens while the user presses the board's reset button.
 *   Level 3  Ask             only once confirmed, one probe at a time, exactly the bytes probe_sends_text() lists:
 *                            MicroPython (Ctrl-C, Ctrl-B), AT modem (AT, ATI), Marlin (M115), GRBL ($I), the Arduino
 *                            boot loader (STK500 v1 sync / signature / version / leave, after a DTR reset; v2 sign-on
 *                            and signature for a Mega), the ESP ROM boot loader (SYNC, GET_SECURITY_INFO, READ_REG of
 *                            the chip id and MAC registers; then the board is reset into its program). Read-only:
 *                            bootproto.hpp refuses every write and erase command.
 * Every wait is bounded and checks the cancel flag (Esc) every 50 ms; the lines are put back as they were whatever
 * happens (LineGuard), and a board taken into its boot loader is always reset back into its program first.
 */
#pragma once

#include "identify.hpp"
#include "linectl.hpp"
#include "ports.hpp"
#include "settings.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sc {

enum class Probe { Listen, Reset, MicroPython, At, Marlin, Grbl, Stk500, EspRom };

int probe_level(Probe p);               /* 1, 2 or 3 */
const char *probe_name(Probe p);        /* "MicroPython / CircuitPython" (the Ask page's rows) */
const char *probe_key(Probe p);         /* "micropython" */
Probe probe_from_key(const std::string &key); /* Listen when unknown */
/* The exact bytes it sends, for the confirmation dialog ("" for Listen). */
std::string probe_sends_text(Probe p);
/* Whether it may restart the board (the dialog says so). */
bool probe_resets(Probe p, ResetStyle style);
/* The level 3 probes, in the Ask page's order. */
const std::vector<Probe> &ask_probes();

/* What the probe needs to know about the port. */
struct ProbeTarget {
    std::string path;          /* the /dev path now */
    std::string id;            /* the port id (a board that re-enumerates after a reset is found again) */
    Roots roots;
    int baud = kDefaultBaud;   /* the port's speed (the saved or recommended one) */
    Frame frame;
    ResetStyle style = ResetStyle::AutoReset;
    Context ctx;
};

ProbeTarget probe_target(const PortInfo &port, int baud, const Frame &frame, const Roots &roots);
ResetStyle reset_style(const PortInfo &port);
bool native_usb(const PortInfo &port);
/* Level 1's speeds, in order. */
std::vector<int> listen_speeds(const ProbeTarget &t);

/* Timings (the tests shorten them). */
struct ProbeTimes {
    int listen_ms = 1000;        /* level 1, each speed */
    int first_listen_ms = 2000;  /* level 1, the first speed: opening the port restarts an Arduino-style board (DTR) */
    int native_listen_ms = 3000; /* level 1 on a microcontroller's own USB (one speed) */
    int boot_ms = 3000;          /* level 2 */
    int rom_ms = 1000;           /* level 2's 74880 try */
    int manual_ms = 6000;        /* no reset line: the user presses the button */
    int answer_ms = 1500;        /* level 3, each answer */
    int settle_ms = 60;          /* after a speed change */
    int reopen_ms = 2500;        /* a board whose USB port went at its reset (it comes back) */
};

struct ProbeRequest {
    Probe probe = Probe::Listen;
    ProbeTarget target;
    ProbeTimes times;
    bool confirmed = false;      /* levels 2 and 3 run only when the user confirmed the dialog */
    LinesFactory lines;          /* empty: the port's real DTR / RTS (TtyLines) */
};

struct ProbeProgress {
    bool running = false;
    bool done = false;
    bool cancelled = false;
    int error = 0;               /* errno of the open; ENODEV: the port went; EPERM: not confirmed */
    std::string text;            /* the Busy row: "Listening at 9600... (2 of 11)" */
    std::string note;            /* after: "No DTR/RTS lines: listened while you pressed reset" */
    Identity result;             /* when done (kind None when cancelled or failed) */
};

class Prober {
public:
    explicit Prober(ProbeRequest request);
    ~Prober(); /* cancel + join */
    Prober(const Prober &) = delete;
    Prober &operator=(const Prober &) = delete;

    void start();
    void cancel();
    ProbeProgress progress() const;
    const ProbeRequest &request() const { return req_; }

private:
    struct Port;
    void run();
    Identity listen(Port &port);
    Identity reset_listen(Port &port);
    Identity ask_text(Port &port, Probe p);
    Identity ask_stk500(Port &port);
    Identity ask_esp(Port &port);
    void set_text(const std::string &text);
    void set_note(const std::string &note);

    ProbeRequest req_;
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    ProbeProgress progress_;
    std::thread thread_;
};

} // namespace sc
