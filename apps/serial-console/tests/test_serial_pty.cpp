/*
 * SPDX-License-Identifier: MIT
 *
 * Tests of the serial I/O on pseudo-terminals (core/serial_io, core/demo): the slave side is the "port", a feeder
 * thread on the master side plays the board. It sends text only while the port is at the board's speed (it reads the
 * slave's speed from the master, as the kernel reports it) and noise at any other, like a real UART at a wrong speed.
 * Checks: open and configure (74880 included), receive, send, the log file, a flood faster than the UI drains it (no
 * byte lost in the log), an unplug and the reconnect, a refused open, the speed finder (found, silent, cancelled) and
 * the demonstration world.
 */
#include "check.hpp"

#include "demo.hpp"
#include "fsutil.hpp"
#include "serial_io.hpp"
#include "term.hpp"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <thread>

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

/* Waits up to ms for cond. */
bool wait_for(const std::function<bool()> &cond, int ms)
{
    const std::int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        if (cond()) return true;
        sleep_ms(10);
    }
    return cond();
}

/* A board on the master side: text at `baud`, noise at any other speed, every period_ms; collects what it receives. */
class Feeder {
public:
    Feeder(int master, int baud, std::string text, int period_ms = 40)
        : master_(master), baud_(baud), text_(std::move(text)), period_(period_ms)
    {
        thread_ = std::thread([this] { run(); });
    }
    ~Feeder() { stop(); }
    void stop()
    {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }
    std::string received()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }
    std::atomic<bool> silent{false};

private:
    void run()
    {
        std::uint32_t seed = 99;
        while (!stop_.load()) {
            char buf[256];
            const ssize_t n = read(master_, buf, sizeof(buf));
            if (n > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                received_.append(buf, static_cast<std::size_t>(n));
            }
            if (!silent.load()) {
                std::string out;
                if (port_speed(master_) == baud_) {
                    out = text_;
                } else {
                    for (int i = 0; i < 16; ++i) {
                        seed = seed * 1103515245u + 12345u;
                        out += static_cast<char>((seed >> 16) & 0xFF);
                    }
                }
                ssize_t w = write(master_, out.data(), out.size());
                (void)w;
            }
            sleep_ms(period_);
        }
    }
    int master_, baud_;
    std::string text_;
    int period_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::string received_;
    std::thread thread_;
};

std::string drain(Session &s)
{
    std::string out;
    s.take(out, 1 << 20);
    return out;
}

void test_session_basics(const std::string &dir)
{
    demo::Pty pty;
    CHECK(demo::open_pty(pty));
    const std::string link = dir + "/ttyFAKE0"; /* the "port" path: a link to the slave, re-pointed at a re-plug */
    CHECK(fs::make_link(pty.path, link));

    Feeder feeder(pty.master, 9600, "hello at 9600\r\n");
    SessionOptions o;
    o.path = link;
    o.baud = 9600;
    o.log = true;
    o.log_path = dir + "/logs/ttyFAKE0-test.log";
    o.retry_ms = 100;
    Session s(o);
    s.start();
    CHECK(wait_for([&] { return s.status().state == LinkState::Open; }, 2000));
    CHECK_EQ(port_speed(pty.master), 9600); /* the master reports the speed the session set on its slave */
    CHECK(wait_for([&] { return s.status().rx > 30; }, 2000));
    const std::string got = drain(s);
    CHECK(got.find("hello at 9600\r\n") != std::string::npos);

    s.send("ping\r");
    CHECK(wait_for([&] { return feeder.received().find("ping\r") != std::string::npos; }, 2000));
    CHECK(wait_for([&] { return s.status().tx == 5; }, 1000));
    CHECK(s.status().log_on);

    // the log holds every byte received, whatever the display took
    feeder.silent.store(true);
    sleep_ms(150);
    drain(s);
    const SessionStatus before = s.status();
    std::string log = fs::read_all(o.log_path);
    CHECK_EQ(static_cast<std::uint64_t>(log.size()), before.rx);

    // unplugged: the master goes, the session sees it and waits
    feeder.stop();
    demo::close_pty(pty);
    CHECK(wait_for([&] { return s.status().state == LinkState::Lost; }, 2000));
    s.send("lost\r"); /* dropped, not queued for later */

    // plugged again (a new pseudo-terminal behind the same path): it reconnects by itself
    demo::Pty again;
    CHECK(demo::open_pty(again));
    CHECK(fs::make_link(again.path, link));
    CHECK(wait_for([&] { return s.status().state == LinkState::Open; }, 3000));
    CHECK_EQ(s.status().reconnects, 1);
    CHECK_EQ(port_speed(again.master), 9600); /* the same settings */
    Feeder feeder2(again.master, 9600, "back\r\n");
    CHECK(wait_for([&] { std::string t = drain(s); return t.find("back") != std::string::npos; }, 2000));
    CHECK(feeder2.received().find("lost") == std::string::npos);

    // log off then on again: a new file
    s.set_log(false, "");
    CHECK(wait_for([&] { return !s.status().log_on; }, 1000));
    s.set_log(true, dir + "/logs/second.log");
    CHECK(wait_for([&] { return s.status().log_on; }, 1000));
    CHECK(wait_for([&] { return fs::read_all(dir + "/logs/second.log").find("back") != std::string::npos; }, 2000));

    // a pseudo-terminal has no modem lines: the reset says so
    s.reset_board();
    CHECK(wait_for([&] { return s.status().note.find("No DTR") != std::string::npos; }, 1000));
    feeder2.stop();
    s.stop();
    CHECK(s.status().state == LinkState::Closed && !s.status().log_on);
    demo::close_pty(again);
}

void test_flood(const std::string &dir)
{
    // a flood (921600 baud is about 92 kB/s; this sends 2 MB as fast as the pseudo-terminal takes it) and a slow UI:
    // the display skips, the log keeps everything
    demo::Pty pty;
    CHECK(demo::open_pty(pty));
    SessionOptions o;
    o.path = pty.path;
    o.baud = 921600;
    o.log = true;
    o.log_path = dir + "/logs/flood.log";
    o.ui_cap = 64 * 1024;
    Session s(o);
    s.start();
    CHECK(wait_for([&] { return s.status().state == LinkState::Open; }, 2000));
    std::string block;
    for (int i = 0; block.size() < 4096; ++i) block += "flood line " + std::to_string(i) + " 0123456789abcdef\r\n";
    std::uint64_t sent = 0;
    std::uint64_t shown = 0;
    TermBuffer term;
    const std::int64_t start = now_ms();
    while (sent < 2u * 1024u * 1024u && now_ms() - start < 20000) {
        const ssize_t n = write(pty.master, block.data(), block.size());
        if (n > 0) sent += static_cast<std::uint64_t>(n);
        else sleep_ms(1);
        if ((sent / 4096) % 64 == 0) { /* the UI comes now and then and takes a bounded chunk */
            std::string chunk;
            shown += s.take(chunk, 16 * 1024);
            term.feed(chunk);
        }
    }
    CHECK(wait_for([&] { return s.status().rx == sent; }, 5000));
    std::string rest;
    shown += s.take(rest, 1 << 22);
    const SessionStatus st = s.status();
    CHECK_EQ(shown + st.ui_dropped, sent);
    CHECK(st.ui_dropped > 0);
    s.stop();
    struct stat sb;
    CHECK(stat(o.log_path.c_str(), &sb) == 0 && static_cast<std::uint64_t>(sb.st_size) == sent);
    demo::close_pty(pty);
}

void test_refusals(const std::string &dir)
{
    // a node the user may not open, a file that is not a terminal
    const std::string locked = dir + "/locked";
    fs::write_file(locked, "");
    chmod(locked.c_str(), 0000);
    int err = 0;
    if (geteuid() != 0) {
        CHECK(open_port(locked, &err) < 0 && err == EACCES);
        SessionOptions o;
        o.path = locked;
        Session s(o);
        s.start();
        CHECK(wait_for([&] { return s.status().state == LinkState::Failed; }, 2000));
        CHECK_EQ(s.status().error, EACCES);
        SpeedFinder f(locked, Frame(), 115200, false, 100);
        f.start();
        CHECK(wait_for([&] { return f.progress().done; }, 2000));
        CHECK_EQ(f.progress().error, EACCES);
    }
    const std::string plain = dir + "/plain";
    fs::write_file(plain, "x");
    CHECK(open_port(plain, &err) < 0 && err == ENOTTY);
    SessionOptions gone;
    gone.path = dir + "/nothing";
    gone.retry_ms = 50;
    Session s(gone);
    s.start();
    CHECK(wait_for([&] { return s.status().state == LinkState::Lost; }, 2000)); /* not there: it waits for it */
    s.stop();
}

void test_finder()
{
    // text at 57600 only: the recommended 115200 and 9600 give noise, 57600 is found third
    {
        demo::Pty pty;
        CHECK(demo::open_pty(pty));
        Feeder feeder(pty.master, 57600, "uptime 123 s, load 0.12\r\n", 30);
        SpeedFinder f(pty.path, Frame(), 115200, false, 400);
        f.start();
        CHECK(f.progress().running);
        CHECK(wait_for([&] { return f.progress().done; }, 10000));
        const FinderProgress p = f.progress();
        CHECK(p.result.found);
        CHECK_EQ(p.result.baud, 57600);
        CHECK_EQ(p.index, 2);
        CHECK_EQ(p.result.message, std::string("Found 57600 - text looks clean"));
        feeder.stop();
        demo::close_pty(pty);
    }
    // 74880 (no B constant: termios2) is set for real
    {
        demo::Pty pty;
        CHECK(demo::open_pty(pty));
        Feeder feeder(pty.master, 74880, "ets Jan  8 2013,rst cause:2, boot mode:(3,6)\r\n", 30);
        SpeedFinder f(pty.path, Frame(), 74880, false, 300);
        f.start();
        CHECK(wait_for([&] { return f.progress().done; }, 5000));
        CHECK(f.progress().result.found && f.progress().result.baud == 74880);
        feeder.stop();
        demo::close_pty(pty);
    }
    // a silent board: nothing found, the advice
    {
        demo::Pty pty;
        CHECK(demo::open_pty(pty));
        SpeedFinder f(pty.path, Frame(), 9600, false, 60);
        f.start();
        CHECK(wait_for([&] { return f.progress().done; }, 10000));
        CHECK(!f.progress().result.found);
        CHECK(f.progress().result.message.find("No readable text") != std::string::npos);
        demo::close_pty(pty);
    }
    // cancelled while listening: it stops at once
    {
        demo::Pty pty;
        CHECK(demo::open_pty(pty));
        SpeedFinder f(pty.path, Frame(), 9600, true, 700);
        f.start();
        sleep_ms(150);
        const std::int64_t t0 = now_ms();
        f.cancel();
        CHECK(wait_for([&] { return f.progress().done; }, 1000));
        CHECK(now_ms() - t0 < 400);
        CHECK_EQ(f.progress().result.message, std::string("Stopped"));
        demo::close_pty(pty);
    }
}

void test_demo_world()
{
    {
        demo::World w("boards");
        CHECK(w.ok());
        const std::vector<PortInfo> ports = scan_ports(w.roots(), true);
        CHECK_EQ(ports.size(), static_cast<std::size_t>(4)); /* ttyACM0, ttyUSB0, ttyUSB1, ttyAMA0 */
        std::string esp, ftdi;
        for (const PortInfo &p : ports) {
            if (p.tty == "ttyUSB0") esp = p.path;
            if (p.tty == "ttyUSB1") ftdi = p.path;
        }
        CHECK(!esp.empty() && !ftdi.empty());
        SpeedFinder f(ftdi, Frame(), 115200, false, 500);
        f.start();
        CHECK(wait_for([&] { return f.progress().done; }, 10000));
        CHECK(f.progress().result.found && f.progress().result.baud == 57600);

        SessionOptions o;
        o.path = esp;
        o.baud = 115200;
        Session s(o);
        s.start();
        TermBuffer term;
        bool banner = false;
        wait_for([&] {
            std::string chunk;
            s.take(chunk, 1 << 16);
            term.feed(chunk);
            for (std::uint64_t i = term.first(); i < term.end(); ++i)
                banner = banner || term.line(i) == "I (29) boot: ESP-IDF v5.1.2 2nd stage bootloader";
            return banner;
        }, 3000);
        CHECK(banner);
        s.send("\r");
        CHECK(wait_for([&] {
            std::string chunk;
            s.take(chunk, 1 << 16);
            term.feed(chunk);
            for (std::uint64_t i = term.first(); i < term.end(); ++i)
                if (term.line(i).compare(0, 7, "esp32> ") == 0) return true;
            return false;
        }, 2000));
    }
    {
        demo::World w("noaccess");
        CHECK(w.ok() && w.no_group());
        int err = 0;
        if (geteuid() != 0) CHECK(open_port(w.roots().dev + "/ttyUSB0", &err) < 0 && err == EACCES);
    }
    {
        demo::World w("lost");
        const std::string id = "usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0";
        SessionOptions o;
        o.id = id;
        o.path = resolve_port(w.roots(), id);
        o.roots = w.roots();
        o.baud = 115200;
        o.retry_ms = 200;
        Session s(o);
        s.start();
        CHECK(wait_for([&] { return s.status().state == LinkState::Open; }, 2000));
        CHECK(wait_for([&] { return s.status().state == LinkState::Lost; }, 5000));
        CHECK(resolve_port(w.roots(), id).empty());
        std::string old;
        s.take(old, 1 << 20); /* what came before the unplug (taken now: the banner may start as soon as it reopens) */
        CHECK(wait_for([&] { return s.status().state == LinkState::Open; }, 6000));
        CHECK_EQ(s.status().reconnects, 1);
        TermBuffer term; /* the board came back at its speed: its banner again, as after a power-up */
        CHECK(wait_for([&] {
            std::string chunk;
            s.take(chunk, 1 << 16);
            term.feed(chunk);
            for (std::uint64_t i = term.first(); i < term.end(); ++i)
                if (term.line(i) == "ets Jun  8 2016 00:22:57") return true;
            return false;
        }, 2000));
    }
    {
        demo::World w("empty");
        CHECK(scan_ports(w.roots(), false).empty());
        CHECK(wait_for([&] { return scan_ports(w.roots(), false).size() == 1; }, 5000)); /* hot-plug */
    }
    CHECK(demo::World::from_env() == nullptr || std::getenv("SERIAL_CONSOLE_DEMO"));
}

} // namespace

void test_serial_pty()
{
    const std::string dir = test_tmpdir("pty");
    test_session_basics(dir);
    test_flood(dir);
    test_refusals(dir);
    test_finder();
    test_demo_world();
    fs::remove_tree(dir);
}
