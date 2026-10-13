/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: simulated boards on pseudo-terminals, for the probes' tests (tests/test_probe_pty.cpp) and the
 * demonstration world's "identify" scenario (demo.hpp). No LVGL.
 *
 * A Host owns a pseudo-terminal pair and a thread that plays one Board on its master side, like a UART: what the board
 * says goes out at its own speed and arrives as noise when the port is at another speed (the master reads the speed
 * the app set on the slave); a board on its own USB (MicroPython) is heard at any speed. A pseudo-terminal has no
 * modem lines, so the Host gives the probe a LinesFactory: a FakeLines per open of the port, wired to the board's reset
 * circuit (opening raises DTR and RTS, closing drops them, as Linux does: an Arduino-style board restarts when the port
 * opens). Everything the app sent is kept (received()), with the boot loader commands seen (ops()), so a test can
 * check that nothing was sent, or only what the probe may send.
 *
 * Boards (the texts are representative of each firmware's real output, not captures):
 *   esp32      ESP32 / ESP32-S3 on an auto-reset circuit: ESP-IDF log lines; a reset prints the ROM banner and the
 *              boot of an ESP-IDF program; the boot loader entry answers the esptool protocol (SYNC, READ_REG,
 *              GET_SECURITY_INFO) and records any other command
 *   esp8266    the ROM at 74880, the sketch at 115200; "wdt" makes it reboot every 0.4 s (garbage at 115200)
 *   arduino    Uno with optiboot: a DTR edge starts the boot loader (STK500 v1 at 115200, signature 1E 95 0F) for
 *              boot_ms, then the sketch prints at 9600
 *   micropython Raspberry Pi Pico (own USB): a running program; Ctrl-C stops it, Ctrl-B prints the banner
 *   gps        u-blox NEO-M8N: NMEA bursts at 9600 with valid checksums, each with its TXT lines (a real module
 *              prints those at power-up only); the variant is the period in ms (default 1000)
 *   grbl       GRBL 1.1h on an Uno (restarts when the port opens): banner, answers $I
 *   marlin     Marlin 2.1.2.1 (restarts when the port opens): banner, temperature reports, answers M115
 *   at         a SIM800 modem: silent, answers AT and ATI
 *   silent     nothing, ever
 */
#pragma once

#include "linectl.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sc {
namespace sim {

enum class Wiring { None, Esp, Arduino };
enum class Mode { Running, Held, Bootloader };

class Board {
public:
    Board(Wiring wiring, int baud) : wiring_(wiring), baud_(baud) {}
    virtual ~Board() = default;

    /* --- what the Host calls (its thread; lines() may come from the probe's thread: queued) */
    void lines(const Lines &l);
    void step(int master, std::int64_t now);

    /* --- for the tests */
    std::string received() const;
    std::vector<int> ops() const;            /* boot loader commands seen (esptool op codes, STK500 command bytes) */
    std::vector<int> forbidden() const;      /* commands that are not read-only */
    Mode mode() const;
    int resets() const;
    int baud() const { return baud_; }

protected:
    /* the board's behaviour */
    virtual void power(std::int64_t now) { (void)now; }
    virtual void reset(std::int64_t now, bool download) { (void)now; (void)download; }
    virtual void bytes(const std::string &in, std::int64_t now) { (void)in; (void)now; }
    virtual void tick(std::int64_t now) { (void)now; }
    virtual bool any_speed() const { return false; } /* its own USB */

    /* output: text at a speed (0: the board's), after the previous output and delay_ms */
    void say(const std::string &text, int delay_ms = 0, int baud = 0);
    void quiet(); /* drop what is not said yet */
    void record_op(int op, bool allowed);
    void set_mode(Mode m);
    int port_baud() const { return port_baud_; } /* the speed the app set now */
    std::int64_t now() const { return now_; }

private:
    struct Out {
        std::int64_t due;
        std::string text;
        int baud;
    };
    void apply_lines(const Lines &l);

    Wiring wiring_;
    int baud_;
    mutable std::mutex mutex_;
    std::vector<Lines> pending_;
    Lines last_{false, false};
    bool powered_ = false;
    std::deque<Out> out_;
    std::int64_t last_due_ = 0;
    std::int64_t now_ = 0;
    int port_baud_ = -1;
    std::string received_;
    std::vector<int> ops_, forbidden_;
    std::atomic<Mode> mode_{Mode::Running};
    int resets_ = 0;
    std::uint32_t seed_ = 7;
};

std::unique_ptr<Board> make_board(const std::string &name, const std::string &variant = std::string());

class Host {
public:
    explicit Host(std::unique_ptr<Board> board);
    ~Host();
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    bool ok() const { return master_ >= 0; }
    const std::string &path() const { return path_; }
    Board &board() { return *board_; }
    /* A FakeLines per open, wired to the board (supported = false: a port without modem lines). */
    LinesFactory factory(bool supported = true);
    /* The FakeLines of the last open (its trace), or nullptr. */
    std::shared_ptr<FakeLines> last_lines() const;
    int opens() const;

private:
    void run();

    std::unique_ptr<Board> board_;
    int master_ = -1, slave_ = -1;
    std::string path_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::shared_ptr<FakeLines> last_;
    int opens_ = 0;
};

/* "$GPGGA,...": the sentence with its checksum ("*hh") */
std::string nmea(const std::string &body);

} // namespace sim
} // namespace sc
