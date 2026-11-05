/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the sniffer and the port detector, against fake ports.
 */
#include "check.hpp"

#include "detect.hpp"
#include "fake_sys.hpp"
#include "sniff.hpp"

#include <cstdio>
#include <string>

using namespace mmw;

namespace {

std::string radar_stream() { return test_read_file(test_data_dir() + "ld2452-9600-walk-15s.bin"); }

std::string nmea(const std::string &body)
{
    unsigned x = 0;
    for (char c : body) x ^= static_cast<unsigned char>(c);
    char tail[8];
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", x);
    return "$" + body + tail;
}

std::string gps_stream()
{
    std::string s;
    for (int i = 0; i < 5; ++i) {
        s += nmea("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
        s += nmea("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W");
    }
    return s;
}

std::string ubx_stream()
{
    std::string m = {static_cast<char>(0xB5), 0x62, 0x01, 0x07, 0x04, 0x00, 1, 2, 3, 4};
    unsigned a = 0, b = 0;
    for (std::size_t i = 2; i < m.size(); ++i) {
        a = (a + static_cast<unsigned char>(m[i])) & 0xFF;
        b = (b + a) & 0xFF;
    }
    m += static_cast<char>(a);
    m += static_cast<char>(b);
    std::string s;
    for (int i = 0; i < 6; ++i) s += m;
    return s;
}

std::string ld2410_stream()
{
    std::string m = {static_cast<char>(0xF4), static_cast<char>(0xF3), static_cast<char>(0xF2), static_cast<char>(0xF1),
                     0x0D, 0x00, 0x02, static_cast<char>(0xAA), 0x02, 0x1E, 0x00, 0x3C, 0x00, 0x00, 0x39, 0x00, 0x00, 0x55, 0x00,
                     static_cast<char>(0xF8), static_cast<char>(0xF7), static_cast<char>(0xF6), static_cast<char>(0xF5)};
    /* length 0x0D = 13 bytes of data, then the footer */
    std::string s;
    for (int i = 0; i < 6; ++i) s += m;
    return s;
}

/* Runs the detector against the fake clock until it finishes (or 20 s); returns the elapsed fake ms. */
long run(FakeSys &sys, Detector &det, long limit_ms = 20000)
{
    const long t0 = sys.now_ms;
    while (det.running() && sys.now_ms - t0 < limit_ms) {
        sys.now_ms += 20;
        det.poll(sys.now_ms);
    }
    return sys.now_ms - t0;
}

void test_sniffer()
{
    Sniffer s;
    const std::string g = gps_stream();
    s.feed(reinterpret_cast<const std::uint8_t *>(g.data()), g.size());
    CHECK(s.kind() == Sniffed::Gps);
    CHECK_EQ(s.describe(), "GPS receiver (NMEA)");

    Sniffer b; /* a bad checksum is not NMEA */
    const std::string bad = "$GPGGA,1,2,3*00\r\n$GPGGA,1,2,3*00\r\n";
    b.feed(reinterpret_cast<const std::uint8_t *>(bad.data()), bad.size());
    CHECK(b.kind() == Sniffed::None);
    CHECK_EQ(b.describe_unknown(), "text output (a console or a log)");

    Sniffer one; /* byte at a time */
    for (char c : g) {
        const std::uint8_t u = static_cast<std::uint8_t>(c);
        one.feed(&u, 1);
    }
    CHECK(one.kind() == Sniffed::Gps);

    Sniffer u;
    const std::string ux = ubx_stream();
    u.feed(reinterpret_cast<const std::uint8_t *>(ux.data()), ux.size());
    CHECK(u.kind() == Sniffed::Ubx);

    Sniffer l;
    const std::string ld = ld2410_stream();
    l.feed(reinterpret_cast<const std::uint8_t *>(ld.data()), ld.size());
    CHECK(l.kind() == Sniffed::Ld2410);

    Sniffer r; /* the radar's own stream is none of these */
    const std::string rs = radar_stream();
    r.feed(reinterpret_cast<const std::uint8_t *>(rs.data()), rs.size());
    CHECK(r.kind() == Sniffed::None);
    CHECK_EQ(r.describe_unknown(), "unknown data");

    Sniffer z;
    const std::string zeros(200, '\0');
    z.feed(reinterpret_cast<const std::uint8_t *>(zeros.data()), zeros.size());
    CHECK_EQ(z.describe_unknown(), "only zeros");
    Sniffer e;
    CHECK_EQ(e.describe_unknown(), "no data");
}

void test_remembered()
{
    Remembered r;
    r.port_id = "usb-1a86_USB_Serial-if00-port0";
    r.baud = 9600;
    const Remembered p = Remembered::parse(r.serialize());
    CHECK_EQ(p.port_id, r.port_id);
    CHECK_EQ(p.baud, 9600);
    CHECK(Remembered::parse("garbage").empty());
    CHECK(Remembered::parse("port=x\nbaud=5\n").empty());
    CHECK(Remembered().serialize().empty());
    PortMeta m;
    m.tty = "ttyUSB0";
    m.vid = 0x1a86;
    m.pid = 0x7523;
    CHECK_EQ(port_id(m), "usb-1a86:7523");
    CHECK_EQ(port_title(m), "ttyUSB0 (CH340)");
    m.by_id = "usb-1a86_USB_Serial-if00-port0";
    CHECK_EQ(port_id(m), "usb-1a86_USB_Serial-if00-port0");
    PortMeta n;
    n.tty = "ttyACM1";
    CHECK_EQ(port_id(n), "ttyACM1");
    CHECK_EQ(port_title(n), "ttyACM1");
}

void test_radar_found()
{
    FakeSys sys;
    FakePort &p = sys.add("ttyUSB0");
    p.stream[9600] = radar_stream();
    Detector det(sys);
    det.start(sys.now_ms, Remembered());
    const long t = run(sys, det);
    CHECK(det.finished());
    CHECK(det.found());
    CHECK(t >= 100 && t <= 800); /* the report: 0.3-0.8 s on a real board; the fake line has no latency */
    CHECK_EQ(det.reports().size(), static_cast<std::size_t>(1));
    CHECK(det.reports()[0].verdict == Verdict::Radar);
    CHECK_EQ(det.reports()[0].baud, 9600);
    /* 256000 was tried first, then 9600 */
    CHECK_EQ(p.bauds.size(), static_cast<std::size_t>(2));
    CHECK_EQ(p.bauds[0], 256000);
    CHECK_EQ(p.bauds[1], 9600);
    CHECK_EQ(p.handle >= 0, true); /* still open and locked */
    RadarPort r = det.take_radar();
    CHECK_EQ(r.baud, 9600);
    CHECK_EQ(r.meta.tty, "ttyUSB0");
    CHECK_EQ(r.title, "ttyUSB0 (CH340)");
    CHECK(r.handle >= 0);
    CHECK(!det.found());
    sys.close(r.handle);
    CHECK_EQ(sys.open_handles(), 0);

    /* the same at 256000: found at once */
    FakeSys s2;
    FakePort &q = s2.add("ttyUSB0");
    q.stream[256000] = radar_stream();
    q.rate_bps = 300.0;
    Detector d2(s2);
    d2.start(0, Remembered());
    run(s2, d2);
    CHECK(d2.found());
    CHECK_EQ(q.bauds.size(), static_cast<std::size_t>(1));
    CHECK_EQ(q.bauds[0], 256000);
}

void test_remembered_first()
{
    FakeSys sys;
    FakePort &p = sys.add("ttyUSB0");
    p.stream[9600] = radar_stream();
    Remembered mem;
    mem.port_id = "usb-1a86:7523";
    mem.baud = 9600;
    Detector det(sys);
    det.start(0, mem);
    const long t = run(sys, det);
    CHECK(det.found());
    CHECK_EQ(p.bauds.size(), static_cast<std::size_t>(1));
    CHECK_EQ(p.bauds[0], 9600);
    CHECK(t <= 300); /* about 0.2 s once remembered */
    /* the remembered port is probed first even when listed second */
    FakeSys s2;
    s2.add("ttyUSB0", 0x10c4, 0xea60).stream[9600] = gps_stream();
    s2.add("ttyUSB1").stream[9600] = radar_stream();
    Detector d2(s2);
    d2.start(0, mem);
    run(s2, d2);
    CHECK(d2.found());
    CHECK_EQ(d2.reports().front().meta.tty, "ttyUSB1");
    CHECK_EQ(s2.by_tty("ttyUSB0")->opens, 0); /* the rest were not even opened */
    CHECK(d2.reports().back().verdict == Verdict::Skipped);
    sys.close(d2.take_radar().handle);
}

void test_not_radar_devices()
{
    struct Case {
        std::string stream;
        const char *expect;
        int baud;
    } cases[] = {
        {gps_stream(), "not a supported radar: GPS receiver (NMEA)", 9600},
        {ubx_stream(), "not a supported radar: GPS receiver (u-blox UBX)", 9600},
        {ld2410_stream(), "not a supported radar: LD2410 presence radar (not supported yet)", 9600},
    };
    for (const Case &c : cases) {
        FakeSys sys;
        FakePort &p = sys.add("ttyACM0", 0x1234, 0x5678);
        p.stream[9600] = c.stream;
        p.rate_bps = 960;
        Detector det(sys);
        det.start(0, Remembered());
        run(sys, det);
        CHECK(det.finished());
        CHECK(!det.found());
        CHECK_EQ(det.reports().size(), static_cast<std::size_t>(1));
        CHECK(det.reports()[0].verdict == Verdict::NotRadar);
        CHECK_EQ(det.reports()[0].text, c.expect);
        CHECK_EQ(det.reports()[0].baud, c.baud);
        /* left alone: restored, released, nothing kept */
        CHECK_EQ(p.opens, 1);
        CHECK_EQ(p.restores, 1);
        CHECK_EQ(p.closes, 1);
        CHECK_EQ(sys.open_handles(), 0);
    }
    /* a plain text device (a console or a log) at every speed: all speeds tried */
    FakeSys sys;
    FakePort &t = sys.add("ttyUSB0");
    t.stream[256000] = t.stream[9600] = t.stream[115200] = "boot: hello world, this is a log line\r\n";
    t.wrong_speed = "boot: hello world, this is a log line\r\n";
    t.rate_bps = 2000;
    Detector det(sys);
    det.start(0, Remembered());
    run(sys, det);
    CHECK(det.reports()[0].verdict == Verdict::NotRadar);
    CHECK_EQ(det.reports()[0].text, "not a supported radar: text output (a console or a log)");
    CHECK_EQ(t.bauds.size(), static_cast<std::size_t>(8));
    CHECK_EQ(t.restores, 1);
    CHECK_EQ(sys.open_handles(), 0);
    /* binary noise at every speed */
    FakeSys s3;
    FakePort &n = s3.add("ttyUSB0");
    n.wrong_speed = std::string("\x13\x37\xC0\xDE\x81\x7F\x99\xE5", 8);
    Detector d3(s3);
    d3.start(0, Remembered());
    run(s3, d3);
    CHECK_EQ(d3.reports()[0].text, "not a supported radar: unknown data");
    CHECK_EQ(n.bauds.size(), static_cast<std::size_t>(8));
}

void test_silent_and_busy()
{
    FakeSys sys;
    FakePort &silent = sys.add("ttyUSB0");
    silent.wrong_speed.clear(); /* nothing at any speed */
    Detector det(sys);
    det.start(0, Remembered());
    const long t = run(sys, det);
    CHECK(det.reports()[0].verdict == Verdict::Silent);
    CHECK_EQ(det.reports()[0].text, "not a supported radar: silent (no data)");
    CHECK(t >= 600 && t <= 700); /* 0.6 s, then the port is left */
    CHECK_EQ(silent.bauds.size(), static_cast<std::size_t>(1));
    CHECK_EQ(silent.restores, 1);
    CHECK_EQ(silent.closes, 1);

    FakeSys s2;
    s2.add("ttyUSB0").excl_busy = true;
    s2.add("ttyUSB1").lock_busy = true;
    s2.add("ttyUSB2").proc_held = true;
    FakePort &ok = s2.add("ttyUSB3");
    ok.stream[9600] = radar_stream();
    Detector d2(s2);
    d2.start(0, Remembered());
    run(s2, d2);
    CHECK(d2.found());
    CHECK_EQ(d2.reports().size(), static_cast<std::size_t>(4));
    for (int i = 0; i < 3; ++i) {
        CHECK(d2.reports()[i].verdict == Verdict::Busy);
        CHECK_EQ(d2.reports()[i].text, "busy (held by another app)");
    }
    /* a held port is never configured: no speed was set on it */
    for (const char *tty : {"ttyUSB0", "ttyUSB1", "ttyUSB2"}) CHECK(s2.by_tty(tty)->bauds.empty());
    CHECK_EQ(s2.by_tty("ttyUSB2")->opens, 0); /* seen in /proc: not even opened */
    sys.close(d2.take_radar().handle);

    FakeSys s3;
    s3.add("ttyUSB0").denied = true;
    Detector d3(s3);
    d3.start(0, Remembered());
    run(s3, d3);
    CHECK(d3.reports()[0].verdict == Verdict::Denied);
    CHECK(d3.reports()[0].text.find("dialout") != std::string::npos);
}

void test_vanish_and_budget()
{
    FakeSys sys;
    FakePort &a = sys.add("ttyUSB0");
    a.stream[9600] = radar_stream();
    a.vanish_at_ms = 100; /* unplugged while we listen */
    FakePort &b = sys.add("ttyUSB1");
    b.stream[9600] = radar_stream();
    Detector det(sys);
    det.start(0, Remembered());
    run(sys, det);
    CHECK(det.reports()[0].verdict == Verdict::Gone);
    CHECK(det.found());
    CHECK(det.reports()[1].verdict == Verdict::Radar);
    CHECK_EQ(sys.by_tty("ttyUSB0")->closes, 1);
    sys.close(det.take_radar().handle);

    /* the time budget: many ports that talk at every speed and are none of ours */
    FakeSys s2;
    for (int i = 0; i < 10; ++i) {
        FakePort &p = s2.add("ttyUSB" + std::to_string(i));
        p.wrong_speed = std::string("\x13\x37\xC0\xDE\x81\x7F\x99\xE5", 8);
        p.rate_bps = 100000; /* fast noise */
    }
    DetectConfig cfg;
    cfg.total_budget_ms = 500;
    Detector d2(s2, cfg);
    d2.start(0, Remembered());
    const long t = run(s2, d2);
    CHECK(t < 4000);
    CHECK(d2.finished());
    CHECK_EQ(d2.reports().size(), static_cast<std::size_t>(10));
    int skipped = 0;
    for (const PortReport &r : d2.reports()) skipped += r.verdict == Verdict::Skipped ? 1 : 0;
    CHECK(skipped >= 1);
    CHECK_EQ(s2.open_handles(), 0);

    /* cancel closes the port in progress */
    FakeSys s3;
    s3.add("ttyUSB0").wrong_speed.clear();
    s3.add("ttyUSB1").wrong_speed.clear();
    Detector d3(s3);
    d3.start(0, Remembered());
    s3.now_ms = 100;
    d3.poll(100);
    CHECK_EQ(s3.open_handles(), 1);
    d3.cancel();
    CHECK_EQ(s3.open_handles(), 0);
    CHECK_EQ(s3.by_tty("ttyUSB0")->restores, 1);
    CHECK(d3.finished());
    CHECK_EQ(d3.reports().size(), static_cast<std::size_t>(2));

    /* settled ports are not opened again */
    FakeSys s4;
    s4.add("ttyUSB0").wrong_speed.clear();
    Detector d4(s4);
    d4.start(0, Remembered(), {"/dev/ttyUSB0"});
    run(s4, d4);
    CHECK_EQ(s4.total_opens(), 0);
    CHECK(d4.reports().empty());
}

void test_noise_with_header_bytes()
{
    /* header bytes with implausible values are not a radar, however many */
    std::string fake;
    for (int i = 0; i < 50; ++i) {
        fake += std::string("\xAA\xFF\x03\x00", 4);
        fake += std::string("\xFF\xFF\xFF\xFF\x00\x00\x00\x00", 8);
        fake += std::string(16, '\0');
        fake += std::string("\x55\xCC", 2);
    }
    FakeSys sys;
    FakePort &p = sys.add("ttyUSB0");
    p.stream[9600] = fake;
    Detector det(sys);
    det.start(0, Remembered());
    run(sys, det);
    CHECK(!det.found());
    CHECK(det.reports()[0].verdict == Verdict::NotRadar);
}

} // namespace

void test_detect()
{
    test_sniffer();
    test_remembered();
    test_radar_found();
    test_remembered_first();
    test_not_radar_devices();
    test_silent_and_busy();
    test_vanish_and_budget();
    test_noise_with_header_bytes();
}
