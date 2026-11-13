/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the tag reader's scan mode (report 060 2.3, test_tags_scan): files with a 2 MB picture (an MP3 APIC and a
 * GEOB, a FLAC PICTURE block and a METADATA_BLOCK_PICTURE comment, an Ogg Vorbis picture comment across many pages, a WAV
 * id3 chunk, an APEv2 cover) are read in under 64 kB, with the fields that come after the picture; and the APEv2 reader
 * (WavPack, an MP3 with mp3gain's tag before ID3v1).
 */
#include "check.hpp"

#include "tags/tag_reader.hpp"

using namespace vinyl;

namespace {

constexpr uint64_t kScanBudget = 64u << 10;
const size_t kBig = 2u << 20; /* the 2 MB picture */

std::string syncsafe(uint32_t n) {
    std::string s;
    for (int i = 3; i >= 0; --i) s += static_cast<char>((n >> (7 * i)) & 0x7F);
    return s;
}

std::string frame23(const std::string &id, const std::string &body) {
    std::string f = id;
    put_be32(f, static_cast<uint32_t>(body.size()));
    f += std::string(2, '\0');
    return f + body;
}

std::string id3v23(const std::string &frames) { return std::string("ID3\x03\x00\x00", 6) + syncsafe(static_cast<uint32_t>(frames.size())) + frames; }

std::string text_body(const std::string &v) { return std::string(1, '\0') + v; } /* Latin-1 */

std::string apic_body(size_t n) {
    return std::string("\x00image/jpeg\x00\x03\x00", 14) + std::string(n, '\xAB');
}

/* three MPEG-1 layer III frames, 128 kbit/s, 44.1 kHz */
std::string mpeg_frames() {
    std::string f;
    for (int i = 0; i < 3; ++i) {
        std::string fr("\xFF\xFB\x90\x64", 4);
        fr.resize(417, '\0');
        f += fr;
    }
    return f;
}

std::string base64(const std::string &in) {
    static const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint32_t(static_cast<unsigned char>(in[i])) << 16) | (uint32_t(static_cast<unsigned char>(in[i + 1])) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        o += a[v >> 18];
        o += a[(v >> 12) & 63];
        o += a[(v >> 6) & 63];
        o += a[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint32_t(static_cast<unsigned char>(in[i])) << 16;
        if (i + 1 < in.size()) v |= uint32_t(static_cast<unsigned char>(in[i + 1])) << 8;
        o += a[v >> 18];
        o += a[(v >> 12) & 63];
        o += i + 1 < in.size() ? a[(v >> 6) & 63] : '=';
        o += '=';
    }
    return o;
}

/* a FLAC PICTURE block body */
std::string flac_picture(size_t n) {
    std::string p;
    put_be32(p, 3);
    put_be32(p, 10);
    p += "image/jpeg";
    put_be32(p, 0);
    p += std::string(16, '\0');
    put_be32(p, static_cast<uint32_t>(n));
    return p + std::string(n, '\xCD');
}

std::string vorbis_comments(const std::vector<std::string> &items) {
    std::string c;
    put_le32(c, 4);
    c += "test";
    put_le32(c, static_cast<uint32_t>(items.size()));
    for (const std::string &i : items) {
        put_le32(c, static_cast<uint32_t>(i.size()));
        c += i;
    }
    return c;
}

/* Ogg pages for the packets (each laced across as many pages as it needs); the last page carries `granule` */
std::string ogg_stream(const std::vector<std::string> &packets, uint64_t granule) {
    std::string out;
    uint32_t seq = 0;
    for (size_t p = 0; p < packets.size(); ++p) {
        const std::string &pk = packets[p];
        size_t off = 0;
        bool first = true;
        do {
            std::string lace, body;
            while (lace.size() < 255) {
                const size_t left = pk.size() - off;
                const size_t seg = std::min<size_t>(left, 255);
                lace += static_cast<char>(seg);
                body.append(pk, off, seg);
                off += seg;
                if (seg < 255) break;
            }
            const bool done = off >= pk.size() && static_cast<unsigned char>(lace.back()) < 255;
            std::string h = "OggS";
            h += '\0';
            h += static_cast<char>((first ? 0 : 1) | (p == 0 ? 2 : 0) | (done && p + 1 == packets.size() ? 4 : 0));
            const uint64_t g = done && p + 1 == packets.size() ? granule : 0;
            put_le32(h, static_cast<uint32_t>(g));
            put_le32(h, static_cast<uint32_t>(g >> 32));
            put_le32(h, 0x1234);
            put_le32(h, seq++);
            put_le32(h, 0); /* the CRC: not checked by the tag reader */
            h += static_cast<char>(lace.size());
            out += h + lace + body;
            first = false;
            if (done) break;
        } while (true);
    }
    return out;
}

std::string ape_item(const std::string &key, const std::string &value, bool binary = false) {
    std::string i;
    put_le32(i, static_cast<uint32_t>(value.size()));
    put_le32(i, binary ? 2u : 0u);
    return i + key + std::string(1, '\0') + value;
}

/* an APEv2 tag with a header and a footer */
std::string ape_tag(const std::vector<std::string> &items) {
    std::string body;
    for (const std::string &i : items) body += i;
    auto hf = [&](bool header) {
        std::string s = "APETAGEX";
        put_le32(s, 2000);
        put_le32(s, static_cast<uint32_t>(body.size() + 32));
        put_le32(s, static_cast<uint32_t>(items.size()));
        put_le32(s, (1u << 31) | (header ? (1u << 29) : 0));
        return s + std::string(8, '\0');
    };
    return hf(true) + body + hf(false);
}

std::string id3v1(const std::string &title) {
    std::string t = "TAG" + title;
    t.resize(128, '\0');
    return t;
}

}  // namespace

void test_tags_scan() {
    const std::string dir = make_temp_dir("tagscan");
    TrackTags t;
    uint64_t bytes = 0;

    /* MP3: TIT2, TPE1, a 2 MB APIC, a 100 kB GEOB, then TALB and a TXXX ReplayGain after them */
    {
        std::string frames = frame23("TIT2", text_body("Scan Title")) + frame23("TPE1", text_body("Scan Artist")) +
                             frame23("APIC", apic_body(kBig)) +
                             frame23("GEOB", std::string("\x00" "application/octet-stream\x00\x00\x00", 28) +
                                                 std::string(100000, 'g')) +
                             frame23("TALB", text_body("After The Cover")) +
                             frame23("TXXX", std::string("\x00replaygain_track_gain\x00-7.50 dB", 31));
        const std::string path = dir + "/cover.mp3";
        write_file(path, id3v23(frames) + mpeg_frames());
        CHECK(read_tags_scan(path, t, &bytes));
        CHECK(bytes < kScanBudget);
        std::fprintf(stderr, "  scan mode, MP3 with a 2 MB APIC: %llu bytes read\n", static_cast<unsigned long long>(bytes));
        CHECK_EQ(t.title, std::string("Scan Title"));
        CHECK_EQ(t.artist, std::string("Scan Artist"));
        CHECK_EQ(t.album, std::string("After The Cover"));
        CHECK(t.has_picture);
        CHECK(t.rg.has_track);
        CHECK_NEAR(t.rg.track_db, -7.5, 0.01);
        CHECK(t.ok);
        /* the cover is still there when asked for */
        Picture pic;
        CHECK(read_tags(path, t, &pic, 4u << 20, &bytes));
        CHECK_EQ(pic.data.size(), kBig);
        CHECK(bytes > kBig);
    }

    /* FLAC: a picture comment in VORBIS_COMMENT (fields after it) and a 2 MB PICTURE block */
    {
        std::string f = "fLaC";
        std::string si;
        put_be16(si, 4096);
        put_be16(si, 4096);
        si += std::string(6, '\0');
        const uint64_t packed = (uint64_t(44100) << 44) | (uint64_t(1) << 41) | (uint64_t(15) << 36) | 441000u;
        put_be32(si, static_cast<uint32_t>(packed >> 32));
        put_be32(si, static_cast<uint32_t>(packed));
        si += std::string(16, '\0');
        f += '\x00';
        f += std::string("\x00\x00\x22", 3);
        f += si;
        const std::string vc = vorbis_comments({"TITLE=Flac Title", "METADATA_BLOCK_PICTURE=" + base64(flac_picture(1500000)),
                                                "ARTIST=Flac Artist", "REPLAYGAIN_TRACK_GAIN=-3.00 dB"});
        f += '\x04';
        f += static_cast<char>((vc.size() >> 16) & 0xFF);
        f += static_cast<char>((vc.size() >> 8) & 0xFF);
        f += static_cast<char>(vc.size() & 0xFF);
        f += vc;
        const std::string pb = flac_picture(kBig);
        f += '\x86'; /* PICTURE, the last block */
        f += static_cast<char>((pb.size() >> 16) & 0xFF);
        f += static_cast<char>((pb.size() >> 8) & 0xFF);
        f += static_cast<char>(pb.size() & 0xFF);
        f += pb;
        f += std::string(1000, '\0');
        const std::string path = dir + "/cover.flac";
        write_file(path, f);
        CHECK(read_tags_scan(path, t, &bytes));
        std::fprintf(stderr, "  scan mode, FLAC with two 2 MB pictures: %llu bytes read\n", static_cast<unsigned long long>(bytes));
        CHECK(bytes < kScanBudget);
        CHECK_EQ(t.title, std::string("Flac Title"));
        CHECK_EQ(t.artist, std::string("Flac Artist"));
        CHECK(t.has_picture);
        CHECK(t.rg.has_track);
        CHECK_NEAR(t.rg.track_db, -3.0, 0.01);
        CHECK_EQ(t.rate, 44100u);
        CHECK_EQ(t.duration_ms, static_cast<uint64_t>(10000));
        Picture pic;
        CHECK(read_tags(path, t, &pic, 4u << 20));
        CHECK_EQ(pic.data.size(), static_cast<size_t>(1500000)); /* the comment's front cover came first and stays */
        CHECK_EQ(t.artist, std::string("Flac Artist"));
    }

    /* Ogg Vorbis: a 2 MB picture comment across about 45 pages, the artist after it */
    {
        std::string ident("\x01vorbis", 7);
        put_le32(ident, 0);
        ident += '\x02';
        put_le32(ident, 44100);
        put_le32(ident, 0);
        put_le32(ident, 128000);
        put_le32(ident, 0);
        ident += '\xB8';
        ident += '\x01';
        const std::string comment = std::string("\x03vorbis", 7) +
                                    vorbis_comments({"TITLE=Ogg Title", "METADATA_BLOCK_PICTURE=" + base64(flac_picture(kBig)),
                                                     "ARTIST=Ogg Artist"}) +
                                    std::string(1, '\x01');
        const std::string path = dir + "/cover.ogg";
        write_file(path, ogg_stream({ident, comment, std::string(3000, '\x55')}, 441000));
        CHECK(read_tags_scan(path, t, &bytes));
        std::fprintf(stderr, "  scan mode, Ogg Vorbis with a 2 MB picture comment: %llu bytes read\n",
                     static_cast<unsigned long long>(bytes));
        CHECK(bytes < kScanBudget);
        CHECK_EQ(t.codec, std::string("Vorbis"));
        CHECK_EQ(t.title, std::string("Ogg Title"));
        CHECK_EQ(t.artist, std::string("Ogg Artist"));
        CHECK(t.has_picture);
        CHECK_EQ(t.duration_ms, static_cast<uint64_t>(10000));
        /* the whole packet with a picture asked for: the same fields and the picture */
        Picture pic;
        CHECK(read_tags(path, t, &pic, 4u << 20));
        CHECK_EQ(t.artist, std::string("Ogg Artist"));
        CHECK_EQ(pic.data.size(), kBig);
    }

    /* WAV: an id3 chunk with a 2 MB APIC between the title and the artist */
    {
        std::string fmt;
        put_le16(fmt, 1);
        put_le16(fmt, 2);
        put_le32(fmt, 44100);
        put_le32(fmt, 44100 * 4);
        put_le16(fmt, 4);
        put_le16(fmt, 16);
        const std::string tag = id3v23(frame23("TIT2", text_body("Wav Title")) + frame23("APIC", apic_body(kBig)) +
                                       frame23("TPE1", text_body("Wav Artist")));
        std::string body = "WAVE";
        body += "fmt ";
        put_le32(body, 16);
        body += fmt;
        body += "data";
        put_le32(body, 44100 * 4);
        body += std::string(44100 * 4, '\0');
        body += "id3 ";
        put_le32(body, static_cast<uint32_t>(tag.size()));
        body += tag;
        if (tag.size() & 1) body += '\0';
        std::string riff = "RIFF";
        put_le32(riff, static_cast<uint32_t>(body.size()));
        const std::string path = dir + "/cover.wav";
        write_file(path, riff + body);
        CHECK(read_tags_scan(path, t, &bytes));
        CHECK(bytes < kScanBudget);
        CHECK_EQ(t.title, std::string("Wav Title"));
        CHECK_EQ(t.artist, std::string("Wav Artist"));
        CHECK(t.has_picture);
        CHECK_EQ(t.duration_ms, static_cast<uint64_t>(1000));
    }

    /* APEv2: a WavPack file (header, then the tag at the end) with a 2 MB cover item; then with an ID3v1 after it */
    {
        std::string wv("wvpk", 4);
        wv += std::string(28, '\0');
        wv += std::string(5000, '\x11');
        const std::vector<std::string> items = {
            ape_item("Title", "Ape Title"),          ape_item("Artist", "Ape Artist"), ape_item("Album", "Ape Album"),
            ape_item("Track", "3/12"),               ape_item("Disc", "2"),            ape_item("Year", "2001"),
            ape_item("Genre", std::string("Jazz\0Blues", 10)),                         ape_item("REPLAYGAIN_TRACK_GAIN", "-6.50 dB"),
            ape_item("Cover Art (Front)", std::string("cover.jpg\0", 10) + std::string(kBig, '\xEE'), true),
            ape_item("Comment", "after the cover")};
        const std::string path = dir + "/tagged.wv";
        write_file(path, wv + ape_tag(items));
        CHECK(read_tags_scan(path, t, &bytes));
        std::fprintf(stderr, "  scan mode, WavPack with a 2 MB APEv2 cover: %llu bytes read\n", static_cast<unsigned long long>(bytes));
        CHECK(bytes < kScanBudget);
        CHECK_EQ(t.codec, std::string("WavPack"));
        CHECK_EQ(t.title, std::string("Ape Title"));
        CHECK_EQ(t.artist, std::string("Ape Artist"));
        CHECK_EQ(t.album, std::string("Ape Album"));
        CHECK_EQ(t.track, 3);
        CHECK_EQ(t.disc, 2);
        CHECK_EQ(t.year(), std::string("2001"));
        CHECK_EQ(t.genre, std::string("Jazz")); /* the first of two values */
        CHECK(t.rg.has_track);
        CHECK_NEAR(t.rg.track_db, -6.5, 0.01);
        CHECK(t.has_picture);
        /* a small cover is taken when asked for */
        const std::vector<std::string> small = {ape_item("Title", "Small"),
                                                ape_item("Cover Art (Front)", std::string("c.jpg\0", 6) + std::string(1000, '\xEE'), true)};
        write_file(path, wv + ape_tag(small) + id3v1("V1 Title"));
        Picture pic;
        CHECK(read_tags(path, t, &pic));
        CHECK_EQ(t.title, std::string("Small"));
        CHECK_EQ(pic.data.size(), static_cast<size_t>(1000));
        CHECK_EQ(pic.type, 3);
        /* damaged: a footer whose size runs past the start, an item longer than the tag */
        std::string bad = wv + ape_tag(items);
        bad[bad.size() - 32 + 12] = '\x7F';
        bad[bad.size() - 32 + 15] = '\x7F';
        write_file(path, bad);
        CHECK(read_tags_scan(path, t, &bytes));
        CHECK(t.title.empty());
        std::string bad2 = wv + ape_tag({ape_item("Title", "Cut")});
        bad2[wv.size() + 32] = '\x7F'; /* the item's length */
        write_file(path, bad2);
        CHECK(read_tags_scan(path, t, &bytes));
        CHECK(t.title.empty());
    }

    /* MP3 with mp3gain's APEv2 tag before ID3v1: the APE values before ID3v1's; an ID3v2 title stays first */
    {
        const std::string ape = ape_tag({ape_item("REPLAYGAIN_TRACK_GAIN", "-4.20 dB"), ape_item("Title", "Ape In Mp3")});
        const std::string path = dir + "/gain.mp3";
        write_file(path, mpeg_frames() + ape + id3v1("V1 Title"));
        CHECK(read_tags_scan(path, t, &bytes));
        CHECK_EQ(t.title, std::string("Ape In Mp3"));
        CHECK(t.rg.has_track);
        CHECK_NEAR(t.rg.track_db, -4.2, 0.01);
        CHECK(t.ok);
        write_file(path, id3v23(frame23("TIT2", text_body("Id3 Title"))) + mpeg_frames() + ape + id3v1("V1 Title"));
        CHECK(read_tags_scan(path, t, &bytes));
        CHECK_EQ(t.title, std::string("Id3 Title"));
        CHECK(t.rg.has_track);
    }
    rm_tree(dir);
}
