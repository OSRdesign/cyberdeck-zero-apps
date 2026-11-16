/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: recording (brief 15.1). Pure parts with synthetic data (A/V sync and drift, the Sound-on lamp and the
 * silence detector, the disk guard and the split rule, the journal), then the recorder on the build box's real
 * libavformat (FFmpeg 6.1 = the 60 set in WSL) with synthetic MJPEG frames (the ffmpeg-made testsrc2 fixture, each frame
 * tagged with its number in a COM segment) and a synthetic tone, checked by ffprobe / ffmpeg and by reading the packets
 * back in-process: streams, frame count, duration, the A/V offset at tone bursts, a video-only scene, the split with no
 * frame lost or duplicated, the drift, the disk guard's stop, an empty scene; then the repair of files truncated at
 * fixed-seed offsets and of a file that is not MKV.
 */
#include "check.hpp"

#include "audio/level.hpp"
#include "record/av_libs.hpp"
#include "record/avsync.hpp"
#include "record/disk_guard.hpp"
#include "record/journal.hpp"
#include "record/mkv_writer.hpp"
#include "record/recorder.hpp"
#include "scenes/fileops.hpp"

#include <sys/stat.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

using namespace vhs;

namespace {

/* ------------------------------------------------------------------------------------------------ pure parts */

void avsync()
{
    /* no audio: pts = ts - t0 */
    AvSync none(0);
    none.start(5000000);
    CHECK_EQ(none.video_ms(5000000), 0ll);
    CHECK_EQ(none.video_ms(5033333), 33ll);
    CHECK_EQ(none.video_ms(6000400), 1000ll);

    /* drift +-0.05 % (and the CM969's +0.046 %, P0-f) over 2 hours: pairs every 2 s, the line over the last 30 s keeps
     * every frame on the audio timeline within 1 ms; the nominal rate would be 3.6 s off at the end */
    for (double drift : {0.0005, -0.0005, 0.00046}) {
        const int rate = 16000;
        const double eff = rate * (1.0 + drift);
        const std::int64_t ta0 = 1000LL * 1000000000LL; /* the audio capture's start (ns) */
        AvSync s(rate);
        std::int64_t next_pair = ta0 + 64000000LL;
        auto pos_at = [&](std::int64_t ns) { return static_cast<double>(ns - ta0) * 1e-9 * eff; };
        const std::int64_t t0_us = (ta0 + 700000000LL) / 1000;
        s.pair(next_pair, static_cast<std::uint64_t>(std::llround(pos_at(next_pair))));
        next_pair += 2000000000LL;
        s.start(t0_us);
        const double start_pos = pos_at(t0_us * 1000);
        CHECK_NEAR(static_cast<double>(s.start_position()), start_pos, 8.0); /* one pair yet: the nominal rate */
        double worst = 0;
        for (std::int64_t ts_us = t0_us; ts_us < t0_us + 7200LL * 1000000; ts_us += 333333) {
            while (next_pair <= ts_us * 1000) {
                s.pair(next_pair, static_cast<std::uint64_t>(std::llround(pos_at(next_pair))));
                next_pair += 2000000000LL;
            }
            const double want_ms = (pos_at(ts_us * 1000) - start_pos) * 1000.0 / rate; /* where the audio has it */
            worst = std::max(worst, std::fabs(static_cast<double>(s.video_ms(ts_us)) - want_ms));
        }
        CHECK(worst <= 1.5); /* pts are whole ms: 0.5 of it is the rounding */
        CHECK_NEAR(s.fitted_rate(), eff, 0.05);
        std::fprintf(stderr, "avsync: drift %+.3f %%: worst error over 2 h %.2f ms (fitted %.3f Hz)\n", drift * 100, worst,
                     s.fitted_rate());
    }

    /* a clock off by more than 1 % is not believed */
    AvSync bad(16000);
    bad.pair(1000000000LL, 0);
    bad.pair(3000000000LL, 34000); /* 17000 Hz: 6 % */
    CHECK_NEAR(bad.fitted_rate(), 16000.0, 0.01);

    /* the provisional pair is replaced by the first real one */
    AvSync seed(16000);
    seed.seed(2000000000LL, 8000);
    CHECK(seed.ready());
    seed.pair(2500000000LL, 16000);
    seed.pair(4500000000LL, 48000);
    CHECK_NEAR(seed.fitted_rate(), 16000.0, 0.01);
    CHECK_NEAR(seed.position_at(2500000000LL), 16000.0, 0.5);

    /* audio before t0 has negative indices */
    AvSync idx(16000);
    idx.pair(1000000000LL, 16000);
    idx.start(1500000); /* 0.5 s later: position 24000 */
    CHECK_EQ(idx.start_position(), 24000ll);
    CHECK_EQ(idx.audio_index(23000), -1000ll);
    CHECK_EQ(idx.audio_index(24160), 160ll);
    CHECK_EQ(idx.video_ms(1500000 + 40000), 40ll);
}

std::vector<std::int16_t> tone(int frames, int ch, double amp, double hz, int rate)
{
    std::vector<std::int16_t> v(static_cast<std::size_t>(frames) * static_cast<std::size_t>(ch));
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c)
            v[static_cast<std::size_t>(i * ch + c)] = static_cast<std::int16_t>(std::lround(amp * 32767.0 * std::sin(2 * M_PI * hz * i / rate)));
    return v;
}

void level()
{
    SoundLevel l;
    l.reset(16000, 2);
    CHECK(!l.lamp());
    /* -40 dBFS RMS (a sine of amplitude 0.01 * sqrt 2): on within one 200 ms window */
    const std::vector<std::int16_t> loud = tone(1600, 2, 0.01 * std::sqrt(2.0), 440, 16000); /* 100 ms */
    l.feed(loud.data(), 1600);
    CHECK(!l.lamp()); /* half a window */
    l.feed(loud.data(), 1600);
    CHECK(l.lamp());
    CHECK_NEAR(l.last_dbfs(), -40.0, 0.5);
    /* -60 dBFS: stays on for 1 s, then off */
    const std::vector<std::int16_t> soft = tone(1600, 2, 0.001 * std::sqrt(2.0), 440, 16000);
    for (int i = 0; i < 8; ++i) l.feed(soft.data(), 1600); /* 0.8 s */
    CHECK(l.lamp());
    for (int i = 0; i < 4; ++i) l.feed(soft.data(), 1600); /* 1.2 s */
    CHECK(!l.lamp());
    CHECK_NEAR(l.last_dbfs(), -60.0, 0.5);
    l.feed(loud.data(), 1600);
    l.feed(loud.data(), 1600);
    CHECK(l.lamp());
    /* silence: 3 s of exact zeros, once per scene; quiet noise is not silence */
    SoundLevel z;
    z.reset(16000, 1);
    std::vector<std::int16_t> zeros(1600, 0);
    for (int i = 0; i < 29; ++i) z.feed(zeros.data(), 1600);
    CHECK(!z.silence_event());
    z.feed(zeros.data(), 1600);
    z.feed(zeros.data(), 1600);
    CHECK(z.silence_event());
    CHECK(!z.silence_event());
    CHECK(z.silence_seen());
    for (int i = 0; i < 40; ++i) z.feed(zeros.data(), 1600);
    CHECK(!z.silence_event()); /* once per scene */
    z.reset(16000, 1);
    CHECK(!z.silence_seen());
    std::vector<std::int16_t> hiss(1600);
    for (std::size_t i = 0; i < hiss.size(); ++i) hiss[i] = static_cast<std::int16_t>(i % 3) - 1;
    for (int i = 0; i < 50; ++i) z.feed(hiss.data(), 1600);
    CHECK(!z.silence_event());
    CHECK(!z.lamp());
}

void disk_guard()
{
    const std::uint64_t MB = 1ull << 20;
    CHECK(DiskGuard::refuse_at_rec(499 * MB, 500 * MB));
    CHECK(!DiskGuard::refuse_at_rec(500 * MB, 500 * MB));
    DiskGuard g(500 * MB);
    CHECK(g.step(0, 400 * MB, 0) == DiskGuard::Verdict::Floor);
    DiskGuard r(500 * MB);
    CHECK(r.step(0, 10000 * MB, 0) == DiskGuard::Verdict::Ok);
    CHECK_EQ(r.seconds_left(), -1ll); /* no rate yet */
    CHECK(r.step(1000, 10000 * MB, 4 * MB) == DiskGuard::Verdict::Ok);
    CHECK(r.rate() == 0); /* under 2 s of readings */
    CHECK(r.step(2000, 10000 * MB, 8 * MB) == DiskGuard::Verdict::Ok);
    CHECK_NEAR(r.rate(), 4.0 * MB, 1.0);
    CHECK_EQ(r.seconds_left(), static_cast<long long>(9500 * MB / (4 * MB)));
    /* 2 minutes at 4 MB/s = 480 MB: 900 MB free is fine; under the floor stops */
    CHECK(r.step(3000, 900 * MB, 12 * MB) == DiskGuard::Verdict::Ok);
    CHECK(r.step(4000, 470 * MB, 16 * MB) == DiskGuard::Verdict::Floor);
    DiskGuard t(100 * MB);
    for (int i = 0; i <= 10; ++i) t.step(i * 1000, 5000 * MB, static_cast<std::uint64_t>(i) * 10 * MB); /* 10 MB/s */
    CHECK(t.step(11000, 1199 * MB, 110 * MB) == DiskGuard::Verdict::TwoMinutes);
    CHECK(t.step(12000, 1300 * MB, 120 * MB) == DiskGuard::Verdict::Ok);
    /* the rate is the last 10 s */
    DiskGuard w(0);
    w.step(0, 1, 0);
    for (int i = 1; i <= 30; ++i) w.step(i * 1000, 5000 * MB, (i <= 15 ? 100 : 1500 + (i - 15) * 1) * MB);
    CHECK(w.rate() < 2.0 * MB);
    /* the split rule */
    CHECK(!split_due(0, 900 * MB, 1024 * MB));            /* a part holds at least one frame */
    CHECK(!split_due(100 * MB, 1 * MB, 0));               /* Split off */
    CHECK(split_due(1007 * MB + 1, 1 * MB, 1024 * MB));   /* + 16 MiB margin would pass */
    CHECK(!split_due(1007 * MB - 1, 1 * MB, 1024 * MB));
    CHECK(split_due(2 * MB, 1, 2 * MB, 0));
    CHECK(!split_due(2 * MB - 1, 1, 2 * MB, 0));
    /* the texts */
    CHECK_EQ(bytes_text(1400000000ull), std::string("1.4 GB"));
    CHECK_EQ(bytes_text(62400000ull), std::string("62 MB"));
    CHECK_EQ(bytes_text(820000ull), std::string("820 KB"));
    CHECK_EQ(time_left_text(12000), std::string("3 h 20 min left"));
    CHECK_EQ(time_left_text(720), std::string("12 min left"));
    CHECK_EQ(time_left_text(40), std::string("40 s left"));
    CHECK_EQ(time_left_text(-1), std::string(""));
    CHECK_EQ(clock_text(45296000), std::string("12:34:56"));
    CHECK_EQ(clock_text(5400), std::string("00:00:05"));
}

void journal()
{
    Journal j;
    j.file = "scene_014_20261114-185200.mkv";
    j.parts = {j.file, "scene_014_20261114-185200_p2.mkv"};
    j.started = 1763145120;
    const std::string text = journal_to_json(j);
    Journal b;
    CHECK(journal_from_json(text, b));
    CHECK_EQ(b.state, std::string("open"));
    CHECK_EQ(b.file, j.file);
    CHECK(b.parts == j.parts);
    CHECK_EQ(b.started, 1763145120ll);
    CHECK(!b.repaired);
    Journal c;
    CHECK(journal_from_json("{\"state\":\"closed\",\"file\":\"a.mkv\",\"repaired\":true}", c));
    CHECK(c.repaired && c.state == "closed" && c.parts.size() == 1 && c.parts[0] == "a.mkv");
    CHECK(!journal_from_json("[1,2]", c));
    CHECK(!journal_from_json("not json", c));
    const std::string dir = make_temp_dir("jrnl");
    CHECK_EQ(journal_write(posix_file_ops(), dir, "scene_001_nodate", j), 0);
    Journal r;
    CHECK(journal_read(posix_file_ops(), journal_path(dir, "scene_001_nodate"), r));
    CHECK(r.parts == j.parts);
    CHECK_EQ(journal_path("/x/VHS", "s"), std::string("/x/VHS/.journal/s.json"));
    rm_tree(dir);
}

void codec_helpers()
{
    CHECK_EQ(rescale(1024, 1000, 16000), 64ll);
    CHECK_EQ(rescale(-1024, 1000, 16000), -64ll);
    CHECK_EQ(rescale(33333, 1, 1000), 33ll);
    CHECK_EQ(rescale(1500, 1, 1000), 2ll); /* rounds half away from zero */
    CHECK_EQ(rescale(INT64_C(9000000000000), 48000, 1000), INT64_C(432000000000000));
    const std::int16_t in[6] = {0, 32767, -32768, 16384, 1, -1};
    float l[3], r[3];
    float *planes[2] = {l, r};
    s16_to_float_planar(in, 3, 2, planes);
    CHECK_NEAR(l[0], 0.0, 1e-9);
    CHECK_NEAR(r[0], 32767.0 / 32768.0, 1e-6);
    CHECK_NEAR(l[1], -1.0, 1e-9);
    CHECK_NEAR(r[1], 0.5, 1e-9);
    CHECK_NEAR(l[2], 1.0 / 32768.0, 1e-9);
    CHECK_NEAR(r[2], -1.0 / 32768.0, 1e-9);
    CHECK_EQ(AacEncoder::bit_rate_for(16000), 64000);
    CHECK_EQ(AacEncoder::bit_rate_for(24000), 64000);
    CHECK_EQ(AacEncoder::bit_rate_for(44100), 128000);
    CHECK_EQ(std::string(video_codec_name(VideoCodec::Mjpeg)), std::string("MJPEG"));
}

/* ------------------------------------------------------------------------------------------------ the recorder */

bool have_tool(const char *t)
{
    return std::system(("command -v " + std::string(t) + " > /dev/null 2>&1").c_str()) == 0;
}

std::string run_cmd(const std::string &cmd)
{
    std::string out;
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

std::map<std::string, std::string> kv_lines(const std::string &text)
{
    std::map<std::string, std::string> m;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq != std::string::npos) m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}

std::map<std::string, std::string> probe_stream(const std::string &file, const char *sel)
{
    return kv_lines(run_cmd(std::string("ffprobe -v error -select_streams ") + sel +
                            " -count_packets -show_entries stream=codec_name,start_time,nb_read_packets,sample_rate,channels,"
                            "width,height -of default=nw=1 '" + file + "' 2>&1"));
}

int stream_count(const std::string &file)
{
    /* the lines that are a stream index (a warning on stderr is not one) */
    std::istringstream in(run_cmd("ffprobe -v error -show_entries stream=index -of csv=p=0 '" + file + "' 2> /dev/null"));
    std::string line;
    int n = 0;
    while (std::getline(in, line))
        if (!line.empty() && line.find_first_not_of("0123456789") == std::string::npos) ++n;
    return n;
}

double probe_duration(const std::string &file)
{
    return std::atof(run_cmd("ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 '" + file + "'").c_str());
}

std::vector<double> lines_as_numbers(const std::string &text)
{
    std::vector<double> v;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty() && line != "N/A") v.push_back(std::atof(line.c_str()));
    return v;
}

std::vector<double> video_pts(const std::string &file)
{
    return lines_as_numbers(run_cmd("ffprobe -v error -select_streams v -show_entries packet=pts_time -of csv=p=0 '" + file + "'"));
}

/* the time of decoded audio sample 0 (after the decoder's own priming handling) and the decoded mono samples */
bool decode_audio(const std::string &file, double &t0, std::vector<std::int16_t> &out)
{
    const std::vector<double> f = lines_as_numbers(
        run_cmd("ffprobe -v error -select_streams a -show_entries frame=pts_time -read_intervals %+#1 -of csv=p=0 '" + file + "'"));
    if (f.empty()) return false;
    t0 = f.front();
    const std::string raw = run_cmd("ffmpeg -v error -i '" + file + "' -map 0:a -f s16le -ac 1 -");
    out.resize(raw.size() / 2);
    std::memcpy(out.data(), raw.data(), out.size() * 2);
    return !out.empty();
}

/* the onset (first sample over the threshold) at or after `from` */
long onset(const std::vector<std::int16_t> &s, long from, int threshold)
{
    for (std::size_t i = static_cast<std::size_t>(std::max(0L, from)); i < s.size(); ++i)
        if (std::abs(static_cast<int>(s[i])) > threshold) return static_cast<long>(i);
    return -1;
}

/* the fixture's JPEGs, each tagged with its frame number in a COM segment and padded to one size */
struct Frames {
    std::vector<std::string> src;
    std::size_t size = 0;
    std::string make(int i) const
    {
        const std::string &j = src[static_cast<std::size_t>(i) % src.size()];
        char tag[32];
        std::snprintf(tag, sizeof tag, "VHSF%06d", i);
        std::string body = tag;
        const std::size_t want = size - j.size() - 4; /* SOI + this COM's marker and length come on top */
        body.resize(want, '.');
        std::string out = j.substr(0, 2);
        out += static_cast<char>(0xFF);
        out += static_cast<char>(0xFE);
        const std::size_t len = body.size() + 2;
        out += static_cast<char>(len >> 8);
        out += static_cast<char>(len & 0xFF);
        out += body;
        out += j.substr(2);
        return out;
    }
};

Frames load_frames()
{
    Frames f;
    const std::string p = fixture("testsrc-720.mjpg");
    std::string all;
    if (!p.empty()) {
        std::ifstream in(p, std::ios::binary);
        all.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    std::size_t at = 0;
    while (at + 4 < all.size()) {
        const std::size_t s = all.find("\xFF\xD8", at);
        if (s == std::string::npos) break;
        const std::size_t e = all.find("\xFF\xD9", s + 2);
        if (e == std::string::npos) break;
        f.src.push_back(all.substr(s, e + 2 - s));
        at = e + 2;
    }
    if (f.src.empty()) f.src.push_back(std::string("\xFF\xD8\xFF\xD9", 4)); /* no fixture: a bare SOI/EOI */
    std::size_t big = 0;
    for (const std::string &s : f.src) big = std::max(big, s.size());
    f.size = std::max<std::size_t>(big + 64, 60000); /* at least a 720p frame's size: the split cuts at 2 MiB */
    return f;
}

/* the frame numbers and pts (ms) of a file's video packets, read back in-process */
bool read_tags(const AvLibs &av, const std::string &file, std::vector<int> &tags, std::vector<std::int64_t> &pts_ms, int *streams)
{
    av::FormatContext *ic = nullptr;
    if (av.open_input(&ic, file.c_str(), nullptr, nullptr) < 0) return false;
    if (av.find_stream_info(ic, nullptr) < 0) {
        av.close_input(&ic);
        return false;
    }
    const unsigned n = av.fmt_nb_streams(ic);
    if (streams) *streams = static_cast<int>(n);
    int vs = -1;
    av::Rational tb{1, 1000};
    for (unsigned i = 0; i < n; ++i)
        if (av.at<int>(av.stream_par(av.fmt_stream(ic, i)), av.off.par_codec_type) == av::kMediaVideo) {
            vs = static_cast<int>(i);
            tb = av.stream_time_base(av.fmt_stream(ic, i));
        }
    av::Packet *pkt = av.packet_alloc();
    while (av.read_frame(ic, pkt) >= 0) {
        if (av.at<int>(pkt, av.off.pkt_stream_index) == vs) {
            const std::uint8_t *d = av.at<std::uint8_t *>(pkt, av.off.pkt_data);
            const int sz = av.at<int>(pkt, av.off.pkt_size);
            const std::string s(reinterpret_cast<const char *>(d), static_cast<std::size_t>(sz));
            const std::size_t t = s.find("VHSF");
            tags.push_back(t == std::string::npos ? -1 : std::atoi(s.c_str() + t + 4));
            pts_ms.push_back(rescale(av.at<std::int64_t>(pkt, av.off.pkt_pts), static_cast<std::int64_t>(tb.num) * 1000, tb.den));
        }
        av.packet_unref(pkt);
    }
    av.packet_free(&pkt);
    av.close_input(&ic);
    return true;
}

struct Sim {
    int frames = 90, fps = 30, rate = 16000, ch = 2;
    double drift = 0;
    bool audio = true;
    std::vector<int> bursts;               /* frame numbers with a 1 kHz burst at their timestamp */
    std::uint64_t split = 0, margin = 16ull << 20;
    std::uint64_t min_free = 0;
    std::function<std::uint64_t()> free_space;
    int guard_ms = 1000;
    std::string folder, stem;
};

RecorderStatus simulate(const AvLibs &av, const Frames &fr, const Sim &o)
{
    RecorderConfig rc;
    rc.av = &av;
    rc.ops = &posix_file_ops();
    rc.folder = o.folder;
    rc.stem = o.stem;
    rc.video.codec = VideoCodec::Mjpeg;
    rc.video.w = 1280;
    rc.video.h = 720;
    rc.video.fps = o.fps;
    if (o.audio) {
        rc.audio_rate = o.rate;
        rc.audio_channels = o.ch;
    }
    rc.split_bytes = o.split;
    rc.split_margin = o.margin;
    rc.min_free_bytes = o.min_free;
    rc.ring_seconds = 60; /* the test pushes faster than real time */
    rc.free_space = o.free_space ? o.free_space : [] { return UINT64_MAX / 2; };
    rc.guard_ms = o.guard_ms;
    rc.started_unix = 1763145120;
    Recorder r(rc);
    r.start();
    const std::int64_t ta0 = 2000LL * 1000000000LL; /* the microphone started 0.5 s before the first frame */
    const std::int64_t tv0_us = (ta0 + 500000000LL) / 1000 + 137;
    const double eff = o.rate * (1.0 + o.drift);
    auto t_of = [&](std::uint64_t p) { return ta0 + static_cast<std::int64_t>(std::llround(static_cast<double>(p) / eff * 1e9)); };
    auto ts_of = [&](int i) { return tv0_us + static_cast<std::int64_t>(i) * 1000000 / o.fps; };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> bursts; /* [start, end) positions */
    for (int k : o.bursts) {
        const std::uint64_t p = static_cast<std::uint64_t>(std::llround(static_cast<double>(ts_of(k) * 1000 - ta0) * 1e-9 * eff));
        bursts.emplace_back(p, p + static_cast<std::uint64_t>(o.rate / 10));
    }
    std::uint64_t pushed = 0, next_clock = 1024;
    std::vector<std::int16_t> chunk(1024 * static_cast<std::size_t>(o.ch));
    auto push_audio_until = [&](std::int64_t ns) {
        while (o.audio && t_of(pushed + 1024) <= ns) {
            for (int i = 0; i < 1024; ++i) {
                const std::uint64_t p = pushed + static_cast<std::uint64_t>(i);
                std::int16_t v = 0;
                for (const auto &b : bursts)
                    if (p >= b.first && p < b.second)
                        v = static_cast<std::int16_t>(std::lround(16000.0 * std::sin(2 * M_PI * 1000.0 * static_cast<double>(p - b.first) / o.rate)));
                for (int c = 0; c < o.ch; ++c) chunk[static_cast<std::size_t>(i * o.ch + c)] = v;
            }
            r.write(chunk.data(), 1024);
            pushed += 1024;
            if (pushed >= next_clock) { /* the capture clock every 2 s (AudioCapture's first pair: after the first read) */
                r.audio_clock(t_of(pushed), pushed);
                next_clock += static_cast<std::uint64_t>(o.rate) * 2;
            }
        }
    };
    for (int i = 0; i < o.frames; ++i) {
        push_audio_until(ts_of(i) * 1000 + 40000000LL);
        const std::string f = fr.make(i);
        r.push_video(reinterpret_cast<const std::uint8_t *>(f.data()), f.size(), ts_of(i));
        if (o.guard_ms < 1000 && i % 5 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (r.status().finished) break; /* the disk guard */
    }
    push_audio_until(ts_of(o.frames) * 1000 + 100000000LL);
    wait_for(5000, [&] { return r.status().frames >= static_cast<std::uint64_t>(o.frames) || r.status().finished; });
    r.request_stop(RecStop::User);
    r.join();
    return r.status();
}

void recorder_fixtures()
{
    std::string why;
    const AvLibs *av = real_av_libs(&why);
    if (!av) {
        SKIP(("recorder fixtures: no FFmpeg libraries on the build box (" + why + ")").c_str());
        return;
    }
    std::fprintf(stderr, "recorder: FFmpeg %s (libavformat %d)\n", av->name.c_str(), av->lavf_major);
    const bool probe = have_tool("ffprobe") && have_tool("ffmpeg");
    if (!probe) std::fprintf(stderr, "recorder: ffprobe / ffmpeg MISSING on the build box: the ffprobe checks are NOT run\n");
    const Frames fr = load_frames();
    if (fr.src.size() < 2) std::fprintf(stderr, "recorder: no testsrc fixture (no ffmpeg): bare JPEG markers as frames\n");
    const std::string dir = make_temp_dir("rec");

    /* 1. 3 s of MJPEG 1280x720 30 fps + a 16 kHz stereo tone: V_MJPEG + AAC, 90 frames, 3 s, the A/V offset */
    {
        Sim o;
        o.folder = dir;
        o.stem = "scene_001_nodate";
        o.bursts = {15, 60};
        const RecorderStatus st = simulate(*av, fr, o);
        CHECK_EQ(st.frames, 90ull);
        CHECK_EQ(st.ring_drops, 0ull);
        CHECK(st.audio);
        CHECK_EQ(st.parts, 1);
        CHECK(st.stop == RecStop::User);
        CHECK(st.error.empty());
        const std::string f = dir + "/scene_001_nodate.mkv";
        Journal j;
        CHECK(journal_read(posix_file_ops(), journal_path(dir, "scene_001_nodate"), j));
        CHECK_EQ(j.state, std::string("closed"));
        CHECK(j.parts.size() == 1 && j.parts[0] == "scene_001_nodate.mkv");
        std::vector<int> tags;
        std::vector<std::int64_t> pts;
        int streams = 0;
        CHECK(read_tags(*av, f, tags, pts, &streams));
        CHECK_EQ(streams, 2);
        CHECK_EQ(tags.size(), static_cast<std::size_t>(90));
        bool seq = true;
        for (std::size_t i = 0; i < tags.size(); ++i) seq = seq && tags[i] == static_cast<int>(i);
        CHECK(seq);
        if (probe) {
            auto v = probe_stream(f, "v:0");
            auto a = probe_stream(f, "a:0");
            CHECK_EQ(stream_count(f), 2);
            CHECK_EQ(v["codec_name"], std::string("mjpeg"));
            CHECK_EQ(v["nb_read_packets"], std::string("90"));
            CHECK_EQ(v["width"], std::string("1280"));
            CHECK_EQ(a["codec_name"], std::string("aac"));
            CHECK_EQ(a["sample_rate"], std::string("16000"));
            CHECK_EQ(a["channels"], std::string("2"));
            const double dur = probe_duration(f);
            const std::vector<double> vp = video_pts(f);
            CHECK_EQ(vp.size(), static_cast<std::size_t>(90));
            const double vdur = vp.empty() ? 0 : vp.back() - vp.front() + 1.0 / 30;
            CHECK_NEAR(vdur, 3.0, 1.0 / 30 + 0.001); /* the video's duration within one frame */
            const double vstart = std::atof(v["start_time"].c_str()), astart = std::atof(a["start_time"].c_str());
            CHECK(std::fabs(vstart - astart) <= 0.064 + 0.0005); /* the audio starts within one AAC frame (1024 at 16 kHz) */
            double t0 = 0;
            std::vector<std::int16_t> pcm;
            CHECK(decode_audio(f, t0, pcm));
            double worst = 0;
            for (int k : o.bursts) {
                if (vp.size() != 90 || pcm.empty()) break;
                const long at = onset(pcm, 0 + (k == 15 ? 0 : static_cast<long>((vp[45] - t0) * 16000)), 4000);
                CHECK(at >= 0);
                const double ta = t0 + static_cast<double>(at) / 16000.0;
                const double off = ta - vp[static_cast<std::size_t>(k)];
                worst = std::max(worst, std::fabs(off));
                std::fprintf(stderr, "recorder: burst at frame %d: video %.4f s, audio onset %.4f s, A/V offset %+.1f ms\n", k,
                             vp[static_cast<std::size_t>(k)], ta, off * 1000);
            }
            CHECK(worst <= 0.010);
            std::fprintf(stderr, "recorder: 3 s scene: %zu bytes, duration %.3f s, video start %.3f s, audio start %.3f s\n",
                         static_cast<std::size_t>(st.bytes), dur, vstart, astart);
        }
    }

    /* 2. video only */
    {
        Sim o;
        o.folder = dir;
        o.stem = "scene_002_nodate";
        o.audio = false;
        const RecorderStatus st = simulate(*av, fr, o);
        CHECK_EQ(st.frames, 90ull);
        CHECK(!st.audio);
        std::vector<int> tags;
        std::vector<std::int64_t> pts;
        int streams = 0;
        CHECK(read_tags(*av, dir + "/scene_002_nodate.mkv", tags, pts, &streams));
        CHECK_EQ(streams, 1);
        CHECK_EQ(tags.size(), static_cast<std::size_t>(90));
        CHECK(!pts.empty() && pts.front() == 0);
        CHECK(!pts.empty() && std::llabs(pts.back() - 2967) <= 1); /* frame 89 at 89/30 s */
        if (probe) {
            CHECK_EQ(stream_count(dir + "/scene_002_nodate.mkv"), 1);
            CHECK_EQ(probe_stream(dir + "/scene_002_nodate.mkv", "v:0")["nb_read_packets"], std::string("90"));
        }
    }

    /* 3. the split at 2 MiB (margin 0): parts _p2 ..., each starting at 0, no frame lost or duplicated */
    {
        Sim o;
        o.folder = dir;
        o.stem = "scene_003_nodate";
        o.split = 2ull << 20;
        o.margin = 0;
        const RecorderStatus st = simulate(*av, fr, o);
        CHECK_EQ(st.frames, 90ull);
        CHECK(st.parts >= 2);
        Journal j;
        CHECK(journal_read(posix_file_ops(), journal_path(dir, "scene_003_nodate"), j));
        CHECK_EQ(j.parts.size(), static_cast<std::size_t>(st.parts));
        CHECK_EQ(j.state, std::string("closed"));
        std::vector<int> all;
        bool starts_at_zero = true, sizes_ok = true;
        for (std::size_t p = 0; p < j.parts.size(); ++p) {
            const std::string name = p == 0 ? "scene_003_nodate.mkv" : "scene_003_nodate_p" + std::to_string(p + 1) + ".mkv";
            CHECK_EQ(j.parts[p], name);
            std::vector<int> tags;
            std::vector<std::int64_t> pts;
            int streams = 0;
            CHECK(read_tags(*av, dir + "/" + name, tags, pts, &streams));
            CHECK_EQ(streams, 2);
            starts_at_zero = starts_at_zero && !pts.empty() && pts.front() <= 1;
            struct stat sb;
            if (stat((dir + "/" + name).c_str(), &sb) == 0 && p + 1 < j.parts.size())
                sizes_ok = sizes_ok && sb.st_size <= static_cast<off_t>((2ull << 20) + (256u << 10)); /* + headers, audio, cues */
            all.insert(all.end(), tags.begin(), tags.end());
            if (probe) CHECK_EQ(probe_stream(dir + "/" + name, "v:0")["nb_read_packets"], std::to_string(tags.size()));
        }
        CHECK(starts_at_zero);
        CHECK(sizes_ok);
        bool seq = all.size() == 90;
        for (std::size_t i = 0; seq && i < all.size(); ++i) seq = all[i] == static_cast<int>(i);
        CHECK(seq);
        std::fprintf(stderr, "recorder: split at 2 MiB: %d parts, %zu frames in order, none lost or duplicated: %s\n", st.parts,
                     all.size(), seq ? "yes" : "NO");
    }

    /* 4. drift: the microphone's clock +0.3 % fast over 12 s; bursts at 1 s and 11 s stay on their frames */
    if (probe) {
        Sim o;
        o.folder = dir;
        o.stem = "scene_004_nodate";
        o.frames = 360;
        o.drift = 0.003;
        o.bursts = {30, 330};
        const RecorderStatus st = simulate(*av, fr, o);
        CHECK_EQ(st.frames, 360ull);
        const std::string f = dir + "/scene_004_nodate.mkv";
        const std::vector<double> vp = video_pts(f);
        double t0 = 0;
        std::vector<std::int16_t> pcm;
        CHECK(vp.size() == 360 && decode_audio(f, t0, pcm));
        if (vp.size() == 360 && !pcm.empty()) {
            double worst = 0;
            for (int k : o.bursts) {
                const long from = static_cast<long>((vp[static_cast<std::size_t>(k)] - 0.5 - t0) * 16000);
                const long at = onset(pcm, from, 4000);
                CHECK(at >= 0);
                const double off = t0 + static_cast<double>(at) / 16000.0 - vp[static_cast<std::size_t>(k)];
                worst = std::max(worst, std::fabs(off));
                std::fprintf(stderr, "recorder: drift +0.3 %%: burst at frame %d: A/V offset %+.1f ms (unmapped it would be %+.1f ms)\n",
                             k, off * 1000, k / 30.0 * 0.003 * 1000);
            }
            CHECK(worst <= 0.010);
            /* the video follows the audio clock: 330 frames span 11 s * 1.003 on the scene's timeline */
            CHECK_NEAR(vp[330] - vp[0], 11.0 * 1.003, 0.002);
        }
    }

    /* 5. the disk guard stops the scene by itself; the file is finished and plays */
    {
        Sim o;
        o.folder = dir;
        o.stem = "scene_005_nodate";
        o.frames = 300;
        o.min_free = 500ull << 20;
        o.guard_ms = 50;
        auto calls = std::make_shared<std::atomic<int>>(0);
        o.free_space = [calls] { return ++*calls > 3 ? (400ull << 20) : (900ull << 20); };
        const RecorderStatus st = simulate(*av, fr, o);
        CHECK(st.stop == RecStop::DiskFull);
        CHECK(st.frames > 0 && st.frames < 300);
        Journal j;
        CHECK(journal_read(posix_file_ops(), journal_path(dir, "scene_005_nodate"), j));
        CHECK_EQ(j.state, std::string("closed"));
        std::vector<int> tags;
        std::vector<std::int64_t> pts;
        CHECK(read_tags(*av, dir + "/scene_005_nodate.mkv", tags, pts, nullptr));
        CHECK_EQ(tags.size(), static_cast<std::size_t>(st.frames));
    }

    /* 6. a stop before the first frame leaves nothing (no file, no journal) */
    {
        RecorderConfig rc;
        rc.av = av;
        rc.ops = &posix_file_ops();
        rc.folder = dir;
        rc.stem = "scene_006_nodate";
        rc.video.w = 1280;
        rc.video.h = 720;
        rc.audio_rate = 16000;
        rc.audio_channels = 1;
        Recorder r(rc);
        r.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        r.request_stop(RecStop::User);
        r.join();
        CHECK_EQ(r.status().frames, 0ull);
        CHECK(r.status().files.empty());
        struct stat sb;
        CHECK(stat((dir + "/scene_006_nodate.mkv").c_str(), &sb) != 0);
        CHECK(stat(journal_path(dir, "scene_006_nodate").c_str(), &sb) != 0);
    }

    /* 7. the shared counter names a scene (rc.stem empty): prefix_NNN_when.mkv */
    {
        RecorderConfig rc;
        rc.av = av;
        rc.ops = &posix_file_ops();
        rc.folder = dir + "/named";
        rc.prefix = "take";
        rc.when = "20261114-185200";
        rc.video.w = 1280;
        rc.video.h = 720;
        Recorder r(rc);
        r.start();
        const std::string f = fr.make(0);
        for (int i = 0; i < 3; ++i) r.push_video(reinterpret_cast<const std::uint8_t *>(f.data()), f.size(), 5000000 + i * 33333);
        wait_for(2000, [&] { return r.status().frames == 3; });
        r.request_stop(RecStop::User);
        r.join();
        CHECK_EQ(r.status().stem, std::string("take_001_20261114-185200"));
        CHECK(r.status().files.size() == 1 && r.status().files[0] == "take_001_20261114-185200.mkv");
    }

    /* 8. the repair: the 3 s scene cut at fixed-seed offsets, an open journal each; then a file that is not MKV */
    {
        Sim o;
        o.folder = dir + "/long";
        o.stem = "scene_010_nodate";
        o.frames = 300; /* 10 s */
        const RecorderStatus st = simulate(*av, fr, o);
        CHECK_EQ(st.frames, 300ull);
        const std::string src = o.folder + "/scene_010_nodate.mkv";
        std::ifstream in(src, std::ios::binary);
        const std::string whole((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string rep = dir + "/repair";
        make_dirs(rep);
        std::uint32_t seed = 20261115u;
        int repaired = 0, cuts = 0;
        for (int n = 0; n < 6; ++n) {
            seed = seed * 1664525u + 1013904223u;
            const double frac = 0.15 + 0.8 * static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
            const std::size_t cut = static_cast<std::size_t>(static_cast<double>(whole.size()) * frac);
            const std::string stem = "cut_" + std::to_string(n);
            CHECK(write_file(rep + "/" + stem + ".mkv", whole.substr(0, cut)));
            Journal j;
            j.file = stem + ".mkv";
            j.parts = {j.file};
            CHECK_EQ(journal_write(posix_file_ops(), rep, stem, j), 0);
            ++cuts;
        }
        const std::vector<RepairItem> items = repair_open_journals(posix_file_ops(), *av, rep);
        CHECK_EQ(items.size(), static_cast<std::size_t>(cuts));
        seed = 20261115u;
        for (int n = 0; n < 6; ++n) {
            seed = seed * 1664525u + 1013904223u;
            const double frac = 0.15 + 0.8 * static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
            const double cut_s = 10.0 * frac; /* the frames are one size: the time is about linear in the bytes */
            const std::string stem = "cut_" + std::to_string(n);
            const RepairItem *it = nullptr;
            for (const RepairItem &x : items)
                if (x.stem == stem) it = &x;
            CHECK(it != nullptr);
            if (!it) continue;
            CHECK(it->repaired);
            repaired += it->repaired;
            CHECK(static_cast<double>(it->duration_ms) / 1000.0 >= cut_s - 2.0);
            Journal j;
            CHECK(journal_read(posix_file_ops(), journal_path(rep, stem), j));
            CHECK(j.state == "closed" && j.repaired);
            std::vector<int> tags;
            std::vector<std::int64_t> pts;
            CHECK(read_tags(*av, rep + "/" + stem + ".mkv", tags, pts, nullptr));
            bool seq = !tags.empty();
            for (std::size_t i = 0; seq && i < tags.size(); ++i) seq = tags[i] == static_cast<int>(i);
            CHECK(seq);
            double pd = -1;
            if (probe) {
                pd = probe_duration(rep + "/" + stem + ".mkv");
                CHECK(pd >= cut_s - 2.0);
            }
            std::fprintf(stderr, "repair: cut at %.0f %% (about %.2f s): repaired, %lld ms, %zu frames in order, ffprobe %.2f s\n",
                         frac * 100, cut_s, static_cast<long long>(it->duration_ms), tags.size(), pd);
        }
        CHECK_EQ(repaired, cuts);
        /* a file that is not MKV: the original stays, the journal says damaged */
        const std::string bad = dir + "/bad";
        make_dirs(bad);
        CHECK(write_file(bad + "/junk.mkv", "this is not a matroska file at all\n"));
        Journal jb;
        jb.file = "junk.mkv";
        jb.parts = {jb.file};
        CHECK_EQ(journal_write(posix_file_ops(), bad, "junk", jb), 0);
        const std::vector<RepairItem> bi = repair_open_journals(posix_file_ops(), *av, bad);
        CHECK(bi.size() == 1 && bi[0].damaged && !bi[0].repaired);
        std::ifstream jf(bad + "/junk.mkv");
        std::string content((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
        CHECK_EQ(content, std::string("this is not a matroska file at all\n"));
        Journal jr;
        CHECK(journal_read(posix_file_ops(), journal_path(bad, "junk"), jr));
        CHECK_EQ(jr.state, std::string("damaged"));
        struct stat sb;
        CHECK(stat((bad + "/.junk.repair.mkv").c_str(), &sb) != 0); /* no temporary left */
        /* closed journals are left alone */
        CHECK(repair_open_journals(posix_file_ops(), *av, rep).empty());
    }
    if (std::getenv("VHS_KEEP_TEST_FILES")) std::fprintf(stderr, "recorder: kept %s\n", dir.c_str());
    else rm_tree(dir);
}

} // namespace

void test_record()
{
    avsync();
    level();
    disk_guard();
    journal();
    codec_helpers();
}

void test_recorder()
{
    recorder_fixtures();
}
