/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests (P2a): ReplayGain from tags only (owner decision D25): the parsing of each format's tags (ID3v2.3 and
 * v2.4 TXXX in Latin-1, UTF-8 and UTF-16, FLAC and Ogg Vorbis comments, Opus R128 gains, MP4 freeform items, a WAV's
 * id3 chunk), the gain and its clipping guard in each mode, the engine applying it to the samples before the output's
 * volume (bit-exact when off), gapless off, the sleep timer's end fade; and the MP3 tag walk that no longer reads a
 * cover it does not need (report 060 finding 3).
 */
#include "check.hpp"

#include "demo/demo_world.hpp"
#include "dsp/replaygain.hpp"
#include "engine/engine.hpp"
#include "tags/tag_reader.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <dirent.h>
#include <mutex>
#include <thread>

using namespace vinyl;

namespace {

std::string syncsafe(uint32_t v) {
    std::string s;
    s += static_cast<char>((v >> 21) & 0x7F);
    s += static_cast<char>((v >> 14) & 0x7F);
    s += static_cast<char>((v >> 7) & 0x7F);
    s += static_cast<char>(v & 0x7F);
    return s;
}

std::string frame23(const char *id, const std::string &body) {
    std::string f = id;
    put_be32(f, static_cast<uint32_t>(body.size()));
    f += std::string("\0\0", 2);
    return f + body;
}

std::string frame24(const char *id, const std::string &body) {
    std::string f = id;
    f += syncsafe(static_cast<uint32_t>(body.size()));
    f += std::string("\0\0", 2);
    return f + body;
}

std::string id3(int ver, const std::string &frames, size_t padding = 64) {
    std::string t = "ID3";
    t += static_cast<char>(ver);
    t += std::string("\0\0", 2);
    t += syncsafe(static_cast<uint32_t>(frames.size() + padding));
    return t + frames + std::string(padding, '\0');
}

/* TXXX in Latin-1 (encoding 0) or UTF-8 (3) */
std::string txxx(const std::string &desc, const std::string &value, char enc = 0) {
    std::string b;
    b += enc;
    b += desc;
    b += '\0';
    b += value;
    return b;
}

/* TXXX in UTF-16 with byte-order marks (encoding 1) */
std::string txxx16(const std::string &desc, const std::string &value) {
    auto u16 = [](const std::string &a) {
        std::string s = "\xFF\xFE";
        for (char c : a) {
            s += c;
            s += '\0';
        }
        return s;
    };
    std::string b;
    b += '\x01';
    b += u16(desc);
    b += std::string("\0\0", 2);
    b += u16(value);
    return b;
}

std::string mp3_frames() {
    std::string a = "\xFF\xFB\x90\x64";
    a += std::string(32, '\0');
    a += "Xing";
    put_be32(a, 3);
    put_be32(a, 100);
    put_be32(a, 41700);
    a.resize(417, '\0');
    std::string b = "\xFF\xFB\x90\x64";
    b.resize(417, '\0');
    return a + b + b;
}

std::string vorbis_comments(const std::vector<std::string> &kv) {
    std::string c;
    put_le32(c, 6);
    c += "vendor";
    put_le32(c, static_cast<uint32_t>(kv.size()));
    for (const std::string &s : kv) {
        put_le32(c, static_cast<uint32_t>(s.size()));
        c += s;
    }
    return c;
}

std::string flac_file(const std::vector<std::string> &kv) {
    std::string f = "fLaC";
    std::string si;
    put_be16(si, 4096);
    put_be16(si, 4096);
    si += std::string(6, '\0');
    const uint64_t v = (uint64_t(44100) << 44) | (uint64_t(1) << 41) | (uint64_t(15) << 36) | 441000;
    for (int i = 7; i >= 0; --i) si += static_cast<char>((v >> (8 * i)) & 0xFF);
    si += std::string(16, '\0');
    f += '\x00';
    f += static_cast<char>(0);
    f += static_cast<char>(0);
    f += static_cast<char>(34);
    f += si;
    const std::string vc = vorbis_comments(kv);
    f += '\x84'; /* last block, VORBIS_COMMENT */
    f += static_cast<char>((vc.size() >> 16) & 0xFF);
    f += static_cast<char>((vc.size() >> 8) & 0xFF);
    f += static_cast<char>(vc.size() & 0xFF);
    f += vc;
    return f + std::string(64, '\0');
}

std::string ogg_page(uint8_t type, uint64_t granule, uint32_t seq, const std::string &packet) {
    std::string lace;
    size_t n = packet.size();
    while (n >= 255) {
        lace += static_cast<char>(255);
        n -= 255;
    }
    lace += static_cast<char>(n);
    std::string h = "OggS";
    h += '\0';
    h += static_cast<char>(type);
    for (int i = 0; i < 8; ++i) h += static_cast<char>((granule >> (8 * i)) & 0xFF);
    put_le32(h, 0x1234);
    put_le32(h, seq);
    put_le32(h, 0);
    h += static_cast<char>(lace.size());
    return h + lace + packet;
}

std::string ogg_vorbis(const std::vector<std::string> &kv) {
    std::string id = "\x01vorbis";
    put_le32(id, 0);
    id += '\x02';
    put_le32(id, 44100);
    put_le32(id, 0);
    put_le32(id, 128000);
    put_le32(id, 0);
    id += '\xB8';
    id += '\x01';
    const std::string com = "\x03vorbis" + vorbis_comments(kv) + "\x01";
    return ogg_page(2, 0, 0, id) + ogg_page(0, 0, 1, com) + ogg_page(4, 44100 * 10, 2, std::string(20, '\0'));
}

std::string ogg_opus(const std::vector<std::string> &kv, int16_t header_gain) {
    std::string head = "OpusHead";
    head += '\x01';
    head += '\x02';
    put_le16(head, 312);
    put_le32(head, 48000);
    put_le16(head, static_cast<uint16_t>(header_gain));
    head += '\0';
    const std::string tags = "OpusTags" + vorbis_comments(kv);
    return ogg_page(2, 0, 0, head) + ogg_page(0, 0, 1, tags) + ogg_page(4, 48000 * 10 + 312, 2, std::string(20, '\0'));
}

std::string box(const char *type, const std::string &body) {
    std::string b;
    put_be32(b, static_cast<uint32_t>(8 + body.size()));
    b += type;
    return b + body;
}

std::string full_box(const char *type, const std::string &body) { return box(type, std::string(4, '\0') + body); }

std::string freeform(const std::string &name, const std::string &value) {
    std::string data;
    put_be32(data, 1);
    put_be32(data, 0);
    return box("----", full_box("mean", "com.apple.iTunes") + full_box("name", name) + box("data", data + value));
}

std::string mp4_file(const std::string &items) {
    std::string ftyp = box("ftyp", std::string("M4A ") + std::string(4, '\0') + "M4A isom");
    std::string mvhd(100, '\0');
    put_be32(mvhd, 0); /* version 0: the scale at 12, the duration at 16 */
    mvhd = std::string(12, '\0');
    put_be32(mvhd, 1000);
    put_be32(mvhd, 245000);
    mvhd += std::string(80, '\0');
    const std::string meta = full_box("meta", box("ilst", items));
    return ftyp + box("moov", box("mvhd", mvhd) + box("udta", meta));
}

/* a 16-bit stereo WAV whose every sample is `v` */
std::string flat_wav(uint32_t rate, int frames, int16_t v) {
    std::string d;
    d.reserve(static_cast<size_t>(frames) * 4);
    for (int i = 0; i < frames; ++i) {
        put_le16(d, static_cast<uint16_t>(v));
        put_le16(d, static_cast<uint16_t>(v));
    }
    std::string f = "fmt ";
    put_le32(f, 16);
    put_le16(f, 1);
    put_le16(f, 2);
    put_le32(f, rate);
    put_le32(f, rate * 4);
    put_le16(f, 4);
    put_le16(f, 16);
    std::string dd = "data";
    put_le32(dd, static_cast<uint32_t>(d.size()));
    std::string w = "RIFF";
    put_le32(w, static_cast<uint32_t>(4 + f.size() + 8 + d.size()));
    return w + "WAVE" + f + dd + d;
}

/* ---------------------------------------------------------------------------------------------- the engine's fakes */

struct Record {
    std::mutex mu;
    std::vector<int16_t> samples;
    int opens = 0, drains = 0;
};

class FakeSink final : public PlaybackSink {
public:
    explicit FakeSink(Record &r) : r_(r) {}
    int open(const AudioFormat &src, SinkFormat &g) override {
        std::lock_guard<std::mutex> l(r_.mu);
        ++r_.opens;
        g.rate = src.rate;
        g.channels = src.channels;
        g.format = src.bits > 16 ? SampleFormat::S32_LE : SampleFormat::S16_LE;
        return 0;
    }
    long write(const void *f, size_t n) override {
        std::lock_guard<std::mutex> l(r_.mu);
        const int16_t *s = static_cast<const int16_t *>(f);
        r_.samples.insert(r_.samples.end(), s, s + n * 2);
        return static_cast<long>(n);
    }
    int recover(int) override { return 0; }
    long delay() override { return 0; }
    void drop() override {}
    void drain() override {
        std::lock_guard<std::mutex> l(r_.mu);
        ++r_.drains;
    }
    void close() override {}

private:
    Record &r_;
};

class FakeProvider final : public SinkProvider {
public:
    explicit FakeProvider(Record &r) : r_(r) {}
    int open(const OutputTarget &, OpenedSink &o, std::string &) override {
        o.sink = std::make_unique<FakeSink>(r_);
        o.name = o.device = "default";
        o.label = "Fake output";
        return 0;
    }
    bool still_there(const OpenedSink &) override { return true; }

private:
    Record &r_;
};

bool wait_end(Engine &e, int ms = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        EngineEvent ev;
        while (e.poll_event(ev))
            if (ev.kind == EngineEvent::Kind::QueueEnd) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

/* ---------------------------------------------------------------------------------------------- the tests */

void test_parse() {
    float f = 0;
    CHECK(parse_gain_db("-6.54 dB", f));
    CHECK_NEAR(f, -6.54, 1e-5);
    CHECK(parse_gain_db("+1.20 dB", f));
    CHECK_NEAR(f, 1.2, 1e-5);
    CHECK(parse_gain_db("  -0,5 dB", f)); /* a comma written by a localised tool */
    CHECK_NEAR(f, -0.5, 1e-5);
    CHECK(parse_gain_db("3", f));
    CHECK_NEAR(f, 3.0, 1e-5);
    CHECK(!parse_gain_db("dB", f));
    CHECK(!parse_gain_db("", f));
    CHECK(!parse_gain_db("-99 dB", f)); /* a damaged tag */
    CHECK(parse_peak("0.988553", f));
    CHECK_NEAR(f, 0.988553, 1e-6);
    CHECK(!parse_peak("-1", f));
    CHECK(parse_r128_db("-1280", f)); /* -5 dB relative to -23 LUFS: 0 dB at ReplayGain's -18 LUFS */
    CHECK_NEAR(f, 0.0, 1e-5);
    CHECK(parse_r128_db("0", f));
    CHECK_NEAR(f, 5.0, 1e-5);
    ReplayGainTags rg;
    CHECK(apply_replaygain_tag(rg, "replaygain_track_gain", "-7.10 dB")); /* any case */
    CHECK(rg.has_track);
    CHECK(!apply_replaygain_tag(rg, "TITLE", "x"));
    /* R128 wins over REPLAYGAIN in either order */
    ReplayGainTags o1, o2;
    apply_replaygain_tag(o1, "REPLAYGAIN_TRACK_GAIN", "-9.00 dB");
    apply_replaygain_tag(o1, "R128_TRACK_GAIN", "-512");
    apply_replaygain_tag(o2, "R128_TRACK_GAIN", "-512");
    apply_replaygain_tag(o2, "REPLAYGAIN_TRACK_GAIN", "-9.00 dB");
    CHECK_NEAR(o1.track_db, 3.0, 1e-5);
    CHECK_NEAR(o2.track_db, 3.0, 1e-5);
}

void test_gain_math() {
    ReplayGainTags rg;
    CHECK_NEAR(replaygain_db(ReplayGainMode::Track, rg), 0.0, 1e-12); /* untagged: 0 dB */
    CHECK_EQ(replaygain_chip_text(ReplayGainMode::Track, rg), std::string(""));
    rg.has_track = true;
    rg.track_db = -6.5f;
    rg.has_album = true;
    rg.album_db = -4.0f;
    CHECK_NEAR(replaygain_db(ReplayGainMode::Off, rg), 0.0, 1e-12);
    CHECK_NEAR(replaygain_db(ReplayGainMode::Track, rg), -6.5, 1e-6);
    CHECK_NEAR(replaygain_db(ReplayGainMode::Album, rg), -4.0, 1e-6);
    CHECK_NEAR(replaygain_factor(ReplayGainMode::Track, rg), std::pow(10.0, -6.5 / 20), 1e-6);
    CHECK_EQ(replaygain_chip_text(ReplayGainMode::Track, rg), std::string("RG -6.5 dB"));
    CHECK_EQ(replaygain_chip_text(ReplayGainMode::Off, rg), std::string(""));
    /* fall-backs: Album without an album gain takes the track's, Track without a track gain the album's */
    ReplayGainTags only_track, only_album;
    only_track.has_track = true;
    only_track.track_db = -3.2f;
    only_album.has_album = true;
    only_album.album_db = -2.0f;
    CHECK_NEAR(replaygain_db(ReplayGainMode::Album, only_track), -3.2, 1e-6);
    CHECK_NEAR(replaygain_db(ReplayGainMode::Track, only_album), -2.0, 1e-6);
    /* the clipping guard: a quiet track (+6 dB) whose peak is 0.9 gets at most -20 log10(0.9) = +0.915 dB */
    ReplayGainTags quiet;
    quiet.has_track = true;
    quiet.track_db = 6.0f;
    quiet.has_track_peak = true;
    quiet.track_peak = 0.9f;
    CHECK_NEAR(replaygain_db(ReplayGainMode::Track, quiet), -20 * std::log10(0.9), 1e-4);
    CHECK(replaygain_factor(ReplayGainMode::Track, quiet) * 0.9f <= 1.0f + 1e-5f); /* the loudest sample at full scale */
    /* the album peak guards the album gain */
    quiet.has_album = true;
    quiet.album_db = 6.0f;
    quiet.has_album_peak = true;
    quiet.album_peak = 0.5f;
    CHECK_NEAR(replaygain_db(ReplayGainMode::Album, quiet), 6.0, 1e-4); /* 0.5 allows +6.02 dB */
    /* a pre-amp (later: 0 dB now) is under the guard too */
    CHECK_NEAR(replaygain_db(ReplayGainMode::Track, quiet, 3.0), -20 * std::log10(0.9), 1e-4);
    CHECK_NEAR(kReplayGainPreampDb, 0.0, 1e-12);
    /* a negative gain stays below the guard */
    CHECK_NEAR(replaygain_db(ReplayGainMode::Track, rg), -6.5, 1e-6);
    CHECK_EQ(std::string(replaygain_mode_word(ReplayGainMode::Album)), std::string("Album"));
    CHECK(replaygain_mode_from("album", ReplayGainMode::Off) == ReplayGainMode::Album);
    CHECK(replaygain_mode_from("x", ReplayGainMode::Off) == ReplayGainMode::Off);
}

void test_files(const std::string &dir) {
    TrackTags t;
    /* ID3v2.3, Latin-1 */
    write_file(dir + "/v23.mp3", id3(3, frame23("TIT2", std::string("\0Song", 5)) +
                                            frame23("TXXX", txxx("REPLAYGAIN_TRACK_GAIN", "-7.03 dB")) +
                                            frame23("TXXX", txxx("REPLAYGAIN_TRACK_PEAK", "0.977")) +
                                            frame23("TXXX", txxx("replaygain_album_gain", "-6.10 dB")) +
                                            frame23("TXXX", txxx("REPLAYGAIN_ALBUM_PEAK", "0.999"))) +
                                         mp3_frames());
    CHECK(read_tags(dir + "/v23.mp3", t));
    CHECK_EQ(t.title, std::string("Song"));
    CHECK(t.rg.has_track && t.rg.has_track_peak && t.rg.has_album && t.rg.has_album_peak);
    CHECK_NEAR(t.rg.track_db, -7.03, 1e-4);
    CHECK_NEAR(t.rg.track_peak, 0.977, 1e-5);
    CHECK_NEAR(t.rg.album_db, -6.10, 1e-4);
    /* ID3v2.4, UTF-8 */
    write_file(dir + "/v24.mp3", id3(4, frame24("TXXX", txxx("REPLAYGAIN_TRACK_GAIN", "+2.50 dB", 3))) + mp3_frames());
    CHECK(read_tags(dir + "/v24.mp3", t));
    CHECK(t.rg.has_track);
    CHECK_NEAR(t.rg.track_db, 2.5, 1e-4);
    /* ID3v2.4, UTF-16 with byte-order marks */
    write_file(dir + "/v24u.mp3", id3(4, frame24("TXXX", txxx16("REPLAYGAIN_ALBUM_GAIN", "-8.25 dB"))) + mp3_frames());
    CHECK(read_tags(dir + "/v24u.mp3", t));
    CHECK(t.rg.has_album && !t.rg.has_track);
    CHECK_NEAR(t.rg.album_db, -8.25, 1e-4);
    /* another TXXX is ignored */
    write_file(dir + "/other.mp3", id3(3, frame23("TXXX", txxx("MusicBrainz Album Id", "abc"))) + mp3_frames());
    CHECK(read_tags(dir + "/other.mp3", t));
    CHECK(!t.rg.any());
    /* FLAC */
    write_file(dir + "/a.flac", flac_file({"TITLE=F", "REPLAYGAIN_TRACK_GAIN=-5.31 dB", "REPLAYGAIN_TRACK_PEAK=0.81",
                                            "REPLAYGAIN_ALBUM_GAIN=-4.00 dB"}));
    CHECK(read_tags(dir + "/a.flac", t));
    CHECK_EQ(t.title, std::string("F"));
    CHECK_NEAR(t.rg.track_db, -5.31, 1e-4);
    CHECK_NEAR(t.rg.track_peak, 0.81, 1e-5);
    CHECK_NEAR(t.rg.album_db, -4.0, 1e-4);
    /* Ogg Vorbis */
    write_file(dir + "/a.ogg", ogg_vorbis({"TITLE=V", "replaygain_track_gain=-3.75 dB"}));
    CHECK(read_tags(dir + "/a.ogg", t));
    CHECK_EQ(t.codec, std::string("Vorbis"));
    CHECK(t.rg.has_track);
    CHECK_NEAR(t.rg.track_db, -3.75, 1e-4);
    /* Opus: R128_TRACK_GAIN -1280 (-5 dB to -23 LUFS) is 0 dB of ReplayGain, R128_ALBUM_GAIN 256 is +6 dB; the header's
     * output gain is the decoder's (applied there), never added here */
    write_file(dir + "/a.opus", ogg_opus({"TITLE=O", "R128_TRACK_GAIN=-1280", "R128_ALBUM_GAIN=256"}, 768));
    CHECK(read_tags(dir + "/a.opus", t));
    CHECK_EQ(t.codec, std::string("Opus"));
    CHECK(t.rg.has_track && t.rg.has_album);
    CHECK_NEAR(t.rg.track_db, 0.0, 1e-4);
    CHECK_NEAR(t.rg.album_db, 6.0, 1e-4);
    CHECK(t.rg.r128_track);
    /* MP4 freeform items */
    write_file(dir + "/a.m4a", mp4_file(freeform("replaygain_track_gain", "-9.12 dB") + freeform("replaygain_track_peak", "1.000000") +
                                        freeform("iTunNORM", " 00000A0C")));
    CHECK(read_tags(dir + "/a.m4a", t));
    CHECK(t.rg.has_track && t.rg.has_track_peak && !t.rg.has_album);
    CHECK_NEAR(t.rg.track_db, -9.12, 1e-4);
    CHECK_NEAR(t.rg.track_peak, 1.0, 1e-6);
    /* the demonstration world's WAV with its id3 chunk */
    CHECK(write_demo_wav(dir + "/demo.wav", 2, 44100, "D", "A", "B", "2026", 1, -3.2));
    CHECK(read_tags(dir + "/demo.wav", t));
    CHECK_EQ(t.title, std::string("D"));
    CHECK_NEAR(t.rg.track_db, -3.2, 1e-4);
    CHECK_EQ(replaygain_chip_text(ReplayGainMode::Track, t.rg), std::string("RG -3.2 dB"));
    ReplayGainTags only;
    CHECK(read_replaygain(dir + "/a.flac", only));
    CHECK_NEAR(only.track_db, -5.31, 1e-4);
}

/* report 060 finding 3: the MP3 tag walked in the file; a 2 MB cover is skipped by its size unless a picture is asked */
void test_bounded_mp3(const std::string &dir) {
    std::string apic;
    apic += '\0';
    apic += "image/jpeg";
    apic += '\0';
    apic += '\x03';
    apic += '\0';
    apic += std::string("\xFF\xD8\xFF\xE0", 4);
    apic += std::string(2u << 20, 'x');
    const std::string tag = id3(3, frame23("TIT2", std::string("\0Big cover", 10)) + frame23("APIC", apic) +
                                       frame23("TPE1", std::string("\0After the picture", 18)) +
                                       frame23("TXXX", txxx("REPLAYGAIN_TRACK_GAIN", "-4.00 dB")));
    write_file(dir + "/cover.mp3", tag + mp3_frames());
    TrackTags t;
    uint64_t bytes = 0;
    CHECK(read_tags(dir + "/cover.mp3", t, nullptr, 4u << 20, &bytes));
    CHECK_EQ(t.title, std::string("Big cover"));
    CHECK_EQ(t.artist, std::string("After the picture")); /* the frames after the picture are read */
    CHECK(t.has_picture);
    CHECK_NEAR(t.rg.track_db, -4.0, 1e-4);
    CHECK(t.duration_ms > 0);
    CHECK(bytes < 64u * 1024);
    std::printf("  a 2 MB cover: %llu bytes read for the tags (0.1.0 read the whole tag: %zu)\n",
                static_cast<unsigned long long>(bytes), tag.size());
    /* the cover on demand (the 1200 px cap is the decoder's): read when asked */
    Picture pic;
    CHECK(read_tags(dir + "/cover.mp3", t, &pic, 4u << 20, &bytes));
    CHECK_EQ(pic.data.size(), static_cast<size_t>((2u << 20) + 4));
    CHECK(bytes > (2u << 20));
    /* a picture larger than the cap is never read */
    CHECK(read_tags(dir + "/cover.mp3", t, &pic, 1u << 20, &bytes));
    CHECK(pic.data.empty());
    CHECK(bytes < 64u * 1024);
    /* a tag unsynchronised as a whole (v2.3 flag): read in one piece, its first 256 kB without a picture asked */
    std::string un = id3(3, frame23("TIT2", std::string("\0Unsynced", 9)));
    un[5] = static_cast<char>(0x80);
    write_file(dir + "/unsync.mp3", un + mp3_frames());
    CHECK(read_tags(dir + "/unsync.mp3", t));
    CHECK_EQ(t.title, std::string("Unsynced"));
    /* the tag-read bench: VINYL_TAG_BENCH=<folder> (the study's MP3s), bytes and time per file */
    if (const char *b = std::getenv("VINYL_TAG_BENCH")) {
        DIR *d = ::opendir(b);
        int files = 0;
        uint64_t total = 0, old_total = 0, covers = 0;
        std::vector<std::string> names;
        if (d) {
            while (dirent *e = ::readdir(d)) {
                const std::string n = e->d_name;
                if (n.size() >= 4 && n.compare(n.size() - 4, 4, ".mp3") == 0) names.push_back(std::string(b) + "/" + n);
            }
            ::closedir(d);
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (const std::string &path : names) {
            TrackTags bt;
            uint64_t br = 0;
            if (!read_tags(path, bt, nullptr, 4u << 20, &br)) continue;
            ++files;
            total += br;
            covers += bt.has_picture ? 1 : 0;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (const std::string &path : names) { /* what 0.1.0 read: the whole ID3 tag, the 64 kB frame scan, ID3v1 */
            FILE *f = std::fopen(path.c_str(), "rb");
            unsigned char h[10] = {0};
            if (!f) continue;
            const size_t got = std::fread(h, 1, 10, f);
            std::fclose(f);
            uint64_t tag = 0;
            if (got == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3')
                tag = 10 + ((uint64_t(h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F));
            old_total += tag + 64 * 1024 + 128;
        }
        std::printf("  tag bench %s: %d MP3s (%llu with a cover), %.1f kB read a file (0.1.0: %.1f kB), %.2f ms a file\n", b,
                    files, static_cast<unsigned long long>(covers), files ? total / 1024.0 / files : 0.0,
                    files ? old_total / 1024.0 / files : 0.0, files ? ms / files : 0.0);
    }
}

void test_engine_gain(const std::string &dir) {
    write_file(dir + "/flat.wav", flat_wav(44100, 8192, 16000));
    write_file(dir + "/flat2.wav", flat_wav(44100, 4096, 16000));
    auto reader = [](const std::string &path, ReplayGainTags &out) {
        out = ReplayGainTags();
        if (path.find("flat.wav") != std::string::npos) {
            out.has_track = true;
            out.track_db = -6.0206f; /* half the amplitude */
            out.has_album = true;
            out.album_db = -12.0412f;
        }
        return true;
    };
    /* Track: every sample halved (before the output: nothing else changes) */
    {
        Record r;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p, reader);
        e.set_replaygain(ReplayGainMode::Track);
        e.load(0, dir + "/flat.wav", 0, true);
        CHECK(wait_end(e));
        std::lock_guard<std::mutex> l(r.mu);
        CHECK_EQ(r.samples.size(), static_cast<size_t>(8192 * 2));
        bool half = !r.samples.empty();
        for (int16_t v : r.samples) half = half && std::abs(v - 8000) <= 1;
        CHECK(half);
        CHECK_NEAR(e.snapshot().rg.track_db, -6.0206, 1e-3);
    }
    /* Off: bit-exact */
    {
        Record r;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p, reader);
        e.set_replaygain(ReplayGainMode::Off);
        e.load(0, dir + "/flat.wav", 0, true);
        CHECK(wait_end(e));
        std::lock_guard<std::mutex> l(r.mu);
        bool exact = !r.samples.empty();
        for (int16_t v : r.samples) exact = exact && v == 16000;
        CHECK(exact);
    }
    /* Album, gapless into an untagged track: the gain switches exactly at the boundary, on one stream */
    {
        Record r;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p, reader);
        e.set_replaygain(ReplayGainMode::Album);
        e.load(0, dir + "/flat.wav", 0, true);
        e.set_next(1, dir + "/flat2.wav");
        CHECK(wait_end(e));
        std::lock_guard<std::mutex> l(r.mu);
        CHECK_EQ(r.opens, 1);
        CHECK_EQ(r.samples.size(), static_cast<size_t>((8192 + 4096) * 2));
        bool ok = r.samples.size() == static_cast<size_t>((8192 + 4096) * 2);
        for (size_t i = 0; ok && i < r.samples.size(); ++i)
            ok = std::abs(r.samples[i] - (i < 8192 * 2 ? 4000 : 16000)) <= 1;
        CHECK(ok);
    }
    /* gapless off: the same format still drains and opens again */
    {
        Record r;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p, reader);
        e.set_gapless(false);
        e.load(0, dir + "/flat2.wav", 0, true);
        e.set_next(1, dir + "/flat2.wav");
        CHECK(wait_end(e));
        std::lock_guard<std::mutex> l(r.mu);
        CHECK_EQ(r.opens, 2);
        CHECK_EQ(r.drains, 2);
    }
    /* the sleep timer's end fade: nothing named next, the last 10 s of a 12 s track fade to -60 dB */
    {
        write_file(dir + "/twelve.wav", flat_wav(44100, 44100 * 12, 16000));
        Record r;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p, reader);
        e.set_end_fade(true);
        e.load(0, dir + "/twelve.wav", 0, true);
        CHECK(wait_end(e, 10000));
        std::lock_guard<std::mutex> l(r.mu);
        CHECK_EQ(r.samples.size(), static_cast<size_t>(44100 * 12 * 2));
        if (r.samples.size() == static_cast<size_t>(44100 * 12 * 2)) {
            CHECK_EQ(r.samples[2 * 44100], 16000);                         /* at 1 s: untouched */
            const int at7 = r.samples[2 * (44100 * 7)];                   /* 5 s before the end: about -30 dB */
            CHECK(at7 > 16000 * 0.025 && at7 < 16000 * 0.04);
            CHECK(std::abs(r.samples[r.samples.size() - 2]) <= 20);      /* the last period: about -60 dB */
        }
    }
}

}  // namespace

void test_replaygain() {
    test_parse();
    test_gain_math();
    const std::string dir = make_temp_dir("rg");
    test_files(dir);
    test_bounded_mp3(dir);
    test_engine_gain(dir);
    rm_tree(dir);
}
