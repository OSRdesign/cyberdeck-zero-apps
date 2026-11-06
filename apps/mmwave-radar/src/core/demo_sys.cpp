/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the scripted port layer (see demo_sys.hpp).
 */
#include "demo_sys.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace mmw {

namespace {

constexpr double kBytesPerSecond = 578.0;   /* 19.3 frames of 30 bytes, as the owner's LD2452 at 9600 baud */
constexpr double kNmeaBytesPerSecond = 960.0;
constexpr std::uint64_t kWalkStartFrame = 60;

void put16(std::uint8_t *p, int v, bool sign_magnitude)
{
    std::uint16_t raw;
    if (sign_magnitude) raw = static_cast<std::uint16_t>(v >= 0 ? (0x8000 | (v & 0x7FFF)) : ((-v) & 0x7FFF));
    else raw = static_cast<std::uint16_t>(v & 0xFFFF);
    p[0] = static_cast<std::uint8_t>(raw & 0xFF);
    p[1] = static_cast<std::uint8_t>(raw >> 8);
}

/* person i at time t (mm) */
void person(int i, double t, double &x, double &y)
{
    switch (i) {
    case 0: /* a slow loop in front */
        x = 1300.0 * std::sin(0.6 * t);
        y = 2700.0 + 1000.0 * std::cos(0.6 * t);
        break;
    case 1: /* walks toward the sensor and back, on the left */
        x = -1500.0 + 200.0 * std::sin(0.2 * t);
        y = 3200.0 + 1600.0 * std::sin(0.55 * t + 1.0);
        break;
    default: /* close, on the right, moving a little */
        x = 700.0 + 300.0 * std::sin(0.8 * t);
        y = 1300.0 + 250.0 * std::cos(0.5 * t);
        break;
    }
}

std::string nmea_sentence(const char *body)
{
    unsigned char ck = 0;
    for (const char *p = body; *p; ++p) ck ^= static_cast<unsigned char>(*p);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "$%s*%02X\r\n", body, ck);
    return buf;
}

} // namespace

void encode_frame(const Frame &f, std::uint8_t out[30])
{
    out[0] = 0xAA;
    out[1] = 0xFF;
    out[2] = 0x03;
    out[3] = 0x00;
    for (int i = 0; i < 3; ++i) {
        std::uint8_t *q = out + 4 + 8 * i;
        const Target &t = f.target[i];
        if (!t.present) {
            for (int k = 0; k < 8; ++k) q[k] = 0;
            continue;
        }
        put16(q, t.x_mm, true);
        put16(q + 2, t.y_mm, true);
        put16(q + 4, t.speed_cm_s, true);
        put16(q + 6, t.resolution_mm, false);
    }
    out[28] = 0x55;
    out[29] = 0xCC;
}

Frame demo_frame(double t_s, int count)
{
    Frame f;
    for (int i = 0; i < 3; ++i) {
        Target &t = f.target[i];
        t.slot = i + 1;
        if (i >= count) continue;
        double x, y, x2, y2;
        person(i, t_s, x, y);
        person(i, t_s + 0.1, x2, y2);
        const double d = std::hypot(x, y), d2 = std::hypot(x2, y2);
        double v = (d2 - d) / 0.1 / 10.0; /* cm/s, + receding */
        v = std::round(v / 17.0) * 17.0;  /* the sensor's 17 cm/s steps (report 056) */
        t.present = true;
        t.x_mm = static_cast<int>(std::lround(x));
        t.y_mm = static_cast<int>(std::lround(y));
        t.speed_cm_s = static_cast<int>(v);
        t.resolution_mm = 360;
    }
    return f;
}

DemoSys::DemoSys(const std::string &scenario, Clock clock) : scenario_(scenario), clock_(std::move(clock))
{
    t0_ = clock_ ? clock_() : 0;
    auto add = [&](const char *tty, std::uint16_t vid, std::uint16_t pid, const char *product, Kind kind) -> Port & {
        Port p;
        p.meta.tty = tty;
        p.meta.path = std::string("/dev/") + tty;
        p.meta.vid = vid;
        p.meta.pid = pid;
        p.meta.product = product;
        p.meta.driver = vid == 0x1a86 ? "ch341" : (vid == 0x10c4 ? "cp210x" : "cdc_acm");
        p.kind = kind;
        ports_.push_back(p);
        return ports_.back();
    };
    if (scenario == "walk") {
        add("ttyUSB0", 0x1a86, 0x7523, "USB Serial", Kind::Walk);
        std::string path;
        if (const char *e = std::getenv("MMWAVE_DEMO_FILE"); e && *e) path = e;
#ifdef CDK_APP_DIR
        else path = std::string(CDK_APP_DIR) + "/tests/data/ld2452-9600-walk-15s.bin";
#endif
        if (!path.empty()) {
            std::ifstream in(path, std::ios::binary);
            walk_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    } else if (scenario == "one") {
        add("ttyUSB0", 0x1a86, 0x7523, "USB Serial", Kind::One);
    } else if (scenario == "three") {
        add("ttyUSB0", 0x1a86, 0x7523, "USB Serial", Kind::Three);
    } else if (scenario == "lost") {
        add("ttyUSB0", 0x1a86, 0x7523, "USB Serial", Kind::One).stop_after_ms = 1500;
    } else if (scenario == "none") {
        add("ttyUSB0", 0x10c4, 0xea60, "CP2102 USB to UART Bridge Controller", Kind::Gps);
        add("ttyACM0", 0x2e8a, 0x000a, "Pico", Kind::Silent);
    } else if (scenario == "busy") {
        add("ttyUSB0", 0x1a86, 0x7523, "USB Serial", Kind::One).held = true;
    } else if (scenario == "denied") {
        add("ttyUSB0", 0x1a86, 0x7523, "USB Serial", Kind::One).denied = true;
    }
    nmea_ = nmea_sentence("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,") +
            nmea_sentence("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W");
}

std::vector<PortMeta> DemoSys::list_ports()
{
    std::vector<PortMeta> out;
    for (const Port &p : ports_) out.push_back(p.meta);
    return out;
}

DemoSys::Port *DemoSys::by_path(const std::string &path)
{
    for (Port &p : ports_)
        if (p.meta.path == path) return &p;
    return nullptr;
}

DemoSys::Port *DemoSys::by_handle(int handle)
{
    for (Port &p : ports_)
        if (p.handle == handle && handle >= 0) return &p;
    return nullptr;
}

bool DemoSys::held_by_other(const std::string &path)
{
    const Port *p = by_path(path);
    return p && p->held;
}

int DemoSys::open(const std::string &path, OpenErr &err)
{
    Port *p = by_path(path);
    if (!p) {
        err = OpenErr::Gone;
        return -1;
    }
    if (p->held || p->handle >= 0) {
        err = OpenErr::Busy;
        return -1;
    }
    if (p->denied) {
        err = OpenErr::Denied;
        return -1;
    }
    const std::int64_t now = clock_ ? clock_() : 0;
    if (p->first_open_ms < 0) p->first_open_ms = now;
    p->handle = next_handle_++;
    p->pos = stream_now(*p, now);
    err = OpenErr::None;
    return p->handle;
}

bool DemoSys::configure(int handle, int baud)
{
    Port *p = by_handle(handle);
    if (!p) return false;
    p->baud = baud;
    return true;
}

void DemoSys::flush_input(int handle)
{
    if (Port *p = by_handle(handle)) p->pos = stream_now(*p, clock_ ? clock_() : 0);
}

std::uint64_t DemoSys::stream_now(const Port &p, std::int64_t now) const
{
    const double rate = p.kind == Kind::Gps ? kNmeaBytesPerSecond : kBytesPerSecond;
    std::int64_t t = now - t0_;
    if (p.stop_after_ms >= 0 && p.first_open_ms >= 0) t = std::min<std::int64_t>(t, p.first_open_ms - t0_ + p.stop_after_ms);
    if (t < 0) t = 0;
    std::uint64_t b = static_cast<std::uint64_t>(rate * static_cast<double>(t) / 1000.0);
    if (p.kind == Kind::Walk) b += kWalkStartFrame * kFrameSize;
    return b;
}

std::uint8_t DemoSys::byte_at(Port &p, std::uint64_t index)
{
    switch (p.kind) {
    case Kind::Walk: return walk_.empty() ? 0 : walk_[index % walk_.size()];
    case Kind::Gps: return static_cast<std::uint8_t>(nmea_[index % nmea_.size()]);
    case Kind::One:
    case Kind::Three: {
        const std::uint64_t k = index / kFrameSize;
        const int count = p.kind == Kind::Three ? 3 : 1;
        if (k != cached_frame_ || count != cached_count_) {
            encode_frame(demo_frame(static_cast<double>(k) * kFrameSize / kBytesPerSecond, count), cache_);
            cached_frame_ = k;
            cached_count_ = count;
        }
        return cache_[index % kFrameSize];
    }
    default: return 0;
    }
}

long DemoSys::read(int handle, std::uint8_t *buf, std::size_t n)
{
    Port *p = by_handle(handle);
    if (!p) return -1;
    if (p->kind == Kind::Silent) return 0;
    if (p->kind == Kind::Walk && walk_.empty()) return 0;
    const std::uint64_t end = stream_now(*p, clock_ ? clock_() : 0);
    if (end > p->pos + 4096) p->pos = end - 4096; /* a tty buffer keeps about 4 KB */
    std::size_t got = 0;
    const bool wrong_speed = p->kind != Kind::Gps && p->baud != 9600;
    while (got < n && p->pos < end) {
        const std::uint8_t b = byte_at(*p, p->pos++);
        buf[got++] = wrong_speed ? 0 : b;
    }
    return static_cast<long>(got);
}

void DemoSys::restore(int) {}

void DemoSys::close(int handle)
{
    if (Port *p = by_handle(handle)) p->handle = -1;
}

} // namespace mmw
