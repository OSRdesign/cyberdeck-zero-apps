/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the demonstration world (see demo.hpp).
 */
#include "demo.hpp"

#include "fsutil.hpp"
#include "serial_io.hpp"

#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace sc {
namespace demo {

struct World::Board {
    fake::UsbTty port;
    int baud = 115200;
    int period_ms = 500;           /* a periodic line */
    std::vector<std::string> banner; /* sent when the app comes to the board's speed (as after a reset) */
    std::function<std::string(int)> line;
    std::string prompt;            /* the answer to Enter */
    bool noaccess = false;
    std::int64_t plug_at = 0;      /* -1: plugged at start (done) */
    int lose_after_ms = -1, back_after_ms = -1;

    Pty pty;
    bool plugged = false;
    int last_speed = -1;
    std::size_t banner_pos = 0;
    std::int64_t next_emit = 0;
    int tick = 0;
    int right_ms = 0;              /* time spent at the right speed (the lost timeline) */
    bool lost_done = false;
    std::uint32_t noise_seed = 12345;
};

namespace {

constexpr int kStepMs = 20;

void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

void write_some(int fd, const std::string &text)
{
    if (fd < 0 || text.empty()) return;
    ssize_t n = write(fd, text.data(), text.size()); /* a full pseudo-terminal drops it: a board does not wait */
    (void)n;
}

/* What a UART at the wrong speed makes of a line: bytes of every value, NUL and 0xFF among them. */
std::string noise(std::uint32_t &seed, std::size_t size)
{
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        seed = seed * 1103515245u + 12345u;
        out += static_cast<char>((seed >> 16) & 0xFF);
    }
    return out;
}

std::unique_ptr<World::Board> esp_board()
{
    auto b = std::make_unique<World::Board>();
    b->port.tty = "ttyUSB0";
    b->port.vid = 0x10c4;
    b->port.pid = 0xea60;
    b->port.manufacturer = "Silicon Labs";
    b->port.product = "CP2102 USB to UART Bridge Controller";
    b->port.serial = "0001";
    b->port.driver = "cp210x";
    b->port.by_id = "usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0";
    b->port.usb_port = 1;
    b->baud = 115200;
    b->period_ms = 700;
    b->banner = {
        "ets Jun  8 2016 00:22:57",
        "rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)",
        "\x1b[0;32mI (29) boot: ESP-IDF v5.1.2 2nd stage bootloader\x1b[0m",
        "\x1b]0;esp32\x07\x1b[0;32mI (312) cpu_start: Starting scheduler on PRO CPU.\x1b[0m",
        "Loading 10%\rLoading 55%\rLoading 100%",
        "\x1b[0;33mW (402) wifi: this warning is longer than one row of the screen, so the text view wraps it onto "
        "the next rows and keeps every character: 0123456789 abcdefghijklmnopqrstuvwxyz\x1b[0m",
        "\x1b[0;31mE (515) app: sensor not found at I2C address 0x76\x1b[0m",
        "I (600) app: city=Montr\xe9" "al\tzone=2\tok",
    };
    b->line = [](int k) {
        return "I (" + std::to_string(1000 + 700 * k) + ") app: tick " + std::to_string(k + 1) + ", free heap " +
               std::to_string(214332 - 16 * k) + " bytes";
    };
    b->prompt = "esp32> ";
    return b;
}

std::unique_ptr<World::Board> arduino_board()
{
    auto b = std::make_unique<World::Board>();
    b->port.tty = "ttyACM0";
    b->port.vid = 0x2341;
    b->port.pid = 0x0043;
    b->port.manufacturer = "Arduino (www.arduino.cc)";
    b->port.serial = "8573531303035171E0A1";
    b->port.driver = "cdc_acm";
    b->port.by_id = "usb-Arduino__www.arduino.cc__0043_8573531303035171E0A1-if00";
    b->port.usb_port = 2;
    b->baud = 9600;
    b->period_ms = 500;
    b->banner = {"Arduino Uno ready", "Sensor DHT22 found"};
    b->line = [](int k) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Temperature: %d.%d C  Humidity: %d %%", 21 + (k % 3), (k * 7) % 10,
                      40 + (k % 5));
        return std::string(buf);
    };
    b->prompt = "> ";
    return b;
}

std::unique_ptr<World::Board> ftdi_board()
{
    auto b = std::make_unique<World::Board>();
    b->port.tty = "ttyUSB1";
    b->port.vid = 0x0403;
    b->port.pid = 0x6001;
    b->port.manufacturer = "FTDI";
    b->port.product = "FT232R USB UART";
    b->port.serial = "A50285BI";
    b->port.driver = "ftdi_sio";
    b->port.by_id = "usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0";
    b->port.usb_port = 3;
    b->baud = 57600;
    b->period_ms = 400;
    b->banner = {"U-Boot 2023.01 (Jan 01 2026 - 12:00:00)", "DRAM:  128 MiB", "Hit any key to stop autoboot:  0"};
    b->line = [](int k) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "uptime %d s, load 0.%02d", 120 + k, (k * 13) % 100);
        return std::string(buf);
    };
    b->prompt = "router login: ";
    return b;
}

void plug(const Roots &roots, World::Board &b)
{
    if (b.noaccess) {
        b.port.node_target.clear();
        b.port.node_mode = 0000;
    } else if (open_pty(b.pty)) {
        b.port.node_target = b.pty.path;
    }
    fake::add_usb_tty(roots, b.port);
    b.plugged = true;
    b.last_speed = -1;
    b.right_ms = 0;
}

void unplug(const Roots &roots, World::Board &b)
{
    fake::remove_tty(roots, b.port.tty);
    close_pty(b.pty);
    b.plugged = false;
}

/* One step of a board: answer what came, emit text at its speed and noise at any other. */
void step(const Roots &roots, World::Board &b, std::int64_t now)
{
    if (!b.plugged) {
        if (b.plug_at >= 0 && now >= b.plug_at) {
            b.plug_at = -1;
            plug(roots, b);
        }
        return;
    }
    if (b.pty.master < 0) return; /* noaccess: a plain file, nothing to say */
    char in[256];
    const ssize_t n = read(b.pty.master, in, sizeof(in));
    for (ssize_t i = 0; i < n; ++i) {
        const char c = in[i];
        if (c == '\r' || c == '\n') write_some(b.pty.master, "\r\n" + b.prompt);
        else if (c >= 0x20 && c < 0x7F) write_some(b.pty.master, std::string(1, c)); /* a shell echoes */
    }
    const int speed = port_speed(b.pty.master);
    if (speed != b.last_speed) {
        b.last_speed = speed;
        if (speed == b.baud) {
            b.banner_pos = 0; /* the app came to our speed: as after a reset, the banner again */
            b.next_emit = now;
        }
    }
    const bool right = speed == b.baud;
    if (right) b.right_ms += kStepMs;
    if (b.lose_after_ms >= 0 && !b.lost_done && b.right_ms >= b.lose_after_ms) {
        b.lost_done = true;
        unplug(roots, b);
        b.plug_at = now + b.back_after_ms;
        return;
    }
    if (now < b.next_emit) return;
    if (!right) {
        write_some(b.pty.master, noise(b.noise_seed, 40));
        b.next_emit = now + b.period_ms;
    } else if (b.banner_pos < b.banner.size()) {
        write_some(b.pty.master, b.banner[b.banner_pos++] + "\r\n");
        b.next_emit = now + 60;
    } else if (b.line) {
        write_some(b.pty.master, b.line(b.tick++) + "\r\n");
        b.next_emit = now + b.period_ms;
    }
}

} // namespace

bool open_pty(Pty &pty, int initial_baud)
{
    close_pty(pty);
    pty.master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (pty.master < 0) return false;
    char name[128];
    if (grantpt(pty.master) != 0 || unlockpt(pty.master) != 0 || ptsname_r(pty.master, name, sizeof(name)) != 0) {
        close_pty(pty);
        return false;
    }
    pty.path = name;
    fcntl(pty.master, F_SETFL, fcntl(pty.master, F_GETFL) | O_NONBLOCK);
    // the held slave: raw from the start (no echo of the board's own output back to it), at a speed no board uses,
    // so the app's first configuration is seen as a change
    pty.slave = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (pty.slave < 0) {
        close_pty(pty);
        return false;
    }
    Frame frame;
    configure_port(pty.slave, initial_baud, frame);
    return true;
}

void close_pty(Pty &pty)
{
    if (pty.slave >= 0) close(pty.slave);
    if (pty.master >= 0) close(pty.master);
    pty.slave = pty.master = -1;
    pty.path.clear();
}

std::unique_ptr<World> World::from_env()
{
    const char *name = std::getenv("SERIAL_CONSOLE_DEMO");
    if (!name || !name[0]) return nullptr;
    const std::string s = name;
    if (s != "empty" && s != "boards" && s != "lost" && s != "noaccess" && s != "identify") {
        std::fprintf(stderr, "[serial-console] SERIAL_CONSOLE_DEMO=%s: no such scenario (empty, boards, lost, "
                             "noaccess, identify)\n", name);
        return nullptr;
    }
    auto world = std::make_unique<World>(s);
    if (!world->ok()) return nullptr;
    return world;
}

World::World(const std::string &scenario) : scenario_(scenario)
{
    const char *tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/serial-console-demo-XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) return;
    base_ = buf.data();
    roots_ = fake::make_roots(base_);
    no_group_ = scenario == "noaccess";

    // the on-board UARTs: GPIO 14/15 (serial0, a silent pseudo-terminal) and an empty 8250 slot
    if (open_pty(onboard_)) fake::add_onboard_tty(roots_, "ttyAMA0", "uart-pl011", 1, onboard_.path);
    fake::add_alias(roots_, "serial0", "ttyAMA0");
    fake::add_onboard_tty(roots_, "ttyS0", "serial8250", 0, "");
    fake::add_virtual_tty(roots_, "tty1");

    const std::int64_t now = now_ms();
    if (scenario == "identify") {
        const auto add = [this](const char *board, const char *variant, fake::UsbTty port) {
            auto host = std::make_unique<sim::Host>(sim::make_board(board, variant));
            if (!host->ok()) return;
            port.node_target = host->path();
            fake::add_usb_tty(roots_, port);
            sims_.emplace_back(port.tty, std::move(host));
        };
        fake::UsbTty esp;
        esp.tty = "ttyUSB0";
        esp.vid = 0x1a86;
        esp.pid = 0x7523;
        esp.product = "USB Serial";
        esp.driver = "ch341";
        esp.bcd = 0x0264;
        esp.by_id = "usb-1a86_USB_Serial-if00-port0";
        esp.usb_port = 1;
        add("esp32", "s3", esp);
        fake::UsbTty gps;
        gps.tty = "ttyUSB1";
        gps.vid = 0x067b;
        gps.pid = 0x2303;
        gps.manufacturer = "Prolific Technology Inc.";
        gps.product = "USB-Serial Controller";
        gps.driver = "pl2303";
        gps.bcd = 0x0300;
        gps.by_id = "usb-Prolific_Technology_Inc._USB-Serial_Controller-if00-port0";
        gps.usb_port = 2;
        add("gps", "", gps);
        fake::UsbTty pico;
        pico.tty = "ttyACM0";
        pico.vid = 0x2e8a;
        pico.pid = 0x0005;
        pico.manufacturer = "MicroPython";
        pico.product = "Board in FS mode";
        pico.serial = "e6614c311b4a7e2a";
        pico.driver = "cdc_acm";
        pico.interfaces = 2;
        pico.by_id = "usb-MicroPython_Board_in_FS_mode_e6614c311b4a7e2a-if00";
        pico.usb_port = 3;
        add("micropython", "", pico);
        fake::UsbTty uno;
        uno.tty = "ttyACM1";
        uno.vid = 0x2341;
        uno.pid = 0x0043;
        uno.manufacturer = "Arduino (www.arduino.cc)";
        uno.serial = "8573531303035171E0A1";
        uno.driver = "cdc_acm";
        uno.bcd = 0x0001;
        uno.interfaces = 2;
        uno.by_id = "usb-Arduino__www.arduino.cc__0043_8573531303035171E0A1-if00";
        uno.usb_port = 4;
        add("arduino", "", uno);
        fake::UsbTty boot; /* a Pico held in BOOTSEL: a USB drive, no tty */
        boot.vid = 0x2e8a;
        boot.pid = 0x0003;
        boot.manufacturer = "Raspberry Pi";
        boot.product = "RP2 Boot";
        boot.serial = "E0C9125B0D9B";
        boot.interfaces = 2;
        boot.usb_port = 5;
        fake::add_usb_device(roots_, boot);
    } else if (scenario == "empty") {
        boards_.push_back(esp_board());
        boards_.back()->plug_at = now + 3000;
    } else {
        boards_.push_back(esp_board());
        boards_.push_back(arduino_board());
        boards_.push_back(ftdi_board());
        if (scenario == "lost") {
            boards_[0]->lose_after_ms = 2500;
            boards_[0]->back_after_ms = 3000;
        }
        if (scenario == "noaccess") boards_[0]->noaccess = true;
        for (auto &b : boards_) {
            b->plug_at = -1;
            plug(roots_, *b);
        }
    }
    ok_ = true;
    thread_ = std::thread([this] { run(); });
    std::fprintf(stderr, "[serial-console] demonstration world \"%s\" in %s\n", scenario.c_str(), base_.c_str());
}

LinesFactory World::lines_for(const std::string &tty) const
{
    for (const auto &s : sims_)
        if (s.first == tty) return s.second->factory();
    return LinesFactory();
}

World::~World()
{
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    sims_.clear();
    for (auto &b : boards_) close_pty(b->pty);
    close_pty(onboard_);
    if (!base_.empty()) fs::remove_tree(base_);
}

void World::run()
{
    while (!stop_.load()) {
        const std::int64_t now = now_ms();
        for (auto &b : boards_) step(roots_, *b, now);
        sleep_ms(kStepMs);
    }
}

} // namespace demo
} // namespace sc
