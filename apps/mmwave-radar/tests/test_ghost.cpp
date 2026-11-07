/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: no ghost target. Our own recordings of the sensor with nobody in front (data/empty-*.bin) go
 * through the decoder, the Link (fake port, polls 20-40 ms apart) and the tracker: no slot may be active or present,
 * and a slot that was active (walk fixture first) must be released within drop_ms + one poll although frames never stop.
 */
#include "check.hpp"

#include "fake_sys.hpp"
#include "demo_sys.hpp"
#include "diag.hpp"
#include "link.hpp"
#include "settings.hpp"

#include <cstdlib>
#include <cstring>
#include <unistd.h>

using namespace mmw;

namespace {

std::string walk() { return test_read_file(test_data_dir() + "ld2452-9600-walk-15s.bin"); }

int active_slots(const Tracker &t)
{
    int n = 0;
    for (int i = 0; i < 3; ++i) n += t.slot(i).active ? 1 : 0;
    return n;
}
int present_slots(const Tracker &t)
{
    int n = 0;
    for (int i = 0; i < 3; ++i) n += t.slot(i).present_now ? 1 : 0;
    return n;
}

void decoder_empty(const char *file, std::size_t allow_nonempty_frames)
{
    const std::string d = test_read_file(test_data_dir() + file);
    CHECK(d.size() > 10000);
    Decoder dec;
    std::vector<Frame> fr;
    std::size_t at = 0;
    unsigned x = 12345;
    while (at < d.size()) { /* chunks of 1..64 bytes */
        x = x * 1103515245u + 12345u;
        const std::size_t n = std::min<std::size_t>(1 + (x >> 16) % 64, d.size() - at);
        dec.feed(reinterpret_cast<const std::uint8_t *>(d.data()) + at, n, &fr);
        at += n;
    }
    CHECK(fr.size() > 380);
    std::size_t nonempty = 0;
    for (const Frame &f : fr) nonempty += f.present_count() ? 1 : 0;
    CHECK(nonempty <= allow_nonempty_frames);
    CHECK_EQ(dec.stats().frames_bad, 0u);
    CHECK_EQ(dec.stats().frames_cut, 0u);
}

/* The recording through a Link; polls 20-40 ms apart. leading_walk: the walk fixture first (slots become active).
 * No slot may be active or present once `grace_ms` (drop_ms + a poll, plus the start-up) passed after the last byte of
 * the part that may hold targets (the walk, or the first `allow_ms` of the deck recording). */
void link_empty(const char *file, bool leading_walk, long run_ms, long allow_ms)
{
    FakeSys sys;
    FakePort &p = sys.add("ttyUSB0");
    const std::string e = test_read_file(test_data_dir() + file);
    p.stream[9600] = (leading_walk ? walk() : std::string()) + e;
    Link link(sys);
    link.start(0);
    unsigned x = 777;
    const std::uint64_t walk_bytes = leading_walk ? walk().size() : 0;
    long walk_end = -1, found_at = -1, bad = 0, last_active = -1;
    while (sys.now_ms < run_ms) {
        x = x * 1103515245u + 12345u;
        sys.now_ms += 20 + (x >> 16) % 21;
        link.poll(sys.now_ms);
        if (link.state() != LinkState::Found) continue;
        if (found_at < 0) found_at = sys.now_ms;
        if (walk_end < 0 && link.stats().bytes >= walk_bytes) walk_end = sys.now_ms;
        const bool any = active_slots(link.tracker()) || present_slots(link.tracker());
        if (any) last_active = sys.now_ms;
        if (walk_end >= 0 && sys.now_ms > walk_end + allow_ms + 540 + 60 && any) {
            if (!bad) std::fprintf(stderr, "ghost %s walk=%d t=%ld walk_end=%ld bytes=%llu\n", file, (int)leading_walk, (long)sys.now_ms, walk_end, (unsigned long long)link.stats().bytes);
            ++bad;
        }
    }
    CHECK(found_at >= 0);
    CHECK(walk_end >= 0);
    CHECK_EQ(bad, 0L);
    CHECK(link.state() == LinkState::Found);
    CHECK(link.tracker().frames() > 100);
    if (leading_walk) CHECK(last_active > walk_end - 3000); /* the walk really made slots active */
    else CHECK(allow_ms > 0 || last_active < 0);
}

/* The diagnostic log text (MMWAVE_LOG): state, counters, slots, the first target frames as raw hex. */
void diag_text()
{
    FakeSys sys;
    sys.add("ttyUSB0").stream[9600] = walk();
    Link link(sys);
    link.start(0);
    unsigned sent = 0;
    std::string extra;
    while (sys.now_ms < 6000) {
        sys.now_ms += 30;
        link.poll(sys.now_ms);
        extra += diag_extra(link, sent);
    }
    const std::string l = diag_line(link, sys.now_ms);
    CHECK(l.find("state=Found port=ttyUSB0 baud=9600") != std::string::npos);
    CHECK(l.find("T1[act=") != std::string::npos && l.find("T3[act=") != std::string::npos);
    CHECK(l.find(" held=") != std::string::npos);
    CHECK(extra.find("target-frame 1 aaff0300") != std::string::npos);
    CHECK(extra.find("target-frame 3 aaff0300") != std::string::npos);
    CHECK(extra.find("target-frame 4") == std::string::npos);
    unsigned again = sent;
    CHECK(diag_extra(link, again).empty()); /* each only once */
}

/* A tracking frame's 30 bytes: one target at (x, y) mm, speed 0, or none (x = y = 0). */
std::string frame_at(int x, int y)
{
    Frame f;
    if (x || y) {
        f.target[0].present = true;
        f.target[0].x_mm = x;
        f.target[0].y_mm = y;
        f.target[0].resolution_mm = 360;
    }
    std::uint8_t raw[30];
    encode_frame(f, raw);
    return std::string(reinterpret_cast<const char *>(raw), 30);
}
Frame decoded(const std::string &bytes)
{
    Frame f;
    CHECK(decode_frame(reinterpret_cast<const std::uint8_t *>(bytes.data()), f));
    return f;
}

/* The frame counters on single frames: empty, distinct, identical (byte for byte, raw target bytes). */
void diag_counts_direct()
{
    const std::string e = frame_at(0, 0), a = frame_at(-300, 1500), b = frame_at(-300, 1501);
    DiagFrameCounts c;
    c.count(decoded(e));
    c.count(decoded(a));
    c.count(decoded(a));
    c.count(decoded(e)); /* an empty frame between two identical ones does not break the run */
    c.count(decoded(a));
    c.count(decoded(b)); /* one byte apart: not the same */
    c.count(decoded(a));
    CHECK_EQ(c.empty, 2u);
    CHECK_EQ(c.nonempty, 5u);
    CHECK_EQ(c.same, 2u);
    CHECK_EQ(c.total_empty, 2ull);
    CHECK_EQ(c.total_nonempty, 5ull);
    CHECK_EQ(c.total_same, 2ull);
    CHECK(c.latest_new);
    CHECK(std::memcmp(c.latest, a.data(), 30) == 0);
}

struct CountRun {
    std::string lines, hex;
    std::uint64_t window_empty = 0, window_nonempty = 0, window_same = 0;
    int hex_lines = 0;
};
/* A Link on a fake port that cycles `stream`, a diag line every second as the app writes it, until `until` ms. */
void run_counts(FakeSys &sys, Link &link, long until, CountRun &r)
{
    long last = sys.now_ms;
    while (sys.now_ms < until) {
        sys.now_ms += 30;
        link.poll(sys.now_ms);
        if (sys.now_ms - last < 1000) continue;
        last = sys.now_ms;
        std::string hex;
        const std::string l = diag_frames(link, sys.now_ms, &hex);
        unsigned e = 0, n = 0, s = 0;
        CHECK_EQ(std::sscanf(l.c_str(), " empty=%u nonempty=%u same_as_prev=%u", &e, &n, &s), 3);
        r.window_empty += e;
        r.window_nonempty += n;
        r.window_same += s;
        r.lines += diag_line(link, sys.now_ms) + l + "\n" + hex;
        if (!hex.empty()) {
            ++r.hex_lines;
            r.hex += hex;
        }
    }
}

/* The counters through a Link: the per-second windows add up to the totals and to the frames decoded; identical
 * frames counted; nothing counted while off; start() begins again; the 10 s hex line only when a target came. */
void diag_counts_link()
{
    const std::string a = frame_at(-300, 1500), b = frame_at(250, 2200), e = frame_at(0, 0);
    { /* off (the default): nothing counted */
        FakeSys sys;
        sys.add("ttyUSB0").stream[9600] = a;
        Link link(sys);
        link.start(0);
        CountRun r;
        run_counts(sys, link, 5000, r);
        CHECK(link.stats().frames > 40);
        CHECK_EQ(link.diag_counts().total_nonempty, 0ull);
        CHECK_EQ(r.window_nonempty, 0ull);
    }
    { /* a latched target: the same frame again and again */
        FakeSys sys;
        sys.add("ttyUSB0").stream[9600] = a;
        Link link(sys);
        link.set_diag_counting(true);
        link.start(0);
        CountRun r;
        run_counts(sys, link, 21500, r);
        const DiagFrameCounts &c = link.diag_counts();
        CHECK(link.state() == LinkState::Found);
        CHECK(c.total_nonempty > 300);
        CHECK_EQ(c.total_empty, 0ull);
        CHECK_EQ(c.total_same, c.total_nonempty - 1);
        CHECK_EQ(c.total_nonempty, static_cast<std::uint64_t>(link.stats().frames));
        CHECK_EQ(r.window_nonempty + c.nonempty, c.total_nonempty);
        CHECK_EQ(r.window_same + c.same, c.total_same);
        CHECK(r.lines.find(" empty=0 nonempty=19 same_as_prev=19 ") != std::string::npos);
        /* the stale target: live in the first seconds, held=1 from 3 s of identical frames, at the end of the line */
        CHECK(r.lines.find("T3[act=0 now=0 x=0 y=0 v=0 id=0] held=0 empty=") != std::string::npos);
        CHECK(r.lines.find("T3[act=0 now=0 x=0 y=0 v=0 id=0] held=1 empty=") != std::string::npos);
        CHECK(r.lines.rfind(" held=1 empty=") > r.lines.find(" held=0 empty="));
        CHECK_EQ(r.hex_lines, 2); /* at 10 s and 20 s */
        std::string ahex;
        for (unsigned char v : a) {
            char h[3];
            std::snprintf(h, sizeof(h), "%02x", v);
            ahex += h;
        }
        CHECK_EQ(r.hex.substr(0, 18 + 60 + 1), "last-target-frame " + ahex + "\n");
        if (std::getenv("MMW_SHOW_DIAG")) std::fputs(r.lines.c_str(), stderr);
        /* a new session: everything from zero */
        link.start(sys.now_ms);
        CHECK_EQ(link.diag_counts().total_nonempty, 0ull);
        CHECK_EQ(link.diag_counts().total_same, 0ull);
        CHECK(!link.diag_counts().have_prev);
        CHECK(!link.diag_counts().latest_new);
        CountRun r2;
        run_counts(sys, link, sys.now_ms + 5000, r2);
        CHECK(link.diag_counts().total_nonempty > 0);
        CHECK(link.diag_counts().total_nonempty < 120);
        CHECK_EQ(r2.hex_lines, 0); /* not 10 s into the new session yet */
    }
    { /* distinct frames with empty ones between */
        FakeSys sys;
        sys.add("ttyUSB0").stream[9600] = a + e + b + e;
        Link link(sys);
        link.set_diag_counting(true);
        link.start(0);
        CountRun r;
        run_counts(sys, link, 12000, r);
        const DiagFrameCounts &c = link.diag_counts();
        CHECK(c.total_nonempty > 80);
        CHECK(c.total_empty + 1 >= c.total_nonempty && c.total_empty <= c.total_nonempty + 1);
        CHECK_EQ(c.total_same, 0ull);
        CHECK_EQ(c.total_empty + c.total_nonempty, static_cast<std::uint64_t>(link.stats().frames));
        CHECK_EQ(r.window_empty + c.empty, c.total_empty);
        CHECK_EQ(r.hex_lines, 1);
        CHECK(r.hex.compare(0, 26, "last-target-frame aaff0300") == 0);
    }
    { /* empty frames only: counted, never a hex line */
        FakeSys sys;
        sys.add("ttyUSB0").stream[9600] = e;
        Link link(sys);
        link.set_diag_counting(true);
        link.start(0);
        CountRun r;
        run_counts(sys, link, 21500, r);
        const DiagFrameCounts &c = link.diag_counts();
        CHECK(link.state() == LinkState::Found);
        CHECK(c.total_empty > 300);
        CHECK_EQ(c.total_nonempty, 0ull);
        CHECK_EQ(c.total_empty, static_cast<std::uint64_t>(link.stats().frames));
        CHECK_EQ(r.hex_lines, 0);
        CHECK(r.lines.find(" empty=19 nonempty=0 same_as_prev=0 ") != std::string::npos);
    }
}

/* The diagnostic switch: $MMWAVE_LOG first, else the marker <config dir>/diag -> <config dir>/mmwave.log; no marker,
 * no file. The marker log is cut to its last kDiagKeepBytes, from a whole line. */
void diag_switch()
{
    char tmpl[] = "/tmp/mmw-diag-XXXXXX";
    const char *base = mkdtemp(tmpl);
    CHECK(base != nullptr);
    if (!base) return;
    const std::string dir = std::string(base) + "/mmwave-config";
    bool marker = true;
    /* off: no variable, no folder / no marker */
    CHECK_EQ(diag_log_path(nullptr, "", &marker), "");
    CHECK(!marker);
    CHECK_EQ(diag_log_path(nullptr, dir, &marker), "");
    CHECK(make_dirs(dir));
    CHECK_EQ(diag_log_path("", dir, &marker), "");
    CHECK(!marker);
    CHECK(::access((dir + "/mmwave.log").c_str(), F_OK) != 0); /* nothing written */
    /* the variable wins (with or without the marker) */
    CHECK_EQ(diag_log_path("/tmp/x.log", dir, &marker), "/tmp/x.log");
    CHECK(!marker);
    /* the marker */
    CHECK(write_file_atomic(dir + "/diag", ""));
    CHECK_EQ(diag_log_path(nullptr, dir, &marker), dir + "/mmwave.log");
    CHECK(marker);
    CHECK_EQ(diag_log_path("/tmp/x.log", dir, &marker), "/tmp/x.log");
    CHECK(!marker);
    /* trimming: a missing or small file is left alone */
    const std::string log = dir + "/mmwave.log";
    CHECK(!diag_trim(log, 100));
    CHECK(::access(log.c_str(), F_OK) != 0);
    CHECK(write_file_atomic(log, "line 1\nline 2\n"));
    CHECK(!diag_trim(log, 100));
    CHECK_EQ(test_read_file(log), "line 1\nline 2\n");
    /* a big one keeps its last bytes from a whole line */
    std::string big;
    for (int i = 0; i < 1000; ++i) big += "t=" + std::to_string(i) + " state=Found frames=" + std::to_string(i * 19) + "\n";
    CHECK(write_file_atomic(log, big));
    CHECK(diag_trim(log, 2000));
    const std::string kept = test_read_file(log);
    CHECK(kept.size() <= 2000 && kept.size() > 1900);
    CHECK(kept.rfind("t=999 state=Found frames=18981\n") == kept.size() - 31);
    CHECK(kept.compare(0, 2, "t=") == 0); /* starts on a whole line */
    CHECK(big.size() > kept.size() && big.compare(big.size() - kept.size(), kept.size(), kept) == 0);
    CHECK(!diag_trim(log, 2000)); /* now within the bound */
    CHECK(kDiagKeepBytes >= 100 * 1024 && kDiagKeepBytes <= 256 * 1024);
    unlink(log.c_str());
    unlink((dir + "/diag").c_str());
    rmdir(dir.c_str());
    rmdir(base);
}

} // namespace

void test_ghost()
{
    diag_text();
    diag_counts_direct();
    diag_counts_link();
    diag_switch();
    decoder_empty("empty-pi3a-30s.bin", 0);
    decoder_empty("empty-pi3a-20s.bin", 0);
    decoder_empty("empty-deck-30s.bin", 3);
    link_empty("empty-pi3a-30s.bin", false, 28000, 0);
    link_empty("empty-pi3a-20s.bin", false, 19000, 0);
    link_empty("empty-deck-30s.bin", false, 23000, 300); /* the fake cycles the file: stop before the blip comes round again */
    link_empty("empty-pi3a-30s.bin", true, 15500 + 25000, 0);
}
