/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the sensor driver seam (driver.hpp): the registry, the driver contract on the recorded LD2450
 * fixtures (the decoded frames equal the golden file and the direct Decoder's), the Sensor type setting in the detector,
 * the link and the settings file.
 */
#include "check.hpp"

#include "csv.hpp"
#include "detect.hpp"
#include "diag.hpp"
#include "driver.hpp"
#include "fake_sys.hpp"
#include "ld2410.hpp"
#include "ld2450.hpp"
#include "link.hpp"
#include "settings.hpp"

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace mmw;

namespace {

std::vector<std::uint8_t> bytes_of(const std::string &s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

std::string walk() { return test_read_file(test_data_dir() + "ld2452-9600-walk-15s.bin"); }

std::string ld2410_stream()
{
    const unsigned char m[] = {0xF4, 0xF3, 0xF2, 0xF1, 0x0D, 0x00, 0x02, 0xAA, 0x02, 0x1E, 0x00, 0x3C, 0x00,
                               0x00, 0x39, 0x00, 0x00, 0x55, 0x00, 0xF8, 0xF7, 0xF6, 0xF5};
    std::string s;
    for (int i = 0; i < 6; ++i) s.append(reinterpret_cast<const char *>(m), sizeof(m));
    return s;
}

/* Runs the detector on the fake clock until it finishes (or 20 s). */
void run(FakeSys &sys, Detector &det)
{
    const long t0 = sys.now_ms;
    while (det.running() && sys.now_ms - t0 < 20000) {
        sys.now_ms += 20;
        det.poll(sys.now_ms);
    }
}

std::vector<Frame> decode_with(const SensorDriver &d, const std::vector<std::uint8_t> &data, std::size_t chunk,
                               DecoderStats *stats = nullptr)
{
    std::unique_ptr<FrameDecoder> dec = d.make_decoder();
    std::vector<Frame> frames;
    for (std::size_t i = 0; i < data.size(); i += chunk)
        dec->feed(data.data() + i, std::min(chunk, data.size() - i), &frames);
    if (stats) *stats = dec->stats();
    return frames;
}

void test_registry()
{
    std::size_t n = 0;
    const SensorDriver *const *all = drivers(n);
    CHECK_EQ(n, static_cast<std::size_t>(2));
    CHECK(all[0] == &kDriverLd2450); /* real drivers first */
    CHECK(all[1] == &kDriverLd2410);
    CHECK(find_driver("ld2450") == &kDriverLd2450);
    CHECK(find_driver("ld2410") == &kDriverLd2410);
    CHECK(find_driver("nope") == nullptr);
    CHECK(find_driver("") == nullptr);
    CHECK(find_driver(nullptr) == nullptr);

    const SensorDriver &d = kDriverLd2450;
    CHECK(d.supported());
    CHECK_EQ(std::string(d.id), "ld2450");
    CHECK_EQ(std::string(d.name), "HLK-LD2452 / LD2450");
    CHECK_EQ(std::string(d.found_text), "mmWave radar (LD2450 family)");
    CHECK(d.caps.tracking_targets && !d.caps.presence_gates);
    CHECK(d.view == ViewKind::Tracking);
    const int want[] = {256000, 9600, 115200, 57600, 38400, 19200, 230400, 460800}; /* the detector's order */
    CHECK_EQ(d.baud_count, static_cast<std::size_t>(8));
    for (std::size_t i = 0; i < 8 && i < d.baud_count; ++i) CHECK_EQ(d.bauds[i], want[i]);

    const SensorDriver &s = kDriverLd2410; /* the stub */
    CHECK(!s.supported());
    CHECK_EQ(std::string(s.stub_text), "LD2410 presence radar (not supported yet)");
    CHECK(!s.caps.tracking_targets && s.caps.presence_gates);
    CHECK(s.view == ViewKind::Presence);
    CHECK_EQ(s.baud_count, static_cast<std::size_t>(0));

    /* the Sensor type values: Auto and the real drivers only */
    CHECK(valid_sensor_type("auto"));
    CHECK(valid_sensor_type("ld2450"));
    CHECK(!valid_sensor_type("ld2410")); /* a stub is not a choice */
    CHECK(!valid_sensor_type("zzz"));
    CHECK(!valid_sensor_type(nullptr));
    CHECK_EQ(std::string(next_sensor_type("auto")), "ld2450");
    CHECK_EQ(std::string(next_sensor_type("ld2450")), "auto");
    CHECK_EQ(std::string(next_sensor_type("junk")), "auto");
    CHECK_EQ(std::string(sensor_type_label("auto")), "Auto");
    CHECK_EQ(std::string(sensor_type_label("ld2450")), "HLK-LD2452 / LD2450");
    CHECK_EQ(std::string(sensor_type_label("ld2410")), "Auto");
}

void test_contract_ld2450()
{
    const std::string bin = walk();
    const std::string csv = test_read_file(test_data_dir() + "ld2452-9600-walk-15s.csv");
    CHECK_EQ(bin.size(), static_cast<std::size_t>(8700));
    const std::vector<std::uint8_t> data = bytes_of(bin);

    /* through the driver: the golden file, row by row, whatever the chunking */
    for (std::size_t chunk : {static_cast<std::size_t>(1), static_cast<std::size_t>(7), static_cast<std::size_t>(512),
                              data.size()}) {
        DecoderStats st;
        const std::vector<Frame> f = decode_with(kDriverLd2450, data, chunk, &st);
        CHECK_EQ(f.size(), static_cast<std::size_t>(290));
        CHECK_EQ(st.frames_ok, 290u);
        CHECK_EQ(st.good_run, 290u);
        std::istringstream in(csv);
        std::string line;
        std::getline(in, line);
        for (std::size_t i = 0; i < f.size() && std::getline(in, line); ++i) {
            const std::size_t c1 = line.find(',');
            const std::size_t c2 = line.find(',', c1 + 1);
            const long t_ms = std::atol(line.substr(c1 + 1, c2 - c1 - 1).c_str());
            CHECK_EQ(csv_row(static_cast<std::uint32_t>(i), t_ms, f[i]), line + "\n");
        }
    }

    /* the same frames as the Decoder itself, the config session too (cut frame, damaged answer): bytes, counters, raw */
    for (const char *name : {"ld2452-9600-walk-15s.bin", "ld2452-9600-config-session.bin"}) {
        const std::vector<std::uint8_t> d = bytes_of(test_read_file(test_data_dir() + name));
        CHECK(!d.empty());
        Decoder direct;
        std::vector<Frame> a;
        direct.feed(d.data(), d.size(), &a);
        DecoderStats st;
        const std::vector<Frame> b = decode_with(kDriverLd2450, d, d.size(), &st);
        CHECK_EQ(a.size(), b.size());
        CHECK_EQ(st.frames_ok, direct.stats().frames_ok);
        CHECK_EQ(st.frames_cut, direct.stats().frames_cut);
        CHECK_EQ(st.answers, direct.stats().answers);
        CHECK_EQ(st.garbage_bytes, direct.stats().garbage_bytes);
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
            CHECK(std::memcmp(a[i].raw, b[i].raw, sizeof(a[i].raw)) == 0);
            CHECK_EQ(csv_row(0, 0, a[i]), csv_row(0, 0, b[i]));
            for (const Target &t : b[i].target) CHECK_EQ(t.snr, -1); /* this family sends none */
        }
    }
    {
        DecoderStats st;
        decode_with(kDriverLd2450, bytes_of(test_read_file(test_data_dir() + "ld2452-9600-config-session.bin")), 1778, &st);
        CHECK_EQ(st.frames_ok, 58u); /* the numbers test_frame.cpp has always expected */
        CHECK_EQ(st.answers, 2u);
        CHECK_EQ(st.frames_cut, 1u);
    }

    /* reset gives a fresh decoder */
    std::unique_ptr<FrameDecoder> dec = kDriverLd2450.make_decoder();
    dec->feed(data.data(), 300, nullptr);
    CHECK(dec->stats().frames_ok > 0);
    dec->reset();
    CHECK_EQ(dec->stats().frames_ok, 0u);
    CHECK_EQ(dec->stats().bytes, static_cast<std::uint64_t>(0));
}

void test_sniff_contract()
{
    const std::string w = walk();
    const std::string l = ld2410_stream();
    const auto *wp = reinterpret_cast<const std::uint8_t *>(w.data());
    const auto *lp = reinterpret_cast<const std::uint8_t *>(l.data());
    CHECK(kDriverLd2450.sniff(wp, w.size()) == SniffResult::Match);
    CHECK(kDriverLd2450.sniff(wp, 10) == SniffResult::Maybe);   /* the start of a frame */
    CHECK(kDriverLd2450.sniff(wp, 29) == SniffResult::Maybe);
    CHECK(kDriverLd2450.sniff(wp, 1) == SniffResult::Maybe);
    CHECK(kDriverLd2450.sniff(wp, 0) == SniffResult::No);
    CHECK(kDriverLd2450.sniff(lp, l.size()) == SniffResult::No);
    const std::vector<std::uint8_t> zeros(100, 0);
    CHECK(kDriverLd2450.sniff(zeros.data(), zeros.size()) == SniffResult::No);
    /* header bytes with a wrong footer are not a frame */
    std::vector<std::uint8_t> bad(wp, wp + 30);
    bad[29] = 0x00;
    CHECK(kDriverLd2450.sniff(bad.data(), bad.size()) == SniffResult::No);

    CHECK(kDriverLd2410.sniff(lp, l.size()) == SniffResult::Match);
    CHECK(kDriverLd2410.sniff(lp, 6) == SniffResult::Maybe);
    CHECK(kDriverLd2410.sniff(lp, 20) == SniffResult::Maybe);
    CHECK(kDriverLd2410.sniff(wp, w.size()) == SniffResult::No);
    CHECK(kDriverLd2410.sniff(zeros.data(), zeros.size()) == SniffResult::No);
    CHECK(ld2410_frame_at(lp, l.size()));
    CHECK(!ld2410_frame_at(lp, 22));
}

void test_forced_type()
{
    /* Auto (the default): the LD2410 is named, as ever */
    {
        FakeSys sys;
        sys.add("ttyUSB0").stream[256000] = ld2410_stream();
        Detector det(sys);
        det.start(0, Remembered());
        run(sys, det);
        CHECK(det.finished() && !det.found());
        CHECK(det.reports()[0].verdict == Verdict::NotRadar);
        CHECK_EQ(det.reports()[0].text, "not a supported radar: LD2410 presence radar (not supported yet)");
    }
    /* a fixed LD2450 ignores it: not named, not a radar, left alone */
    {
        FakeSys sys;
        sys.add("ttyUSB0").stream[256000] = ld2410_stream();
        DetectConfig cfg;
        cfg.sensor = "ld2450";
        Detector det(sys, cfg);
        det.start(0, Remembered());
        run(sys, det);
        CHECK(det.finished() && !det.found());
        CHECK(det.reports()[0].verdict == Verdict::NotRadar);
        CHECK(det.reports()[0].text.find("LD2410") == std::string::npos);
        CHECK(det.reports()[0].text.find("not a supported radar") == 0);
        CHECK_EQ(sys.open_handles(), 0);
    }
    /* a fixed LD2450 finds its radar, at the same speeds and order as Auto */
    for (const char *type : {"auto", "ld2450", "bogus"}) {
        FakeSys sys;
        FakePort &p = sys.add("ttyUSB0");
        p.stream[9600] = walk();
        DetectConfig cfg;
        cfg.sensor = type;
        Detector det(sys, cfg);
        det.start(0, Remembered());
        run(sys, det);
        CHECK(det.found());
        CHECK(det.reports()[0].verdict == Verdict::Radar);
        CHECK_EQ(det.reports()[0].text, "mmWave radar (LD2450 family)");
        CHECK_EQ(p.bauds.size(), static_cast<std::size_t>(2));
        CHECK_EQ(p.bauds[0], 256000);
        CHECK_EQ(p.bauds[1], 9600);
        RadarPort r = det.take_radar();
        CHECK(r.driver == &kDriverLd2450);
        sys.close(r.handle);
    }
    /* "bogus" reads as Auto: the LD2410 is still named */
    {
        FakeSys sys;
        sys.add("ttyUSB0").stream[256000] = ld2410_stream();
        DetectConfig cfg;
        cfg.sensor = "bogus";
        Detector det(sys, cfg);
        det.start(0, Remembered());
        run(sys, det);
        CHECK(det.reports()[0].text.find("LD2410") != std::string::npos);
    }
    /* a stub cannot be a fixed type: it reads as Auto too */
    {
        FakeSys sys;
        sys.add("ttyUSB0").stream[256000] = ld2410_stream();
        DetectConfig cfg;
        cfg.sensor = "ld2410";
        Detector det(sys, cfg);
        det.start(0, Remembered());
        run(sys, det);
        CHECK(det.reports()[0].text.find("LD2410") != std::string::npos);
    }
    /* GPS is still named with a fixed type (it is not a sensor model) */
    {
        FakeSys sys;
        std::string body = "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";
        unsigned x = 0;
        for (char c : body) x ^= static_cast<unsigned char>(c);
        char tail[8];
        std::snprintf(tail, sizeof(tail), "*%02X\r\n", x);
        std::string one = "$" + body + tail, s;
        for (int i = 0; i < 5; ++i) s += one;
        sys.add("ttyUSB0").stream[9600] = s;
        DetectConfig cfg;
        cfg.sensor = "ld2450";
        Detector det(sys, cfg);
        det.start(0, Remembered());
        run(sys, det);
        CHECK_EQ(det.reports()[0].text, "not a supported radar: GPS receiver (NMEA)");
    }
}

void test_link_driver()
{
    FakeSys sys;
    sys.add("ttyUSB0").stream[9600] = walk();
    LinkConfig cfg;
    cfg.detect.sensor = "ld2450";
    Link link(sys, cfg);
    CHECK(link.driver() == nullptr);
    CHECK_EQ(std::string(link.model()), "LD2450-family tracking radar (LD2450, LD2452)");
    link.start(0, Remembered());
    while (sys.now_ms < 6000) {
        sys.now_ms += 30;
        link.poll(sys.now_ms);
    }
    CHECK(link.state() == LinkState::Found);
    CHECK(link.driver() == &kDriverLd2450);
    CHECK_EQ(link.sensor_type(), "ld2450");
    CHECK(link.stats().frames > 50);
    CHECK(diag_line(link, sys.now_ms).find(" baud=9600 driver=ld2450 frames=") != std::string::npos);

    /* changing the type while streaming leaves the radar alone */
    link.set_sensor_type("auto", sys.now_ms);
    CHECK(link.state() == LinkState::Found);
    CHECK_EQ(link.sensor_type(), "auto");
    link.close();

    /* nothing found: the line says so, and a type change rescans at once */
    FakeSys s2;
    s2.add("ttyUSB0").stream[256000] = ld2410_stream();
    Link l2(s2);
    l2.start(0, Remembered());
    while (s2.now_ms < 9000) {
        s2.now_ms += 30;
        l2.poll(s2.now_ms);
    }
    CHECK(l2.driver() == nullptr);
    CHECK(diag_line(l2, s2.now_ms).find(" driver=- ") != std::string::npos);
    CHECK(l2.reports()[0].text.find("LD2410") != std::string::npos);
    l2.set_sensor_type("ld2450", s2.now_ms);
    s2.now_ms += 30;
    l2.poll(s2.now_ms);
    CHECK(l2.status().find("Looking at") == 0); /* a scan started */
    while (s2.now_ms < 20000) {
        s2.now_ms += 30;
        l2.poll(s2.now_ms);
    }
    CHECK(l2.reports()[0].text.find("LD2410") == std::string::npos);
}

void test_sensor_setting()
{
    const Settings d;
    CHECK_EQ(d.sensor, "auto");
    CHECK_EQ(serialize_settings(d), "units=metres\norientation=normal\nrange=auto\ntrails=on\nhide_held=off\n");
    /* an old file (no sensor line) loads as Auto */
    CHECK_EQ(parse_settings("units=metres\norientation=mirror\nrange=4\ntrails=off\nhide_held=on\n").sensor, "auto");
    CHECK_EQ(parse_settings("").sensor, "auto");
    /* a fixed type round-trips */
    Settings s;
    s.sensor = "ld2450";
    s.range_m = 6;
    CHECK(serialize_settings(s).find("sensor=ld2450\n") != std::string::npos);
    const Settings r = parse_settings(serialize_settings(s));
    CHECK_EQ(r.sensor, "ld2450");
    CHECK_EQ(r.range_m, 6);
    CHECK_EQ(parse_settings(serialize_settings(r)).sensor, "ld2450");
    /* Auto is written as no line at all; the other settings are not disturbed */
    s.sensor = "auto";
    CHECK(serialize_settings(s).find("sensor") == std::string::npos);
    CHECK_EQ(parse_settings("sensor=ld2450\r\nunits=mm\r\n").sensor, "ld2450");
    /* what is not a real driver reads as Auto */
    CHECK_EQ(parse_settings("sensor=ld2410\n").sensor, "auto");
    CHECK_EQ(parse_settings("sensor=bogus\n").sensor, "auto");
    CHECK_EQ(parse_settings("sensor=\n").sensor, "auto");
    CHECK_EQ(parse_settings("sensor=AUTO\n").sensor, "auto");
}

} // namespace

void test_driver()
{
    test_registry();
    test_contract_ld2450();
    test_sniff_contract();
    test_forced_type();
    test_link_driver();
    test_sensor_setting();
}
