/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the demonstration world, for the headless scenes, screenshots and trying the app without a board.
 *
 *   SERIAL_CONSOLE_DEMO=<scenario> serial-console
 *
 * The app then scans a fake /sys and /dev tree in a temporary folder (fake_sys.hpp) instead of the real ones. Its
 * "boards" are pseudo-terminals: the app opens, configures and reads them with its real code. A board answers like a
 * real one: text at its own speed only (it reads the speed the app set from the master side) and noise at any other,
 * a banner when the app comes to its speed (as after a reset), an echo of what is typed and a prompt after Enter.
 *
 *   empty     no USB port; an ESP32 board (CP2102) is plugged in after 3 s (hot-plug); an on-board UART
 *   boards    ESP32 board on CP2102 (ttyUSB0, 115200: ESP-IDF log with colours, a long line, a Latin-1 byte),
 *             Arduino Uno (ttyACM0, 9600), FT232R cable to a device at 57600 (ttyUSB1, a boot loader and a login
 *             prompt); on-board UART on GPIO 14/15 (serial0, silent), an empty ttyS0 slot (never listed)
 *   lost      as boards; the ESP32 board is unplugged 2.5 s after the app first talks to it at its speed, and comes
 *             back 3 s later (the same by-id name)
 *   noaccess  as boards, but the ESP32 board's node refuses access and the app acts as if the user were not in the
 *             dialout group
 *   identify  the simulated boards of core/sim.hpp, for "What is on the other end?": an ESP32-S3 board on a CH340
 *             (ttyUSB0: ESP-IDF log, its ROM and boot after a reset, the esptool protocol in its boot loader), a
 *             u-blox GPS on a PL2303 cable (ttyUSB1, NMEA at 9600), a Raspberry Pi Pico running MicroPython
 *             (ttyACM0), an Arduino Uno with optiboot (ttyACM1, its sketch at 9600), and an RP2040 in BOOTSEL mode
 *             (a USB drive, no port); their DTR / RTS lines are simulated (lines_for())
 *
 * Nothing outside the temporary folder is touched; it is removed when the world goes.
 */
#pragma once

#include "fake_sys.hpp"
#include "sim.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sc {
namespace demo {

/* A pseudo-terminal pair: the master (the "board" side, non-blocking) and a held slave in raw mode at initial_baud. */
struct Pty {
    int master = -1;
    int slave = -1;
    std::string path; /* the slave's /dev/pts/N */
};
bool open_pty(Pty &pty, int initial_baud = 300);
void close_pty(Pty &pty);

class World {
public:
    /* The world SERIAL_CONSOLE_DEMO asks for; nullptr when it is unset or names no scenario. */
    static std::unique_ptr<World> from_env();
    explicit World(const std::string &scenario);
    ~World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;

    bool ok() const { return ok_; }
    const Roots &roots() const { return roots_; }
    const std::string &scenario() const { return scenario_; }
    bool no_group() const { return no_group_; } /* noaccess: the user counts as not in dialout */
    /* A home folder inside the world: the app keeps its settings and logs there in a demonstration. */
    std::string home() const { return base_ + "/home"; }
    /* The simulated DTR / RTS lines of a simulated board's port (identify), else empty (the port's real lines). */
    LinesFactory lines_for(const std::string &tty) const;

    struct Board;

private:
    void run();

    std::string scenario_;
    std::string base_;
    Roots roots_;
    bool ok_ = false;
    bool no_group_ = false;
    std::vector<std::unique_ptr<Board>> boards_;
    std::vector<std::pair<std::string, std::unique_ptr<sim::Host>>> sims_; /* tty, its simulated board */
    Pty onboard_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace demo
} // namespace sc
