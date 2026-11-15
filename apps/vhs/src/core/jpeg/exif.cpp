/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the photo file and its EXIF segment (see exif.hpp). The TIFF / EXIF layout follows the EXIF 2.32 specification
 * (CIPA DC-008): a TIFF header ("II", 42, the offset of IFD0), each IFD a count, 12-byte entries in ascending tag order
 * and the offset of the next IFD (0); a value longer than 4 bytes lives after the IFD and the entry holds its offset.
 */
#include "exif.hpp"

#include "dht.hpp"

#include <cstring>
#include <ctime>

namespace vhs {
namespace {

constexpr std::uint16_t kAscii = 2, kShort = 3, kLong = 4;

struct Entry {
    std::uint16_t tag, type;
    std::uint32_t count;
    std::vector<std::uint8_t> data; /* the value's bytes (padded to 4 when shorter) */
};

void put16(std::vector<std::uint8_t> &v, std::size_t at, std::uint16_t x)
{
    v[at] = static_cast<std::uint8_t>(x & 0xFF);
    v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}

void put32(std::vector<std::uint8_t> &v, std::size_t at, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i) v[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF);
}

Entry ascii(std::uint16_t tag, const std::string &s)
{
    Entry e{tag, kAscii, static_cast<std::uint32_t>(s.size() + 1), {}};
    e.data.assign(s.begin(), s.end());
    e.data.push_back(0);
    return e;
}

Entry short1(std::uint16_t tag, std::uint16_t v)
{
    Entry e{tag, kShort, 1, std::vector<std::uint8_t>(4, 0)};
    put16(e.data, 0, v);
    return e;
}

Entry long1(std::uint16_t tag, std::uint32_t v)
{
    Entry e{tag, kLong, 1, std::vector<std::uint8_t>(4, 0)};
    put32(e.data, 0, v);
    return e;
}

/* Writes one IFD at tiff[at...]; returns the offset after it and its data. `next_ifd`: the next-IFD field's value.
 * patch: the index of an entry whose value is filled in later (the Exif IFD pointer), its value's offset returned. */
std::size_t write_ifd(std::vector<std::uint8_t> &tiff, std::size_t at, const std::vector<Entry> &entries, int patch,
                      std::size_t *patch_at)
{
    const std::size_t head = 2 + entries.size() * 12 + 4;
    std::size_t data_at = at + head;
    if (tiff.size() < data_at) tiff.resize(data_at, 0);
    put16(tiff, at, static_cast<std::uint16_t>(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry &e = entries[i];
        const std::size_t p = at + 2 + i * 12;
        put16(tiff, p, e.tag);
        put16(tiff, p + 2, e.type);
        put32(tiff, p + 4, e.count);
        if (e.data.size() <= 4) {
            for (std::size_t k = 0; k < e.data.size(); ++k) tiff[p + 8 + k] = e.data[k];
            if (static_cast<int>(i) == patch && patch_at) *patch_at = p + 8;
        } else {
            put32(tiff, p + 8, static_cast<std::uint32_t>(data_at));
            tiff.resize(data_at + e.data.size() + (e.data.size() & 1), 0); /* word-aligned */
            std::memcpy(tiff.data() + data_at, e.data.data(), e.data.size());
            data_at += e.data.size() + (e.data.size() & 1);
        }
    }
    put32(tiff, at + 2 + entries.size() * 12, 0); /* no next IFD */
    return data_at;
}

struct Reader {
    const std::uint8_t *t = nullptr;
    std::size_t n = 0;
    bool le = true;
    bool ok16(std::size_t at) const { return at + 2 <= n; }
    bool ok32(std::size_t at) const { return at + 4 <= n; }
    std::uint16_t u16(std::size_t at) const
    {
        return le ? static_cast<std::uint16_t>(t[at] | (t[at + 1] << 8)) : static_cast<std::uint16_t>((t[at] << 8) | t[at + 1]);
    }
    std::uint32_t u32(std::size_t at) const
    {
        if (le)
            return static_cast<std::uint32_t>(t[at]) | (static_cast<std::uint32_t>(t[at + 1]) << 8) |
                   (static_cast<std::uint32_t>(t[at + 2]) << 16) | (static_cast<std::uint32_t>(t[at + 3]) << 24);
        return (static_cast<std::uint32_t>(t[at]) << 24) | (static_cast<std::uint32_t>(t[at + 1]) << 16) |
               (static_cast<std::uint32_t>(t[at + 2]) << 8) | static_cast<std::uint32_t>(t[at + 3]);
    }
    /* the value of entry e as a string (ASCII) or a number (SHORT / LONG) */
    bool str(std::size_t e, std::string &out) const
    {
        const std::uint32_t count = u32(e + 4);
        if (u16(e + 2) != kAscii || count == 0 || count > 4096) return false;
        const std::size_t at = count <= 4 ? e + 8 : u32(e + 8);
        if (at + count > n) return false;
        out.assign(reinterpret_cast<const char *>(t + at), count);
        while (!out.empty() && out.back() == '\0') out.pop_back();
        return true;
    }
    bool num(std::size_t e, std::uint32_t &out) const
    {
        const std::uint16_t type = u16(e + 2);
        if (type == kShort) out = u16(e + 8);
        else if (type == kLong) out = u32(e + 8);
        else return false;
        return true;
    }
};

std::size_t seg_len(const std::uint8_t *p, std::size_t i)
{
    return (static_cast<std::size_t>(p[i + 2]) << 8) | p[i + 3];
}

} // namespace

std::vector<std::uint8_t> exif_app1(const ExifFields &f)
{
    std::vector<Entry> ifd0;
    if (!f.make.empty()) ifd0.push_back(ascii(0x010F, f.make));
    if (!f.model.empty()) ifd0.push_back(ascii(0x0110, f.model));
    ifd0.push_back(short1(0x0112, static_cast<std::uint16_t>(f.orientation)));
    ifd0.push_back(ascii(0x0131, f.software.empty() ? kSoftware : f.software));
    if (!f.datetime.empty()) ifd0.push_back(ascii(0x0132, f.datetime));
    ifd0.push_back(long1(0x8769, 0)); /* patched below */
    std::vector<Entry> exif;
    if (!f.datetime.empty()) exif.push_back(ascii(0x9003, f.datetime));
    exif.push_back(long1(0xA002, f.width));
    exif.push_back(long1(0xA003, f.height));

    std::vector<std::uint8_t> tiff(8, 0);
    tiff[0] = 'I';
    tiff[1] = 'I';
    put16(tiff, 2, 42);
    put32(tiff, 4, 8);
    std::size_t ptr_at = 0;
    const std::size_t exif_at = write_ifd(tiff, 8, ifd0, static_cast<int>(ifd0.size()) - 1, &ptr_at);
    put32(tiff, ptr_at, static_cast<std::uint32_t>(exif_at));
    write_ifd(tiff, exif_at, exif, -1, nullptr);

    std::vector<std::uint8_t> seg = {0xFF, 0xE1, 0, 0, 'E', 'x', 'i', 'f', 0, 0};
    seg.insert(seg.end(), tiff.begin(), tiff.end());
    const std::size_t len = seg.size() - 2;
    seg[2] = static_cast<std::uint8_t>(len >> 8);
    seg[3] = static_cast<std::uint8_t>(len & 0xFF);
    return seg;
}

bool exif_parse(const std::uint8_t *p, std::size_t n, ExifFields &out)
{
    out = ExifFields();
    if (!p || n < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
    std::size_t i = 2;
    while (i + 4 <= n && p[i] == 0xFF) {
        const std::uint8_t m = p[i + 1];
        if (m == 0xDA || m == 0xD9) break;
        const std::size_t len = seg_len(p, i);
        if (len < 2 || i + 2 + len > n) return false;
        if (m == 0xE1 && len >= 8 + 8 && std::memcmp(p + i + 4, "Exif\0\0", 6) == 0) {
            Reader r;
            r.t = p + i + 10;
            r.n = len - 8;
            if (r.t[0] == 'I' && r.t[1] == 'I') r.le = true;
            else if (r.t[0] == 'M' && r.t[1] == 'M') r.le = false;
            else return false;
            if (r.u16(2) != 42) return false;
            std::size_t ifd = r.u32(4);
            std::size_t exif_ifd = 0;
            for (int pass = 0; pass < 2 && ifd; ++pass) {
                if (!r.ok16(ifd)) return false;
                const std::uint16_t count = r.u16(ifd);
                if (ifd + 2 + static_cast<std::size_t>(count) * 12 > r.n) return false;
                for (std::uint16_t k = 0; k < count; ++k) {
                    const std::size_t e = ifd + 2 + static_cast<std::size_t>(k) * 12;
                    const std::uint16_t tag = r.u16(e);
                    std::uint32_t v = 0;
                    switch (tag) {
                    case 0x010F: r.str(e, out.make); break;
                    case 0x0110: r.str(e, out.model); break;
                    case 0x0112:
                        if (r.num(e, v)) {
                            out.orientation = static_cast<int>(v);
                            out.has_orientation = true;
                        }
                        break;
                    case 0x0131: r.str(e, out.software); break;
                    case 0x0132: r.str(e, out.datetime); break;
                    case 0x8769:
                        if (r.num(e, v)) exif_ifd = v;
                        break;
                    case 0x9003: r.str(e, out.datetime_original); break;
                    case 0xA002:
                        if (r.num(e, v)) out.width = v;
                        break;
                    case 0xA003:
                        if (r.num(e, v)) out.height = v;
                        break;
                    default: break;
                    }
                }
                if (pass == 0) {
                    out.has_exif_ifd = exif_ifd != 0;
                    ifd = exif_ifd;
                } else {
                    ifd = 0;
                }
            }
            return true;
        }
        i += 2 + len;
    }
    return false;
}

std::string exif_datetime(std::int64_t wall_s, bool utc)
{
    const std::time_t t = static_cast<std::time_t>(wall_s);
    struct tm tmv;
    if (!(utc ? gmtime_r(&t, &tmv) : localtime_r(&t, &tmv))) return "";
    char buf[32];
    if (std::strftime(buf, sizeof buf, "%Y:%m:%d %H:%M:%S", &tmv) != 19) return "";
    return buf;
}

std::vector<std::uint8_t> jpeg_markers(const std::uint8_t *p, std::size_t n)
{
    std::vector<std::uint8_t> out;
    if (!p || n < 2 || p[0] != 0xFF || p[1] != 0xD8) return out;
    out.push_back(0xD8);
    std::size_t i = 2;
    while (i + 4 <= n && p[i] == 0xFF) {
        const std::uint8_t m = p[i + 1];
        out.push_back(m);
        if (m == 0xDA || m == 0xD9) break;
        const std::size_t len = seg_len(p, i);
        if (len < 2) break;
        i += 2 + len;
    }
    return out;
}

bool make_photo(const std::uint8_t *frame, std::size_t n, ExifFields f, std::vector<std::uint8_t> &out, std::string *err,
                PhotoFacts *facts)
{
    PhotoFacts pf;
    std::vector<std::uint8_t> with_dht;
    if (!ensure_dht(frame, n, with_dht, &pf.dht_inserted)) {
        if (err) *err = "not a JPEG frame";
        return false;
    }
    /* the frame's size from its SOF; drop APP0 (JFIF) and an EXIF APP1 */
    const std::uint8_t *p = with_dht.data();
    const std::size_t m = with_dht.size();
    std::vector<std::uint8_t> body;
    body.reserve(m);
    std::size_t i = 2;
    int w = 0, h = 0;
    while (i + 4 <= m) {
        if (p[i] != 0xFF) {
            if (err) *err = "a broken JPEG segment";
            return false;
        }
        const std::uint8_t mk = p[i + 1];
        if (mk == 0xFF) {
            ++i;
            continue;
        }
        if (mk == 0xDA) {
            body.insert(body.end(), p + i, p + m); /* SOS and the entropy-coded data to EOI, unchanged */
            break;
        }
        const std::size_t len = seg_len(p, i);
        if (len < 2 || i + 2 + len > m) {
            if (err) *err = "a broken JPEG segment";
            return false;
        }
        const bool app0 = mk == 0xE0;
        const bool exif = mk == 0xE1 && len >= 8 && std::memcmp(p + i + 4, "Exif\0\0", 6) == 0;
        if ((mk == 0xC0 || mk == 0xC1 || mk == 0xC2) && len >= 7) {
            h = (p[i + 5] << 8) | p[i + 6];
            w = (p[i + 7] << 8) | p[i + 8];
        }
        if (app0) pf.app0_removed = true;
        if (!app0 && !exif) body.insert(body.end(), p + i, p + i + 2 + len);
        i += 2 + len;
    }
    if (w <= 0 || h <= 0) {
        if (err) *err = "no frame size (SOF) in the JPEG";
        return false;
    }
    pf.width = w;
    pf.height = h;
    if (f.width == 0) f.width = static_cast<std::uint32_t>(w);
    if (f.height == 0) f.height = static_cast<std::uint32_t>(h);
    const std::vector<std::uint8_t> app1 = exif_app1(f);
    out.clear();
    out.reserve(2 + app1.size() + body.size());
    out.push_back(0xFF);
    out.push_back(0xD8);
    out.insert(out.end(), app1.begin(), app1.end());
    out.insert(out.end(), body.begin(), body.end());
    if (facts) *facts = pf;
    return true;
}

} // namespace vhs
