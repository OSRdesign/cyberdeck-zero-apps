/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the frame decoder, the CSV rows and the fixtures.
 */
#include "check.hpp"

#include "csv.hpp"
#include "ld2450.hpp"

#include <cstdint>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace mmw;

namespace {

std::vector<std::uint8_t> bytes_of(const std::string &s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

std::vector<std::uint8_t> hex(const char *h)
{
    std::vector<std::uint8_t> v;
    for (const char *p = h; *p;) {
        if (*p == ' ') { ++p; continue; }
        v.push_back(static_cast<std::uint8_t>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
        p += 2;
    }
    return v;
}

std::vector<std::uint8_t> frame_bytes(const std::vector<std::vector<std::uint8_t>> &slots)
{
    std::vector<std::uint8_t> f = {0xAA, 0xFF, 0x03, 0x00};
    for (int i = 0; i < 3; ++i) {
        if (i < static_cast<int>(slots.size())) f.insert(f.end(), slots[i].begin(), slots[i].end());
        else f.insert(f.end(), 8, 0);
    }
    f.push_back(0x55);
    f.push_back(0xCC);
    return f;
}

struct Result {
    std::vector<Frame> frames;
    DecoderStats stats;
};

Result run_whole(const std::vector<std::uint8_t> &d)
{
    Decoder dec;
    Result r;
    dec.feed(d.data(), d.size(), &r.frames);
    r.stats = dec.stats();
    return r;
}

Result run_chunked(const std::vector<std::uint8_t> &d, std::size_t chunk)
{
    Decoder dec;
    Result r;
    for (std::size_t i = 0; i < d.size(); i += chunk) dec.feed(d.data() + i, std::min(chunk, d.size() - i), &r.frames);
    r.stats = dec.stats();
    return r;
}

Result run_random_chunks(const std::vector<std::uint8_t> &d, unsigned seed)
{
    std::mt19937 rng(seed);
    Decoder dec;
    Result r;
    for (std::size_t i = 0; i < d.size();) {
        const std::size_t n = std::min<std::size_t>(1 + rng() % 70, d.size() - i);
        dec.feed(d.data() + i, n, &r.frames);
        i += n;
    }
    r.stats = dec.stats();
    return r;
}

bool same(const Result &a, const Result &b)
{
    if (a.frames.size() != b.frames.size()) return false;
    for (std::size_t i = 0; i < a.frames.size(); ++i)
        if (csv_row(0, 0, a.frames[i]) != csv_row(0, 0, b.frames[i])) return false;
    const DecoderStats &x = a.stats, &y = b.stats;
    return x.bytes == y.bytes && x.frames_ok == y.frames_ok && x.frames_bad == y.frames_bad &&
           x.frames_cut == y.frames_cut && x.answers == y.answers && x.garbage_bytes == y.garbage_bytes;
}

void test_manual_example()
{
    /* the worked example of the LD2452 manual (report 056, 2.3) */
    std::vector<std::uint8_t> d = hex("AA FF 03 00 0E 03 B1 86 10 00 68 01");
    d.insert(d.end(), 16, 0);
    d.push_back(0x55);
    d.push_back(0xCC);
    CHECK_EQ(d.size(), static_cast<std::size_t>(30));
    Frame f;
    CHECK(decode_frame(d.data(), f));
    const Target &t = f.target[0];
    CHECK(t.present);
    CHECK_EQ(t.slot, 1);
    CHECK_EQ(t.x_mm, -782);
    CHECK_EQ(t.y_mm, 1713);
    CHECK_EQ(t.speed_cm_s, -16);
    CHECK_EQ(t.resolution_mm, 360);
    CHECK_EQ(t.distance_mm, 1883); /* sqrt(782^2 + 1713^2) = 1883.0 */
    CHECK(t.direction == Direction::Approaching);
    CHECK(t.angle_deg < 0 && t.angle_deg > -25.0); /* to the left: atan2(-782, 1713) = -24.5 deg */
    CHECK(!f.target[1].present && !f.target[2].present);
    CHECK_EQ(f.target[2].slot, 3);
    CHECK_EQ(f.present_count(), 1);
    CHECK(plausible(f));
}

void test_sign_magnitude()
{
    CHECK_EQ(sign_magnitude(0x0000), 0);
    CHECK_EQ(sign_magnitude(0x8000), 0);
    CHECK_EQ(sign_magnitude(0x7FFF), -32767);
    CHECK_EQ(sign_magnitude(0xFFFF), 32767);
    CHECK_EQ(sign_magnitude(0x8001), 1);
    CHECK_EQ(sign_magnitude(0x0001), -1);

    /* a slot is empty only when all 8 bytes are zero: "+0" in y alone is a present target */
    const auto d = frame_bytes({hex("00 00 00 80 00 00 00 00")});
    Frame f;
    CHECK(decode_frame(d.data(), f));
    CHECK(f.target[0].present);
    CHECK_EQ(f.target[0].y_mm, 0);
    CHECK(!plausible(f)); /* y must be positive */
    /* directions */
    const auto e = frame_bytes({hex("00 80 E8 83 11 00 4A 01"), hex("00 80 E8 83 11 80 4A 01"), hex("00 80 E8 83 00 80 4A 01")});
    CHECK(decode_frame(e.data(), f));
    CHECK(f.target[0].direction == Direction::Approaching);
    CHECK(f.target[1].direction == Direction::Receding);
    CHECK(f.target[2].direction == Direction::Still);
    CHECK_EQ(f.target[1].speed_cm_s, 17);
    CHECK_EQ(f.target[1].resolution_mm, 330);
    CHECK_EQ(f.target[0].angle_deg, 0.0); /* straight ahead */
    CHECK_EQ(f.target[1].distance_mm, 1000);
    /* x to the right is positive */
    const auto r = frame_bytes({hex("E8 83 E8 83 00 80 4A 01")});
    CHECK(decode_frame(r.data(), f));
    CHECK(f.target[0].angle_deg > 44.9 && f.target[0].angle_deg < 45.1);
    /* bad header or footer */
    auto bad = frame_bytes({});
    bad[29] = 0;
    CHECK(!decode_frame(bad.data(), f));
}

void test_walk_fixture()
{
    const std::string bin = test_read_file(test_data_dir() + "ld2452-9600-walk-15s.bin");
    const std::string csv = test_read_file(test_data_dir() + "ld2452-9600-walk-15s.csv");
    CHECK_EQ(bin.size(), static_cast<std::size_t>(8700));
    CHECK(!csv.empty());
    const Result r = run_whole(bytes_of(bin));
    CHECK_EQ(r.frames.size(), static_cast<std::size_t>(290));
    CHECK_EQ(r.stats.frames_ok, 290u);
    CHECK_EQ(r.stats.frames_bad, 0u);
    CHECK_EQ(r.stats.frames_cut, 0u);
    CHECK_EQ(r.stats.answers, 0u);
    CHECK_EQ(r.stats.garbage_bytes, static_cast<std::uint64_t>(0));
    CHECK_EQ(r.stats.bytes, static_cast<std::uint64_t>(8700));
    CHECK_EQ(r.stats.good_run, 290u);

    /* the golden file: every decoded row equals the study's CSV, byte for byte */
    std::istringstream in(csv);
    std::string line;
    std::getline(in, line);
    CHECK_EQ(line + "\n", csv_header());
    std::size_t i = 0;
    int one = 0, zero = 0;
    while (std::getline(in, line) && i < r.frames.size()) {
        const std::size_t c1 = line.find(',');
        const std::size_t c2 = line.find(',', c1 + 1);
        const long t_ms = std::atol(line.substr(c1 + 1, c2 - c1 - 1).c_str());
        CHECK_EQ(csv_row(static_cast<std::uint32_t>(i), t_ms, r.frames[i]), line + "\n");
        (r.frames[i].present_count() ? one : zero)++;
        ++i;
    }
    CHECK_EQ(i, static_cast<std::size_t>(290));
    CHECK_EQ(zero, 29);
    CHECK_EQ(one, 261);
    for (const Frame &f : r.frames) CHECK(plausible(f));
}

void test_config_session()
{
    const std::string bin = test_read_file(test_data_dir() + "ld2452-9600-config-session.bin");
    CHECK_EQ(bin.size(), static_cast<std::size_t>(1778));
    const Result r = run_whole(bytes_of(bin));
    CHECK_EQ(r.stats.frames_ok, 58u);
    CHECK_EQ(r.stats.answers, 2u);
    CHECK_EQ(r.stats.frames_cut, 1u);
    CHECK_EQ(r.stats.frames_bad, 0u);
    CHECK_EQ(r.frames.size(), static_cast<std::size_t>(58));
    /* the cut data frame was 6 bytes; the damaged first byte of the answer is its 7th: all of it is "cut", not garbage */
    CHECK_EQ(r.stats.garbage_bytes, static_cast<std::uint64_t>(0));
    for (const Frame &f : r.frames) CHECK_EQ(f.present_count(), 0);
}

void test_cut_and_damage()
{
    const auto fr = frame_bytes({hex("0E 03 B1 86 10 00 68 01")});
    /* a data frame cut after 6 bytes by an answer with a damaged first byte (FC), then two good frames */
    std::vector<std::uint8_t> d = fr;
    std::vector<std::uint8_t> cut(fr.begin(), fr.begin() + 6);
    d.insert(d.end(), cut.begin(), cut.end());
    const auto ans = hex("FC FC FB FA 08 00 FF 01 00 00 01 00 40 00 04 03 02 01");
    d.insert(d.end(), ans.begin(), ans.end());
    d.insert(d.end(), fr.begin(), fr.end());
    d.insert(d.end(), fr.begin(), fr.end());
    for (const Result &r : {run_whole(d), run_chunked(d, 1), run_chunked(d, 7), run_random_chunks(d, 3)}) {
        CHECK_EQ(r.stats.frames_ok, 3u);
        CHECK_EQ(r.stats.answers, 1u);
        CHECK_EQ(r.stats.frames_cut, 1u);
        CHECK_EQ(r.stats.frames_bad, 0u);
        CHECK_EQ(r.stats.garbage_bytes, static_cast<std::uint64_t>(0));
        CHECK_EQ(r.frames.size(), static_cast<std::size_t>(3));
    }
    /* a clean FD answer after a complete frame; also an answer with an undamaged first byte inside a cut */
    std::vector<std::uint8_t> e = fr;
    const auto ans2 = hex("FD FC FB FA 04 00 FE 01 00 00 04 03 02 01");
    e.insert(e.end(), ans2.begin(), ans2.end());
    e.insert(e.end(), fr.begin(), fr.end());
    const Result r = run_chunked(e, 1);
    CHECK_EQ(r.stats.frames_ok, 2u);
    CHECK_EQ(r.stats.answers, 1u);
    CHECK_EQ(r.stats.frames_cut + r.stats.frames_bad, 0u);

    /* the answer arrives right after the header (cut after 5 bytes) */
    std::vector<std::uint8_t> g(fr.begin(), fr.begin() + 5);
    g.insert(g.end(), ans.begin(), ans.end());
    g.insert(g.end(), fr.begin(), fr.end());
    const Result rg = run_chunked(g, 1);
    CHECK_EQ(rg.stats.frames_ok, 1u);
    CHECK_EQ(rg.stats.answers, 1u);
    CHECK_EQ(rg.stats.frames_cut, 1u);

    /* a data frame with a wrong footer is dropped and the next one recovered */
    std::vector<std::uint8_t> h = fr;
    h[29] = 0x00;
    h.insert(h.end(), fr.begin(), fr.end());
    for (const Result &x : {run_whole(h), run_chunked(h, 1)}) {
        CHECK_EQ(x.stats.frames_ok, 1u);
        CHECK_EQ(x.stats.frames_bad, 1u);
        CHECK_EQ(x.stats.good_run, 1u);
        CHECK_EQ(x.stats.garbage_bytes, static_cast<std::uint64_t>(30)); /* 1 for the header byte + 29 skipped */
    }
    /* a frame that lost a byte in the middle: the next header resyncs */
    std::vector<std::uint8_t> m = fr;
    m.erase(m.begin() + 12);
    m.insert(m.end(), fr.begin(), fr.end());
    const Result rm = run_chunked(m, 3);
    CHECK_EQ(rm.stats.frames_ok, 1u);
    CHECK_EQ(rm.stats.frames_bad, 1u);
    /* an answer with a bad footer is not an answer */
    auto badans = ans;
    badans[17] = 0x00;
    badans.insert(badans.end(), fr.begin(), fr.end());
    const Result rb = run_chunked(badans, 1);
    CHECK_EQ(rb.stats.answers, 0u);
    CHECK_EQ(rb.stats.frames_ok, 1u);
    /* a frame header split over three feeds */
    Decoder dec;
    std::vector<Frame> out;
    dec.feed(fr.data(), 2, &out);
    dec.feed(fr.data() + 2, 3, &out);
    CHECK(out.empty());
    dec.feed(fr.data() + 5, fr.size() - 5, &out);
    CHECK_EQ(out.size(), static_cast<std::size_t>(1));
    /* garbage before a frame */
    std::vector<std::uint8_t> gg = {1, 2, 3, 0xAA, 0xFF, 7};
    gg.insert(gg.end(), fr.begin(), fr.end());
    const Result rgg = run_chunked(gg, 1);
    CHECK_EQ(rgg.stats.frames_ok, 1u);
    CHECK_EQ(rgg.stats.garbage_bytes, static_cast<std::uint64_t>(6));
    /* an implausible frame is a frame but breaks the good run */
    const auto wild = frame_bytes({hex("FF FF FF FF 00 00 00 00")});
    std::vector<std::uint8_t> w = fr;
    w.insert(w.end(), wild.begin(), wild.end());
    const Result rw = run_whole(w);
    CHECK_EQ(rw.stats.frames_ok, 2u);
    CHECK_EQ(rw.stats.good_run, 0u);
}

void test_chunking_equality()
{
    const std::string a = test_read_file(test_data_dir() + "ld2452-9600-walk-15s.bin");
    const std::string b = test_read_file(test_data_dir() + "ld2452-9600-config-session.bin");
    for (const std::string *s : {&a, &b}) {
        const auto d = bytes_of(*s);
        const Result whole = run_whole(d);
        CHECK(same(whole, run_chunked(d, 1)));
        CHECK(same(whole, run_chunked(d, 2)));
        CHECK(same(whole, run_chunked(d, 29)));
        CHECK(same(whole, run_chunked(d, 31)));
        for (unsigned seed = 1; seed <= 5; ++seed) CHECK(same(whole, run_random_chunks(d, seed)));
    }
}

void test_fuzz()
{
    const std::string walk = test_read_file(test_data_dir() + "ld2452-9600-walk-15s.bin");
    const std::string cfg = test_read_file(test_data_dir() + "ld2452-9600-config-session.bin");
    const std::uint8_t alphabet[] = {0xAA, 0xFF, 0x03, 0x00, 0x55, 0xCC, 0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x02, 0x01, 0x40};
    for (unsigned seed = 1; seed <= 300; ++seed) {
        std::mt19937 rng(seed);
        std::vector<std::uint8_t> d;
        const std::size_t n = rng() % 4000;
        switch (seed % 4) {
        case 0: /* pure noise */
            for (std::size_t i = 0; i < n; ++i) d.push_back(static_cast<std::uint8_t>(rng()));
            break;
        case 1: /* noise rich in protocol bytes */
            for (std::size_t i = 0; i < n; ++i) d.push_back(alphabet[rng() % sizeof(alphabet)]);
            break;
        case 2: /* the walk with bytes flipped, dropped and inserted */
            d = bytes_of(walk);
            for (int k = 0; k < 60; ++k) d[rng() % d.size()] = static_cast<std::uint8_t>(rng());
            for (int k = 0; k < 10; ++k) d.erase(d.begin() + static_cast<std::ptrdiff_t>(rng() % d.size()));
            for (int k = 0; k < 10; ++k)
                d.insert(d.begin() + static_cast<std::ptrdiff_t>(rng() % d.size()), static_cast<std::uint8_t>(rng()));
            break;
        default: /* the config session, mutated */
            d = bytes_of(cfg);
            for (int k = 0; k < 30; ++k) d[rng() % d.size()] = alphabet[rng() % sizeof(alphabet)];
            d.resize(rng() % d.size());
            break;
        }
        const Result whole = run_whole(d);
        CHECK(whole.stats.bytes == d.size());
        CHECK(whole.stats.frames_ok <= d.size() / kFrameSize);
        CHECK(whole.stats.garbage_bytes + whole.stats.frames_ok * kFrameSize <= d.size());
        CHECK(same(whole, run_chunked(d, 1)));
        CHECK(same(whole, run_random_chunks(d, seed)));
    }
}

} // namespace

void test_frame()
{
    test_manual_example();
    test_sign_magnitude();
    test_walk_fixture();
    test_config_session();
    test_cut_and_damage();
    test_chunking_equality();
    test_fuzz();
}
