/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the HLK-LD2451 driver. The three real recordings (tests/data/ld2451-*.bin, README there) through
 * the decoder, the link and the tracker with their own timing; the decoder's edge cases (resync, split reads, truncated
 * and inconsistent frames, count above the slot cap); the polar maths and the angle sign; a removed flip_angle settings key; the per-driver
 * timeouts (quiet_ms, drop_ms); the discrimination from the LD2410 both ways; the demo scenarios; listen-only.
 */
#include "check.hpp"

#include "demo_sys.hpp"
#include "detect.hpp"
#include "diag.hpp"
#include "driver.hpp"
#include "fake_sys.hpp"
#include "ld2410.hpp"
#include "ld2450.hpp"
#include "ld2451.hpp"
#include "link.hpp"
#include "settings.hpp"
#include "sniff.hpp"
#include "view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace mmw;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes file_bytes(const std::string &name)
{
    const std::string s = test_read_file(test_data_dir() + name);
    return Bytes(s.begin(), s.end());
}

std::string as_string(const Bytes &b) { return std::string(b.begin(), b.end()); }

/* the capture's chunks: (offset, length, seconds) from the capture script's .ts.csv */
struct Chunk {
    std::size_t off, len;
    double t;
};

std::vector<Chunk> chunks_of(const std::string &name)
{
    std::vector<Chunk> v;
    std::istringstream in(test_read_file(test_data_dir() + name));
    std::string line;
    std::getline(in, line); /* header */
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t c1 = line.find(','), c2 = line.find(',', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        v.push_back({static_cast<std::size_t>(std::atol(line.c_str())),
                     static_cast<std::size_t>(std::atol(line.c_str() + c1 + 1)), std::atof(line.c_str() + c2 + 1)});
    }
    return v;
}

struct Capture {
    const char *stem;
    std::size_t bytes, frames, empty, with_target;
    std::uint64_t garbage;
};
const Capture kCaptures[] = {
    {"ld2451-room-300s", 2758, 242, 194, 48, 2},
    {"ld2451-walk-240s", 2334, 201, 155, 46, 2},
    {"ld2451-115200-60s", 745, 57, 32, 25, 0},
};

Bytes frame_bytes(const std::vector<std::array<int, 5>> &targets, int alarm = 1)
{
    Bytes b = {0xF4, 0xF3, 0xF2, 0xF1};
    const int n = static_cast<int>(targets.size());
    const int len = n ? 2 + 5 * n : 0;
    b.push_back(static_cast<std::uint8_t>(len & 0xFF));
    b.push_back(static_cast<std::uint8_t>(len >> 8));
    if (n) {
        b.push_back(static_cast<std::uint8_t>(n));
        b.push_back(static_cast<std::uint8_t>(alarm));
        for (const auto &t : targets) {
            b.push_back(static_cast<std::uint8_t>(t[0] + 0x80)); /* angle */
            for (int k = 1; k < 5; ++k) b.push_back(static_cast<std::uint8_t>(t[k]));
        }
    }
    for (std::uint8_t f : {0xF8, 0xF7, 0xF6, 0xF5}) b.push_back(f);
    return b;
}
Bytes empty_frame() { return frame_bytes({}); }

std::vector<Frame> decode_all(const Bytes &d, std::size_t chunk, DecoderStats *st = nullptr)
{
    Ld2451Decoder dec;
    std::vector<Frame> f;
    for (std::size_t i = 0; i < d.size(); i += chunk) dec.feed(d.data() + i, std::min(chunk, d.size() - i), &f);
    if (st) *st = dec.stats();
    return f;
}

void same_frames(const std::vector<Frame> &a, const std::vector<Frame> &b)
{
    CHECK_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        bool same = true;
        for (int k = 0; k < 30; ++k) same = same && a[i].raw[k] == b[i].raw[k];
        for (int s = 0; s < 3; ++s)
            same = same && a[i].target[s].present == b[i].target[s].present && a[i].target[s].x_mm == b[i].target[s].x_mm &&
                   a[i].target[s].speed_cm_s == b[i].target[s].speed_cm_s;
        CHECK(same);
    }
}

/* A port that replays a recording with its own timing (the chunk times of the .ts.csv), at 115200 only; any other speed
 * is zeros, as for the other fake ports. */
class ReplaySys : public FakeSys {
public:
    ReplaySys(const Bytes &data, const std::vector<Chunk> &chunks) : data_(data), chunks_(chunks) {}
    bool configure(int h, int baud) override
    {
        start_ms_ = now_ms;
        pos_ = 0;
        return FakeSys::configure(h, baud);
    }
    long read(int h, std::uint8_t *buf, std::size_t n) override
    {
        if (ports.empty() || ports[0].handle != h || ports[0].cur_baud != 115200) return FakeSys::read(h, buf, n);
        const double t = (now_ms - start_ms_) / 1000.0;
        std::size_t avail_end = 0;
        for (const Chunk &c : chunks_)
            if (c.t <= t) avail_end = c.off + c.len;
        std::size_t k = 0;
        while (k < n && pos_ < avail_end && pos_ < data_.size()) buf[k++] = data_[pos_++];
        return static_cast<long>(k);
    }
private:
    Bytes data_;
    std::vector<Chunk> chunks_;
    long start_ms_ = 0;
    std::size_t pos_ = 0;
};

void step(FakeSys &sys, Link &link, long ms, long dt = 30)
{
    const long end = sys.now_ms + ms;
    while (sys.now_ms < end) {
        sys.now_ms += dt;
        link.poll(sys.now_ms);
    }
}

/* ------------------------------------------------------------------------------------------ the recordings */

void test_recordings_decode()
{
    for (const Capture &c : kCaptures) {
        const Bytes d = file_bytes(std::string(c.stem) + ".bin");
        CHECK_EQ(d.size(), c.bytes);
        DecoderStats st;
        const std::vector<Frame> all = decode_all(d, d.size(), &st);
        CHECK_EQ(all.size(), c.frames);
        CHECK_EQ(st.frames_ok, static_cast<std::uint32_t>(c.frames));
        CHECK_EQ(st.frames_bad, 0u);
        CHECK_EQ(st.garbage_bytes, c.garbage); /* the stray "f4 f3" at offset 40 */
        CHECK_EQ(st.bytes, static_cast<std::uint64_t>(c.bytes));
        std::size_t empty = 0, one = 0, other = 0;
        for (const Frame &f : all) {
            if (f.present_count() == 0) ++empty;
            else if (f.present_count() == 1) ++one;
            else ++other;
        }
        CHECK_EQ(empty, c.empty);
        CHECK_EQ(one, c.with_target);
        CHECK_EQ(other, static_cast<std::size_t>(0)); /* never more than one target in the recordings */

        /* any chunking: bytes, 7, 64, and one split at every offset */
        same_frames(decode_all(d, 1), all);
        same_frames(decode_all(d, 7), all);
        same_frames(decode_all(d, 64), all);
        for (std::size_t cut = 1; cut < d.size(); ++cut) {
            Ld2451Decoder dec;
            std::vector<Frame> f;
            dec.feed(d.data(), cut, &f);
            dec.feed(d.data() + cut, d.size() - cut, &f);
            if (f.size() != all.size() || dec.stats().frames_ok != c.frames) {
                CHECK(false); /* reported once per file, not per offset */
                break;
            }
        }
        /* the driver's factory gives the same decoder */
        std::unique_ptr<FrameDecoder> viad = kDriverLd2451.make_decoder();
        std::vector<Frame> g;
        viad->feed(d.data(), d.size(), &g);
        same_frames(g, all);
    }
}

/* Direction semantics from the room recording: the legs where the distance grows carry direction byte 0 (receding),
 * the legs where it falls carry byte 1 (approaching), against the document's reading. */
void test_direction_semantics()
{
    for (const Capture &c : kCaptures) {
        const std::vector<Frame> all = decode_all(file_bytes(std::string(c.stem) + ".bin"), 64);
        std::vector<std::vector<const Frame *>> runs(1);
        for (const Frame &f : all) {
            if (f.present_count() == 0) {
                if (!runs.back().empty()) runs.emplace_back();
            } else {
                runs.back().push_back(&f);
            }
        }
        int checked = 0;
        for (const auto &r : runs) {
            if (r.size() < 6) continue;
            bool uniform = true;
            for (const Frame *f : r) uniform = uniform && f->target[0].direction == r[0]->target[0].direction;
            const int d0 = r.front()->target[0].distance_mm, d1 = r.back()->target[0].distance_mm;
            if (!uniform || d0 == d1) continue;
            ++checked;
            CHECK(r[0]->target[0].direction == (d1 > d0 ? Direction::Receding : Direction::Approaching));
            CHECK((r[0]->target[0].speed_cm_s > 0) == (d1 > d0)); /* approaching is negative */
        }
        CHECK(checked >= 1);
    }
    /* the room recording: the three long passes, as found */
    const std::vector<Frame> room = decode_all(file_bytes("ld2451-room-300s.bin"), 64);
    std::vector<std::vector<const Frame *>> runs(1);
    for (const Frame &f : room) {
        if (f.present_count() == 0) {
            if (!runs.back().empty()) runs.emplace_back();
        } else {
            runs.back().push_back(&f);
        }
    }
    CHECK(runs.size() >= 5);
    CHECK_EQ(runs[0].size(), static_cast<std::size_t>(15)); /* away: 2 m to 4 m, byte 0 */
    CHECK_EQ(runs[0].front()->target[0].distance_mm, 2000);
    CHECK_EQ(runs[0].back()->target[0].distance_mm, 4000);
    for (const Frame *f : runs[0]) CHECK(f->target[0].direction == Direction::Receding);
    CHECK_EQ(runs[3].size(), static_cast<std::size_t>(6)); /* toward: 4 m to 2 m, byte 1 */
    CHECK_EQ(runs[3].front()->target[0].distance_mm, 4000);
    CHECK_EQ(runs[3].back()->target[0].distance_mm, 2000);
    for (const Frame *f : runs[3]) CHECK(f->target[0].direction == Direction::Approaching);
    CHECK_EQ(runs[4].size(), static_cast<std::size_t>(9)); /* away again */
    for (const Frame *f : runs[4]) CHECK(f->target[0].direction == Direction::Receding);
}

/* The recordings through the decoder and the tracker with their own timing: the per-driver release time keeps a pass
 * together; the LD2450's 0.5 s would split it at every gap. */
void test_recordings_tracker()
{
    for (const Capture &c : kCaptures) {
        const Bytes d = file_bytes(std::string(c.stem) + ".bin");
        const std::vector<Chunk> ch = chunks_of(std::string(c.stem) + ".ts.csv");
        CHECK(!ch.empty());
        std::uint32_t ids[2] = {0, 0};
        std::int64_t longest_gap_ms = 0; /* between two target frames of one pass (no empty frame in between) */
        for (int mode = 0; mode < 2; ++mode) {
            TrackerConfig tc;
            tc.drop_ms = mode == 0 ? kDriverLd2451.drop_ms : kDriverLd2450.drop_ms;
            Tracker tr(tc);
            Ld2451Decoder dec;
            std::int64_t last_target_ms = -1, clock = 0;
            for (const Chunk &k : ch) {
                std::vector<Frame> f;
                dec.feed(d.data() + k.off, k.len, &f);
                const std::int64_t t = static_cast<std::int64_t>(std::llround(k.t * 1000.0));
                for (; clock + 30 < t; clock += 30) tr.tick(clock); /* the app's timer runs between the frames */
                clock = t;
                for (const Frame &fr : f) {
                    tr.update(fr, t);
                    if (fr.present_count()) {
                        if (mode == 0 && last_target_ms >= 0 && t - last_target_ms > longest_gap_ms)
                            longest_gap_ms = t - last_target_ms;
                        last_target_ms = t;
                    } else {
                        last_target_ms = -1; /* the sensor said "nothing": the pass is over */
                    }
                }
                tr.tick(t);
                ids[mode] = std::max(ids[mode], tr.slot(0).id);
            }
            CHECK_EQ(tr.frames(), static_cast<std::uint32_t>(c.frames));
        }
        /* the longest gap inside a pass (2.1 s, 2.4 s, 2.65 s in the three recordings) is far above the LD2450's 0.5 s and
         * under the LD2451's release time */
        CHECK(longest_gap_ms > 2000);
        CHECK(longest_gap_ms < kDriverLd2451.drop_ms);
        CHECK(ids[0] >= 1);
        CHECK(ids[0] < ids[1]); /* fewer new targets with the longer release time */
    }
}

/* the same, end to end through a Link: found at 115200 on the recording's own timing, never lost between frames */
void test_recordings_link()
{
    for (const Capture &c : kCaptures) {
        const Bytes d = file_bytes(std::string(c.stem) + ".bin");
        ReplaySys sys(d, chunks_of(std::string(c.stem) + ".ts.csv"));
        sys.add("ttyUSB0");
        Link link(sys); /* Auto */
        link.set_diag_counting(true);
        link.start(0);
        double last_t = chunks_of(std::string(c.stem) + ".ts.csv").back().t;
        bool ever_found = false, lost_after_found = false;
        while (sys.now_ms < static_cast<long>((last_t + 1.0) * 1000)) { /* to the end of the recording */
            sys.now_ms += 30;
            link.poll(sys.now_ms);
            if (link.state() == LinkState::Found) ever_found = true;
            else if (ever_found) lost_after_found = true;
        }
        CHECK(link.state() == LinkState::Found);
        CHECK(ever_found);
        CHECK(!lost_after_found); /* the 1.4 s empty frames and the 2.8 s gaps never read as Lost */
        CHECK(link.driver() == &kDriverLd2451);
        CHECK_EQ(link.baud(), 115200);
        CHECK_EQ(link.remembered().baud, 115200);
        /* every frame after the first burst (the detector reads those): all of the target frames, none bad */
        CHECK_EQ(link.diag_counts().total_nonempty, static_cast<std::uint64_t>(c.with_target));
        CHECK(link.diag_counts().total_empty + link.diag_counts().total_nonempty >= c.frames - 5);
        CHECK(link.diag_counts().total_empty + link.diag_counts().total_nonempty <= c.frames);
        CHECK_EQ(link.stats().bad_frames, 0u);
        CHECK_EQ(link.tracker().frames(), link.stats().frames);
        CHECK(link.tracker().frames() >= c.frames - 5);
        link.close();
    }
}

/* ------------------------------------------------------------------------------------------ crafted frames */

void test_decode_values()
{
    /* a target: angle +10, 40 m, approaching (byte 1), 60 km/h, snr 0x15 */
    Frame f;
    Bytes b = frame_bytes({{10, 40, 1, 60, 0x15}});
    CHECK(ld2451_decode_frame(b.data(), b.size(), f));
    const Target &t = f.target[0];
    CHECK(t.present && !f.target[1].present && !f.target[2].present);
    CHECK_EQ(t.slot, 1);
    CHECK_EQ(t.distance_mm, 40000);
    CHECK_EQ(t.angle_deg, 10.0);
    CHECK(t.direction == Direction::Approaching);
    CHECK_EQ(t.speed_cm_s, -1667); /* 60 km/h = 1666.7 cm/s, approaching = negative */
    CHECK_EQ(t.snr, 0x15);
    CHECK_EQ(t.resolution_mm, 1000);
    CHECK_EQ(t.x_mm, static_cast<int>(std::lround(40000 * std::sin(10 * M_PI / 180))));
    CHECK_EQ(t.y_mm, static_cast<int>(std::lround(40000 * std::cos(10 * M_PI / 180))));
    CHECK_EQ(f.raw[0], 0xF4);
    CHECK_EQ(f.raw[4], 7);
    /* receding: direction byte 0, positive speed */
    b = frame_bytes({{-10, 30, 0, 60, 0x0F}});
    CHECK(ld2451_decode_frame(b.data(), b.size(), f));
    CHECK(f.target[0].direction == Direction::Receding);
    CHECK_EQ(f.target[0].speed_cm_s, 1667);
    CHECK(f.target[0].x_mm < 0 && f.target[0].y_mm > 0);
    /* the km/h of the screen: 3 km/h reads 3.0 */
    b = frame_bytes({{0, 4, 1, 3, 255}});
    CHECK(ld2451_decode_frame(b.data(), b.size(), f));
    CHECK_EQ(format_speed(f.target[0].speed_cm_s, UnitMode::MetresKmh), "-3.0");
    b = frame_bytes({{0, 4, 0, 120, 255}});
    CHECK(ld2451_decode_frame(b.data(), b.size(), f));
    CHECK_EQ(format_speed(f.target[0].speed_cm_s, UnitMode::MetresKmh), "120.0");
    /* the alarm byte is read and not used: a different value changes nothing */
    const Bytes b1 = frame_bytes({{5, 9, 1, 7, 1}}, 1), b0 = frame_bytes({{5, 9, 1, 7, 1}}, 0);
    Frame f1, f0;
    CHECK(ld2451_decode_frame(b1.data(), b1.size(), f1) && ld2451_decode_frame(b0.data(), b0.size(), f0));
    CHECK(f1.target[0].x_mm == f0.target[0].x_mm && f1.target[0].speed_cm_s == f0.target[0].speed_cm_s &&
          f1.target[0].direction == f0.target[0].direction);
    /* the empty frame */
    b = empty_frame();
    CHECK_EQ(b.size(), static_cast<std::size_t>(10));
    CHECK(ld2451_decode_frame(b.data(), b.size(), f));
    CHECK_EQ(f.present_count(), 0);
    for (int i = 0; i < 3; ++i) CHECK_EQ(f.target[i].slot, i + 1);
    /* the same with len 2 and count 0 (the document's form) */
    const Bytes z = {0xF4, 0xF3, 0xF2, 0xF1, 0x02, 0x00, 0x00, 0x00, 0xF8, 0xF7, 0xF6, 0xF5};
    CHECK(ld2451_decode_frame(z.data(), z.size(), f));
    CHECK_EQ(f.present_count(), 0);
    std::vector<Frame> out;
    Ld2451Decoder dec;
    dec.feed(z.data(), z.size(), &out);
    CHECK_EQ(out.size(), static_cast<std::size_t>(1));
    CHECK_EQ(dec.stats().frames_bad, 0u);
    CHECK_EQ(dec.stats().good_run, 1u);
    /* the document's example: 3 targets */
    const Bytes ex = {0xF4, 0xF3, 0xF2, 0xF1, 0x11, 0x00, 0x03, 0x01, 0x8A, 0x28, 0x00, 0x3C, 0x15, 0x8A, 0x1E, 0x01, 0x3C,
                      0x0F, 0x76, 0x5F, 0x00, 0x3C, 0x0F, 0xF8, 0xF7, 0xF6, 0xF5};
    CHECK(ld2451_decode_frame(ex.data(), ex.size(), f));
    CHECK_EQ(f.present_count(), 3);
    CHECK_EQ(f.target[0].angle_deg, 10.0);
    CHECK_EQ(f.target[0].distance_mm, 40000);
    CHECK_EQ(f.target[2].angle_deg, -10.0);
    CHECK_EQ(f.target[2].distance_mm, 95000);
    CHECK(f.target[0].direction == Direction::Receding && f.target[1].direction == Direction::Approaching);
    /* angle byte edges: 0x80 is straight ahead, 0x00 is -128 (not plausible), 0xFF is +127 */
    b = frame_bytes({{0, 5, 1, 3, 9}});
    CHECK(ld2451_decode_frame(b.data(), b.size(), f));
    CHECK_EQ(f.target[0].angle_deg, 0.0);
    CHECK_EQ(f.target[0].x_mm, 0);
    CHECK_EQ(f.target[0].y_mm, 5000);
    CHECK(ld2451_plausible(b.data(), b.size()));
    b = frame_bytes({{-128, 5, 1, 3, 9}});
    CHECK(ld2451_decode_frame(b.data(), b.size(), f));
    CHECK_EQ(f.target[0].angle_deg, -128.0);
    CHECK(!ld2451_plausible(b.data(), b.size()));
    b = frame_bytes({{127, 5, 1, 3, 9}});
    CHECK(!ld2451_plausible(b.data(), b.size()));
    /* a direction byte that is neither 0 nor 1 reads as Still and is not plausible */
    b = frame_bytes({{0, 5, 2, 3, 9}});
    CHECK(ld2451_decode_frame(b.data(), b.size(), f));
    CHECK(f.target[0].direction == Direction::Still);
    CHECK(!ld2451_plausible(b.data(), b.size()));
}

void test_polar_maths()
{
    int x, y;
    ld2451_polar_to_xy(10, 0, x, y);
    CHECK(x == 0 && y == 10000);
    ld2451_polar_to_xy(10, 30, x, y);
    CHECK(x == 5000 && y == 8660);
    ld2451_polar_to_xy(10, -30, x, y);
    CHECK(x == -5000 && y == 8660);
    ld2451_polar_to_xy(2, 90, x, y);
    CHECK(x == 2000 && y == 0);
    ld2451_polar_to_xy(100, 45, x, y);
    CHECK(x == 70711 && y == 70711);
    ld2451_polar_to_xy(0, 20, x, y);
    CHECK(x == 0 && y == 0);
    /* distance and angle stay exact in the Frame, whatever the rounding of x and y */
    const Bytes b = frame_bytes({{37, 100, 0, 5, 1}});
    Frame f;
    CHECK(ld2451_decode_frame(b.data(), b.size(), f));
    CHECK_EQ(f.target[0].distance_mm, 100000);
    CHECK_EQ(f.target[0].angle_deg, 37.0);
    const double r = std::hypot(static_cast<double>(f.target[0].x_mm), static_cast<double>(f.target[0].y_mm));
    CHECK(std::fabs(r - 100000.0) < 1.0);
}

void test_decoder_edges()
{
    const Bytes t1 = frame_bytes({{3, 6, 1, 4, 100}}), e = empty_frame();
    /* resync after garbage, a stray fragment first */
    Bytes d = {0xF4, 0xF3};
    d.insert(d.end(), t1.begin(), t1.end());
    d.insert(d.end(), {0x00, 0xAA, 0xF4, 0x13, 0xF4, 0xF3, 0xF2});
    d.insert(d.end(), e.begin(), e.end());
    d.insert(d.end(), {0xF8, 0xF7, 0xF6, 0xF5, 0xF4}); /* a footer without a frame, then a lone F4 */
    d.insert(d.end(), t1.begin(), t1.end());
    DecoderStats st;
    std::vector<Frame> f = decode_all(d, d.size(), &st);
    CHECK_EQ(f.size(), static_cast<std::size_t>(3));
    CHECK_EQ(st.frames_ok, 3u);
    CHECK_EQ(st.garbage_bytes, static_cast<std::uint64_t>(2 + 2 + 2 + 3 + 4 + 1)); /* stray, 00 AA, F4 13, F4 F3 F2, footer, F4 */
    CHECK_EQ(st.good_run, 1u); /* the garbage before the last frame reset the run */
    for (std::size_t chunk : {static_cast<std::size_t>(1), static_cast<std::size_t>(3), static_cast<std::size_t>(5)}) {
        DecoderStats s2;
        same_frames(decode_all(d, chunk, &s2), f);
        CHECK_EQ(s2.garbage_bytes, st.garbage_bytes);
    }
    /* a truncated frame at the end: nothing yielded, nothing malformed, it completes when the rest arrives */
    {
        Ld2451Decoder dec;
        std::vector<Frame> out;
        dec.feed(t1.data(), t1.size() - 3, &out);
        CHECK(out.empty());
        CHECK_EQ(dec.stats().frames_bad, 0u);
        dec.feed(t1.data() + t1.size() - 3, 3, &out);
        CHECK_EQ(out.size(), static_cast<std::size_t>(1));
        /* a header alone, then silence, then the rest */
        dec.reset();
        out.clear();
        dec.feed(e.data(), 4, &out);
        dec.feed(e.data() + 4, 5, &out);
        CHECK(out.empty());
        dec.feed(e.data() + 9, 1, &out);
        CHECK_EQ(out.size(), static_cast<std::size_t>(1));
        CHECK_EQ(dec.stats().garbage_bytes, static_cast<std::uint64_t>(0));
    }
    /* length inconsistencies: each one malformed, counted, and the next good frame still decodes */
    struct Bad {
        const char *what;
        Bytes bytes;
    };
    Bytes len3 = {0xF4, 0xF3, 0xF2, 0xF1, 0x03, 0x00, 0x01, 0x01, 0x80, 0xF8, 0xF7, 0xF6, 0xF5};      /* len not 0 or 2+5n */
    Bytes len1 = {0xF4, 0xF3, 0xF2, 0xF1, 0x01, 0x00, 0x00, 0xF8, 0xF7, 0xF6, 0xF5};                  /* len 1 */
    Bytes cnt = t1;                                                                                   /* count != len */
    cnt[6] = 2;
    Bytes foot = t1;                                                                                  /* wrong footer */
    foot[foot.size() - 1] = 0xF4;
    Bytes huge = {0xF4, 0xF3, 0xF2, 0xF1, 0xFF, 0xFF};                                                /* len 65535 */
    Bytes over = {0xF4, 0xF3, 0xF2, 0xF1, static_cast<std::uint8_t>(2 + 5 * 17), 0x00, 17, 1};
    over.insert(over.end(), 17 * 5, 0x80);
    over.insert(over.end(), {0xF8, 0xF7, 0xF6, 0xF5});
    const Bad bads[] = {{"len 3", len3}, {"len 1", len1}, {"count", cnt}, {"footer", foot}, {"len ffff", huge}, {"count 17", over}};
    for (const Bad &bd : bads) {
        Bytes s = bd.bytes;
        s.insert(s.end(), t1.begin(), t1.end());
        s.insert(s.end(), e.begin(), e.end());
        DecoderStats sb;
        const std::vector<Frame> fb = decode_all(s, s.size(), &sb);
        if (fb.size() != 2) std::fprintf(stderr, "bad frame case '%s': %zu frames\n", bd.what, fb.size());
        CHECK_EQ(fb.size(), static_cast<std::size_t>(2));
        CHECK(sb.frames_bad >= 1u);
        CHECK(fb.size() == 2 && fb[0].present_count() == 1 && fb[1].present_count() == 0);
        same_frames(decode_all(s, 1), fb);
        /* none of them is a frame for the direct decoder either */
        Frame x;
        CHECK(!ld2451_decode_frame(bd.bytes.data(), bd.bytes.size(), x));
    }
    /* a count above the slot cap: the first three fill the slots, the rest are read and dropped, nothing overruns */
    for (int n : {4, 5, 16}) {
        std::vector<std::array<int, 5>> ts;
        for (int i = 0; i < n; ++i) ts.push_back({i * 2 - 10, 3 + i, 1, 3 + i, 100 + i});
        const Bytes b = frame_bytes(ts);
        DecoderStats sn;
        const std::vector<Frame> fn = decode_all(b, b.size(), &sn);
        CHECK_EQ(fn.size(), static_cast<std::size_t>(1));
        CHECK_EQ(sn.frames_bad, 0u);
        CHECK_EQ(fn[0].present_count(), 3);
        CHECK_EQ(fn[0].target[2].distance_mm, 5000);
        CHECK_EQ(fn[0].target[2].angle_deg, -6.0);
        CHECK_EQ(fn[0].target[2].slot, 3);
        same_frames(decode_all(b, 1), fn);
    }
    /* count 0 with len 2 between targets */
    {
        Bytes s = t1;
        const Bytes z = {0xF4, 0xF3, 0xF2, 0xF1, 0x02, 0x00, 0x00, 0x00, 0xF8, 0xF7, 0xF6, 0xF5};
        s.insert(s.end(), z.begin(), z.end());
        s.insert(s.end(), t1.begin(), t1.end());
        DecoderStats sz;
        const std::vector<Frame> fz = decode_all(s, 1, &sz);
        CHECK_EQ(fz.size(), static_cast<std::size_t>(3));
        CHECK_EQ(sz.frames_bad, 0u);
        CHECK_EQ(fz[1].present_count(), 0);
    }
    /* a footer-sized garbage run inside a long stream does not hang or grow the buffer */
    {
        Ld2451Decoder dec;
        Bytes noise(5000);
        unsigned x = 12345;
        for (auto &v : noise) {
            x = x * 1103515245u + 12345u;
            v = static_cast<std::uint8_t>(x >> 16);
        }
        std::vector<Frame> out;
        dec.feed(noise.data(), noise.size(), &out);
        dec.feed(t1.data(), t1.size(), &out);
        CHECK(out.size() >= 1);
        CHECK_EQ(out.back().present_count(), 1);
    }
    /* reset clears the counters */
    {
        Ld2451Decoder dec;
        dec.feed(t1.data(), t1.size(), nullptr);
        CHECK_EQ(dec.stats().frames_ok, 1u);
        dec.reset();
        CHECK_EQ(dec.stats().frames_ok, 0u);
        CHECK_EQ(dec.stats().bytes, static_cast<std::uint64_t>(0));
    }
}

/* ------------------------------------------------------------------------------------------ sniff vs the LD2410 */

Bytes ld2410_frame()
{
    return {0xF4, 0xF3, 0xF2, 0xF1, 0x0D, 0x00, 0x02, 0xAA, 0x02, 0x1E, 0x00, 0x3C, 0x00, 0x00, 0x39, 0x00, 0x00, 0x55, 0x00,
            0xF8, 0xF7, 0xF6, 0xF5};
}
Bytes ld2410_engineering_frame()
{
    Bytes b = {0xF4, 0xF3, 0xF2, 0xF1, 0x23, 0x00, 0x01, 0xAA};
    for (int i = 0; i < 0x23 - 4; ++i) b.push_back(static_cast<std::uint8_t>(i + 1));
    b.insert(b.end(), {0x55, 0x00, 0xF8, 0xF7, 0xF6, 0xF5});
    return b;
}

Bytes repeat(const Bytes &f, int n)
{
    Bytes s;
    for (int i = 0; i < n; ++i) s.insert(s.end(), f.begin(), f.end());
    return s;
}

void test_sniff_discrimination()
{
    const SniffResult M = SniffResult::Match, MB = SniffResult::Maybe, N = SniffResult::No;
    /* the real recordings: LD2451 only */
    for (const Capture &c : kCaptures) {
        const Bytes d = file_bytes(std::string(c.stem) + ".bin");
        CHECK(kDriverLd2451.sniff(d.data(), d.size()) == M);
        CHECK(kDriverLd2410.sniff(d.data(), d.size()) == N);
        CHECK(kDriverLd2450.sniff(d.data(), d.size()) == N);
        CHECK(!ld2410_frame_at(d.data(), d.size()));
        Sniffer sn;
        sn.feed(d.data(), d.size());
        CHECK(sn.kind() == Sniffed::None); /* not named as an LD2410 */
    }
    /* the empty frame alone (len 0): LD2451, not LD2410, not "only zeros" or unknown */
    {
        const Bytes e = repeat(empty_frame(), 4);
        CHECK(kDriverLd2451.sniff(e.data(), e.size()) == M);
        CHECK(kDriverLd2451.sniff(e.data(), 10) == M);
        CHECK(kDriverLd2410.sniff(e.data(), e.size()) == N);
        Sniffer sn;
        sn.feed(e.data(), e.size());
        CHECK(sn.kind() == Sniffed::None);
        CHECK(sn.describe_unknown() == "unknown data"); /* only reached when nothing claimed it: the decoder does */
        CHECK(sn.describe_unknown() != "only zeros");
        /* partial: the start of a frame is "maybe" for the LD2451 and nothing for the LD2410 */
        for (std::size_t n = 1; n < 10; ++n) CHECK(kDriverLd2451.sniff(e.data(), n) == MB);
        CHECK(kDriverLd2410.sniff(e.data(), 4) == MB); /* a bare header could still be anyone's */
        CHECK(kDriverLd2410.sniff(e.data(), 10) == N);
    }
    /* the LD2410's frames: LD2410 only (basic and engineering) */
    for (const Bytes &l : {ld2410_frame(), ld2410_engineering_frame()}) {
        const Bytes s = repeat(l, 3);
        CHECK(kDriverLd2410.sniff(s.data(), s.size()) == M);
        CHECK(kDriverLd2451.sniff(s.data(), s.size()) == N);
        CHECK(ld2410_frame_at(s.data(), s.size()));
        Sniffer sn;
        sn.feed(s.data(), s.size());
        CHECK(sn.kind() == Sniffed::Ld2410);
        /* the decoder of the LD2451 finds no frame in it */
        DecoderStats st;
        CHECK(decode_all(s, s.size(), &st).empty());
        CHECK(st.frames_bad >= 1u && st.frames_ok == 0u);
    }
    /* a window that ends inside a frame */
    {
        const Bytes l = ld2410_frame();
        CHECK(kDriverLd2410.sniff(l.data(), 6) == MB);
        CHECK(kDriverLd2410.sniff(l.data(), 8) == MB);
        CHECK(kDriverLd2410.sniff(l.data(), 20) == MB);
        CHECK(kDriverLd2451.sniff(l.data(), 6) == N);  /* length 13 is not 0 or 2+5n: not ours */
        CHECK(kDriverLd2451.sniff(l.data(), 4) == MB); /* the bare header */
        const Bytes t = frame_bytes({{3, 6, 1, 4, 100}});
        CHECK(kDriverLd2451.sniff(t.data(), 6) == MB);
        CHECK(kDriverLd2451.sniff(t.data(), t.size() - 1) == MB);
        CHECK(kDriverLd2451.sniff(t.data(), t.size()) == M);
        CHECK(kDriverLd2410.sniff(t.data(), 6) == MB);         /* len 7: the type and head byte are not in the window yet */
        CHECK(kDriverLd2410.sniff(t.data(), 8) == N);          /* count 1 passes for a data type, but AA is expected: alarm 1 */
        CHECK(kDriverLd2410.sniff(t.data(), t.size()) == N);
    }
    /* an LD2451 target frame is never an LD2410 one, whatever the values; one that would pass the LD2410's own checks
     * (alarm AA, speed 55, snr 00) is the LD2410's and not the LD2451's: it needs an alarm byte of 0 or 1 */
    {
        for (int count = 1; count <= 3; ++count) {
            std::vector<std::array<int, 5>> ts;
            for (int i = 0; i < count; ++i) ts.push_back({i, 5 + i, 1, 0x55, 0});
            const Bytes b = frame_bytes(ts, 1);
            CHECK(!ld2410_frame_at(b.data(), b.size()));
            CHECK(kDriverLd2451.sniff(b.data(), b.size()) == M);
        }
        const Bytes odd = frame_bytes({{0, 5, 1, 0x55, 0}}, 0xAA);
        CHECK(ld2410_frame_at(odd.data(), odd.size()));
        CHECK(kDriverLd2410.sniff(odd.data(), odd.size()) == M);
        CHECK(kDriverLd2451.sniff(odd.data(), odd.size()) == N); /* decodes, but an alarm of AA is not plausible */
    }
    /* the stricter LD2410 rule: header and footer with a good length but not an LD2410 frame is no LD2410 */
    {
        Bytes l = ld2410_frame();
        l[6] = 0x07; /* data type */
        CHECK(!ld2410_frame_at(l.data(), l.size()));
        l = ld2410_frame();
        l[7] = 0x00; /* head byte */
        CHECK(!ld2410_frame_at(l.data(), l.size()));
        l = ld2410_frame();
        l[17] = 0x56; /* tail */
        CHECK(!ld2410_frame_at(l.data(), l.size()));
        CHECK(ld2410_frame_at(ld2410_frame().data(), ld2410_frame().size()));
    }
    /* mixed streams in the detector: each is its own thing, neither is called "unknown" or "only zeros" */
    const struct {
        const char *name;
        Bytes stream;
        const char *expect_text;
    } cases[] = {
        {"empty only", repeat(empty_frame(), 6), "mmWave radar (LD2451)"},
        {"ld2410", repeat(ld2410_frame(), 6), "not a supported radar: LD2410 presence radar (not supported yet)"},
        {"ld2451 capture", file_bytes("ld2451-room-300s.bin"), "mmWave radar (LD2451)"},
    };
    for (const auto &cs : cases) {
        FakeSys sys;
        FakePort &p = sys.add("ttyUSB0");
        p.stream[115200] = as_string(cs.stream);
        p.rate_bps = 11520;
        Detector det(sys);
        det.start(0, Remembered());
        const long t0 = sys.now_ms;
        while (det.running() && sys.now_ms - t0 < 30000) {
            sys.now_ms += 20;
            det.poll(sys.now_ms);
        }
        CHECK(det.finished());
        if (det.reports().empty()) {
            CHECK(false);
            continue;
        }
        if (det.reports()[0].text != cs.expect_text)
            std::fprintf(stderr, "%s: [%s]\n", cs.name, det.reports()[0].text.c_str());
        CHECK_EQ(det.reports()[0].text, cs.expect_text);
        if (det.found()) sys.close(det.take_radar().handle);
    }
}

/* ------------------------------------------------------------------------------------------ detect and link */

void test_empty_only_stream()
{
    /* a sensor with nothing in view: one empty frame every 1.4 s at 115200, junk at any other speed */
    for (const char *type : {"auto", "ld2451"}) {
        FakeSys sys;
        FakePort &p = sys.add("ttyUSB0");
        p.stream[115200] = as_string(empty_frame());
        p.rate_bps = 10.0 / 1.4;
        LinkConfig cfg;
        cfg.detect.sensor = type;
        Link link(sys, cfg);
        link.start(0);
        bool lost_or_search_after_found = false, found = false;
        while (sys.now_ms < 90000) {
            sys.now_ms += 30;
            link.poll(sys.now_ms);
            if (link.state() == LinkState::Found) found = true;
            else if (found) lost_or_search_after_found = true;
        }
        CHECK(found);
        CHECK(!lost_or_search_after_found);
        CHECK(link.state() == LinkState::Found); /* present, no targets */
        CHECK(link.driver() == &kDriverLd2451);
        CHECK_EQ(link.baud(), 115200);
        CHECK(link.stats().frames > 40);
        CHECK_EQ(link.tracker().slot(0).active ? 1 : 0, 0);
        CHECK(link.last_frame().present_count() == 0);
        CHECK(diag_line(link, sys.now_ms).find(" baud=115200 driver=ld2451 frames=") != std::string::npos);
        CHECK(diag_line(link, sys.now_ms).find("state=Found") != std::string::npos);
        /* it is never named as something else */
        CHECK_EQ(link.reports().size(), static_cast<std::size_t>(1));
        CHECK_EQ(link.reports()[0].text, "mmWave radar (LD2451)");
        CHECK_EQ(std::string(link.model()), "LD2451 target radar (angle, range, speed)");
        link.close();
    }
    /* a fixed LD2450 does not take it: not a radar */
    {
        FakeSys sys;
        FakePort &p = sys.add("ttyUSB0");
        p.stream[115200] = as_string(empty_frame());
        p.rate_bps = 10.0 / 1.4;
        DetectConfig cfg;
        cfg.sensor = "ld2450";
        Detector det(sys, cfg);
        det.start(0, Remembered());
        while (det.running() && sys.now_ms < 30000) {
            sys.now_ms += 20;
            det.poll(sys.now_ms);
        }
        CHECK(det.finished() && !det.found());
    }
    /* a port that sends one byte every 1.5 s at the first speed (what a quiet LD2451 sounds like at the wrong speed) is
     * not "silent" in Auto, where the LD2451 listens; a fixed LD2450 keeps the 0.6 s rule */
    for (const char *type : {"auto", "ld2450"}) {
        FakeSys sys;
        FakePort &p = sys.add("ttyUSB0");
        p.stream[256000] = std::string(1, '');
        p.rate_bps = 1.0 / 1.5;
        p.wrong_speed.clear();
        DetectConfig cfg;
        cfg.sensor = type;
        Detector det(sys, cfg);
        det.start(0, Remembered());
        while (det.running() && sys.now_ms < 30000) {
            sys.now_ms += 20;
            det.poll(sys.now_ms);
        }
        CHECK(det.finished());
        CHECK(!det.reports().empty());
        if (!det.reports().empty())
            CHECK((det.reports()[0].verdict == Verdict::Silent) == (std::string(type) == "ld2450"));
    }
}

void test_per_driver_timeouts()
{
    CHECK_EQ(kDriverLd2450.quiet_ms, 2000); /* exactly 0.1.0's lost_ms */
    CHECK_EQ(kDriverLd2450.drop_ms, 500);   /* exactly TrackerConfig's */
    CHECK_EQ(kDriverLd2450.listen_ms, 0);
    CHECK_EQ(kDriverLd2450.silent_ms, 0);
    CHECK_EQ(kDriverLd2450.max_range_m, 6);
    CHECK_EQ(kDriverLd2451.quiet_ms, 5000);
    CHECK_EQ(kDriverLd2451.drop_ms, 3000);
    CHECK_EQ(TrackerConfig().drop_ms, 500);
    CHECK_EQ(LinkConfig().lost_ms, 0);
    CHECK(kDriverLd2451.max_range_m >= 100);
    CHECK_EQ(kDriverLd2451.baud_count, static_cast<std::size_t>(1));
    CHECK_EQ(kDriverLd2451.bauds[0], 115200);
    CHECK(kDriverLd2451.supported());
    CHECK(kDriverLd2451.caps.tracking_targets && kDriverLd2451.caps.polar);
    CHECK(!kDriverLd2450.caps.polar);
    CHECK_EQ(std::string(kDriverLd2451.id), "ld2451");
    CHECK_EQ(std::string(kDriverLd2451.name), "LD2451");
    CHECK(std::string(kDriverLd2451.name).size() <= 16);
    CHECK(std::string(kDriverLd2450.name).size() <= 16);

    /* the LD2451 sensor going quiet: still Found after 4 s, Lost after 5 s; the tracker's release time is the driver's */
    {
        FakeSys sys;
        FakePort &p = sys.add("ttyUSB0");
        p.stream[115200] = as_string(frame_bytes({{3, 6, 1, 4, 100}}));
        p.rate_bps = 17 / 0.14;
        LinkConfig cfg;
        cfg.detect.sensor = "ld2451";
        Link link(sys, cfg);
        link.start(0);
        step(sys, link, 8000);
        CHECK(link.state() == LinkState::Found);
        CHECK_EQ(link.tracker().drop_ms(), 3000);
        CHECK_EQ(link.lost_after_ms(), 5000);
        CHECK(link.tracker().slot(0).active);
        p.stopped = true;
        step(sys, link, 2500);
        CHECK(link.state() == LinkState::Found);
        CHECK(link.tracker().slot(0).active); /* kept: drop_ms is 3 s */
        step(sys, link, 1000);
        CHECK(link.state() == LinkState::Found); /* 3.5 s: quiet, not lost (5 s) */
        CHECK(!link.tracker().slot(0).active);   /* released */
        step(sys, link, 1000);
        CHECK(link.state() == LinkState::Found); /* 4.5 s */
        step(sys, link, 1000);
        CHECK(link.state() != LinkState::Found); /* 5.5 s */
        link.close();
    }
    /* the LD2450's values are exactly the old ones: lost after 2 s, released after 0.5 s */
    {
        FakeSys sys;
        FakePort &p = sys.add("ttyUSB0");
        p.stream[9600] = test_read_file(test_data_dir() + "ld2452-9600-walk-15s.bin");
        Link link(sys);
        link.start(0);
        step(sys, link, 6000);
        CHECK(link.state() == LinkState::Found);
        CHECK(link.driver() == &kDriverLd2450);
        CHECK_EQ(link.tracker().drop_ms(), 500);
        CHECK_EQ(link.lost_after_ms(), 2000);
        p.stopped = true;
        step(sys, link, 1800);
        CHECK(link.state() == LinkState::Found);
        step(sys, link, 400);
        CHECK(link.state() != LinkState::Found);
        link.close();
    }
    /* an explicit lost_ms still overrides the driver's */
    {
        FakeSys sys;
        FakePort &p = sys.add("ttyUSB0");
        p.stream[9600] = test_read_file(test_data_dir() + "ld2452-9600-walk-15s.bin");
        LinkConfig cfg;
        cfg.lost_ms = 800;
        Link link(sys, cfg);
        link.start(0);
        step(sys, link, 6000);
        CHECK_EQ(link.lost_after_ms(), 800);
        p.stopped = true;
        step(sys, link, 1200);
        CHECK(link.state() != LinkState::Found);
        link.close();
    }
}

void test_tracker_gap()
{
    /* a target with a gap: held through 2.65 s with the LD2451's release time, released with the LD2450's */
    const Bytes tb = frame_bytes({{3, 6, 1, 4, 100}});
    Frame f;
    CHECK(ld2451_decode_frame(tb.data(), tb.size(), f));
    for (int mode = 0; mode < 2; ++mode) {
        TrackerConfig tc;
        tc.drop_ms = mode == 0 ? 3000 : 500;
        Tracker tr(tc);
        tr.update(f, 1000);
        tr.update(f, 1140);
        const std::uint32_t id = tr.slot(0).id;
        tr.tick(1140 + 2650 - 10);
        tr.update(f, 1140 + 2650);
        if (mode == 0) {
            CHECK(tr.slot(0).active);
            CHECK_EQ(tr.slot(0).id, id); /* the same target */
        } else {
            CHECK(tr.slot(0).id != id); /* a new one */
        }
    }
}

/* ------------------------------------------------------------------------------------------ angle sign */

/* The angle sign was confirmed by one walk test on 2026-11-06 (sensor horizontal, cable on the left as seen when facing it):
 * positive = to the right as seen from behind the sensor; Orientation > Mirror gives the facing view. The polar maths is
 * not mirrored anywhere (the Flip angle setting is gone). */
void test_angle_sign()
{
    const Bytes tb = frame_bytes({{20, 8, 1, 4, 100}, {-15, 6, 0, 4, 100}});
    Frame f;
    CHECK(ld2451_decode_frame(tb.data(), tb.size(), f));
    CHECK(f.target[0].x_mm > 0);
    CHECK(f.target[1].x_mm < 0);
    const int x0 = f.target[0].x_mm, x1 = f.target[1].x_mm;
    CHECK_EQ(f.target[0].angle_deg, 20.0);
    CHECK_EQ(f.target[1].angle_deg, -15.0);

    FakeSys sys;
    FakePort &p = sys.add("ttyUSB0");
    p.stream[115200] = as_string(repeat(tb, 1));
    p.rate_bps = 17 / 0.14;
    Link link(sys);
    link.start(0);
    step(sys, link, 8000);
    CHECK(link.state() == LinkState::Found);
    CHECK(link.driver() == &kDriverLd2451);
    CHECK(link.tracker().slot(0).active);
    CHECK_EQ(link.tracker().slot(0).last.x_mm, x0);
    CHECK_EQ(link.tracker().slot(0).last.angle_deg, 20.0);
    CHECK_EQ(link.tracker().slot(1).last.x_mm, x1);
    link.close();
}

void test_flip_setting()
{
    /* the default file is as before: no line for it */
    CHECK_EQ(serialize_settings(Settings()), "units=metres\norientation=normal\nrange=auto\ntrails=on\nhide_held=off\n");
    /* an old file with the removed flip_angle key still loads (the key is ignored) and it is never written back */
    for (const char *v : {"on", "off", "yes", ""}) {
        const Settings r = parse_settings(std::string("units=raw\nflip_angle=") + v + "\nsensor=ld2451\nrange=4\n");
        CHECK(r.units == UnitMode::MmCms);
        CHECK_EQ(r.sensor, "ld2451");
        CHECK_EQ(r.range_m, 4);
        CHECK(serialize_settings(r).find("flip") == std::string::npos);
    }
    {
        const Settings r = parse_settings("flip_angle=on\r\nunits=raw\r\n");
        CHECK(r.units == UnitMode::MmCms);
        CHECK(serialize_settings(r).find("flip") == std::string::npos);
    }
    Settings s;
    s.sensor = "ld2451";
    s.range_m = 4;
    const Settings r = parse_settings(serialize_settings(s));
    CHECK_EQ(r.sensor, "ld2451");
    CHECK_EQ(r.range_m, 4);
    CHECK_EQ(serialize_settings(r), serialize_settings(s));
    /* the Sensor type value */
    CHECK_EQ(parse_settings("sensor=ld2451\n").sensor, "ld2451");
    CHECK(valid_sensor_type("ld2451"));
    CHECK_EQ(std::string(sensor_type_label("ld2451")), "LD2451");
    CHECK(find_driver("ld2451") == &kDriverLd2451);
    /* the whole cycle: auto -> LD2452 / LD2450 -> LD2451 -> auto */
    CHECK_EQ(std::string(next_sensor_type("auto")), "ld2450");
    CHECK_EQ(std::string(next_sensor_type("ld2450")), "ld2451");
    CHECK_EQ(std::string(next_sensor_type("ld2451")), "auto");
}

/* ------------------------------------------------------------------------------------------ range */

void test_range()
{
    /* the LD2450's range is as before: 2, 4, 6 m and no more */
    CHECK_EQ(auto_range(4, 1000, true), 2);
    CHECK_EQ(auto_range(2, 5000, true), 6);
    CHECK_EQ(auto_range(2, 20000, true), 6);
    CHECK_EQ(auto_range(100, 1000, false, 6), 6);
    /* the LD2451's reaches 100 m */
    CHECK_EQ(auto_range(4, 8000, true, 100), 10);
    CHECK_EQ(auto_range(4, 12000, true, 100), 20); /* 12 m + 0.5 m */
    CHECK_EQ(auto_range(10, 12000, true, 100), 20);
    CHECK_EQ(auto_range(4, 45000, true, 100), 50);
    CHECK_EQ(auto_range(4, 99000, true, 100), 100);
    CHECK_EQ(auto_range(4, 250000, true, 100), 100);
    CHECK_EQ(auto_range(100, 5000, true, 100), 50); /* shrinks one step at a time */
    CHECK_EQ(auto_range(50, 5000, true, 100), 20);
    CHECK_EQ(auto_range(20, 5000, true, 100), 10);
    CHECK_EQ(auto_range(10, 5000, true, 100), 6);
    CHECK_EQ(auto_range(6, 3000, true, 100), 4);
    CHECK_EQ(auto_range(10, 9400, true, 100), 10); /* no flapping at a boundary */
    CHECK_EQ(auto_range(50, 20000, true, 100), 50);
    CHECK_EQ(auto_range(100, 1000, false, 100), 100);
    /* a range left over from the LD2451 is brought back for the LD2450 */
    CHECK_EQ(auto_range(50, 3000, true, 6), 4);
    CHECK_EQ(auto_range(100, 3000, false, 6), 6);
    /* the fixed steps are the same three */
    CHECK_EQ(next_range_setting(6), 0);
    CHECK_EQ(step_range(0, 20, 1), 6);
    CHECK_EQ(step_range(0, 20, -1), 4);
    CHECK_EQ(step_range(0, 4, 1), 6);
    /* rings: every metre up to 6, then fewer; never more than 6 */
    CHECK_EQ(ring_step_m(2), 1);
    CHECK_EQ(ring_step_m(6), 1);
    CHECK_EQ(ring_step_m(10), 2);
    CHECK_EQ(ring_step_m(20), 5);
    CHECK_EQ(ring_step_m(50), 10);
    CHECK_EQ(ring_step_m(100), 20);
    for (int r : {2, 4, 6, 10, 20, 50, 100}) CHECK(r / ring_step_m(r) <= 6);
}

/* ------------------------------------------------------------------------------------------ the demo scenarios */

void test_demo_scenarios()
{
    for (const char *name : {"ld2451", "ld2451-away", "ld2451-two"}) {
        long clock = 1000;
        DemoSys sys(name, [&clock] { return static_cast<std::int64_t>(clock); });
        Link link(sys);
        link.start(clock);
        int approaching = 0, receding = 0, frames_with = 0;
        while (clock < 40000) {
            clock += 30;
            link.poll(clock);
            const Frame &f = link.last_frame();
            if (link.state() == LinkState::Found && link.stats().frames > 0)
                for (const Target &t : f.target)
                    if (t.present) {
                        ++frames_with;
                        if (t.direction == Direction::Approaching && t.speed_cm_s < 0) ++approaching;
                        if (t.direction == Direction::Receding && t.speed_cm_s > 0) ++receding;
                    }
        }
        CHECK(link.state() == LinkState::Found);
        CHECK(link.driver() == &kDriverLd2451);
        CHECK(frames_with > 100);
        const bool two = std::string(name) == "ld2451-two";
        if (std::string(name) == "ld2451" || two) CHECK(approaching > 100);
        else CHECK_EQ(approaching, 0);
        if (std::string(name) == "ld2451-away" || two) CHECK(receding > 100);
        else CHECK_EQ(receding, 0);
        CHECK_EQ(link.stats().bad_frames, 0u);
        link.close();
    }
}

/* ------------------------------------------------------------------------------------------ listen only */

void test_listen_only()
{
    std::string dir = test_data_dir() + "../../src/core/";
#ifdef CDK_APP_DIR
    dir = std::string(CDK_APP_DIR) + "/src/core/";
#endif
    for (const char *f : {"ld2451.cpp", "ld2451.hpp"}) {
        const std::string text = test_read_file(dir + f);
        CHECK(!text.empty());
        CHECK(text.find("write(") == std::string::npos);
        CHECK(text.find("send(") == std::string::npos);
        /* no command frame header either: FD FC FB FA is the other direction's */
        CHECK(text.find("0xFD") == std::string::npos);
        CHECK(text.find("0xFC") == std::string::npos);
        CHECK(text.find("#include \"port_sys.hpp\"") == std::string::npos);
    }
    for (const char *f : {"link.cpp", "detect.cpp", "demo_sys.cpp"}) {
        const std::string text = test_read_file(dir + f);
        CHECK(!text.empty());
        CHECK(text.find("::write(") == std::string::npos);
        CHECK(text.find("sys_.write") == std::string::npos);
    }
}

} // namespace

void test_ld2451()
{
    test_recordings_decode();
    test_direction_semantics();
    test_recordings_tracker();
    test_recordings_link();
    test_decode_values();
    test_polar_maths();
    test_decoder_edges();
    test_sniff_discrimination();
    test_empty_only_stream();
    test_per_driver_timeouts();
    test_tracker_gap();
    test_angle_sign();
    test_flip_setting();
    test_range();
    test_demo_scenarios();
    test_listen_only();
}
