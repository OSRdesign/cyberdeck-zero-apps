/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: simulated boards (see sim.hpp). Their texts follow the real formats of each firmware; they are
 * representative, not captures of a particular device.
 */
#include "sim.hpp"

#include "bootproto.hpp"
#include "serial_io.hpp"
#include "settings.hpp"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace sc {
namespace sim {
namespace {

void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

std::string le32(std::uint32_t v)
{
    std::string s;
    for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
    return s;
}

std::uint32_t get_le32(const std::string &s, std::size_t at)
{
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<std::uint8_t>(s[at + static_cast<std::size_t>(i)]);
    return v;
}

/* ---------------------------------------------------------------- ESP32 / ESP32-S3 */

class EspBoard : public Board {
public:
    explicit EspBoard(bool s3) : Board(Wiring::Esp, 115200), s3_(s3)
    {
        if (s3_) {
            regs_[0x40001000u] = 0x00000009u;
            regs_[0x60007044u] = 0xFAAABBCCu; /* MAC f4:12:fa:aa:bb:cc */
            regs_[0x60007048u] = 0x0000F412u;
        } else {
            regs_[0x40001000u] = 0x00F01D83u;
            regs_[0x3FF5A004u] = 0xC4123456u; /* MAC 24:0a:c4:12:34:56 */
            regs_[0x3FF5A008u] = 0x6B00240Au;
        }
    }

protected:
    void power(std::int64_t t) override { next_ = t + 50; }

    void reset(std::int64_t t, bool download) override
    {
        quiet();
        next_ = t + 900;
        if (download) {
            set_mode(Mode::Bootloader);
            say(s3_ ? "ESP-ROM:esp32s3-20210327\r\nBuild:Mar 27 2021\r\nrst:0x1 (POWERON),boot:0x0 (DOWNLOAD(USB/UART0))\r\n"
                      "waiting for download\r\n"
                    : "ets Jun  8 2016 00:22:57\r\n\r\nrst:0x1 (POWERON_RESET),boot:0x3 "
                      "(DOWNLOAD_BOOT(UART0/UART1/SDIO_REI_REO_V2))\r\nwaiting for download\r\n",
                2);
            return;
        }
        set_mode(Mode::Running);
        if (s3_)
            say("ESP-ROM:esp32s3-20210327\r\nBuild:Mar 27 2021\r\nrst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n"
                "SPIWP:0xee\r\nmode:DIO, clock div:1\r\nload:0x3fce3810,len:0x178c\r\nload:0x403c9700,len:0x4\r\n"
                "entry 0x403c98d0\r\n",
                2);
        else
            say("ets Jun  8 2016 00:22:57\r\n\r\nrst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)\r\n"
                "configsip: 0, SPIWP:0xee\r\nmode:DIO, clock div:2\r\nload:0x3fff0030,len:7104\r\n"
                "entry 0x4008067c\r\n",
                2);
        const std::string chip = s3_ ? "esp32s3" : "esp32";
        say("\x1b[0;32mI (27) boot: ESP-IDF v5.1.2 2nd stage bootloader\x1b[0m\r\n"
            "\x1b[0;32mI (27) boot: compile time Jan 12 2024 10:00:00\x1b[0m\r\n"
            "\x1b[0;32mI (31) boot: chip revision: v0.2\x1b[0m\r\n"
            "\x1b[0;32mI (36) boot." + chip + ": Boot SPI Speed : 80MHz\x1b[0m\r\n", 30);
        say("\x1b[0;32mI (250) cpu_start: Pro cpu up.\x1b[0m\r\n"
            "\x1b[0;32mI (283) app_init: Application information:\x1b[0m\r\n"
            "\x1b[0;32mI (283) app_init: Project name:     weather-station\x1b[0m\r\n"
            "\x1b[0;32mI (288) app_init: App version:      1.4.2\x1b[0m\r\n"
            "\x1b[0;32mI (305) app_init: ESP-IDF:          v5.1.2\x1b[0m\r\n"
            "\x1b[0;32mI (312) cpu_start: Starting scheduler on PRO CPU.\x1b[0m\r\n", 120);
    }

    void tick(std::int64_t t) override
    {
        if (mode_now() != Mode::Running || t < next_) return;
        next_ = t + 300;
        ++n_;
        say("\x1b[0;32mI (" + std::to_string(1000 + 300 * n_) + ") app: temp 21." + std::to_string(n_ % 10) +
            " C, free heap 214332\x1b[0m\r\n");
    }

    void bytes(const std::string &in, std::int64_t t) override
    {
        (void)t;
        if (mode_now() != Mode::Bootloader || port_baud() != 115200) return;
        dec_.feed(in);
        std::string pk;
        while (dec_.next(pk)) {
            if (pk.size() < 8 || pk[0] != 0) continue;
            const std::uint8_t op = static_cast<std::uint8_t>(pk[1]);
            const std::string data = pk.substr(8);
            record_op(op, esprom::allowed(op));
            std::string status(4, '\0');
            std::string payload;
            std::uint32_t value = 0;
            if (op == esprom::kSync) {
                answer(op, 0, status);
                answer(op, 0, status); /* the ROM answers a SYNC more than once */
                continue;
            }
            if (op == esprom::kReadReg && data.size() >= 4) {
                const auto it = regs_.find(get_le32(data, 0));
                value = it == regs_.end() ? 0 : it->second;
            } else if (op == esprom::kGetSecurityInfo && s3_) {
                payload = std::string(12, '\0') + le32(9) + le32(0);
            } else {
                status = std::string("\x01\x05\x00\x00", 4); /* not supported (and never a write) */
            }
            answer(op, value, payload + status);
        }
    }

private:
    Mode mode_now() const { return mode(); }
    void answer(std::uint8_t op, std::uint32_t value, const std::string &data)
    {
        std::string r;
        r += '\x01';
        r += static_cast<char>(op);
        r += static_cast<char>(data.size() & 0xFF);
        r += static_cast<char>((data.size() >> 8) & 0xFF);
        r += le32(value);
        r += data;
        say(slip::encode(r), 1);
    }

    bool s3_;
    std::int64_t next_ = 0;
    int n_ = 0;
    slip::Decoder dec_;
    std::map<std::uint32_t, std::uint32_t> regs_;
};

/* ---------------------------------------------------------------- ESP8266 */

class Esp8266Board : public Board {
public:
    explicit Esp8266Board(bool wdt) : Board(Wiring::Esp, 115200), wdt_(wdt) {}

protected:
    void power(std::int64_t t) override { next_ = t + 150; }

    void reset(std::int64_t t, bool download) override
    {
        quiet();
        (void)download;
        rom(2);
        if (!wdt_) say("\r\nHello from the ESP8266 sketch\r\nWiFi connecting...\r\nWiFi connected, IP 192.168.1.42\r\n", 80);
        next_ = t + kLoopMs;
    }

    void tick(std::int64_t t) override
    {
        if (!wdt_ || t < next_) return; /* a watchdog reboot loop: the ROM again and again */
        next_ = t + kLoopMs;
        rom(4);
    }

    static constexpr int kLoopMs = 400;

private:
    void rom(int cause)
    {
        say("\r\n ets Jan  8 2013,rst cause:" + std::to_string(cause) + ", boot mode:(3,6)\r\n\r\n" +
                (cause == 4 ? std::string("wdt reset\r\n") : std::string()) +
                "load 0x4010f000, len 3460, room 16 \r\ntail 4\r\nchksum 0xcc\r\nload 0x3fff20b8, len 40, room 4 \r\n"
                "tail 4\r\nchksum 0xc9\r\ncsum 0xc9\r\nv00000000\r\n~ld\r\n",
            2, 74880);
    }

    bool wdt_;
    std::int64_t next_ = 0;
};

/* ---------------------------------------------------------------- Arduino Uno with optiboot */

class ArduinoBoard : public Board {
public:
    explicit ArduinoBoard(int boot_ms) : Board(Wiring::Arduino, 9600), boot_ms_(boot_ms) {}

protected:
    void power(std::int64_t t) override { next_ = t + 100; }

    void reset(std::int64_t t, bool download) override
    {
        (void)download;
        quiet();
        set_mode(Mode::Bootloader);
        boot_until_ = t + boot_ms_;
        buf_.clear();
    }

    void tick(std::int64_t t) override
    {
        if (mode() == Mode::Bootloader) {
            if (t < boot_until_) return;
            set_mode(Mode::Running);
            say("Arduino ready\r\n");
            next_ = t + 500;
            return;
        }
        if (t < next_) return;
        next_ = t + 500;
        ++n_;
        say("Temperature: 2" + std::to_string(1 + n_ % 3) + ".5 C  Humidity: 4" + std::to_string(n_ % 10) + " %\r\n");
    }

    void bytes(const std::string &in, std::int64_t t) override
    {
        if (mode() != Mode::Bootloader || port_baud() != 115200) return;
        buf_ += in;
        while (!buf_.empty()) {
            const std::uint8_t c = static_cast<std::uint8_t>(buf_[0]);
            std::size_t need = 2;
            if (c == 0x41) need = 3;
            if (!stk::allowed(c)) {
                record_op(c, false);
                buf_.erase(0, 1);
                continue;
            }
            if (buf_.size() < need) return;
            if (static_cast<std::uint8_t>(buf_[need - 1]) != stk::kCrcEop) {
                buf_.erase(0, 1);
                continue;
            }
            record_op(c, true);
            std::string reply(1, '\x14');
            if (c == stk::kReadSign) reply += "\x1E\x95\x0F";
            if (c == stk::kGetParameter) {
                const std::uint8_t p = static_cast<std::uint8_t>(buf_[1]);
                reply += static_cast<char>(p == stk::kSwMajor ? 8 : p == stk::kSwMinor ? 0 : 3);
            }
            reply += '\x10';
            say(reply, 1, 115200);
            buf_.erase(0, need);
            boot_until_ = t + (c == stk::kLeaveProgmode ? 20 : boot_ms_);
        }
    }

private:
    int boot_ms_;
    std::int64_t boot_until_ = 0, next_ = 0;
    int n_ = 0;
    std::string buf_;
};

/* ---------------------------------------------------------------- MicroPython / CircuitPython on a Pico */

class PythonBoard : public Board {
public:
    explicit PythonBoard(bool circuit) : Board(Wiring::None, 115200), circuit_(circuit) {}

protected:
    bool any_speed() const override { return true; }
    void power(std::int64_t t) override { next_ = t + 100; }

    void tick(std::int64_t t) override
    {
        if (repl_ || t < next_) return;
        next_ = t + 400;
        say("temp: 21." + std::to_string(++n_ % 10) + "\r\n");
    }

    void bytes(const std::string &in, std::int64_t t) override
    {
        (void)t;
        for (const char c : in) {
            if (c == '\x03') {
                if (!repl_) {
                    say(std::string("Traceback (most recent call last):\r\n  File \"") + (circuit_ ? "code.py" : "main.py") +
                        "\", line 7, in <module>\r\nKeyboardInterrupt: \r\n");
                    if (circuit_) say("\r\nCode done running.\r\n\r\nPress any key to enter the REPL. Use CTRL-D to reload.\r\n");
                    else say(banner());
                    repl_ = true;
                    waiting_key_ = circuit_;
                } else {
                    say("\r\n>>> ");
                }
            } else if (c == '\x02') {
                if (repl_) {
                    say("\r\n" + banner());
                    waiting_key_ = false;
                }
            } else if (repl_ && waiting_key_) {
                say("\r\n" + banner());
                waiting_key_ = false;
            } else if (repl_) {
                say(std::string(1, c)); /* the REPL echoes */
            }
        }
    }

private:
    std::string banner() const
    {
        if (circuit_) return "Adafruit CircuitPython 9.0.0 on 2024-03-19; Raspberry Pi Pico with rp2040\r\n>>> ";
        return "MicroPython v1.22.2 on 2024-02-22; Raspberry Pi Pico with RP2040\r\nType \"help()\" for more "
               "information.\r\n>>> ";
    }

    bool circuit_;
    bool repl_ = false, waiting_key_ = false;
    std::int64_t next_ = 0;
    int n_ = 0;
};

/* ---------------------------------------------------------------- a u-blox GPS */

class GpsBoard : public Board {
public:
    explicit GpsBoard(int period) : Board(Wiring::None, 9600), period_(period) {}

protected:
    void power(std::int64_t t) override { next_ = t + 20; }

    void tick(std::int64_t t) override
    {
        if (t < next_) return;
        next_ = t + period_;
        std::string burst;
        ++n_;
        burst += nmea("GNTXT,01,01,02,u-blox AG - www.u-blox.com") + nmea("GNTXT,01,01,02,HW UBX-M8030 00080000") +
                     nmea("GNTXT,01,01,02,FWVER=SPG 3.01") + nmea("GNTXT,01,01,02,PROTVER=18.00") +
                     nmea("GNTXT,01,01,02,MOD=NEO-M8N-0");
        burst += nmea("GNRMC,123519.00,A,4807.03812,N,01131.00012,E,0.012,,131026,,,A") +
                 nmea("GNGGA,123519.00,4807.03812,N,01131.00012,E,1,08,1.01,545.4,M,46.9,M,,") +
                 nmea("GNGSA,A,3,10,32,23,24,12,15,,,,,,,1.80,1.01,1.49") +
                 nmea("GPGSV,2,1,07,10,63,137,17,12,30,071,33,15,27,295,22,23,31,151,36");
        say(burst);
    }

private:
    int period_;
    std::int64_t next_ = 0;
    int n_ = 0;
};

/* ---------------------------------------------------------------- line-based firmwares: GRBL, Marlin, an AT modem */

class LineBoard : public Board {
public:
    LineBoard(Wiring w, int baud) : Board(w, baud) {}

protected:
    void bytes(const std::string &in, std::int64_t t) override
    {
        if (port_baud() != baud()) return; /* at a wrong speed the board reads garbage */
        for (const char c : in) {
            if (echo_) say(std::string(1, c));
            if (c == '\r' || c == '\n') {
                if (!line_.empty()) answer(line_, t);
                line_.clear();
            } else {
                line_ += c;
            }
        }
    }
    virtual void answer(const std::string &line, std::int64_t t) = 0;
    bool echo_ = false;

private:
    std::string line_;
};

class GrblBoard : public LineBoard {
public:
    GrblBoard() : LineBoard(Wiring::Arduino, 115200) {}

protected:
    void reset(std::int64_t t, bool download) override
    {
        (void)t;
        (void)download;
        quiet();
        say("\r\nGrbl 1.1h ['$' for help]\r\n", 300);
    }
    void answer(const std::string &line, std::int64_t t) override
    {
        (void)t;
        if (line == "$I") say("[VER:1.1h.20190825:]\r\n[OPT:V,15,128]\r\nok\r\n");
        else if (line == "?") say("<Idle|MPos:0.000,0.000,0.000|FS:0,0>\r\n");
        else say("ok\r\n");
    }
};

class MarlinBoard : public LineBoard {
public:
    explicit MarlinBoard(int baud) : LineBoard(Wiring::Arduino, baud) {}

protected:
    void reset(std::int64_t t, bool download) override
    {
        (void)download;
        quiet();
        say("start\r\necho:Marlin 2.1.2.1\r\n\r\necho: Last Updated: 2023-07-22 | Author: (none, default config)\r\n"
            "echo:Compiled: Jul 22 2023\r\necho: Free Memory: 2463  PlannerBufferBytes: 1168\r\n",
            300);
        next_ = t + 800;
        started_ = true;
    }
    void tick(std::int64_t t) override
    {
        if (!started_ || t < next_) return;
        next_ = t + 1000;
        say("T:21.50 /0.00 B:20.80 /0.00 @:0 B@:0\r\n");
    }
    void answer(const std::string &line, std::int64_t t) override
    {
        (void)t;
        if (line == "M115")
            say("FIRMWARE_NAME:Marlin 2.1.2.1 (Jul 22 2023 12:00:00) SOURCE_CODE_URL:github.com/MarlinFirmware/Marlin "
                "PROTOCOL_VERSION:1.0 MACHINE_TYPE:Ender-3 V2 EXTRUDER_COUNT:1 "
                "UUID:cede2a2f-41a2-4748-9b12-c55c62f367ff\r\nCap:SERIAL_XON_XOFF:0\r\nCap:AUTOREPORT_TEMP:1\r\nok\r\n");
        else say("ok\r\n");
    }

private:
    std::int64_t next_ = 0;
    bool started_ = false;
};

class AtBoard : public LineBoard {
public:
    AtBoard() : LineBoard(Wiring::None, 115200) { echo_ = true; }

protected:
    void answer(const std::string &line, std::int64_t t) override
    {
        (void)t;
        if (line == "AT") say("\r\nOK\r\n");
        else if (line == "ATI") say("\r\nSIM800 R14.18\r\n\r\nOK\r\n");
        else say("\r\nERROR\r\n");
    }
};

class SilentBoard : public Board {
public:
    SilentBoard() : Board(Wiring::Esp, 115200) {}
};

/* the LineControl a probe gets for one open: closing the port drops the lines, as Linux does */
class OpenLines : public LineControl {
public:
    OpenLines(std::shared_ptr<FakeLines> f, Board *board) : f_(std::move(f)), board_(board) {}
    ~OpenLines() override
    {
        Lines l;
        if (board_ && f_->get(l)) board_->lines(Lines{false, false});
    }
    bool get(Lines &out) override { return f_->get(out); }
    bool set(const Lines &lines) override { return f_->set(lines); }
    void note_wait(int ms) override { f_->note_wait(ms); }

private:
    std::shared_ptr<FakeLines> f_;
    Board *board_;
};

} // namespace

/* ---------------------------------------------------------------- Board */

void Board::lines(const Lines &l)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(l);
}

void Board::apply_lines(const Lines &l)
{
    const Lines prev = last_;
    last_ = l;
    if (wiring_ == Wiring::Esp) {
        const bool en_low = l.rts && !l.dtr, was_low = prev.rts && !prev.dtr;
        if (en_low && !was_low) {
            mode_ = Mode::Held; /* in reset: silent */
            quiet();
        } else if (!en_low && was_low) {
            ++resets_;
            const bool download = l.dtr && !l.rts; /* IO0 low as EN rises */
            mode_ = download ? Mode::Bootloader : Mode::Running;
            reset(now_, download);
        }
    } else if (wiring_ == Wiring::Arduino && l.dtr && !prev.dtr) {
        ++resets_;
        reset(now_, false);
    }
}

void Board::step(int master, std::int64_t now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    now_ = now;
    port_baud_ = port_speed(master);
    if (!powered_) {
        powered_ = true;
        power(now);
    }
    std::vector<Lines> lines;
    lines.swap(pending_);
    for (const Lines &l : lines) apply_lines(l);
    char buf[512];
    const ssize_t n = read(master, buf, sizeof(buf));
    if (n > 0) {
        const std::string in(buf, static_cast<std::size_t>(n));
        received_ += in;
        if (mode_ != Mode::Held) bytes(in, now);
    }
    if (mode_ != Mode::Held) tick(now);
    while (!out_.empty() && out_.front().due <= now) {
        const Out o = out_.front();
        out_.pop_front();
        const int b = o.baud ? o.baud : baud_;
        std::string text = o.text;
        if (!any_speed() && port_baud_ != b) /* a UART at the wrong speed: noise, as many bytes */
            for (char &c : text) {
                seed_ = seed_ * 1103515245u + 12345u;
                c = static_cast<char>((seed_ >> 16) & 0xFF);
            }
        ssize_t w = write(master, text.data(), text.size());
        (void)w;
    }
}

void Board::say(const std::string &text, int delay_ms, int baud)
{
    const std::int64_t due = (last_due_ > now_ ? last_due_ : now_) + delay_ms;
    last_due_ = due;
    out_.push_back(Out{due, text, baud});
}

void Board::quiet()
{
    out_.clear();
    last_due_ = now_;
}

void Board::record_op(int op, bool allowed)
{
    ops_.push_back(op);
    if (!allowed) forbidden_.push_back(op);
}

void Board::set_mode(Mode m)
{
    mode_ = m;
}

std::string Board::received() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return received_;
}

std::vector<int> Board::ops() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ops_;
}

std::vector<int> Board::forbidden() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return forbidden_;
}

Mode Board::mode() const
{
    return mode_; /* read by the board itself under the lock, by tests after the probe ended */
}

int Board::resets() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return resets_;
}

std::unique_ptr<Board> make_board(const std::string &name, const std::string &variant)
{
    if (name == "esp32") return std::make_unique<EspBoard>(variant == "s3");
    if (name == "esp8266") return std::make_unique<Esp8266Board>(variant == "wdt");
    if (name == "arduino") return std::make_unique<ArduinoBoard>(variant.empty() ? 1000 : std::atoi(variant.c_str()));
    if (name == "micropython") return std::make_unique<PythonBoard>(variant == "circuitpython");
    if (name == "gps") return std::make_unique<GpsBoard>(variant.empty() ? 1000 : std::atoi(variant.c_str()));
    if (name == "grbl") return std::make_unique<GrblBoard>();
    if (name == "marlin") return std::make_unique<MarlinBoard>(variant.empty() ? 115200 : std::atoi(variant.c_str()));
    if (name == "at") return std::make_unique<AtBoard>();
    return std::make_unique<SilentBoard>();
}

std::string nmea(const std::string &body)
{
    std::uint8_t sum = 0;
    for (const char c : body) sum ^= static_cast<std::uint8_t>(c);
    char tail[8];
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", sum);
    return "$" + body + tail;
}

/* ---------------------------------------------------------------- Host */

Host::Host(std::unique_ptr<Board> board) : board_(std::move(board))
{
    master_ = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master_ < 0) return;
    char name[128];
    if (grantpt(master_) != 0 || unlockpt(master_) != 0 || ptsname_r(master_, name, sizeof(name)) != 0) {
        close(master_);
        master_ = -1;
        return;
    }
    path_ = name;
    fcntl(master_, F_SETFL, fcntl(master_, F_GETFL) | O_NONBLOCK);
    slave_ = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC); /* held: the pty stays up between opens */
    if (slave_ >= 0) configure_port(slave_, 300, Frame());
    thread_ = std::thread([this] { run(); });
}

Host::~Host()
{
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (slave_ >= 0) close(slave_);
    if (master_ >= 0) close(master_);
}

void Host::run()
{
    while (!stop_.load()) {
        board_->step(master_, now_ms());
        sleep_ms(3);
    }
}

LinesFactory Host::factory(bool supported)
{
    return [this, supported](int fd) -> std::unique_ptr<LineControl> {
        (void)fd;
        auto f = std::make_shared<FakeLines>(Lines{true, true}, supported);
        Board *b = board_.get();
        f->set_listener([b](const Lines &l) { b->lines(l); });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_ = f;
            ++opens_;
        }
        if (supported) b->lines(Lines{true, true}); /* opening the port raises DTR and RTS */
        return std::unique_ptr<LineControl>(new OpenLines(f, supported ? b : nullptr));
    };
}

std::shared_ptr<FakeLines> Host::last_lines() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
}

int Host::opens() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return opens_;
}

} // namespace sim
} // namespace sc
