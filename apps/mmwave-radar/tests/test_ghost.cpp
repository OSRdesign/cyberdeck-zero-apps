/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: no ghost target. Our own recordings of the sensor with nobody in front (data/empty-*.bin) go
 * through the decoder, the Link (fake port, polls 20-40 ms apart) and the tracker: no slot may be active or present,
 * and a slot that was active (walk fixture first) must be released within drop_ms + one poll although frames never stop.
 */
#include "check.hpp"

#include "fake_sys.hpp"
#include "diag.hpp"
#include "link.hpp"
#include "settings.hpp"

#include <cstdlib>
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
    CHECK(extra.find("target-frame 1 aaff0300") != std::string::npos);
    CHECK(extra.find("target-frame 3 aaff0300") != std::string::npos);
    CHECK(extra.find("target-frame 4") == std::string::npos);
    unsigned again = sent;
    CHECK(diag_extra(link, again).empty()); /* each only once */
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
    diag_switch();
    decoder_empty("empty-pi3a-30s.bin", 0);
    decoder_empty("empty-pi3a-20s.bin", 0);
    decoder_empty("empty-deck-30s.bin", 3);
    link_empty("empty-pi3a-30s.bin", false, 28000, 0);
    link_empty("empty-pi3a-20s.bin", false, 19000, 0);
    link_empty("empty-deck-30s.bin", false, 23000, 300); /* the fake cycles the file: stop before the blip comes round again */
    link_empty("empty-pi3a-30s.bin", true, 15500 + 25000, 0);
}
