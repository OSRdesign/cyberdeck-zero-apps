/*
 * SPDX-License-Identifier: MIT
 *
 * WAV/BWF/RF64 header: build, parse, placeholder and oversized detection, size patches, the RF64 boundary on mocked sizes.
 */
#include "check.hpp"
#include "helpers.hpp"

#include <cstring>

using namespace cassette;
using namespace tst;

static uint32_t rd32(const std::vector<unsigned char> &b, size_t o) {
    return static_cast<uint32_t>(b[o]) | static_cast<uint32_t>(b[o + 1]) << 8 | static_cast<uint32_t>(b[o + 2]) << 16 |
           static_cast<uint32_t>(b[o + 3]) << 24;
}

/* The 44-byte header arecord writes when it streams: RIFF 0x80000024, data 0x80000000 (the P0 power-pull files). */
static std::vector<unsigned char> arecord_header() {
    std::vector<unsigned char> h(44, 0);
    auto p32 = [&](size_t o, uint32_t v) {
        for (int i = 0; i < 4; i++) h[o + static_cast<size_t>(i)] = static_cast<unsigned char>(v >> (8 * i));
    };
    std::memcpy(&h[0], "RIFF", 4);
    p32(4, 0x80000024u);
    std::memcpy(&h[8], "WAVEfmt ", 8);
    p32(16, 16);
    h[20] = 1;
    h[22] = 1;
    p32(24, 48000);
    p32(28, 96000);
    h[32] = 2;
    h[34] = 16;
    std::memcpy(&h[36], "data", 4);
    p32(40, 0x80000000u);
    return h;
}

static void header_round_trip() {
    AudioFormat f;
    BwfInfo b;
    b.description = "test take";
    b.origination_date = "2026-11-08";
    b.origination_time = "12:34:56";
    b.time_reference = 0x1234567890ull;
    b.originator_reference = "ref-1";
    std::vector<unsigned char> h = build_header(f, b);
    CHECK_EQ(h.size(), static_cast<size_t>(kDataOffset));
    CHECK(std::memcmp(&h[0], "RIFF", 4) == 0);
    CHECK_EQ(rd32(h, 4), kDataOffset - 8u); /* valid for a zero-length file */
    CHECK(std::memcmp(&h[12], "JUNK", 4) == 0);
    CHECK(std::memcmp(&h[kDataOffset - 8], "data", 4) == 0);
    CHECK_EQ(rd32(h, kDataOffset - 4), 0u);

    HeaderInfo i = parse_header(h.data(), h.size(), h.size()); /* file = header only */
    CHECK(i.ok);
    CHECK_EQ(i.fmt.rate, 48000u);
    CHECK_EQ(static_cast<int>(i.fmt.channels), 1);
    CHECK_EQ(static_cast<int>(i.fmt.bits), 16);
    CHECK_EQ(i.data_offset, static_cast<uint64_t>(kDataOffset));
    CHECK_EQ(i.declared_data, static_cast<uint64_t>(0));
    CHECK(i.consistent());
    CHECK(!i.suspicious());
    CHECK(i.has_bext && i.has_ixml && i.has_reserve && !i.rf64);
    CHECK(i.ixml_size >= 4096);
    CHECK_EQ(i.ixml_offset + i.ixml_size + 8, static_cast<uint64_t>(kDataOffset));
    /* bext fields */
    const size_t o = static_cast<size_t>(i.bext_offset);
    CHECK(std::memcmp(&h[o], "test take", 9) == 0);
    CHECK(std::memcmp(&h[o + 256], "Cassette", 8) == 0);
    CHECK(std::memcmp(&h[o + 288], "ref-1", 5) == 0);
    CHECK(std::memcmp(&h[o + 320], "2026-11-08", 10) == 0);
    CHECK(std::memcmp(&h[o + 330], "12:34:56", 8) == 0);
    CHECK_EQ(rd32(h, o + 338), 0x34567890u);
    CHECK_EQ(rd32(h, o + 342), 0x12u);
    CHECK(std::string(reinterpret_cast<const char *>(&h[o + 602])) == coding_history_line(f));
    CHECK(coding_history_line(f) == "A=PCM,F=48000,W=16,M=mono,T=Cassette\r\n");
    CHECK(std::memcmp(&h[static_cast<size_t>(i.ixml_offset)], "<?xml", 5) == 0);

    /* the header with some audio: sizes carry the real values */
    std::vector<unsigned char> h2 = build_header(f, b, 1000);
    HeaderInfo i2 = parse_header(h2.data(), h2.size(), kDataOffset + 1000);
    CHECK(i2.ok && i2.consistent());
    CHECK_EQ(i2.declared_data, static_cast<uint64_t>(1000));
}

static void other_formats() {
    AudioFormat st{44100, 2, 24};
    std::vector<unsigned char> h = build_header(st, BwfInfo{});
    HeaderInfo i = parse_header(h.data(), h.size(), h.size());
    CHECK(i.ok);
    CHECK_EQ(i.fmt.rate, 44100u);
    CHECK_EQ(static_cast<int>(i.fmt.channels), 2);
    CHECK_EQ(static_cast<int>(i.fmt.bits), 24);
    CHECK_EQ(i.block_align, 6u);
    CHECK_EQ(h.size(), static_cast<size_t>(kDataOffset));
    AudioFormat six{96000, 6, 32}; /* extensible */
    h = build_header(six, BwfInfo{});
    i = parse_header(h.data(), h.size(), h.size());
    CHECK(i.ok);
    CHECK_EQ(i.block_align, 24u);
    CHECK_EQ(i.data_offset, static_cast<uint64_t>(kDataOffset));
    CHECK_EQ(block_align(AudioFormat{48000, 1, 8}), 0u);
}

static void arecord_placeholder() {
    std::vector<unsigned char> h = arecord_header();
    h.resize(44 + 1001, 0x55); /* 1001 bytes of audio: an odd byte at the end */
    HeaderInfo i = parse_header(h.data(), h.size(), h.size());
    CHECK(i.ok);
    CHECK_EQ(i.data_offset, static_cast<uint64_t>(44));
    CHECK_EQ(i.actual_data, static_cast<uint64_t>(1001));
    CHECK(i.placeholder_riff);
    CHECK(i.placeholder_data);
    CHECK(i.suspicious());
    CHECK(!i.consistent());
    CHECK(!i.has_reserve); /* no room for ds64 */
    CHECK(is_placeholder_size(0x80000000u) && is_placeholder_size(0x7FFFFFFFu) && is_placeholder_size(0xFFFFFFFFu));
    CHECK(!is_placeholder_size(1000) && !is_placeholder_size(0x40000000u));
    /* the same through a file */
    TempDir d;
    write_file(d.file("a.wav"), h);
    HeaderInfo j = parse_header(posix_file_ops(), d.file("a.wav"));
    CHECK(j.ok && j.placeholder_data && j.file_size == h.size());
    CHECK(!parse_header(posix_file_ops(), d.file("missing.wav")).ok);
}

static void oversized_and_garbage() {
    AudioFormat f;
    std::vector<unsigned char> h = build_header(f, BwfInfo{}, 1000000); /* declares 1 MB */
    HeaderInfo i = parse_header(h.data(), h.size(), kDataOffset + 4000);
    CHECK(i.ok);
    CHECK(i.oversized_data && i.oversized_riff);
    CHECK(i.suspicious() && !i.placeholder_data);
    /* a smaller declared size than the file holds is not "suspicious" (a finished file may have chunks after the audio) */
    h = build_header(f, BwfInfo{}, 100);
    i = parse_header(h.data(), h.size(), kDataOffset + 4000);
    CHECK(!i.suspicious() && !i.consistent());
    unsigned char junk[64] = {1, 2, 3};
    CHECK(!parse_header(junk, sizeof junk, sizeof junk).ok);
    CHECK(!parse_header(h.data(), 20, 20).ok);
    CHECK(!parse_header(h.data(), 100, 100).ok); /* no data chunk in the bytes we have */
}

static void patches() {
    AudioFormat f;
    std::vector<unsigned char> h = build_header(f, BwfInfo{});
    HeaderLayout l;
    l.block_align = 2;
    PatchSet ps = size_patches(l, 4000);
    CHECK(!ps.rf64 && !ps.clamped);
    CHECK_EQ(ps.patches.size(), static_cast<size_t>(2));
    for (const Patch &p : ps.patches) std::memcpy(&h[static_cast<size_t>(p.offset)], p.bytes.data(), p.bytes.size());
    HeaderInfo i = parse_header(h.data(), h.size(), kDataOffset + 4000);
    CHECK(i.ok && i.consistent());
    CHECK_EQ(i.declared_data, static_cast<uint64_t>(4000));
    /* each patch is a single small write inside one 512-byte sector */
    for (const Patch &p : ps.patches) CHECK(p.offset / 512 == (p.offset + p.bytes.size() - 1) / 512);
}

static void rf64_boundary() {
    const uint64_t maxData = kRiffMax - (kDataOffset - 8);
    CHECK(!needs_rf64(kDataOffset, maxData));
    CHECK(needs_rf64(kDataOffset, maxData + 1));
    CHECK(!needs_rf64(kDataOffset, 0));
    CHECK(needs_rf64(kDataOffset, 5ull << 30));

    /* a header built for a mocked 5 GiB take is RF64 with ds64 */
    AudioFormat f;
    const uint64_t big = 5ull << 30;
    std::vector<unsigned char> h = build_header(f, BwfInfo{}, big);
    CHECK(std::memcmp(&h[0], "RF64", 4) == 0);
    CHECK(std::memcmp(&h[12], "ds64", 4) == 0);
    CHECK_EQ(rd32(h, 4), 0xFFFFFFFFu);
    CHECK_EQ(rd32(h, kDataOffset - 4), 0xFFFFFFFFu);
    HeaderInfo i = parse_header(h.data(), h.size(), kDataOffset + big);
    CHECK(i.ok && i.rf64 && i.has_ds64);
    CHECK_EQ(i.declared_data, big);
    CHECK_EQ(i.declared_riff, kDataOffset - 8 + big);
    CHECK(i.consistent() && !i.suspicious());
    /* just under the boundary stays a plain RIFF */
    h = build_header(f, BwfInfo{}, maxData);
    CHECK(std::memcmp(&h[0], "RIFF", 4) == 0);
    i = parse_header(h.data(), h.size(), kDataOffset + maxData);
    CHECK(i.ok && !i.rf64 && i.consistent());
    h = build_header(f, BwfInfo{}, maxData + 1);
    CHECK(std::memcmp(&h[0], "RF64", 4) == 0);

    /* the in-place switch: a plain header patched for 5 GiB becomes RF64 and parses */
    h = build_header(f, BwfInfo{});
    HeaderLayout l;
    PatchSet ps = size_patches(l, big);
    CHECK(ps.rf64 && !ps.clamped);
    CHECK(ps.patches.back().offset == 0); /* the RIFF tag goes last */
    for (const Patch &p : ps.patches) std::memcpy(&h[static_cast<size_t>(p.offset)], p.bytes.data(), p.bytes.size());
    i = parse_header(h.data(), h.size(), kDataOffset + big);
    CHECK(i.ok && i.rf64 && i.has_ds64 && i.consistent());
    CHECK_EQ(i.declared_data, big);
    /* every intermediate state of the switch parses as "not consistent" and not as a wrong size */
    std::vector<unsigned char> m = build_header(f, BwfInfo{});
    for (size_t k = 0; k + 1 < ps.patches.size(); k++) {
        const Patch &p = ps.patches[k];
        std::memcpy(&m[static_cast<size_t>(p.offset)], p.bytes.data(), p.bytes.size());
        HeaderInfo mi = parse_header(m.data(), m.size(), kDataOffset + big);
        CHECK(mi.ok && !mi.consistent());
    }
    /* a layout that already is RF64 stays RF64 even for a small size */
    l.rf64 = true;
    CHECK(size_patches(l, 100).rf64);
    /* a foreign file with no JUNK reserve: sizes clamp instead */
    HeaderLayout none;
    none.data_offset = 44;
    none.has_reserve = false;
    PatchSet c = size_patches(none, big);
    CHECK(c.clamped && !c.rf64);
}

void test_wav() {
    header_round_trip();
    other_formats();
    arecord_placeholder();
    oversized_and_garbage();
    patches();
    rf64_boundary();
}
