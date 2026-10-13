/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the probes of "What is on the other end?" (see probe.hpp).
 */
#include "probe.hpp"

#include "baud.hpp"
#include "bootproto.hpp"
#include "serial_io.hpp"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <functional>

namespace sc {
namespace {

void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

bool contains(const std::string &hay, const char *needle)
{
    return hay.find(needle) != std::string::npos;
}

std::string hex_list(const std::string &bytes)
{
    std::string out;
    char buf[4];
    for (const char c : bytes) {
        std::snprintf(buf, sizeof(buf), "%02X", static_cast<unsigned char>(c));
        if (!out.empty()) out += ' ';
        out += buf;
    }
    return out;
}

} // namespace

/* ---------------------------------------------------------------- the open port of a probe */

struct Prober::Port {
    Prober &owner;
    const ProbeTarget &t;
    int fd = -1;
    int baud = 0;
    std::unique_ptr<LineControl> lines;
    bool gone = false;

    Port(Prober &o, const ProbeTarget &target) : owner(o), t(target) {}
    ~Port() { close_port(fd); }

    bool cancelled() const { return owner.cancel_.load(); }

    int open(int speed)
    {
        int err = 0;
        fd = open_port(t.path, &err);
        if (fd < 0) return err ? err : EIO;
        if (!configure(speed)) return errno ? errno : EIO;
        lines = owner.req_.lines ? owner.req_.lines(fd) : std::unique_ptr<LineControl>(new TtyLines(fd));
        return 0;
    }

    bool configure(int speed)
    {
        baud = speed;
        return configure_port(fd, speed, t.frame);
    }

    void flush() { tcflush(fd, TCIFLUSH); }

    /* A board whose USB port went (its reset re-enumerates it): wait for the same port id, open it again. */
    bool reopen()
    {
        close_port(fd);
        fd = -1;
        const std::int64_t end = now_ms() + owner.req_.times.reopen_ms;
        while (now_ms() < end && !cancelled()) {
            const std::string path = t.id.empty() ? t.path : resolve_port(t.roots, t.id);
            if (!path.empty()) {
                int err = 0;
                fd = open_port(path, &err);
                if (fd >= 0 && configure(baud)) return true;
                close_port(fd);
                fd = -1;
            }
            sleep_ms(100);
        }
        return false;
    }

    /* Reads for ms (or until done(bytes so far) says so); appends to out. false: cancelled or the port went. */
    bool read_for(int ms, std::string &out, const std::function<bool(const std::string &)> &done = nullptr)
    {
        const std::int64_t end = now_ms() + ms;
        char buf[1024];
        while (!cancelled()) {
            if (done && done(out)) return true;
            const std::int64_t left = end - now_ms();
            if (left <= 0) return true;
            if (fd < 0) return false;
            struct pollfd p;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            const int ready = poll(&p, 1, static_cast<int>(left < 50 ? left : 50));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) continue;
            if (p.revents & POLLIN) {
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) {
                    if (out.size() < 256 * 1024) out.append(buf, static_cast<std::size_t>(n));
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            } else if (!(p.revents & (POLLHUP | POLLERR | POLLNVAL))) {
                continue;
            }
            // hung up: a board that re-enumerated at its reset comes back; else the port is gone
            if (!reopen()) {
                gone = !cancelled();
                return false;
            }
        }
        return false;
    }

    bool send(const std::string &bytes)
    {
        std::size_t done = 0;
        const std::int64_t end = now_ms() + 1000;
        while (done < bytes.size() && now_ms() < end && fd >= 0) {
            const ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
            if (n > 0) {
                done += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno != EAGAIN && errno != EINTR) return false;
            struct pollfd p;
            p.fd = fd;
            p.events = POLLOUT;
            p.revents = 0;
            poll(&p, 1, 20);
        }
        return done == bytes.size();
    }

    /* the steps, reading what comes while it waits (a boot starts printing at once) */
    bool steps(const std::vector<LineStep> &s, std::string *keep)
    {
        if (!lines) return false;
        return run_steps(*lines, s, [this, keep](int ms) {
            std::string scratch;
            return read_for(ms, keep ? *keep : scratch);
        });
    }
};

/* ---------------------------------------------------------------- probe texts */

int probe_level(Probe p)
{
    switch (p) {
    case Probe::Listen: return 1;
    case Probe::Reset: return 2;
    default: return 3;
    }
}

const char *probe_name(Probe p)
{
    switch (p) {
    case Probe::Listen: return "Listen";
    case Probe::Reset: return "Reset and listen";
    case Probe::MicroPython: return "MicroPython";
    case Probe::At: return "AT modem";
    case Probe::Marlin: return "Marlin printer";
    case Probe::Grbl: return "GRBL controller";
    case Probe::Stk500: return "Arduino boot loader";
    case Probe::EspRom: return "ESP boot loader";
    }
    return "";
}

const char *probe_key(Probe p)
{
    switch (p) {
    case Probe::Listen: return "listen";
    case Probe::Reset: return "reset";
    case Probe::MicroPython: return "micropython";
    case Probe::At: return "at";
    case Probe::Marlin: return "marlin";
    case Probe::Grbl: return "grbl";
    case Probe::Stk500: return "stk500";
    case Probe::EspRom: return "esp-rom";
    }
    return "";
}

Probe probe_from_key(const std::string &key)
{
    for (const Probe p : {Probe::Listen, Probe::Reset, Probe::MicroPython, Probe::At, Probe::Marlin, Probe::Grbl,
                          Probe::Stk500, Probe::EspRom})
        if (key == probe_key(p)) return p;
    return Probe::Listen;
}

std::string probe_sends_text(Probe p)
{
    switch (p) {
    case Probe::Listen: return std::string();
    case Probe::Reset: return "Nothing is sent: the DTR and RTS lines are pulsed (RTS 100 ms with DTR off, then DTR on), "
                              "then the lines are put back as they were.";
    case Probe::MicroPython:
        return "Sends Ctrl-C (03), which stops the program running on the board, then Ctrl-B (02), which prints the "
               "REPL banner. Nothing else is sent and no code runs. The board stays at its >>> prompt: press its "
               "reset button (or Ctrl-D in the terminal) to start its program again.";
    case Probe::At: return "Sends AT and CR (41 54 0D), then ATI and CR (41 54 49 0D), and reads the answers.";
    case Probe::Marlin: return "Sends M115 and LF (4D 31 31 35 0A): the printer answers its firmware name and version. "
                               "Nothing moves or heats.";
    case Probe::Grbl: return "Sends $I and LF (24 49 0A): GRBL answers its version and build options. Nothing moves.";
    case Probe::Stk500:
        return "Resets the board with DTR (its program restarts), then asks its boot loader: get sync (30 20), read "
               "signature (75 20), version (41 81 20, 41 82 20), leave (51 20); a Mega: sign on and read signature "
               "(STK500 v2). Read-only: nothing is written or erased. The board then runs its program again.";
    case Probe::EspRom:
        return "Restarts the board into its ROM boot loader (DTR/RTS), then sends SYNC, GET_SECURITY_INFO and READ_REG "
               "of the chip id register (40001000) and the MAC (esptool protocol). Read-only: the flash is never "
               "written or erased. Then the board is reset into its program.";
    }
    return std::string();
}

bool probe_resets(Probe p, ResetStyle style)
{
    switch (p) {
    case Probe::Reset:
    case Probe::Stk500:
    case Probe::EspRom: return style != ResetStyle::None;
    default: return false;
    }
}

const std::vector<Probe> &ask_probes()
{
    static const std::vector<Probe> list = {Probe::MicroPython, Probe::At,     Probe::Marlin,
                                            Probe::Grbl,        Probe::Stk500, Probe::EspRom};
    return list;
}

bool native_usb(const PortInfo &port)
{
    if (!port.known) return false;
    switch (port.known->family) {
    case Family::NativeUsb:
    case Family::Esp:
    case Family::Cli: return true;
    case Family::Gnss: return port.vid == 0x1546; /* u-blox's own USB */
    case Family::Arduino: return port.pid == 0x8036 || port.pid == 0x003e || port.pid == 0x0069;
    default: return false;
    }
}

ResetStyle reset_style(const PortInfo &port)
{
    if (port.vid == 0x303a && port.pid == 0x1001) return ResetStyle::UsbJtag;
    if (native_usb(port)) return ResetStyle::None;
    if (port.vid == 0x2341 && (port.pid == 0x0058 || port.pid == 0x1002)) return ResetStyle::None; /* 1200-baud touch */
    return ResetStyle::AutoReset;
}

ProbeTarget probe_target(const PortInfo &port, int baud, const Frame &frame, const Roots &roots)
{
    ProbeTarget t;
    t.path = port.path;
    t.id = port.id;
    t.roots = roots;
    t.baud = baud;
    t.frame = frame;
    t.style = reset_style(port);
    t.ctx.usb = port.known ? port.known->family : Family::Unknown;
    t.ctx.native_usb = native_usb(port);
    return t;
}

std::vector<int> listen_speeds(const ProbeTarget &t)
{
    const int first = t.baud > 0 && t.baud != 1200 ? t.baud : kDefaultBaud;
    if (t.ctx.native_usb) return {first};
    std::vector<int> order = {first};
    for (const int s : {115200, 9600, 74880, 57600, 38400, 19200, 230400, 250000, 460800, 921600, 4800})
        if (std::find(order.begin(), order.end(), s) == order.end()) order.push_back(s);
    return order;
}

/* ---------------------------------------------------------------- Prober */

Prober::Prober(ProbeRequest request) : req_(std::move(request)) {}

Prober::~Prober()
{
    cancel();
    if (thread_.joinable()) thread_.join();
}

void Prober::start()
{
    if (thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.running = true;
        progress_.text = req_.probe == Probe::Listen ? "Listening..." : "Starting...";
    }
    thread_ = std::thread([this] { run(); });
}

void Prober::cancel()
{
    cancel_.store(true);
}

ProbeProgress Prober::progress() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

void Prober::set_text(const std::string &text)
{
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.text = text;
}

void Prober::set_note(const std::string &note)
{
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.note = note;
}

void Prober::run()
{
    Identity result;
    int error = 0;
    // safety: a probe that resets the board or sends anything runs only when the user confirmed it
    if (probe_level(req_.probe) >= 2 && !req_.confirmed) {
        error = EPERM;
        set_note("Not confirmed: nothing was done");
    } else {
        Port port(*this, req_.target);
        const int first = req_.probe == Probe::Listen ? listen_speeds(req_.target).front()
                          : req_.probe == Probe::EspRom || req_.probe == Probe::Stk500 ? 115200
                          : req_.target.baud;
        error = port.open(first);
        if (!error) {
            switch (req_.probe) {
            case Probe::Listen: result = listen(port); break;
            case Probe::Reset: result = reset_listen(port); break;
            case Probe::Stk500: result = ask_stk500(port); break;
            case Probe::EspRom: result = ask_esp(port); break;
            default: result = ask_text(port, req_.probe); break;
            }
            if (port.gone) error = ENODEV;
        }
    }
    const bool cancelled = cancel_.load();
    if (cancelled || error) result = Identity();
    result.level = probe_level(req_.probe);
    result.probe = probe_key(req_.probe);
    result.when = std::time(nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.running = false;
    progress_.done = true;
    progress_.cancelled = cancelled;
    progress_.error = cancelled ? 0 : error;
    progress_.result = result;
    progress_.text = cancelled ? "Stopped" : error == ENODEV ? "The port went away" : "Done";
}

/* ---- level 1: listen at the candidate speeds, sending nothing */

Identity Prober::listen(Port &port)
{
    const std::vector<int> speeds = listen_speeds(req_.target);
    const int window = req_.target.ctx.native_usb ? req_.times.native_listen_ms : req_.times.listen_ms;
    std::vector<Capture> caps;
    std::size_t heard = 0;
    int listened_ms = 0;
    for (std::size_t i = 0; i < speeds.size() && !cancel_.load(); ++i) {
        set_text("Listening at " + std::to_string(speeds[i]) + " (" + std::to_string(i + 1) + "/" +
                 std::to_string(speeds.size()) + ")");
        if (port.baud != speeds[i] && !port.configure(speeds[i])) break;
        std::string scratch;
        port.read_for(req_.times.settle_ms, scratch); /* a character cut by the speed change */
        port.flush();
        Capture c;
        c.baud = speeds[i];
        const int ms = i == 0 && !req_.target.ctx.native_usb ? std::max(window, req_.times.first_listen_ms) : window;
        if (!port.read_for(ms, c.bytes)) break;
        listened_ms += ms;
        heard += c.bytes.size();
        caps.push_back(c);
        const Identity now = classify(caps, req_.target.ctx);
        // conclusive: a family found well enough, or clean text (the right speed), or frames that validated
        const TextScore ts = score_text(c.bytes);
        if ((now.found() && now.kind != Kind::Text && now.kind != Kind::Binary && now.confidence >= 40) ||
            looks_clean(ts) || (!now.framing.empty() && now.kind != Kind::None && now.confidence >= 60))
            break;
        if (i >= 2 && heard == 0) break; /* silent: the speed does not change silence */
    }
    if (cancel_.load()) return Identity();
    Identity id = classify(caps, req_.target.ctx);
    if (id.kind == Kind::Silent)
        set_note("Nothing came in " + std::to_string((listened_ms + 500) / 1000) +
                 " s: it may print only when it starts");
    return id;
}

/* ---- level 2: reset, then listen to the boot */

Identity Prober::reset_listen(Port &port)
{
    const ProbeTarget &t = req_.target;
    LineGuard guard(*port.lines);
    Capture cap;
    cap.baud = port.baud;
    port.flush();
    const bool manual = t.style == ResetStyle::None || !guard.supported();
    if (manual) {
        set_note(t.style == ResetStyle::None ? "This board's own USB has no reset line: you pressed its reset button"
                                             : "No DTR/RTS lines on this port: you pressed the board's reset button");
        set_text("Press the reset button now");
        port.read_for(req_.times.manual_ms, cap.bytes);
    } else {
        set_text("Resetting (DTR/RTS)...");
        port.steps(run_reset_steps(guard.original(), t.style), &cap.bytes);
        set_text("Listening " + std::to_string(req_.times.boot_ms / 1000) + " s at " + std::to_string(port.baud));
        port.read_for(req_.times.boot_ms, cap.bytes);
    }
    if (cancel_.load() || port.gone) return Identity();
    std::vector<Capture> caps = {cap};
    Identity id = classify(caps, t.ctx);
    // an ESP8266: its ROM prints at 74880, garbage at the program's speed: listen to it there after a second reset
    const bool esp_adapter = t.ctx.usb == Family::EspBridge || t.ctx.usb == Family::Bridge ||
                             t.ctx.usb == Family::Unknown;
    const std::string head = cap.bytes.substr(0, std::min<std::size_t>(cap.bytes.size(), 300));
    const bool garbled_start = cap.bytes.size() >= 16 && score_text(head).score < 0.6;
    if (!manual && esp_adapter && !t.ctx.native_usb && garbled_start && port.baud != 74880 &&
        id.kind != Kind::EspIdf && id.kind != Kind::EspRom && id.kind != Kind::ArduinoEsp32) {
        set_text("Trying 74880 (ESP8266)");
        Capture rom;
        rom.baud = 74880;
        if (port.configure(74880)) {
            port.flush();
            port.steps(run_reset_steps(guard.original(), t.style), &rom.bytes);
            port.read_for(req_.times.rom_ms, rom.bytes);
            caps.push_back(rom);
            id = classify(caps, t.ctx);
        }
    }
    guard.restore();
    return id;
}

/* ---- level 3: a question in text (MicroPython, AT, Marlin, GRBL) */

Identity Prober::ask_text(Port &port, Probe p)
{
    LineGuard guard(*port.lines); /* nothing changes them; put back all the same */
    Capture cap;
    cap.baud = port.baud;
    const auto ends_with_ok = [](const std::string &s) {
        return contains(s, "OK\r") || contains(s, "OK\n") || contains(s, "ERROR") || contains(s, "ok\n") ||
               contains(s, "ok\r");
    };
    switch (p) {
    case Probe::MicroPython:
        port.read_for(300, cap.bytes);
        set_text("Sending Ctrl-C...");
        port.send(std::string(1, '\x03'));
        port.read_for(500, cap.bytes);
        set_text("Sending Ctrl-B...");
        port.send(std::string(1, '\x02'));
        {
            const std::size_t from = cap.bytes.size();
            port.read_for(req_.times.answer_ms, cap.bytes,
                          [from](const std::string &s) { return s.find(">>>", from) != std::string::npos; });
            port.read_for(100, cap.bytes);
        }
        break;
    case Probe::At:
        set_text("Sending AT...");
        port.flush();
        port.send("AT\r");
        port.read_for(req_.times.answer_ms, cap.bytes, ends_with_ok);
        set_text("Sending ATI...");
        {
            const std::size_t from = cap.bytes.size();
            port.send("ATI\r");
            port.read_for(req_.times.answer_ms, cap.bytes, [from, &ends_with_ok](const std::string &s) {
                return ends_with_ok(s.substr(from));
            });
        }
        break;
    case Probe::Marlin:
        set_text("Waiting for its start...");
        port.read_for(req_.times.answer_ms, cap.bytes); /* opening the port restarted it (DTR): its banner */
        set_text("Sending M115...");
        {
            const std::size_t from = cap.bytes.size();
            port.send("M115\n");
            port.read_for(req_.times.answer_ms + req_.times.answer_ms / 2, cap.bytes, [from](const std::string &s) {
                return s.find("FIRMWARE_NAME", from) != std::string::npos && s.find("\nok", from) != std::string::npos;
            });
        }
        break;
    case Probe::Grbl:
        set_text("Waiting for its start...");
        port.read_for(req_.times.answer_ms, cap.bytes);
        set_text("Sending $I...");
        {
            const std::size_t from = cap.bytes.size();
            port.send("$I\n");
            port.read_for(req_.times.answer_ms, cap.bytes,
                          [from](const std::string &s) { return s.find("ok", from) != std::string::npos; });
        }
        break;
    default: break;
    }
    guard.restore();
    if (cancel_.load() || port.gone) return Identity();
    return classify({cap}, req_.target.ctx);
}

/* ---- level 3: the Arduino boot loader (STK500), read-only */

Identity Prober::ask_stk500(Port &port)
{
    const ProbeTarget &t = req_.target;
    LineGuard guard(*port.lines);
    const bool manual = t.style == ResetStyle::None || !guard.supported();
    if (manual)
        set_note(guard.supported() ? "This board has no DTR reset: you pressed its reset button"
                                   : "No DTR line on this port: you pressed the board's reset button");
    Identity id;
    std::string seen;
    for (const int speed : {115200, 57600}) {
        if (cancel_.load()) break;
        set_text("Boot loader at " + std::to_string(speed) + "...");
        if (!port.configure(speed)) break;
        if (manual) set_text("Press reset now (" + std::to_string(speed) + ")");
        else port.steps(arduino_reset_steps(), nullptr);
        port.flush();
        bool synced = false;
        std::string got;
        const int tries = manual ? 30 : 8;
        for (int i = 0; i < tries && !synced && !cancel_.load(); ++i) {
            port.send(stk::frame(stk::kGetSync));
            port.read_for(120, got, [](const std::string &s) { return stk::parse_reply(s, 0, nullptr); });
            synced = stk::parse_reply(got, 0, nullptr);
        }
        seen += got;
        if (!synced) continue;
        port.read_for(50, got);
        port.flush();
        std::string sig, major, minor, reply;
        port.send(stk::frame(stk::kReadSign));
        port.read_for(300, reply, [](const std::string &s) { return stk::parse_reply(s, 3, nullptr); });
        stk::parse_reply(reply, 3, &sig);
        reply.clear();
        port.send(stk::frame(stk::kGetParameter, std::string(1, static_cast<char>(stk::kSwMajor))));
        port.read_for(300, reply, [](const std::string &s) { return stk::parse_reply(s, 1, nullptr); });
        stk::parse_reply(reply, 1, &major);
        reply.clear();
        port.send(stk::frame(stk::kGetParameter, std::string(1, static_cast<char>(stk::kSwMinor))));
        port.read_for(300, reply, [](const std::string &s) { return stk::parse_reply(s, 1, nullptr); });
        stk::parse_reply(reply, 1, &minor);
        reply.clear();
        port.send(stk::frame(stk::kLeaveProgmode)); /* optiboot then starts the sketch */
        port.read_for(200, reply, [](const std::string &s) { return stk::parse_reply(s, 0, nullptr); });
        id.kind = Kind::ArduinoBoot;
        id.title = kind_title(id.kind);
        id.confidence = 100;
        id.baud = speed;
        if (sig.size() == 3) {
            id.chip = avr_chip_name(static_cast<std::uint8_t>(sig[0]), static_cast<std::uint8_t>(sig[1]),
                                    static_cast<std::uint8_t>(sig[2]));
            id.evidence.push_back("signature " + hex_list(sig) + (id.chip.empty() ? "" : " = " + id.chip));
        }
        if (major.size() == 1) {
            const int ma = static_cast<unsigned char>(major[0]);
            const int mi = minor.size() == 1 ? static_cast<unsigned char>(minor[0]) : 0;
            id.version = std::to_string(ma) + "." + std::to_string(mi);
            id.firmware = (ma >= 4 && ma <= 10) ? "Optiboot" : "STK500 boot loader";
        } else {
            id.firmware = "STK500 boot loader";
        }
        id.evidence.insert(id.evidence.begin(), "STK500 v1 in sync at " + std::to_string(speed) + " (14 10)");
        id.detail = "boot loader at " + std::to_string(speed); /* the sketch has its own speed */
        break;
    }
    if (id.kind == Kind::None && !cancel_.load()) { /* an Arduino Mega: STK500 v2 at 115200 */
        set_text("Boot loader (STK500 v2)...");
        if (port.configure(115200)) {
            if (!manual) port.steps(arduino_reset_steps(), nullptr);
            port.flush();
            std::string got, body;
            std::uint8_t seq = 0;
            bool on = false;
            for (int i = 0; i < 6 && !on && !cancel_.load(); ++i) {
                port.send(stk2::message(static_cast<std::uint8_t>(i + 1), std::string(1, static_cast<char>(stk2::kSignOn))));
                port.read_for(150, got, [](const std::string &s) { return stk2::parse_message(s, nullptr, nullptr); });
                on = stk2::parse_message(got, &seq, &body) && body.size() >= 3 && static_cast<unsigned char>(body[0]) ==
                     stk2::kSignOn && body[1] == 0;
            }
            if (on) {
                const std::string name = body.size() > 3 ? body.substr(3) : std::string();
                std::string sig;
                for (int k = 0; k < 3; ++k) {
                    std::string reply, b;
                    port.send(stk2::message(static_cast<std::uint8_t>(10 + k), stk2::read_signature_body(k)));
                    port.read_for(300, reply, [](const std::string &s) { return stk2::parse_message(s, nullptr, nullptr); });
                    if (stk2::parse_message(reply, nullptr, &b) && b.size() >= 3) sig += b[2];
                }
                std::string leave;
                leave += static_cast<char>(stk2::kLeaveProgmodeIsp);
                leave += static_cast<char>(1);
                leave += static_cast<char>(1);
                std::string reply;
                port.send(stk2::message(20, leave));
                port.read_for(200, reply, [](const std::string &s) { return stk2::parse_message(s, nullptr, nullptr); });
                id.kind = Kind::ArduinoBoot;
                id.title = kind_title(id.kind);
                id.confidence = 100;
                id.baud = 115200;
                id.firmware = "STK500 v2 boot loader";
                id.version = name;
                id.evidence.push_back("STK500 v2 sign-on: " + name);
                if (sig.size() == 3) {
                    id.chip = avr_chip_name(static_cast<std::uint8_t>(sig[0]), static_cast<std::uint8_t>(sig[1]),
                                            static_cast<std::uint8_t>(sig[2]));
                    id.evidence.push_back("signature " + hex_list(sig) + (id.chip.empty() ? "" : " = " + id.chip));
                }
                id.detail = "boot loader at 115200";
            }
            seen += got;
        }
    }
    guard.restore();
    if (cancel_.load() || port.gone) return Identity();
    if (id.kind == Kind::None) {
        id.kind = Kind::Silent;
        id.title = "No boot loader answered";
        set_note("No STK500 boot loader answered at 115200 or 57600");
        if (!seen.empty()) id.evidence.push_back("got " + hex_list(seen.substr(0, 16)));
    }
    return id;
}

/* ---- level 3: the ESP ROM boot loader (esptool protocol), read-only */

Identity Prober::ask_esp(Port &port)
{
    const ProbeTarget &t = req_.target;
    LineGuard guard(*port.lines);
    const bool manual = t.style == ResetStyle::None || !guard.supported();
    std::string banner;

    // one command, one answer: stop as soon as it is there
    const auto ask = [&](const std::string &packet, std::uint8_t op, int ms, esprom::Response &r) {
        if (packet.empty()) return false;
        port.send(slip::encode(packet));
        std::string got;
        bool ok = false;
        port.read_for(ms, got, [&](const std::string &s) {
            slip::Decoder d;
            d.feed(s);
            std::string pk;
            esprom::Response tmp;
            while (d.next(pk))
                if (esprom::parse_response(pk, tmp) && tmp.op == op) {
                    r = tmp;
                    ok = true;
                }
            return ok;
        });
        if (!ok) banner += got;
        return ok;
    };

    bool synced = false;
    bool entered = false; /* the board may be in its boot loader: it must be reset into its program */
    if (manual) {
        set_note(t.style == ResetStyle::None ? "This board's USB has no reset line: you held BOOT and pressed RESET"
                                             : "No DTR/RTS lines: you held BOOT and pressed RESET");
        set_text("Hold BOOT, tap RESET");
        const std::int64_t end = now_ms() + req_.times.manual_ms;
        while (!synced && !cancel_.load() && now_ms() < end) {
            esprom::Response r;
            synced = ask(esprom::sync_packet(), esprom::kSync, 150, r);
        }
    } else {
        for (int attempt = 0; attempt < 2 && !synced && !cancel_.load(); ++attempt) {
            set_text("Entering the ESP ROM...");
            port.flush();
            entered = true;
            port.steps(esp_bootloader_steps(t.style, attempt == 0 ? 50 : 550), &banner);
            if (cancel_.load()) break;
            set_text("Syncing (esptool)...");
            port.read_for(100, banner); /* "waiting for download" */
            for (int i = 0; i < 5 && !synced && !cancel_.load(); ++i) {
                esprom::Response r;
                synced = ask(esprom::sync_packet(), esprom::kSync, 120, r);
            }
        }
    }
    Identity id;
    if (synced && !cancel_.load()) {
        std::string drain;
        port.read_for(100, drain); /* the ROM answers a SYNC several times */
        set_text("Asking the chip...");
        std::string chip;
        esprom::Response r;
        std::uint32_t chip_id = 0;
        if (ask(esprom::security_info_packet(), esprom::kGetSecurityInfo, 300, r) && esprom::security_chip_id(r, &chip_id))
            chip = esprom::chip_from_id(chip_id);
        if (chip.empty() && ask(esprom::read_reg_packet(esprom::kChipDetectMagicReg), esprom::kReadReg, 300, r)) {
            chip = esprom::chip_from_magic(r.value);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "magic 0x%08X at 0x40001000", r.value);
            id.evidence.push_back(buf);
        } else if (!chip.empty()) {
            id.evidence.push_back("GET_SECURITY_INFO chip id " + std::to_string(chip_id));
        }
        std::vector<std::uint32_t> values;
        for (const std::uint32_t reg : esprom::mac_registers(chip)) {
            if (!ask(esprom::read_reg_packet(reg), esprom::kReadReg, 300, r)) break;
            values.push_back(r.value);
        }
        const std::string mac = esprom::mac_from_registers(chip, values);
        id.kind = Kind::EspRom;
        id.title = kind_title(id.kind);
        id.chip = chip.empty() ? "ESP (unknown chip)" : chip;
        id.confidence = chip.empty() ? 60 : 100;
        id.detail = mac.empty() ? "answered its ROM boot loader" : "MAC " + mac;
        id.baud = 115200;
        id.evidence.insert(id.evidence.begin(), "ROM boot loader in sync (SLIP, esptool protocol)");
        if (!mac.empty()) id.evidence.push_back("MAC " + mac);
    }
    // always: out of the boot loader into the program, then hear it start
    std::string boot;
    if (entered || synced) {
        if (!manual) {
            set_text("Back to its program...");
            /* not skipped by Esc: the board must not stay in its boot loader (what it prints waits in the port) */
            run_steps(*port.lines, esp_hard_reset_steps(t.style), [](int ms) {
                sleep_ms(ms);
                return true;
            });
            set_note("The board was reset into its program");
            if (!cancel_.load()) {
                set_text("Listening to it start...");
                port.read_for(req_.times.boot_ms / 2, boot);
            }
        } else {
            set_note("Press the board's RESET button to start its program again");
        }
    }
    guard.restore();
    if (cancel_.load() || port.gone) return Identity();
    if (!boot.empty()) {
        Identity app = classify({Capture{boot, 115200}}, t.ctx);
        if (id.kind == Kind::None) return app;
        if (app.found() && app.kind != Kind::EspRom) {
            /* the program is the better answer; the chip and MAC come from the ROM */
            Identity merged = app;
            merged.chip = id.chip;
            merged.detail = id.detail;
            merged.confidence = 100;
            for (const std::string &e : id.evidence) merged.evidence.insert(merged.evidence.end(), e);
            if (merged.evidence.size() > kEvidenceMax) merged.evidence.resize(kEvidenceMax);
            return merged;
        }
        id.boot = app.boot;
    }
    if (id.kind == Kind::None) {
        id.kind = Kind::Silent;
        id.title = "No ESP boot loader answered";
        if (!banner.empty()) id.boot = clean_lines(banner, 20);
        set_note(manual ? "No answer: hold BOOT while pressing RESET, then run it again"
                        : "No ESP boot loader answered (not an ESP, or no auto-reset circuit)");
    }
    return id;
}

} // namespace sc
