/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the tag reader on files built byte by byte (ID3v2.3 UTF-16 with CJK text and a cover, ID3v2.4 UTF-8,
 * ID3v1, the Xing length, FLAC blocks, Ogg Vorbis and Opus with a comment packet across pages, MP4 items, WAV INFO,
 * AIFF) and the art (the cover cap, decoding and scaling, the drawn sleeve, record and label).
 */
#include "check.hpp"

#include "art/cover.hpp"
#include "demo/demo_world.hpp"
#include "tags/tag_reader.hpp"

#include <cstring>

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

std::string utf16le_bom(const std::u16string &u) {
    std::string s = "\xFF\xFE";
    for (char16_t c : u) {
        s += static_cast<char>(c & 0xFF);
        s += static_cast<char>(c >> 8);
    }
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

/* two MPEG-1 layer III frames, 128 kbps, 44.1 kHz, joint stereo; the first carries a Xing header (frames, bytes) */
std::string mp3_frames(uint32_t xing_frames) {
    std::string f;
    const size_t size = 417;
    std::string a = "\xFF\xFB\x90\x64";
    a += std::string(32, '\0');
    if (xing_frames) {
        a += "Xing";
        put_be32(a, 3);
        put_be32(a, xing_frames);
        put_be32(a, 417 * xing_frames);
    }
    a.resize(size, '\0');
    std::string b = "\xFF\xFB\x90\x64";
    b.resize(size, '\0');
    return a + b + b;
}

const std::string kJpeg = std::string("\xFF\xD8\xFF\xE0", 4) + "fake jpeg bytes";

std::string ogg_page(uint8_t type, uint64_t granule, uint32_t serial, uint32_t seq, const std::vector<std::string> &segs_bodies,
                     bool continued_last = false) {
    /* each body becomes 255-byte segments plus a short last one (unless continued_last: the packet goes on) */
    std::string lace, body;
    for (size_t i = 0; i < segs_bodies.size(); ++i) {
        const std::string &p = segs_bodies[i];
        size_t n = p.size();
        while (n >= 255) {
            lace += static_cast<char>(255);
            n -= 255;
        }
        if (!(continued_last && i + 1 == segs_bodies.size())) lace += static_cast<char>(n);
        body += p;
    }
    std::string h = "OggS";
    h += '\0';
    h += static_cast<char>(type);
    for (int i = 0; i < 8; ++i) h += static_cast<char>((granule >> (8 * i)) & 0xFF);
    put_le32(h, serial);
    put_le32(h, seq);
    put_le32(h, 0); /* the CRC is not checked by the reader */
    h += static_cast<char>(lace.size());
    return h + lace + body;
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

std::string flac_picture_block(const std::string &mime, const std::string &data, uint32_t type = 3) {
    std::string b;
    put_be32(b, type);
    put_be32(b, static_cast<uint32_t>(mime.size()));
    b += mime;
    put_be32(b, 0);
    put_be32(b, 300);
    put_be32(b, 300);
    put_be32(b, 24);
    put_be32(b, 0);
    put_be32(b, static_cast<uint32_t>(data.size()));
    return b + data;
}

std::string base64(const std::string &in) {
    static const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        o += a[v >> 18];
        o += a[(v >> 12) & 63];
        o += a[(v >> 6) & 63];
        o += a[v & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t v = uint8_t(in[i]) << 16;
        o += a[v >> 18];
        o += a[(v >> 12) & 63];
        o += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        o += a[v >> 18];
        o += a[(v >> 12) & 63];
        o += a[(v >> 6) & 63];
        o += '=';
    }
    return o;
}

std::string box(const char *type, const std::string &body) {
    std::string b;
    put_be32(b, static_cast<uint32_t>(8 + body.size()));
    b += type;
    return b + body;
}

std::string data_box(uint32_t type, const std::string &payload) {
    std::string b;
    put_be32(b, type);
    put_be32(b, 0);
    return box("data", b + payload);
}

/* a tiny PNG: stored (uncompressed) deflate blocks, so the test needs no zlib */
uint32_t crc32(const std::string &s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
std::string png_chunk(const char *type, const std::string &data) {
    std::string c;
    put_be32(c, static_cast<uint32_t>(data.size()));
    std::string td = std::string(type) + data;
    c += td;
    put_be32(c, crc32(td));
    return c;
}
std::string tiny_png(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::string raw;
    for (int y = 0; y < h; ++y) {
        raw += '\0';
        for (int x = 0; x < w; ++x) {
            const bool left = x < w / 2; /* left half red-ish, right half the colour */
            raw += static_cast<char>(left ? 250 : r);
            raw += static_cast<char>(left ? 10 : g);
            raw += static_cast<char>(left ? 10 : b);
        }
    }
    std::string z = "\x78\x01";
    size_t off = 0;
    while (off < raw.size() || off == 0) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z += static_cast<char>(last ? 1 : 0);
        put_le16(z, static_cast<uint32_t>(n));
        put_le16(z, static_cast<uint32_t>(~n & 0xFFFF));
        z += raw.substr(off, n);
        off += n;
        if (last) break;
    }
    uint32_t a = 1, bb = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        bb = (bb + a) % 65521;
    }
    put_be32(z, (bb << 16) | a);
    std::string ihdr;
    put_be32(ihdr, static_cast<uint32_t>(w));
    put_be32(ihdr, static_cast<uint32_t>(h));
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);
    return std::string("\x89PNG\r\n\x1a\n", 8) + png_chunk("IHDR", ihdr) + png_chunk("IDAT", z) + png_chunk("IEND", "");
}

}  // namespace

void test_tags() {
    const std::string dir = make_temp_dir("tags");

    /* text conversions */
    const unsigned char lat[] = {'C', 0xE9, 'l', 'i', 'n', 'e'};
    CHECK_EQ(latin1_to_utf8(lat, 6), std::string("Céline"));
    const std::string u16 = utf16le_bom(u"夜明け");
    CHECK_EQ(utf16_to_utf8(reinterpret_cast<const unsigned char *>(u16.data()), u16.size(), true, false), std::string("夜明け"));
    const std::string be = std::string("\x00\x41\xD8\x3D\xDE\x00", 6); /* "A" and U+1F600 (a surrogate pair), big endian */
    CHECK_EQ(utf16_to_utf8(reinterpret_cast<const unsigned char *>(be.data()), be.size(), false, true), std::string("A\xF0\x9F\x98\x80"));
    CHECK_EQ(clean_utf8(std::string("ok\xE9\x01x")), std::string("oké") + "x");
    CHECK_EQ(clean_utf8("日本語"), std::string("日本語"));
    const std::vector<unsigned char> b64 = base64_decode("aGVsbG8=");
    CHECK_EQ(std::string(b64.begin(), b64.end()), std::string("hello"));
    CHECK_EQ(title_from_path("/m/Artist - Song.flac"), std::string("Artist - Song"));
    TrackTags at;
    at.title = "Song";
    CHECK_EQ(artist_title(at, "/x.mp3"), std::string("Song"));
    at.artist = "Band";
    CHECK_EQ(artist_title(at, "/x.mp3"), std::string("Band, Song"));
    at.date = "2013-05-01";
    CHECK_EQ(at.year(), std::string("2013"));

    /* MP3: ID3v2.3 UTF-16 (the study's CJK file), Latin-1 artist, an APIC front cover, the Xing length */
    {
        std::string frames;
        frames += frame23("TIT2", "\x01" + utf16le_bom(u"夜明けのスキャット（テスト曲）"));
        frames += frame23("TPE1", std::string("\x00", 1) + "Phoenix");
        frames += frame23("TALB", "\x01" + utf16le_bom(u"테스트 앨범 059"));
        frames += frame23("TYER", std::string("\x00", 1) + "2013");
        frames += frame23("TRCK", std::string("\x00", 1) + "3/10");
        frames += frame23("APIC", std::string("\x00", 1) + "image/jpeg" + std::string("\x00", 1) + "\x03" + std::string("\x00", 1) + kJpeg);
        write_file(dir + "/a.mp3", id3(3, frames) + mp3_frames(1000));
        TrackTags t;
        Picture pic;
        CHECK(read_tags(dir + "/a.mp3", t, &pic));
        CHECK(t.ok);
        CHECK_EQ(t.codec, std::string("MP3"));
        CHECK_EQ(t.title, std::string("夜明けのスキャット（テスト曲）"));
        CHECK_EQ(t.artist, std::string("Phoenix"));
        CHECK_EQ(t.album, std::string("테스트 앨범 059"));
        CHECK_EQ(t.year(), std::string("2013"));
        CHECK_EQ(t.track, 3);
        CHECK_EQ(t.rate, 44100u);
        CHECK_EQ(t.duration_ms, static_cast<uint64_t>(1000ull * 1152 * 1000 / 44100));
        CHECK(t.has_picture);
        CHECK_EQ(pic.mime, std::string("image/jpeg"));
        CHECK(pic.data.size() == kJpeg.size() && std::memcmp(pic.data.data(), kJpeg.data(), kJpeg.size()) == 0);
        /* a picture over the cap is left out, but reported */
        CHECK(read_tags(dir + "/a.mp3", t, &pic, 4));
        CHECK(t.has_picture && pic.data.empty());
    }
    /* ID3v2.4, UTF-8, syncsafe frame sizes, TDRC; CBR length without a Xing header; ID3v1 as the fall-back */
    {
        std::string frames;
        frames += frame24("TIT2", "\x03" + std::string("Lisztomania"));
        frames += frame24("TDRC", "\x03" + std::string("2009-05-25"));
        std::string v1 = "TAG";
        std::string ttl = "v1 title";
        ttl.resize(30, '\0');
        std::string art = "v1 artist";
        art.resize(30, '\0');
        std::string alb = "v1 album";
        alb.resize(30, '\0');
        v1 += ttl + art + alb + "1999";
        std::string cmt(28, '\0');
        v1 += cmt + std::string("\0\x07\x0c", 3);
        write_file(dir + "/b.mp3", id3(4, frames) + mp3_frames(0) + v1);
        TrackTags t;
        CHECK(read_tags(dir + "/b.mp3", t));
        CHECK_EQ(t.title, std::string("Lisztomania")); /* the v2 tag wins */
        CHECK_EQ(t.artist, std::string("v1 artist"));  /* the v1 fills the rest */
        CHECK_EQ(t.year(), std::string("2009"));
        CHECK_EQ(t.track, 7);
        CHECK_EQ(t.bitrate_kbps, 128u);
        CHECK(t.duration_ms > 0 && t.duration_ms < 200);
    }
    /* FLAC: STREAMINFO, Vorbis comments, a PICTURE */
    {
        std::string f = "fLaC";
        std::string si(34, '\0');
        si[0] = 0x10;
        si[2] = 0x10;
        const uint64_t v = (uint64_t(44100) << 44) | (uint64_t(1) << 41) | (uint64_t(23) << 36) | 441000;
        for (int i = 0; i < 8; ++i) si[10 + i] = static_cast<char>((v >> (56 - 8 * i)) & 0xFF);
        f += '\0';
        f += std::string("\x00\x00\x22", 3);
        f += si;
        const std::string vc = vorbis_comments({"TITLE=Fences (LeMarquis Remix)", "ARTIST=Phoenix", "ALBUM=Fences Remixes", "DATE=2013",
                                                "TRACKNUMBER=03", "title=ignored duplicate"});
        f += '\x04';
        f += static_cast<char>((vc.size() >> 16) & 0xFF);
        f += static_cast<char>((vc.size() >> 8) & 0xFF);
        f += static_cast<char>(vc.size() & 0xFF);
        f += vc;
        const std::string pb = flac_picture_block("image/jpeg", kJpeg);
        f += '\x86';
        f += static_cast<char>((pb.size() >> 16) & 0xFF);
        f += static_cast<char>((pb.size() >> 8) & 0xFF);
        f += static_cast<char>(pb.size() & 0xFF);
        f += pb;
        f += std::string(1000, '\0');
        write_file(dir + "/c.flac", f);
        TrackTags t;
        Picture pic;
        CHECK(read_tags(dir + "/c.flac", t, &pic));
        CHECK_EQ(t.codec, std::string("FLAC"));
        CHECK_EQ(t.rate, 44100u);
        CHECK_EQ(t.channels, 2u);
        CHECK_EQ(t.bits, 24u);
        CHECK_EQ(t.duration_ms, static_cast<uint64_t>(10000));
        CHECK_EQ(t.title, std::string("Fences (LeMarquis Remix)"));
        CHECK_EQ(t.album, std::string("Fences Remixes"));
        CHECK_EQ(t.track, 3);
        CHECK(t.has_picture);
        CHECK_EQ(pic.data.size(), kJpeg.size());
    }
    /* Ogg Vorbis: the comment packet spans two pages and carries a METADATA_BLOCK_PICTURE */
    {
        std::string ident = "\x01vorbis";
        put_le32(ident, 0);
        ident += '\x02';
        put_le32(ident, 44100);
        put_le32(ident, 0);
        put_le32(ident, 160000);
        put_le32(ident, 0);
        ident += std::string("\xB8\x01", 2);
        const std::string pic = flac_picture_block("image/png", std::string(600, 'p'));
        const std::string comments = "\x03vorbis" + vorbis_comments({"TITLE=Ogg Song", "ARTIST=Xiph", std::string("METADATA_BLOCK_PICTURE=") + base64(pic)});
        const size_t cut = 510; /* two full segments on the first page: the packet continues */
        std::string file = ogg_page(2, 0, 77, 0, {ident});
        file += ogg_page(0, 0, 77, 1, {comments.substr(0, cut)}, true);
        file += ogg_page(1, 0, 77, 2, {comments.substr(cut)});
        file += ogg_page(4, 44100 * 7, 77, 3, {std::string(100, 'a')});
        write_file(dir + "/d.ogg", file);
        TrackTags t;
        Picture p;
        CHECK(read_tags(dir + "/d.ogg", t, &p));
        CHECK_EQ(t.codec, std::string("Vorbis"));
        CHECK_EQ(t.title, std::string("Ogg Song"));
        CHECK_EQ(t.artist, std::string("Xiph"));
        CHECK_EQ(t.duration_ms, static_cast<uint64_t>(7000));
        CHECK_EQ(t.bitrate_kbps, 160u);
        CHECK(t.has_picture);
        CHECK_EQ(p.mime, std::string("image/png"));
        CHECK_EQ(p.data.size(), static_cast<size_t>(600));
    }
    /* Ogg Opus: the pre-skip leaves the length */
    {
        std::string head = "OpusHead";
        head += '\x01';
        head += '\x02';
        put_le16(head, 312);
        put_le32(head, 44100);
        put_le16(head, 0);
        head += '\0';
        std::string file = ogg_page(2, 0, 9, 0, {head});
        file += ogg_page(0, 0, 9, 1, {"OpusTags" + vorbis_comments({"TITLE=Opus Song"})});
        file += ogg_page(4, 48000 * 5 + 312, 9, 2, {std::string(50, 'o')});
        write_file(dir + "/e.opus", file);
        TrackTags t;
        CHECK(read_tags(dir + "/e.opus", t));
        CHECK_EQ(t.codec, std::string("Opus"));
        CHECK_EQ(t.title, std::string("Opus Song"));
        CHECK_EQ(t.duration_ms, static_cast<uint64_t>(5000));
    }
    /* MP4 / M4A: mvhd, mp4a, the iTunes items and covr */
    {
        std::string mvhd(100, '\0');
        mvhd[15] = static_cast<char>(0xE8); /* timescale 1000 at 12..15 */
        mvhd[14] = 0x03;
        mvhd[16] = 0;
        mvhd[17] = 0;
        mvhd[18] = 0x30;
        mvhd[19] = 0x39; /* duration 12345 */
        std::string entry;
        entry += std::string(6, '\0');
        put_be16(entry, 1);
        entry += std::string(8, '\0');
        put_be16(entry, 2);
        put_be16(entry, 16);
        put_be16(entry, 0);
        put_be16(entry, 0);
        put_be32(entry, 44100u << 16);
        std::string stsd(4, '\0');
        put_be32(stsd, 1);
        stsd += box("mp4a", entry);
        const std::string trak = box("trak", box("mdia", box("minf", box("stbl", box("stsd", stsd)))));
        std::string trkn(2, '\0');
        put_be16(trkn, 5);
        put_be16(trkn, 12);
        std::string ilst = box("\xA9nam", data_box(1, "AAC Song")) + box("\xA9" "ART", data_box(1, "Apple")) +
                           box("\xA9" "alb", data_box(1, "Album")) + box("trkn", data_box(0, trkn)) + box("covr", data_box(13, kJpeg));
        const std::string meta = box("meta", std::string(4, '\0') + box("hdlr", std::string(25, '\0')) + box("ilst", ilst));
        const std::string moov = box("moov", box("mvhd", mvhd) + trak + box("udta", meta));
        std::string ftyp = box("ftyp", "M4A " + std::string(4, '\0') + "M4A mp42isom");
        write_file(dir + "/f.m4a", ftyp + box("mdat", std::string(2000, 'z')) + moov);
        TrackTags t;
        Picture p;
        CHECK(read_tags(dir + "/f.m4a", t, &p));
        CHECK_EQ(t.codec, std::string("AAC"));
        CHECK_EQ(t.title, std::string("AAC Song"));
        CHECK_EQ(t.artist, std::string("Apple"));
        CHECK_EQ(t.album, std::string("Album"));
        CHECK_EQ(t.track, 5);
        CHECK_EQ(t.duration_ms, static_cast<uint64_t>(12345));
        CHECK_EQ(t.channels, 2u);
        CHECK_EQ(t.rate, 44100u);
        CHECK_EQ(p.mime, std::string("image/jpeg"));
        CHECK_EQ(p.data.size(), kJpeg.size());
    }
    /* WAV with LIST/INFO (the demonstration world's files), AIFF with NAME and COMM */
    {
        CHECK(write_demo_wav(dir + "/g.wav", 244, 44100, "Fences (LeMarquis Remix)", "Phoenix", "Fences Remixes", "2013", 3));
        TrackTags t;
        CHECK(read_tags(dir + "/g.wav", t));
        CHECK_EQ(t.codec, std::string("WAV"));
        CHECK_EQ(t.title, std::string("Fences (LeMarquis Remix)"));
        CHECK_EQ(t.album, std::string("Fences Remixes"));
        CHECK_EQ(t.duration_ms, static_cast<uint64_t>(244000));
        CHECK_EQ(t.track, 3);
        std::string comm;
        put_be16(comm, 2);
        put_be32(comm, 88200);
        put_be16(comm, 16);
        comm += std::string("\x40\x0E\xAC\x44\x00\x00\x00\x00\x00\x00", 10); /* 44100 as an 80-bit float */
        std::string ck = "COMM";
        put_be32(ck, static_cast<uint32_t>(comm.size()));
        ck += comm;
        std::string nm = "NAME";
        put_be32(nm, 10);
        nm += "AIFF Title";
        std::string body = "AIFF" + ck + nm;
        std::string a = "FORM";
        put_be32(a, static_cast<uint32_t>(body.size()));
        write_file(dir + "/h.aif", a + body);
        CHECK(read_tags(dir + "/h.aif", t));
        CHECK_EQ(t.codec, std::string("AIFF"));
        CHECK_EQ(t.title, std::string("AIFF Title"));
        CHECK_EQ(t.rate, 44100u);
        CHECK_EQ(t.duration_ms, static_cast<uint64_t>(2000));
    }
    /* other formats: the codec from the extension; garbage never crashes */
    {
        write_file(dir + "/i.wma", std::string(64, '\x30'));
        TrackTags t;
        CHECK(read_tags(dir + "/i.wma", t));
        CHECK_EQ(t.codec, std::string("WMA"));
        CHECK(t.title.empty());
        std::string junk = id3(3, frame23("TIT2", std::string("\x01\xFF", 2))) + std::string(10, '\xFF');
        junk[6] = 0x7F; /* a tag size past the end */
        write_file(dir + "/j.mp3", junk);
        CHECK(read_tags(dir + "/j.mp3", t));
        CHECK(!read_tags(dir + "/missing.mp3", t));
    }
    rm_tree(dir);
}

void test_art() {
    /* a cover over 1200 px on its long side is never decoded (the memory cap) */
    {
        std::string ihdr;
        put_be32(ihdr, 3000);
        put_be32(ihdr, 3000);
        ihdr += std::string("\x08\x02\x00\x00\x00", 5);
        Picture big;
        const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + png_chunk("IHDR", ihdr) + png_chunk("IEND", "");
        big.data.assign(png.begin(), png.end());
        int w = 0, h = 0;
        CHECK(cover_size(big, w, h));
        CHECK_EQ(w, 3000);
        PixelImage out;
        CHECK(decode_cover(big, 148, out) == CoverStatus::TooLarge);
        CHECK(out.px.empty());
    }
    /* a real (small) PNG: decoded, cut to its centre square, scaled to the sleeve, RGB565 */
    {
        const std::string png = tiny_png(64, 32, 31, 122, 114);
        Picture p;
        p.data.assign(png.begin(), png.end());
        PixelImage out;
        CHECK(decode_cover(p, 16, out) == CoverStatus::Ok);
        CHECK_EQ(out.w, 16);
        CHECK(out.rgb565);
        CHECK_EQ(out.px.size(), static_cast<size_t>(16 * 16 * 2));
        /* the centre square of a 64x32 picture is x 16..47: its left half red, its right half teal */
        const uint16_t left = static_cast<uint16_t>(out.px[0] | (out.px[1] << 8));
        const uint16_t right = static_cast<uint16_t>(out.px[30] | (out.px[31] << 8));
        CHECK((left >> 11) > 25);  /* red */
        CHECK((right >> 11) < 8); /* teal: little red */
        CHECK(cover_peak_bytes() > 0 && cover_peak_bytes() < kCoverMemCeiling);
        Picture broken;
        broken.data.assign(png.begin(), png.begin() + 40);
        CHECK(decode_cover(broken, 16, out) != CoverStatus::Ok);
        Picture none;
        CHECK(decode_cover(none, 16, out) == CoverStatus::None);
    }
    /* the drawn sleeve, record and label */
    {
        const PixelImage s = render_sleeve(148);
        CHECK_EQ(s.w, 148);
        CHECK_EQ(s.px.size(), static_cast<size_t>(148 * 148 * 4));
        CHECK_EQ(static_cast<int>(s.px[3]), 0); /* the rounded corner is transparent */
        const size_t mid = (74 * 148 + 74) * 4;
        CHECK_EQ(static_cast<int>(s.px[mid + 3]), 255);
        const PixelImage d = render_disc(148, 1.0);
        CHECK_EQ(static_cast<int>(d.px[3]), 0);  /* outside the disc */
        CHECK_EQ(static_cast<int>(d.px[mid + 3]), 255);
        CHECK(d.px[mid + 2] < 0x30); /* dark grooves */
        const PixelImage l = render_label(50, 6, 1.0);
        const size_t c = (25 * 50 + 25) * 4;
        CHECK(l.px[c + 2] > 0xC0); /* the cream spindle hole in the middle */
        const size_t teal = (25 * 50 + 4) * 4;
        CHECK_EQ(static_cast<int>(l.px[teal + 1]), 0x7A); /* the teal label */
    }
}
