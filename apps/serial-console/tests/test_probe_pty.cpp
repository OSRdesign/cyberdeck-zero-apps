/*
 * SPDX-License-Identifier: MIT
 *
 * Tests of the probes of "What is on the other end?" (core/probe) against the simulated boards of core/sim on
 * pseudo-terminals: an ESP32 and an ESP32-S3 (ROM banner and ESP-IDF boot on a reset, the esptool protocol in their
 * boot loader), an ESP8266 (its ROM at 74880; in a reboot loop: garbage at 115200), an Arduino Uno with optiboot
 * (STK500 after a DTR reset), a Raspberry Pi Pico with MicroPython or CircuitPython, a u-blox GPS (NMEA at 9600),
 * GRBL, Marlin, a SIM800 modem and a silent board. A pseudo-terminal has no modem lines: the probe gets the
 * simulation's FakeLines (the board's reset circuit sees them), whose trace the tests read.
 *
 * The safety rules are checked here: Listen sends nothing and touches no line; Reset and the Ask probes do nothing
 * without the confirmation; the boot loaders only see read-only commands; the lines are always put back; a board
 * taken into its boot loader is always reset back into its program, even when the probe is cancelled.
 */
#include "check.hpp"

#include "probe.hpp"
#include "serial_io.hpp"
#include "sim.hpp"

#include <time.h>

#include <algorithm>
#include <cerrno>
#include <functional>

using namespace sc;

namespace {

void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

ProbeTimes fast()
{
    ProbeTimes t;
    t.listen_ms = 450;
    t.first_listen_ms = 900;
    t.native_listen_ms = 900;
    t.boot_ms = 1300;
    t.rom_ms = 700;
    t.manual_ms = 600;
    t.answer_ms = 900;
    t.settle_ms = 20;
    t.reopen_ms = 200;
    return t;
}

struct Run {
    ProbeProgress p;
    std::string trace;
    std::vector<Lines> history;
    Lines final_lines;
};

/* Runs one probe to its end against the host's board (with its simulated lines unless lines == false). */
Run probe(sim::Host &host, Probe which, int baud, Family usb, bool native, bool confirmed = true, bool lines = true,
          ResetStyle style = ResetStyle::AutoReset, const std::function<void(Prober &)> &during = nullptr)
{
    ProbeRequest req;
    req.probe = which;
    req.target.path = host.path();
    req.target.baud = baud;
    req.target.style = style;
    req.target.ctx.usb = usb;
    req.target.ctx.native_usb = native;
    req.times = fast();
    req.confirmed = confirmed;
    req.lines = host.factory(lines);
    Prober prober(req);
    prober.start();
    if (during) during(prober);
    const std::int64_t end = now_ms() + 30000;
    while (!prober.progress().done && now_ms() < end) sleep_ms(20);
    Run r;
    r.p = prober.progress();
    if (const auto f = host.last_lines()) {
        r.trace = f->trace();
        r.history = f->history();
        r.final_lines = f->state();
    }
    return r;
}

bool only(const std::vector<int> &ops, std::initializer_list<int> allowed)
{
    return std::all_of(ops.begin(), ops.end(), [&allowed](int op) {
        return std::find(allowed.begin(), allowed.end(), op) != allowed.end();
    });
}

const Lines kOpen{true, true};

void test_esp32()
{
    // level 1: the ESP-IDF log of a running board; nothing sent, no line touched, no reset
    {
        sim::Host host(sim::make_board("esp32"));
        CHECK(host.ok());
        const Run r = probe(host, Probe::Listen, 115200, Family::EspBridge, false);
        CHECK(r.p.done && !r.p.error && !r.p.cancelled);
        CHECK(r.p.result.kind == Kind::EspIdf && r.p.result.baud == 115200);
        CHECK(r.p.result.level == 1 && r.p.result.probe == "listen" && r.p.result.when > 0);
        CHECK(host.board().received().empty());
        CHECK(r.history.empty() && r.trace.empty());
        CHECK_EQ(host.board().resets(), 0);
    }
    // level 2: the reset, the ROM banner and the ESP-IDF boot; nothing sent; the lines put back
    {
        sim::Host host(sim::make_board("esp32"));
        const Run r = probe(host, Probe::Reset, 115200, Family::EspBridge, false);
        const Identity &id = r.p.result;
        CHECK(id.kind == Kind::EspIdf);
        CHECK_EQ(id.chip, std::string("ESP32"));
        CHECK_EQ(id.version, std::string("v5.1.2"));
        CHECK_EQ(id.detail, std::string("project weather-station 1.4.2"));
        CHECK(id.confidence >= 70 && id.baud == 115200 && id.level == 2);
        CHECK(!id.boot.empty() && id.boot[0] == "ets Jun  8 2016 00:22:57");
        CHECK_EQ(r.trace, std::string("D0R1 w100 D0R0 w150 D1R1 w50"));
        CHECK(r.final_lines == kOpen);
        CHECK(host.board().received().empty());
        CHECK_EQ(host.board().resets(), 1);
        CHECK(host.board().mode() == sim::Mode::Running);
    }
    // level 2 is never run without the confirmation: the port is not even opened
    {
        sim::Host host(sim::make_board("esp32"));
        const Run r = probe(host, Probe::Reset, 115200, Family::EspBridge, false, false);
        CHECK(r.p.done && r.p.error == EPERM && r.p.result.kind == Kind::None);
        CHECK_EQ(host.opens(), 0);
        CHECK_EQ(host.board().resets(), 0);
        CHECK(host.board().received().empty());
    }
    // level 3, the ESP ROM boot loader of a classic ESP32: GET_SECURITY_INFO is not there, the magic value is
    {
        sim::Host host(sim::make_board("esp32"));
        const Run r = probe(host, Probe::EspRom, 115200, Family::EspBridge, false);
        const Identity &id = r.p.result;
        CHECK_EQ(id.chip, std::string("ESP32"));
        CHECK(id.detail == "MAC 24:0a:c4:12:34:56");
        CHECK(id.kind == Kind::EspIdf); /* the program it went back to says the rest */
        CHECK_EQ(id.confidence, 100);
        CHECK(only(host.board().ops(), {0x08, 0x0A, 0x14}) && host.board().forbidden().empty());
        CHECK(!host.board().ops().empty());
        CHECK(host.board().mode() == sim::Mode::Running); /* not left in the boot loader */
        CHECK(r.trace.compare(0, 33, "D0R0 D1R1 D0R1 w100 D1R0 w50 D0R0") == 0);
        CHECK(r.trace.find("D0R1 w100 D0R0", 33) != std::string::npos); /* the reset back into the program */
        CHECK(r.final_lines == kOpen);
        CHECK(r.p.note.find("reset into its program") != std::string::npos);
    }
    // level 3 on an ESP32-S3: its chip id from GET_SECURITY_INFO, its MAC
    {
        sim::Host host(sim::make_board("esp32", "s3"));
        const Run r = probe(host, Probe::EspRom, 115200, Family::EspBridge, false);
        CHECK_EQ(r.p.result.chip, std::string("ESP32-S3"));
        CHECK_EQ(r.p.result.detail, std::string("MAC f4:12:fa:aa:bb:cc"));
        CHECK(only(host.board().ops(), {0x08, 0x0A, 0x14}) && host.board().forbidden().empty());
        const std::vector<int> ops = host.board().ops();
        CHECK(std::find(ops.begin(), ops.end(), 0x14) != ops.end());
        CHECK(host.board().mode() == sim::Mode::Running);
    }
    // cancelled while it enters the boot loader: the board is still reset into its program, the lines put back
    {
        sim::Host host(sim::make_board("esp32"));
        const Run r = probe(host, Probe::EspRom, 115200, Family::EspBridge, false, true, true, ResetStyle::AutoReset,
                            [](Prober &p) {
                                sleep_ms(60);
                                p.cancel();
                            });
        CHECK(r.p.cancelled && r.p.result.kind == Kind::None);
        sleep_ms(50); /* the board's thread sees the last line change */
        CHECK(host.board().mode() == sim::Mode::Running);
        CHECK(r.final_lines == kOpen);
        CHECK(r.trace.find("D0R1 w100 D0R0") != std::string::npos);
    }
    // no confirmation: nothing at all
    {
        sim::Host host(sim::make_board("esp32"));
        const Run r = probe(host, Probe::EspRom, 115200, Family::EspBridge, false, false);
        CHECK(r.p.error == EPERM && host.opens() == 0 && host.board().ops().empty());
    }
}

void test_esp8266()
{
    // level 1, a board in a reboot loop: garbage at 115200 and 9600, its ROM clean at 74880
    {
        sim::Host host(sim::make_board("esp8266", "wdt"));
        const Run r = probe(host, Probe::Listen, 115200, Family::EspBridge, false);
        CHECK(r.p.result.kind == Kind::Esp8266 && r.p.result.baud == 74880 && r.p.result.chip == "ESP8266");
        CHECK(host.board().received().empty() && r.history.empty());
    }
    // level 2: a garbled start at 115200, so a second reset and 74880: the ROM there, the program at 115200
    {
        sim::Host host(sim::make_board("esp8266"));
        const Run r = probe(host, Probe::Reset, 115200, Family::EspBridge, false);
        CHECK(r.p.result.kind == Kind::Esp8266 && r.p.result.baud == 115200);
        CHECK_EQ(host.board().resets(), 2);
        CHECK_EQ(r.trace, std::string("D0R1 w100 D0R0 w150 D1R1 w50 D0R1 w100 D0R0 w150 D1R1 w50"));
        CHECK(r.final_lines == kOpen && host.board().received().empty());
    }
}

void test_arduino()
{
    // level 1 at 9600: opening the port restarts it (DTR), the sketch's text; the USB id says Arduino
    {
        sim::Host host(sim::make_board("arduino", "300"));
        const Run r = probe(host, Probe::Listen, 9600, Family::Arduino, false);
        CHECK(r.p.result.kind == Kind::Sketch && r.p.result.baud == 9600);
        CHECK(host.board().received().empty() && r.history.empty());
    }
    // level 3: the optiboot boot loader after a DTR reset; read-only commands; the sketch runs again after
    {
        sim::Host host(sim::make_board("arduino", "300"));
        const Run r = probe(host, Probe::Stk500, 9600, Family::Arduino, false);
        const Identity &id = r.p.result;
        CHECK(id.kind == Kind::ArduinoBoot && id.chip == "ATmega328P");
        CHECK(id.firmware == "Optiboot" && id.version == "8.0" && id.baud == 115200);
        CHECK(only(host.board().ops(), {0x30, 0x41, 0x75, 0x51}) && host.board().forbidden().empty());
        const std::vector<int> ops = host.board().ops();
        CHECK(!ops.empty() && ops.back() == 0x51); /* it left the boot loader */
        CHECK(r.trace.compare(0, 18, "D0R0 w250 D1R1 w50") == 0);
        CHECK(r.final_lines == kOpen);
        sleep_ms(100);
        CHECK(host.board().mode() == sim::Mode::Running);
        CHECK(!identity_recommendation(id, nullptr, nullptr)); /* the boot loader's speed is not the sketch's */
    }
    // a board that is not an Arduino (silent): nobody answers, said so; nothing but sync frames sent
    {
        sim::Host host(sim::make_board("silent"));
        const Run r = probe(host, Probe::Stk500, 9600, Family::Bridge, false);
        CHECK(!r.p.result.found());
        CHECK(r.p.note.find("No STK500") != std::string::npos);
        CHECK(host.board().received().find(std::string("\x30\x20")) != std::string::npos);
        CHECK(r.final_lines == kOpen);
    }
}

void test_python()
{
    // level 3: exactly Ctrl-C then Ctrl-B, nothing else; the banner names the board
    {
        sim::Host host(sim::make_board("micropython"));
        const Run r = probe(host, Probe::MicroPython, 115200, Family::NativeUsb, true, true, true, ResetStyle::None);
        const Identity &id = r.p.result;
        CHECK(id.kind == Kind::MicroPython && id.version == "v1.22.2");
        CHECK(id.board == "Raspberry Pi Pico" && id.chip == "RP2040");
        CHECK_EQ(host.board().received(), std::string("\x03\x02"));
        CHECK(r.history.empty() && host.board().resets() == 0);
    }
    {
        sim::Host host(sim::make_board("micropython", "circuitpython"));
        const Run r = probe(host, Probe::MicroPython, 115200, Family::NativeUsb, true, true, true, ResetStyle::None);
        CHECK(r.p.result.kind == Kind::CircuitPython && r.p.result.version == "9.0.0");
        CHECK_EQ(host.board().received(), std::string("\x03\x02"));
    }
    // level 1 on its own USB: one listen (the speed does not matter), its program's output
    {
        sim::Host host(sim::make_board("micropython"));
        const Run r = probe(host, Probe::Listen, 115200, Family::NativeUsb, true);
        CHECK(r.p.result.kind == Kind::Text && host.board().received().empty());
    }
    // level 2 on a board without a reset line: nothing is pulsed, it says so and listens
    {
        sim::Host host(sim::make_board("micropython"));
        const Run r = probe(host, Probe::Reset, 115200, Family::NativeUsb, true, true, true, ResetStyle::None);
        CHECK(r.history.empty() && r.p.note.find("no reset line") != std::string::npos);
        CHECK(host.board().received().empty());
    }
}

void test_text_firmwares()
{
    {
        sim::Host host(sim::make_board("gps", "300"));
        const Run r = probe(host, Probe::Listen, 115200, Family::Bridge, false);
        const Identity &id = r.p.result;
        CHECK(id.kind == Kind::Gnss && id.baud == 9600 && id.chip == "u-blox NEO-M8N-0");
        CHECK(host.board().received().empty() && r.history.empty());
    }
    {
        sim::Host host(sim::make_board("grbl"));
        Run r = probe(host, Probe::Listen, 115200, Family::Arduino, false);
        CHECK(r.p.result.kind == Kind::Grbl && r.p.result.version == "1.1h");
        CHECK(host.board().received().empty());
        r = probe(host, Probe::Grbl, 115200, Family::Arduino, false);
        CHECK(r.p.result.kind == Kind::Grbl);
        CHECK_EQ(host.board().received(), std::string("$I\n"));
        bool ver = false;
        for (const std::string &e : r.p.result.evidence) ver = ver || e.find("[VER:1.1h") != std::string::npos;
        CHECK(ver);
    }
    {
        sim::Host host(sim::make_board("marlin"));
        Run r = probe(host, Probe::Listen, 115200, Family::Arduino, false);
        CHECK(r.p.result.kind == Kind::Marlin && r.p.result.version == "2.1.2.1");
        r = probe(host, Probe::Marlin, 115200, Family::Arduino, false);
        CHECK(r.p.result.kind == Kind::Marlin && r.p.result.board == "Ender-3 V2");
        CHECK_EQ(host.board().received(), std::string("M115\n"));
    }
    {
        sim::Host host(sim::make_board("at"));
        Run r = probe(host, Probe::Listen, 115200, Family::EspBridge, false);
        CHECK(r.p.result.kind == Kind::Silent && host.board().received().empty());
        CHECK(r.p.note.find("Nothing came") != std::string::npos);
        r = probe(host, Probe::At, 115200, Family::EspBridge, false);
        CHECK(r.p.result.kind == Kind::AtModem && r.p.result.chip == "SIM800");
        CHECK_EQ(host.board().received(), std::string("AT\rATI\r"));
        CHECK(r.history.empty());
    }
}

void test_silent_and_stops()
{
    {
        sim::Host host(sim::make_board("silent"));
        Run r = probe(host, Probe::Listen, 115200, Family::EspBridge, false);
        CHECK(r.p.result.kind == Kind::Silent && !r.p.error && host.board().received().empty());
        r = probe(host, Probe::Reset, 115200, Family::EspBridge, false);
        CHECK(r.p.result.kind == Kind::Silent && r.final_lines == kOpen && host.board().received().empty());
        CHECK_EQ(r.trace, std::string("D0R1 w100 D0R0 w150 D1R1 w50"));
    }
    // Esc during Listen: it stops at once
    {
        sim::Host host(sim::make_board("silent"));
        std::int64_t asked = 0;
        const Run r = probe(host, Probe::Listen, 115200, Family::EspBridge, false, true, true, ResetStyle::AutoReset,
                            [&asked](Prober &p) {
                                sleep_ms(150);
                                asked = now_ms();
                                p.cancel();
                            });
        CHECK(r.p.cancelled && r.p.text == "Stopped" && now_ms() - asked < 1000);
    }
    // Esc during Reset and listen, while EN is held low: the lines come back, the board is not left in reset
    {
        sim::Host host(sim::make_board("esp32"));
        const Run r = probe(host, Probe::Reset, 115200, Family::EspBridge, false, true, true, ResetStyle::AutoReset,
                            [](Prober &p) {
                                sleep_ms(40);
                                p.cancel();
                            });
        CHECK(r.p.cancelled && r.final_lines == kOpen);
        sleep_ms(50);
        CHECK(host.board().mode() == sim::Mode::Running);
    }
    // a port without modem lines (a pseudo-terminal's real lines): Reset and listen says so and only listens
    {
        sim::Host host(sim::make_board("esp32"));
        ProbeRequest req;
        req.probe = Probe::Reset;
        req.target.path = host.path();
        req.target.ctx.usb = Family::EspBridge;
        req.times = fast();
        req.confirmed = true; /* no req.lines: TtyLines on the pseudo-terminal, which has no DTR / RTS */
        Prober prober(req);
        prober.start();
        const std::int64_t end = now_ms() + 10000;
        while (!prober.progress().done && now_ms() < end) sleep_ms(20);
        CHECK(prober.progress().note.find("No DTR/RTS lines") != std::string::npos);
        CHECK(prober.progress().result.kind == Kind::EspIdf); /* its log goes on while it listens */
    }
    // the port cannot be opened: the error, no result
    {
        ProbeRequest req;
        req.probe = Probe::Listen;
        req.target.path = "/nonexistent/ttyUSB9";
        req.times = fast();
        Prober prober(req);
        prober.start();
        const std::int64_t end = now_ms() + 5000;
        while (!prober.progress().done && now_ms() < end) sleep_ms(10);
        CHECK(prober.progress().error == ENOENT && prober.progress().result.kind == Kind::None);
    }
}

void test_probe_texts()
{
    PortInfo esp;
    esp.usb = true;
    esp.vid = 0x10c4;
    esp.pid = 0xea60;
    esp.known = lookup_device(esp.vid, esp.pid);
    CHECK(reset_style(esp) == ResetStyle::AutoReset && !native_usb(esp));
    PortInfo pico = esp;
    pico.vid = 0x2e8a;
    pico.pid = 0x0005;
    pico.known = lookup_device(pico.vid, pico.pid);
    CHECK(reset_style(pico) == ResetStyle::None && native_usb(pico));
    PortInfo jtag = esp;
    jtag.vid = 0x303a;
    jtag.pid = 0x1001;
    jtag.known = lookup_device(jtag.vid, jtag.pid);
    CHECK(reset_style(jtag) == ResetStyle::UsbJtag);
    PortInfo leonardo = esp;
    leonardo.vid = 0x2341;
    leonardo.pid = 0x8036;
    leonardo.known = lookup_device(leonardo.vid, leonardo.pid);
    CHECK(reset_style(leonardo) == ResetStyle::None);

    ProbeTarget t = probe_target(esp, 9600, Frame(), Roots());
    const std::vector<int> speeds = listen_speeds(t);
    CHECK(speeds.front() == 9600 && std::find(speeds.begin(), speeds.end(), 74880) != speeds.end());
    CHECK(std::find(speeds.begin(), speeds.end(), 1200) == speeds.end()); /* never the 1200-baud touch */
    t.baud = 1200;
    CHECK(listen_speeds(t).front() == 115200);
    ProbeTarget n = probe_target(pico, 1200, Frame(), Roots());
    CHECK(listen_speeds(n) == std::vector<int>({115200})); /* its own USB: one speed, never 1200 */

    // what each dialog lists
    CHECK(probe_sends_text(Probe::Listen).empty());
    CHECK(probe_sends_text(Probe::MicroPython).find("Ctrl-C (03)") != std::string::npos);
    CHECK(probe_sends_text(Probe::MicroPython).find("Ctrl-B (02)") != std::string::npos);
    CHECK(probe_sends_text(Probe::At).find("41 54 0D") != std::string::npos);
    CHECK(probe_sends_text(Probe::Marlin).find("M115") != std::string::npos);
    CHECK(probe_sends_text(Probe::Grbl).find("24 49 0A") != std::string::npos);
    CHECK(probe_sends_text(Probe::Stk500).find("30 20") != std::string::npos);
    CHECK(probe_sends_text(Probe::EspRom).find("never written or erased") != std::string::npos);
    CHECK(probe_resets(Probe::EspRom, ResetStyle::AutoReset) && !probe_resets(Probe::At, ResetStyle::AutoReset));
    CHECK(!probe_resets(Probe::Reset, ResetStyle::None));
    for (const Probe p : ask_probes()) {
        CHECK_EQ(probe_level(p), 3);
        CHECK(probe_from_key(probe_key(p)) == p);
        CHECK(std::string(probe_name(p)).size() <= 20); /* a row with its value at 640x480 */
    }
    CHECK(probe_from_key("nonsense") == Probe::Listen);
}

} // namespace

void test_probe_pty()
{
    test_probe_texts();
    test_esp32();
    test_esp8266();
    test_arduino();
    test_python();
    test_text_firmwares();
    test_silent_and_stops();
}
