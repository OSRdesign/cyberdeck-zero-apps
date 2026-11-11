/*
 * SPDX-License-Identifier: MIT
 */
#include "wav.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>

namespace cassette {
namespace {

using Bytes = std::vector<unsigned char>;

void put16(Bytes &b, size_t at, uint32_t v) {
    b[at] = static_cast<unsigned char>(v);
    b[at + 1] = static_cast<unsigned char>(v >> 8);
}
void put32(Bytes &b, size_t at, uint64_t v) {
    for (int i = 0; i < 4; i++) b[at + static_cast<size_t>(i)] = static_cast<unsigned char>(v >> (8 * i));
}
void put64(Bytes &b, size_t at, uint64_t v) {
    for (int i = 0; i < 8; i++) b[at + static_cast<size_t>(i)] = static_cast<unsigned char>(v >> (8 * i));
}
void putTag(Bytes &b, size_t at, const char *t) { std::memcpy(&b[at], t, 4); }
void putStr(Bytes &b, size_t at, const std::string &s, size_t maxlen) {
    std::memcpy(&b[at], s.data(), std::min(s.size(), maxlen));
}
uint32_t get16(const unsigned char *p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; }
uint64_t get32(const unsigned char *p) {
    return static_cast<uint64_t>(p[0]) | static_cast<uint64_t>(p[1]) << 8 | static_cast<uint64_t>(p[2]) << 16 |
           static_cast<uint64_t>(p[3]) << 24;
}
uint64_t get64(const unsigned char *p) { return get32(p) | get32(p + 4) << 32; }

std::string xml_escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

bool extensible(const AudioFormat &f) { return f.channels > 2; }

}  // namespace

uint32_t block_align(const AudioFormat &f) {
    if (f.channels == 0 || f.rate == 0) return 0;
    if (f.bits != 16 && f.bits != 24 && f.bits != 32) return 0;
    return static_cast<uint32_t>(f.channels) * (f.bits / 8u);
}

std::string coding_history_line(const AudioFormat &f) {
    const char *mode = f.channels == 1 ? "mono" : f.channels == 2 ? "stereo" : "multi";
    return "A=PCM,F=" + std::to_string(f.rate) + ",W=" + std::to_string(f.bits) + ",M=" + mode + ",T=Cassette\r\n";
}

bool needs_rf64(uint64_t data_offset, uint64_t data_bytes) { return data_offset - 8 + data_bytes > kRiffMax; }

bool is_placeholder_size(uint64_t v) { return v == 0xFFFFFFFFull || (v >= 0x7FFFFFF0ull && v <= 0x80000100ull); }

Bytes build_header(const AudioFormat &f, const BwfInfo &info, uint64_t data_bytes, bool rf64) {
    rf64 = rf64 || needs_rf64(kDataOffset, data_bytes);
    Bytes b(kDataOffset, 0);
    const uint32_t ba = block_align(f) ? block_align(f) : 1;
    const bool ext = extensible(f);
    const size_t fmtBody = ext ? 40 : 16;
    const uint64_t riff64 = kDataOffset - 8 + data_bytes;

    putTag(b, 0, rf64 ? "RF64" : "RIFF");
    put32(b, 4, rf64 ? 0xFFFFFFFFull : riff64);
    putTag(b, 8, "WAVE");
    /* the reserve at 12: ds64 when RF64, else a JUNK chunk of the same size */
    putTag(b, 12, rf64 ? "ds64" : "JUNK");
    put32(b, 16, 28);
    if (rf64) {
        put64(b, 20, riff64);
        put64(b, 28, data_bytes);
        put64(b, 36, data_bytes / ba);
        put32(b, 44, 0);
    }
    size_t p = 48;
    putTag(b, p, "fmt ");
    put32(b, p + 4, fmtBody);
    p += 8;
    put16(b, p, ext ? 0xFFFE : 1);
    put16(b, p + 2, f.channels);
    put32(b, p + 4, f.rate);
    put32(b, p + 8, static_cast<uint64_t>(f.rate) * ba);
    put16(b, p + 12, ba);
    put16(b, p + 14, f.bits);
    if (ext) {
        put16(b, p + 16, 22);
        put16(b, p + 18, f.bits);
        put32(b, p + 20, (1ull << f.channels) - 1); /* channel mask: the first channels */
        put16(b, p + 24, 1);                        /* PCM sub-format GUID 00000001-0000-0010-8000-00AA00389B71 */
        static const unsigned char guid[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        std::memcpy(&b[p + 26], guid, sizeof guid);
    }
    p += fmtBody;

    putTag(b, p, "bext");
    put32(b, p + 4, kBextBody);
    p += 8;
    putStr(b, p, info.description, 256);
    putStr(b, p + 256, info.originator.empty() ? "Cassette" : info.originator, 32);
    putStr(b, p + 288, info.originator_reference, 32);
    putStr(b, p + 320, info.origination_date, 10);
    putStr(b, p + 330, info.origination_time, 8);
    put32(b, p + 338, info.time_reference & 0xFFFFFFFFull);
    put32(b, p + 342, info.time_reference >> 32);
    put16(b, p + 346, 1); /* BWF version 1 */
    /* UMID (64) and the reserved 190 bytes stay zero; the coding history follows at 602 */
    putStr(b, p + 602, info.coding_history.empty() ? coding_history_line(f) : info.coding_history, 255);
    p += kBextBody;

    const size_t ixmlBody = kDataOffset - p - 8 - 8;
    putTag(b, p, "iXML");
    put32(b, p + 4, ixmlBody);
    p += 8;
    std::string x = info.ixml;
    if (x.empty())
        x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><BWFXML><IXML_VERSION>3.01</IXML_VERSION><NOTE>" +
            xml_escape(info.description) + "</NOTE></BWFXML>";
    if (x.size() > ixmlBody) x.resize(ixmlBody);
    std::memset(&b[p], ' ', ixmlBody); /* padded with blanks: still well-formed XML */
    std::memcpy(&b[p], x.data(), x.size());
    p += ixmlBody;

    putTag(b, p, "data");
    put32(b, p + 4, rf64 ? 0xFFFFFFFFull : data_bytes);
    return b;
}

PatchSet size_patches(const HeaderLayout &l, uint64_t data_bytes, bool force_rf64) {
    PatchSet ps;
    const uint64_t riff64 = l.data_offset - 8 + data_bytes;
    const bool want_rf64 = force_rf64 || l.rf64 || needs_rf64(l.data_offset, data_bytes);
    auto add = [&](uint64_t off, size_t n) -> Bytes & {
        ps.patches.push_back(Patch{off, Bytes(n, 0)});
        return ps.patches.back().bytes;
    };
    if (want_rf64 && l.has_reserve) {
        ps.rf64 = true;
        Bytes &ds = add(12, 36);
        putTag(ds, 0, "ds64");
        put32(ds, 4, 28);
        put64(ds, 8, riff64);
        put64(ds, 16, data_bytes);
        put64(ds, 24, data_bytes / (l.block_align ? l.block_align : 1));
        put32(ds, 32, 0);
        put32(add(l.data_offset - 4, 4), 0, 0xFFFFFFFFull);
        Bytes &r = add(0, 8);
        putTag(r, 0, "RF64");
        put32(r, 4, 0xFFFFFFFFull);
    } else if (want_rf64) {
        /* no room for ds64: readers treat 0xFFFFFFFF as "to the end of the file" */
        ps.clamped = true;
        put32(add(l.data_offset - 4, 4), 0, 0xFFFFFFFFull);
        put32(add(4, 4), 0, 0xFFFFFFFFull);
    } else {
        put32(add(l.data_offset - 4, 4), 0, data_bytes);
        put32(add(4, 4), 0, riff64);
    }
    return ps;
}

bool HeaderInfo::consistent() const {
    return ok && declared_data == actual_data && declared_riff == file_size - 8 && !(rf64 && !has_ds64);
}

HeaderLayout HeaderInfo::layout() const {
    HeaderLayout l;
    l.data_offset = data_offset;
    l.block_align = block_align ? block_align : 1;
    l.rf64 = rf64;
    l.has_reserve = has_reserve;
    return l;
}

HeaderInfo parse_header(const unsigned char *buf, size_t n, uint64_t file_size) {
    HeaderInfo h;
    h.file_size = file_size;
    auto fail = [&](const char *why) {
        h.error = why;
        return h;
    };
    if (n < 12) return fail("too short");
    const bool rf = std::memcmp(buf, "RF64", 4) == 0;
    if (!rf && std::memcmp(buf, "RIFF", 4) != 0) return fail("not a RIFF file");
    if (std::memcmp(buf + 8, "WAVE", 4) != 0) return fail("not a WAVE file");
    h.rf64 = rf;
    h.raw_riff = get32(buf + 4);
    h.declared_riff = h.raw_riff;
    uint64_t ds_riff = 0, ds_data = 0;
    bool haveFmt = false, haveData = false;
    size_t pos = 12;
    while (pos + 8 <= n) {
        const unsigned char *c = buf + pos;
        const uint64_t sz = get32(c + 4);
        const size_t body = pos + 8;
        if (std::memcmp(c, "data", 4) == 0) {
            h.data_offset = body;
            h.raw_data = sz;
            haveData = true;
            break;
        }
        if (std::memcmp(c, "fmt ", 4) == 0 && sz >= 16 && body + 16 <= n) {
            uint32_t tag = static_cast<uint32_t>(get16(buf + body));
            h.fmt.channels = static_cast<uint16_t>(get16(buf + body + 2));
            h.fmt.rate = static_cast<uint32_t>(get32(buf + body + 4));
            h.fmt.bits = static_cast<uint16_t>(get16(buf + body + 14));
            if (tag != 1 && tag != 0xFFFE) return fail("not PCM");
            h.block_align = block_align(h.fmt);
            if (!h.block_align) return fail("unsupported format");
            haveFmt = true;
        } else if (std::memcmp(c, "ds64", 4) == 0 && sz >= 24 && body + 24 <= n) {
            h.has_ds64 = true;
            ds_riff = get64(buf + body);
            ds_data = get64(buf + body + 8);
            if (pos == 12 && sz >= 28) h.has_reserve = true;
        } else if (std::memcmp(c, "JUNK", 4) == 0 && pos == 12 && sz >= 28) {
            h.has_reserve = true;
        } else if (std::memcmp(c, "bext", 4) == 0) {
            h.has_bext = true;
            h.bext_offset = body;
        } else if (std::memcmp(c, "iXML", 4) == 0) {
            h.has_ixml = true;
            h.ixml_offset = body;
            h.ixml_size = sz;
        }
        pos = body + static_cast<size_t>(sz) + (sz & 1);
    }
    if (!haveFmt) return fail("no fmt chunk");
    if (!haveData) return fail("no data chunk in the first bytes");
    if (h.data_offset > file_size) return fail("file shorter than its header");
    h.actual_data = file_size - h.data_offset;
    if (h.rf64 && h.has_ds64) {
        h.declared_riff = ds_riff;
        h.declared_data = h.raw_data == 0xFFFFFFFFull ? ds_data : h.raw_data;
    } else {
        h.declared_data = h.raw_data;
    }
    if (h.rf64 && !h.has_ds64) {
        h.placeholder_data = true; /* RF64 tag but nothing to say how big */
        h.placeholder_riff = true;
    } else {
        h.placeholder_riff = !h.rf64 && is_placeholder_size(h.raw_riff);
        h.placeholder_data = (!h.rf64 && (is_placeholder_size(h.raw_data) || (h.raw_data == 0 && h.actual_data > 0))) ||
                             (h.rf64 && h.declared_data == 0 && h.actual_data > 0);
    }
    h.oversized_riff = !h.placeholder_riff && h.declared_riff > file_size - 8;
    h.oversized_data = !h.placeholder_data && h.declared_data > h.actual_data;
    h.ok = true;
    return h;
}

HeaderInfo parse_header(FileOps &ops, const std::string &path) {
    HeaderInfo h;
    StatInfo st;
    int r = ops.stat(path, st);
    if (r < 0) {
        h.error = "cannot stat (" + std::to_string(-r) + ")";
        return h;
    }
    int fd = ops.open(path, O_RDONLY, 0);
    if (fd < 0) {
        h.error = "cannot open (" + std::to_string(-fd) + ")";
        return h;
    }
    std::vector<unsigned char> buf(static_cast<size_t>(std::min<uint64_t>(st.size, 65536)));
    size_t got = 0;
    while (got < buf.size()) {
        ssize_t k = ops.pread(fd, buf.data() + got, buf.size() - got, got);
        if (k <= 0) break;
        got += static_cast<size_t>(k);
    }
    ops.close(fd);
    return parse_header(buf.data(), got, st.size);
}

}  // namespace cassette
