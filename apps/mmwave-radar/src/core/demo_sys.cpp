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

/* the LD2451 scenario: a loop of kLdSlots frame slots 140 ms apart */
constexpr int kLdSlots = 100;
constexpr int kLdSlotMs = 140;

} // namespace

std::vector<std::uint8_t> encode_ld2451_frame(const Ld2451Wire *targets, int count, int alarm)
{
    std::vector<std::uint8_t> out = {0xF4, 0xF3, 0xF2, 0xF1};
    if (count <= 0) {
        out.push_back(0);
        out.push_back(0);
    } else {
        const int len = 2 + 5 * count;
        out.push_back(static_cast<std::uint8_t>(len & 0xFF));
        out.push_back(static_cast<std::uint8_t>(len >> 8));
        out.push_back(static_cast<std::uint8_t>(count));
        out.push_back(static_cast<std::uint8_t>(alarm));
        for (int i = 0; i < count; ++i) {
            out.push_back(static_cast<std::uint8_t>(targets[i].angle_deg + 0x80));
            out.push_back(static_cast<std::uint8_t>(targets[i].distance_m));
            out.push_back(static_cast<std::uint8_t>(targets[i].direction));
            out.push_back(static_cast<std::uint8_t>(targets[i].speed_kmh));
            out.push_back(static_cast<std::uint8_t>(targets[i].snr));
        }
    }
    out.insert(out.end(), {0xF8, 0xF7, 0xF6, 0xF5});
    return out;
}

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
    } else if (scenario == "ld2451" || scenario == "ld2451-away" || scenario == "ld2451-two") {
        add("ttyUSB0", 0x1a86, 0x7523, "USB Serial", Kind::Ld2451);
        build_ld2451(scenario == "ld2451" ? 0 : scenario == "ld2451-away" ? 1 : 2);
    } else if (scenario == "one") {
        add("ttyUSB0", 0x1a86, 0x7523, "USB Serial", Kind::One);
    } else if (scenario == "three") {
        add("ttyUSB0", 0x1a86, 0x7523, "USB Serial", Kind::Three);
    } else if (scenario == "held") {
        add("ttyUSB0", 0x1a86, 0x7523, "USB Serial", Kind::Held);
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

/* Slots 0..89: a target in every slot (a pass of 12.6 s); slot 90: the empty frame; 91..99: nothing (a frame comes only
 * when there is something to say, as the sensor does). mode 0 approaching, 1 receding, 2 both. */
std::vector<std::uint8_t> ld2451_demo_frame(int mode, int k)
{
    k %= kLdSlots;
    if (k > 90) return std::vector<std::uint8_t>();
    if (k == 90) return encode_ld2451_frame(nullptr, 0, 0);
    Ld2451Wire w[2];
    int n = 0;
    const double u = k / 90.0;
    if (mode == 0 || mode == 2) { /* approaching: 14 m to 8 m, a few degrees to the right of the centre line */
        w[n++] = {static_cast<int>(std::lround(8.0 + 6.0 * std::sin(u * 3.14159265))),
                  static_cast<int>(std::lround(14.0 - 6.0 * u)), 1, 4,
                  150 + (k * 37) % 100};
    }
    if (mode == 1 || mode == 2) { /* receding: 4 m to 12 m, to the left */
        w[n++] = {static_cast<int>(std::lround(-12.0 + 5.0 * std::sin(u * 3.14159265 * 2.0))),
                  static_cast<int>(std::lround(4.0 + 8.0 * u)), 0, 4, 120 + (k * 53) % 130};
    }
    return encode_ld2451_frame(w, n, w[0].direction);
}

void DemoSys::build_ld2451(int mode)
{
    ld_buf_.clear();
    ld_cum_.assign(kLdSlots + 1, 0);
    for (int k = 0; k < kLdSlots; ++k) {
        ld_cum_[k] = ld_buf_.size();
        const std::vector<std::uint8_t> f = ld2451_demo_frame(mode, k);
        ld_buf_.insert(ld_buf_.end(), f.begin(), f.end());
    }
    ld_cum_[kLdSlots] = ld_buf_.size();
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
    if (p.kind == Kind::Ld2451) { /* time in slots of 140 ms, a loop of kLdSlots of them */
        const std::int64_t t = std::max<std::int64_t>(0, now - t0_);
        const std::uint64_t slot = static_cast<std::uint64_t>(t / kLdSlotMs);
        return (slot / kLdSlots) * ld_cum_[kLdSlots] + ld_cum_[slot % kLdSlots];
    }
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
    case Kind::Ld2451: return ld_buf_.empty() ? 0 : ld_buf_[index % ld_buf_.size()];
    case Kind::One:
    case Kind::Held:
    case Kind::Three: {
        const std::uint64_t k = index / kFrameSize;
        const int count = p.kind == Kind::One ? 1 : 3;
        if (k != cached_frame_ || count != cached_count_) {
            Frame f = demo_frame(static_cast<double>(k) * kFrameSize / kBytesPerSecond, count);
            if (p.kind == Kind::Held) { /* T2 is a stale target: the same bytes in every frame, speed 0 */
                Target &t = f.target[1];
                t.x_mm = -1100;
                t.y_mm = 2300;
                t.speed_cm_s = 0;
                t.resolution_mm = 360;
            }
            encode_frame(f, cache_);
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
    const bool wrong_speed = p->kind == Kind::Ld2451 ? p->baud != 115200 : (p->kind != Kind::Gps && p->baud != 9600);
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
