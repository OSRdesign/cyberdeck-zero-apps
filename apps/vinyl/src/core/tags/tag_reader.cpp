/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tags: the parsers (see tag_reader.hpp). Written from the published format descriptions: ID3v2.3 / 2.4 (id3.org),
 * the FLAC format (xiph.org), the Ogg / Vorbis I / Ogg Opus specifications (RFC 3533, RFC 7845), the ISO base media file
 * format with the iTunes metadata item list, RIFF WAVE / AIFF.
 */
#include "tag_reader.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vinyl {

std::string TrackTags::year() const {
    std::string y;
    for (char c : date) {
        if (std::isdigit(static_cast<unsigned char>(c))) y += c;
        else if (!y.empty()) break;
        if (y.size() == 4) return y;
    }
    return y.size() == 4 ? y : "";
}

namespace {

uint32_t be32(const unsigned char *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint32_t be24(const unsigned char *p) { return (uint32_t(p[0]) << 16) | (p[1] << 8) | p[2]; }
uint16_t be16(const unsigned char *p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t le16(const unsigned char *p) { return uint16_t(p[0] | (p[1] << 8)); }
uint64_t be64(const unsigned char *p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
uint64_t le64(const unsigned char *p) { return le32(p) | (uint64_t(le32(p + 4)) << 32); }
uint32_t syncsafe(const unsigned char *p) {
    return (uint32_t(p[0] & 0x7F) << 21) | ((p[1] & 0x7F) << 14) | ((p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

class File {
public:
    explicit File(const std::string &path) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ >= 0) {
            struct stat st;
            if (::fstat(fd_, &st) == 0) size_ = static_cast<uint64_t>(st.st_size);
        }
    }
    ~File() {
        if (fd_ >= 0) ::close(fd_);
    }
    bool ok() const { return fd_ >= 0; }
    uint64_t size() const { return size_; }
    uint64_t bytes_read() const { return bytes_read_; }
    /* exactly n bytes at off, or false */
    bool read(uint64_t off, void *buf, size_t n) const {
        if (off + n > size_) return false;
        size_t got = 0;
        while (got < n) {
            const ssize_t k = ::pread(fd_, static_cast<char *>(buf) + got, n - got, static_cast<off_t>(off + got));
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) return false;
            got += static_cast<size_t>(k);
            bytes_read_ += static_cast<uint64_t>(k);
        }
        return true;
    }
    bool read(uint64_t off, std::vector<unsigned char> &v, size_t n) const {
        v.resize(n);
        return n == 0 || read(off, v.data(), n);
    }

private:
    int fd_ = -1;
    uint64_t size_ = 0;
    mutable uint64_t bytes_read_ = 0;
};

std::string lower_ext(const std::string &path) {
    const size_t dot = path.rfind('.');
    const size_t slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot + 1);
    for (char &c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (static_cast<unsigned char>(s[a]) <= ' ')) ++a;
    while (b > a && (static_cast<unsigned char>(s[b - 1]) <= ' ')) --b;
    return s.substr(a, b - a);
}

int leading_int(const std::string &s) {
    int v = 0;
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    bool any = false;
    for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && v < 100000; ++i) {
        v = v * 10 + (s[i] - '0');
        any = true;
    }
    return any ? v : 0;
}

void put_utf8(std::string &o, uint32_t cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/* Sets a field when it is still empty (the first value wins: a later duplicate frame does not overwrite). */
void set_once(std::string &field, const std::string &v) {
    if (field.empty()) field = trim(clean_utf8(v));
}

/* A Vorbis-comment / MP4 / INFO field by its name, into the tags. */
void apply_named(TrackTags &t, const std::string &name_in, const std::string &v) {
    const std::string name = upper(name_in);
    if (apply_replaygain_tag(t.rg, name, v)) return; /* REPLAYGAIN_*, R128_*_GAIN */
    if (name == "TITLE") set_once(t.title, v);
    else if (name == "ARTIST") set_once(t.artist, v);
    else if (name == "ALBUM") set_once(t.album, v);
    else if (name == "ALBUMARTIST" || name == "ALBUM ARTIST" || name == "ALBUM_ARTIST") set_once(t.album_artist, v);
    else if (name == "DATE" || name == "YEAR") set_once(t.date, v);
    else if (name == "GENRE") set_once(t.genre, v);
    else if (name == "TRACKNUMBER" && !t.track) t.track = leading_int(v);
    else if (name == "DISCNUMBER" && !t.disc) t.disc = leading_int(v);
}

/* ------------------------------------------------------------------------------------------------ ID3v2 */

/* An ID3 text frame body (encoding byte first) as UTF-8; the first of several NUL-separated values. */
std::string id3_text(const unsigned char *p, size_t n) {
    if (n < 1) return "";
    const unsigned enc = p[0];
    ++p;
    --n;
    if (enc == 1 || enc == 2) {
        size_t end = 0;
        while (end + 1 < n && !(p[end] == 0 && p[end + 1] == 0)) end += 2;
        return utf16_to_utf8(p, std::min(end, n & ~size_t(1)), enc == 1, enc == 2);
    }
    size_t end = 0;
    while (end < n && p[end]) ++end;
    if (enc == 3) return std::string(reinterpret_cast<const char *>(p), end);
    return latin1_to_utf8(p, end);
}

/* Skips a NUL-terminated string in the given encoding: the offset after the terminator. */
size_t id3_skip_string(const unsigned char *p, size_t n, unsigned enc) {
    if (enc == 1 || enc == 2) {
        size_t i = 0;
        while (i + 1 < n && !(p[i] == 0 && p[i + 1] == 0)) i += 2;
        return std::min(n, i + 2);
    }
    size_t i = 0;
    while (i < n && p[i]) ++i;
    return std::min(n, i + 1);
}

void id3_picture(const unsigned char *p, size_t n, bool v22, Picture *pic, size_t max_pic, TrackTags &t) {
    if (n < 4) return;
    t.has_picture = true;
    if (!pic) return;
    const unsigned enc = p[0];
    size_t q = 1;
    std::string mime;
    if (v22) {
        if (n < 5) return;
        const std::string f(reinterpret_cast<const char *>(p + 1), 3);
        mime = upper(f) == "PNG" ? "image/png" : "image/jpeg";
        q = 4;
    } else {
        while (q < n && p[q]) mime += static_cast<char>(p[q++]);
        ++q;
    }
    if (q >= n) return;
    const int type = p[q++];
    q += id3_skip_string(p + q, n - q, enc);
    if (q >= n) return;
    const size_t len = n - q;
    if (len > max_pic) return;
    /* the front cover wins over another picture already taken */
    if (!pic->data.empty() && !(type == 3 && pic->type != 3)) return;
    pic->mime = mime;
    pic->type = type;
    pic->data.assign(p + q, p + n);
}

/* Undoes the unsynchronisation scheme (0xFF 0x00 -> 0xFF). */
std::vector<unsigned char> unsync(const unsigned char *p, size_t n) {
    std::vector<unsigned char> o;
    o.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        o.push_back(p[i]);
        if (p[i] == 0xFF && i + 1 < n && p[i + 1] == 0x00) ++i;
    }
    return o;
}

/* TXXX / TXX: encoding, a description, a value; the ReplayGain ones go into t.rg */
void id3_txxx(const unsigned char *b, size_t n, TrackTags &t) {
    if (n < 2) return;
    const unsigned enc = b[0];
    const size_t desc_end = 1 + id3_skip_string(b + 1, n - 1, enc);
    const std::string desc = id3_text(b, desc_end);
    if (desc_end >= n) return;
    std::vector<unsigned char> val;
    val.reserve(n - desc_end + 1);
    val.push_back(static_cast<unsigned char>(enc));
    val.insert(val.end(), b + desc_end, b + n);
    apply_replaygain_tag(t.rg, desc, id3_text(val.data(), val.size()));
}

void id3_frame(const std::string &id, const unsigned char *b, size_t n, bool v22, TrackTags &t, Picture *pic,
               size_t max_pic) {
    if (id == "TXXX" || id == "TXX") id3_txxx(b, n, t);
    else if (id == "TIT2" || id == "TT2") set_once(t.title, id3_text(b, n));
    else if (id == "TPE1" || id == "TP1") set_once(t.artist, id3_text(b, n));
    else if (id == "TALB" || id == "TAL") set_once(t.album, id3_text(b, n));
    else if (id == "TPE2" || id == "TP2") set_once(t.album_artist, id3_text(b, n));
    else if (id == "TDRC" || id == "TYER" || id == "TYE" || id == "TDOR") set_once(t.date, id3_text(b, n));
    else if (id == "TCON" || id == "TCO") set_once(t.genre, id3_text(b, n));
    else if ((id == "TRCK" || id == "TRK") && !t.track) t.track = leading_int(id3_text(b, n));
    else if ((id == "TPOS" || id == "TPA") && !t.disc) t.disc = leading_int(id3_text(b, n));
    else if (id == "APIC" || id == "PIC") id3_picture(b, n, v22, pic, max_pic, t);
}

/* A frame's body as stored (v2.4 frame flags: a data length indicator, per-frame unsynchronisation; compressed or
 * encrypted frames are skipped) into the tags. */
void id3_frame_raw(const std::string &id, const unsigned char *fb, size_t fn, unsigned ver, unsigned fflags, TrackTags &t,
                   Picture *pic, size_t max_pic) {
    std::vector<unsigned char> tmp;
    if (ver == 4) {
        if (fflags & 0x0001) { /* data length indicator */
            if (fn < 4) return;
            fb += 4;
            fn -= 4;
        }
        if (fflags & 0x0002) { /* frame unsynchronised */
            tmp = unsync(fb, fn);
            fb = tmp.data();
            fn = tmp.size();
        }
        if (fflags & 0x000C) return; /* compressed or encrypted */
    } else if (ver == 3 && (fflags & 0x00C0)) {
        return;
    }
    id3_frame(id, fb, fn, ver == 2, t, pic, max_pic);
}

bool id3_frame_id_ok(const unsigned char *f, size_t len) {
    for (size_t i = 0; i < len; ++i)
        if (!(std::isupper(f[i]) || std::isdigit(f[i]))) return false;
    return true;
}

}  // namespace

size_t parse_id3v2(const unsigned char *p, size_t n, TrackTags &t, Picture *pic, size_t max_pic) {
    if (n < 10 || std::memcmp(p, "ID3", 3) != 0) return 0;
    const unsigned ver = p[3], flags = p[5];
    if (ver < 2 || ver > 4) return 0;
    const size_t tag_size = syncsafe(p + 6);
    const size_t total = 10 + tag_size + ((flags & 0x10) ? 10 : 0);
    const size_t body_n = std::min(tag_size, n - 10);
    std::vector<unsigned char> body;
    if ((flags & 0x80) && ver < 4) body = unsync(p + 10, body_n);
    else body.assign(p + 10, p + 10 + body_n);
    size_t q = 0;
    if ((flags & 0x40) && ver >= 3 && body.size() >= 4) { /* extended header */
        const size_t ext = ver == 4 ? syncsafe(body.data()) : be32(body.data()) + 4;
        q = std::min(body.size(), ext);
    }
    const bool v22 = ver == 2;
    const size_t hdr = v22 ? 6 : 10;
    while (q + hdr <= body.size()) {
        const unsigned char *f = body.data() + q;
        if (f[0] == 0) break; /* padding */
        if (!id3_frame_id_ok(f, v22 ? 3 : 4)) return total;
        const std::string id(reinterpret_cast<const char *>(f), v22 ? 3 : 4);
        size_t fsize = v22 ? be24(f + 3) : (ver == 4 ? syncsafe(f + 4) : be32(f + 4));
        unsigned fflags = v22 ? 0 : be16(f + 8);
        q += hdr;
        if (fsize > body.size() - q) break;
        id3_frame_raw(id, body.data() + q, fsize, ver, fflags, t, pic, max_pic);
        q += fsize;
    }
    return total;
}

std::string latin1_to_utf8(const unsigned char *p, size_t n) {
    std::string o;
    o.reserve(n);
    for (size_t i = 0; i < n; ++i) put_utf8(o, p[i]);
    return o;
}

std::string utf16_to_utf8(const unsigned char *p, size_t n, bool bom, bool big_endian) {
    bool be = big_endian;
    size_t i = 0;
    if (bom && n >= 2) {
        if (p[0] == 0xFF && p[1] == 0xFE) {
            be = false;
            i = 2;
        } else if (p[0] == 0xFE && p[1] == 0xFF) {
            be = true;
            i = 2;
        } else {
            be = false; /* no mark: little endian, as the common writers do */
        }
    }
    std::string o;
    while (i + 1 < n) {
        uint32_t u = be ? (uint32_t(p[i]) << 8 | p[i + 1]) : (uint32_t(p[i + 1]) << 8 | p[i]);
        i += 2;
        if (u == 0) break;
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < n) {
            const uint32_t lo = be ? (uint32_t(p[i]) << 8 | p[i + 1]) : (uint32_t(p[i + 1]) << 8 | p[i]);
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (u >= 0xD800 && u < 0xE000) continue; /* a lone surrogate */
        put_utf8(o, u);
    }
    return o;
}

std::string clean_utf8(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned c = p[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && i + len <= n;
        for (size_t k = 1; ok && k < len; ++k) ok = (p[i + k] & 0xC0) == 0x80;
        if (ok && len > 1) {
            uint32_t cp = len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
            for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (p[i + k] & 0x3F);
            static const uint32_t mins[] = {0, 0, 0x80, 0x800, 0x10000};
            ok = cp >= mins[len] && cp <= 0x10FFFF && !(cp >= 0xD800 && cp < 0xE000);
        }
        if (!ok) { /* Latin-1 reading of a stray byte */
            put_utf8(o, c);
            ++i;
            continue;
        }
        if (len == 1 && (c < 0x20 || c == 0x7F)) {
            if (c == '\t' || c == '\n' || c == '\r') o += ' ';
            ++i;
            continue;
        }
        o.append(s, i, len);
        i += len;
    }
    return o;
}

std::vector<unsigned char> base64_decode(const std::string &s) {
    static int8_t map[256];
    static bool ready = false;
    if (!ready) {
        std::memset(map, -1, sizeof map);
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) map[static_cast<unsigned char>(a[i])] = static_cast<int8_t>(i);
        map['-'] = 62; /* URL-safe variants */
        map['_'] = 63;
        ready = true;
    }
    std::vector<unsigned char> o;
    o.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : s) {
        if (c == '=') break;
        const int v = map[c];
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF));
        }
    }
    return o;
}

bool parse_flac_picture(const unsigned char *p, size_t n, Picture &pic, size_t max_pic) {
    if (n < 32) return false;
    size_t q = 0;
    const uint32_t type = be32(p);
    q += 4;
    const uint32_t ml = be32(p + q);
    q += 4;
    if (ml > n - q) return false;
    const std::string mime(reinterpret_cast<const char *>(p + q), ml);
    q += ml;
    if (n - q < 4) return false;
    const uint32_t dl = be32(p + q);
    q += 4;
    if (dl > n - q) return false;
    q += dl;
    if (n - q < 20) return false;
    q += 16; /* width, height, depth, colours */
    const uint32_t len = be32(p + q);
    q += 4;
    if (len > n - q || len > max_pic) return false;
    pic.type = static_cast<int>(type);
    pic.mime = mime;
    pic.data.assign(p + q, p + q + len);
    return true;
}

void parse_vorbis_comments(const unsigned char *p, size_t n, TrackTags &t, Picture *pic, size_t max_pic) {
    if (n < 8) return;
    size_t q = 0;
    const uint32_t vl = le32(p);
    q += 4;
    if (vl > n - q) return;
    q += vl;
    if (n - q < 4) return;
    const uint32_t count = le32(p + q);
    q += 4;
    for (uint32_t i = 0; i < count && n - q >= 4; ++i) {
        const uint32_t l = le32(p + q);
        q += 4;
        if (l > n - q) break;
        const char *s = reinterpret_cast<const char *>(p + q);
        const char *eq = static_cast<const char *>(std::memchr(s, '=', l));
        if (eq && eq > s) {
            const std::string name(s, static_cast<size_t>(eq - s));
            const size_t vlen = l - static_cast<size_t>(eq - s) - 1;
            const std::string uname = upper(name);
            if (uname == "METADATA_BLOCK_PICTURE") {
                t.has_picture = true;
                if (pic && vlen <= max_pic * 4 / 3 + 8) {
                    const std::vector<unsigned char> raw = base64_decode(std::string(eq + 1, vlen));
                    Picture cand;
                    if (parse_flac_picture(raw.data(), raw.size(), cand, max_pic) &&
                        (pic->data.empty() || (cand.type == 3 && pic->type != 3)))
                        *pic = std::move(cand);
                }
            } else if (vlen < 4096) {
                apply_named(t, name, std::string(eq + 1, vlen));
            }
        }
        q += l;
    }
}

std::string title_from_path(const std::string &path) {
    const size_t slash = path.rfind('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return clean_utf8(name);
}

std::string artist_title(const TrackTags &t, const std::string &path) {
    const std::string title = t.title.empty() ? title_from_path(path) : t.title;
    if (t.artist.empty()) return title;
    return t.artist + ", " + title;
}

/* ------------------------------------------------------------------------------------------------ per container */

namespace {

constexpr size_t kMaxTagBytes = 24u << 20; /* a tag (with its pictures) larger than this is not read */

/* MPEG audio frame header facts */
struct MpegFrame {
    bool ok = false;
    int version = 1;  /* 1, 2, 25 (2.5) */
    int layer = 3;
    unsigned bitrate = 0, rate = 0, channels = 2, size = 0, samples = 1152;
};

MpegFrame mpeg_frame(const unsigned char *p) {
    MpegFrame f;
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return f;
    const unsigned vb = (p[1] >> 3) & 3, lb = (p[1] >> 1) & 3, bi = p[2] >> 4, ri = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (vb == 1 || lb == 0 || bi == 0 || bi == 15 || ri == 3) return f;
    f.version = vb == 3 ? 1 : vb == 2 ? 2 : 25;
    f.layer = 4 - static_cast<int>(lb);
    static const unsigned br[2][3][15] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
    static const unsigned rates[3] = {44100, 48000, 32000};
    f.bitrate = br[f.version == 1 ? 0 : 1][f.layer - 1][bi];
    f.rate = rates[ri] / (f.version == 1 ? 1 : f.version == 2 ? 2 : 4);
    f.channels = ((p[3] >> 6) & 3) == 3 ? 1 : 2;
    f.samples = f.layer == 1 ? 384 : (f.layer == 3 && f.version != 1) ? 576 : 1152;
    if (f.layer == 1) f.size = (12 * f.bitrate * 1000 / f.rate + pad) * 4;
    else f.size = f.samples / 8 * f.bitrate * 1000 / f.rate + pad;
    f.ok = f.size > 4;
    return f;
}

constexpr size_t kTextFrameBytes = 64u << 10;   /* a text frame (TIT2, TXXX...) larger than this is not read */
constexpr size_t kUnsyncedTagBytes = 256u << 10; /* a tag unsynchronised as a whole: what is read without a picture asked */

/* The ID3v2 tag at the start of the file, walked frame by frame in the file (report 060 finding 3): each frame header is
 * read, then only the bodies wanted (text frames up to 64 kB, a picture only when `pic` asks for one and it fits), so a
 * cover is skipped by its size. A tag unsynchronised as a whole (v2.2 / v2.3 flag) cannot be walked in the file: it is
 * read whole when a picture is asked for (up to kMaxTagBytes), else its first 256 kB (the text frames come first in
 * practice). Returns the bytes the tag occupies (0: none). */
size_t read_id3v2_file(const File &f, TrackTags &t, Picture *pic, size_t max_pic) {
    unsigned char h[10];
    if (!f.read(0, h, 10) || std::memcmp(h, "ID3", 3) != 0) return 0;
    const unsigned ver = h[3], flags = h[5];
    if (ver < 2 || ver > 4) return 0;
    const uint64_t tag_size = syncsafe(h + 6);
    const size_t total = 10 + tag_size + ((flags & 0x10) ? 10 : 0);
    const uint64_t end = std::min<uint64_t>(10 + tag_size, f.size());
    if ((flags & 0x80) && ver < 4) {
        const uint64_t want = std::min<uint64_t>(total, pic ? kMaxTagBytes : kUnsyncedTagBytes);
        std::vector<unsigned char> tag;
        if (f.read(0, tag, static_cast<size_t>(std::min<uint64_t>(want, f.size())))) parse_id3v2(tag.data(), tag.size(), t, pic, max_pic);
        return total;
    }
    uint64_t pos = 10;
    if ((flags & 0x40) && ver >= 3) { /* extended header */
        unsigned char e[4];
        if (!f.read(pos, e, 4)) return total;
        pos += ver == 4 ? syncsafe(e) : be32(e) + 4;
    }
    const bool v22 = ver == 2;
    const size_t hdr = v22 ? 6 : 10;
    std::vector<unsigned char> body;
    for (int frames = 0; frames < 4096 && pos + hdr <= end; ++frames) {
        unsigned char fh[10];
        if (!f.read(pos, fh, hdr) || fh[0] == 0) break; /* padding */
        if (!id3_frame_id_ok(fh, v22 ? 3 : 4)) break;
        const std::string id(reinterpret_cast<const char *>(fh), v22 ? 3 : 4);
        const uint64_t fsize = v22 ? be24(fh + 3) : (ver == 4 ? syncsafe(fh + 4) : be32(fh + 4));
        const unsigned fflags = v22 ? 0 : be16(fh + 8);
        pos += hdr;
        if (fsize > end - pos) break;
        const bool picture = id == "APIC" || id == "PIC";
        bool wanted = false;
        if (picture) {
            t.has_picture = true;
            wanted = pic != nullptr && fsize <= max_pic + 1024; /* the picture's own header comes first */
        } else {
            wanted = fh[0] == 'T' && fsize <= kTextFrameBytes; /* the text frames and TXXX */
        }
        if (wanted && f.read(pos, body, static_cast<size_t>(fsize)))
            id3_frame_raw(id, body.data(), body.size(), ver, fflags, t, pic, max_pic);
        pos += fsize;
    }
    return total;
}

void read_mp3(const File &f, TrackTags &t, Picture *pic, size_t max_pic) {
    t.codec = "MP3";
    uint64_t audio_start = 0, audio_end = f.size();
    audio_start = read_id3v2_file(f, t, pic, max_pic);
    unsigned char v1[128];
    if (f.size() >= 128 && f.read(f.size() - 128, v1, 128) && std::memcmp(v1, "TAG", 3) == 0) {
        audio_end -= 128;
        auto field = [&](int off, int len) {
            int e = len;
            while (e > 0 && (v1[off + e - 1] == 0 || v1[off + e - 1] == ' ')) --e;
            return latin1_to_utf8(v1 + off, static_cast<size_t>(e));
        };
        set_once(t.title, field(3, 30));
        set_once(t.artist, field(33, 30));
        set_once(t.album, field(63, 30));
        set_once(t.date, field(93, 4));
        if (!t.track && v1[125] == 0 && v1[126]) t.track = v1[126];
    }
    /* the first frame (skip zero padding and junk up to 64 kB: 4 kB first, which holds it in nearly every file) */
    std::vector<unsigned char> buf;
    const uint64_t room = audio_end > audio_start ? audio_end - audio_start : 0;
    for (const size_t step : {size_t(4096), size_t(64 * 1024)}) {
        const size_t scan = static_cast<size_t>(std::min<uint64_t>(step, room));
        if (scan < 4 || !f.read(audio_start, buf, scan)) return;
        bool found = false;
        for (size_t i = 0; i + 4 <= buf.size() && !found; ++i) {
            const MpegFrame fr = mpeg_frame(buf.data() + i);
            /* a frame confirmed by the next header in the buffer (the loop below takes the same one) */
            found = fr.ok && i + fr.size + 4 <= buf.size() && mpeg_frame(buf.data() + i + fr.size).ok;
        }
        if (found || scan == room) break;
    }
    for (size_t i = 0; i + 4 <= buf.size(); ++i) {
        MpegFrame fr = mpeg_frame(buf.data() + i);
        if (!fr.ok) continue;
        /* a second header right after this frame confirms the sync (not a false 0xFF in junk) */
        if (i + fr.size + 4 <= buf.size() && !mpeg_frame(buf.data() + i + fr.size).ok) continue;
        t.ok = true;
        t.rate = fr.rate;
        t.channels = fr.channels;
        const size_t side = fr.version == 1 ? (fr.channels == 1 ? 17 : 32) : (fr.channels == 1 ? 9 : 17);
        const size_t x = i + 4 + side;
        uint64_t frames = 0, bytes = 0;
        if (x + 16 <= buf.size() && (std::memcmp(buf.data() + x, "Xing", 4) == 0 || std::memcmp(buf.data() + x, "Info", 4) == 0)) {
            const uint32_t flags = be32(buf.data() + x + 4);
            size_t q = x + 8;
            if ((flags & 1) && q + 4 <= buf.size()) {
                frames = be32(buf.data() + q);
                q += 4;
            }
            if ((flags & 2) && q + 4 <= buf.size()) bytes = be32(buf.data() + q);
        } else if (i + 36 + 18 <= buf.size() && std::memcmp(buf.data() + i + 36, "VBRI", 4) == 0) {
            bytes = be32(buf.data() + i + 36 + 10);
            frames = be32(buf.data() + i + 36 + 14);
        }
        if (frames) {
            t.duration_ms = frames * fr.samples * 1000ull / fr.rate;
            const uint64_t ab = bytes ? bytes : (audio_end - audio_start - i);
            if (t.duration_ms) t.bitrate_kbps = static_cast<unsigned>(ab * 8 / t.duration_ms);
        } else if (fr.bitrate) {
            const uint64_t ab = audio_end - audio_start - i;
            t.duration_ms = ab * 8 / fr.bitrate;
            t.bitrate_kbps = fr.bitrate;
        }
        return;
    }
}

void read_flac(const File &f, TrackTags &t, Picture *pic, size_t max_pic, uint64_t start) {
    t.codec = "FLAC";
    uint64_t pos = start + 4;
    unsigned char h[4];
    for (int blocks = 0; blocks < 256; ++blocks) {
        if (!f.read(pos, h, 4)) return;
        const bool last = (h[0] & 0x80) != 0;
        const unsigned type = h[0] & 0x7F;
        const uint32_t len = be24(h + 1);
        if (type == 0 && len >= 18) {
            unsigned char s[18];
            if (!f.read(pos + 4, s, 18)) return;
            t.rate = (uint32_t(s[10]) << 12) | (s[11] << 4) | (s[12] >> 4);
            t.channels = ((s[12] >> 1) & 7) + 1;
            t.bits = (((s[12] & 1) << 4) | (s[13] >> 4)) + 1;
            const uint64_t total = (uint64_t(s[13] & 0x0F) << 32) | be32(s + 14);
            if (t.rate) t.duration_ms = total * 1000 / t.rate;
            t.ok = true;
        } else if (type == 4 && len <= kMaxTagBytes) {
            std::vector<unsigned char> b;
            if (f.read(pos + 4, b, len)) parse_vorbis_comments(b.data(), b.size(), t, pic, max_pic);
        } else if (type == 6) {
            t.has_picture = true;
            if (pic && len <= max_pic + 4096) {
                std::vector<unsigned char> b;
                Picture cand;
                if (f.read(pos + 4, b, len) && parse_flac_picture(b.data(), b.size(), cand, max_pic) &&
                    (pic->data.empty() || (cand.type == 3 && pic->type != 3)))
                    *pic = std::move(cand);
            }
        }
        pos += 4 + len;
        if (last) break;
    }
    if (t.duration_ms && f.size() > pos) t.bitrate_kbps = static_cast<unsigned>((f.size() - pos) * 8 / t.duration_ms);
}

/* Ogg: the first logical stream's packets, reassembled across pages (at most max_packets, max_bytes each). */
struct OggReader {
    const File &f;
    uint64_t pos = 0;
    uint32_t serial = 0;
    bool have_serial = false;
    explicit OggReader(const File &file) : f(file) {}
    /* the next packet of the stream; false at the end or on damage */
    bool next(std::vector<unsigned char> &out, size_t max_bytes) {
        out.clear();
        while (true) {
            unsigned char h[27];
            if (!f.read(pos, h, 27) || std::memcmp(h, "OggS", 4) != 0) return false;
            const unsigned nseg = h[26];
            unsigned char lace[255];
            if (!f.read(pos + 27, lace, nseg)) return false;
            const uint32_t ser = le32(h + 14);
            uint64_t body = pos + 27 + nseg;
            size_t total = 0;
            for (unsigned i = 0; i < nseg; ++i) total += lace[i];
            if (!have_serial) {
                serial = ser;
                have_serial = true;
            }
            if (ser != serial) {
                pos = body + total;
                continue;
            }
            /* walk the segments from where the previous call stopped in this page */
            size_t seg_off = 0;
            for (unsigned i = 0; i < seg_start_; ++i) seg_off += lace[i];
            for (unsigned i = seg_start_; i < nseg; ++i) {
                const unsigned l = lace[i];
                if (out.size() + l > max_bytes) return false;
                const size_t at = out.size();
                out.resize(at + l);
                if (l && !f.read(body + seg_off, out.data() + at, l)) return false;
                seg_off += l;
                if (l < 255) { /* the packet ends here */
                    seg_start_ = i + 1;
                    if (seg_start_ >= nseg) {
                        seg_start_ = 0;
                        pos = body + total;
                    }
                    return true;
                }
            }
            seg_start_ = 0;
            pos = body + total; /* the packet continues on the next page */
        }
    }
    unsigned seg_start_ = 0;
};

/* The granule position of the last page of the stream (scanning the last 64 kB). */
int64_t ogg_last_granule(const File &f, uint32_t serial) {
    const uint64_t span = std::min<uint64_t>(f.size(), 64 * 1024);
    std::vector<unsigned char> b;
    if (!f.read(f.size() - span, b, static_cast<size_t>(span))) return -1;
    int64_t g = -1;
    for (size_t i = 0; i + 27 <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, "OggS", 4) != 0) continue;
        if (le32(b.data() + i + 14) != serial) continue;
        const int64_t gp = static_cast<int64_t>(le64(b.data() + i + 6));
        if (gp >= 0) g = gp;
    }
    return g;
}

void read_ogg(const File &f, TrackTags &t, Picture *pic, size_t max_pic) {
    OggReader r(f);
    std::vector<unsigned char> p;
    if (!r.next(p, 1 << 16)) return;
    bool opus = false;
    uint32_t preskip = 0;
    if (p.size() >= 30 && p[0] == 1 && std::memcmp(p.data() + 1, "vorbis", 6) == 0) {
        t.codec = "Vorbis";
        t.channels = p[11];
        t.rate = le32(p.data() + 12);
        const int32_t nominal = static_cast<int32_t>(le32(p.data() + 20));
        if (nominal > 0) t.bitrate_kbps = static_cast<unsigned>(nominal / 1000);
    } else if (p.size() >= 19 && std::memcmp(p.data(), "OpusHead", 8) == 0) {
        t.codec = "Opus";
        opus = true;
        t.channels = p[9];
        preskip = le16(p.data() + 10);
        t.rate = 48000;
    } else if (p.size() >= 13 && p[0] == 0x7F && std::memcmp(p.data() + 1, "FLAC", 4) == 0) {
        t.codec = "FLAC";
        return;
    } else {
        t.codec = "Ogg";
        return;
    }
    t.ok = true;
    if (r.next(p, kMaxTagBytes)) {
        if (!opus && p.size() > 7 && p[0] == 3 && std::memcmp(p.data() + 1, "vorbis", 6) == 0)
            parse_vorbis_comments(p.data() + 7, p.size() - 7, t, pic, max_pic);
        else if (opus && p.size() > 8 && std::memcmp(p.data(), "OpusTags", 8) == 0)
            parse_vorbis_comments(p.data() + 8, p.size() - 8, t, pic, max_pic);
    }
    const int64_t g = ogg_last_granule(f, r.serial);
    if (g > 0 && t.rate) {
        const int64_t samples = opus ? std::max<int64_t>(0, g - preskip) : g;
        t.duration_ms = static_cast<uint64_t>(samples) * 1000 / (opus ? 48000 : t.rate);
        if (t.duration_ms && (opus || !t.bitrate_kbps)) t.bitrate_kbps = static_cast<unsigned>(f.size() * 8 / t.duration_ms);
    }
}

/* ------------------------------------------------------------------------------------------------ MP4 */

struct Mp4 {
    const File &f;
    TrackTags &t;
    Picture *pic;
    size_t max_pic;
    int depth = 0;

    /* walks the boxes in [start, end) */
    void walk(uint64_t start, uint64_t end, const std::string &parent) {
        if (++depth > 12) {
            --depth;
            return;
        }
        uint64_t pos = start;
        while (pos + 8 <= end) {
            unsigned char h[16];
            if (!f.read(pos, h, 8)) break;
            uint64_t size = be32(h);
            const std::string type(reinterpret_cast<const char *>(h + 4), 4);
            uint64_t hdr = 8;
            if (size == 1) {
                if (!f.read(pos + 8, h + 8, 8)) break;
                size = be64(h + 8);
                hdr = 16;
            } else if (size == 0) {
                size = end - pos;
            }
            if (size < hdr || pos + size > end) break;
            const uint64_t body = pos + hdr, blen = size - hdr;
            if (type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "stbl" || type == "udta" ||
                type == "ilst") {
                walk(body, body + blen, type);
            } else if (type == "meta") {
                walk(body + 4, body + blen, type); /* a full box: version and flags first */
            } else if (type == "mvhd" && blen >= 32) {
                unsigned char m[32];
                if (f.read(body, m, 32)) {
                    uint64_t scale, dur;
                    if (m[0] == 1) {
                        scale = be32(m + 20);
                        dur = be64(m + 24);
                    } else {
                        scale = be32(m + 12);
                        dur = be32(m + 16);
                    }
                    if (scale) t.duration_ms = dur * 1000 / scale;
                }
            } else if (type == "stsd" && blen >= 16) {
                unsigned char s[44];
                if (f.read(body, s, std::min<uint64_t>(blen, 44)) && blen >= 44) {
                    const std::string fmt(reinterpret_cast<const char *>(s + 12), 4);
                    if (fmt == "mp4a") t.codec = "AAC";
                    else if (fmt == "alac") t.codec = "ALAC";
                    else if (fmt == "fLaC") t.codec = "FLAC";
                    else if (fmt == "Opus") t.codec = "Opus";
                    else if (fmt == "ac-3") t.codec = "AC-3";
                    t.channels = be16(s + 8 + 8 + 16);
                    t.bits = be16(s + 8 + 8 + 18);
                    t.rate = be16(s + 8 + 8 + 24);
                    if (t.codec == "AAC") t.bits = 0;
                }
            } else if (parent == "ilst") {
                item(type, body, blen);
            }
            pos += size;
        }
        --depth;
    }

    /* a freeform item: 'mean' (com.apple.iTunes), 'name' (replaygain_track_gain ...), 'data' (UTF-8 text) */
    void freeform(uint64_t body, uint64_t blen) {
        std::string mean, name, value;
        uint64_t pos = body;
        const uint64_t end = body + blen;
        while (pos + 8 <= end) {
            unsigned char h[8];
            if (!f.read(pos, h, 8)) return;
            const uint64_t size = be32(h);
            if (size < 8 || pos + size > end) return;
            const std::string type(reinterpret_cast<const char *>(h + 4), 4);
            const uint64_t plen = size - 8;
            if ((type == "mean" || type == "name") && plen >= 4 && plen <= 512) {
                std::vector<unsigned char> b;
                if (!f.read(pos + 12, b, static_cast<size_t>(plen - 4))) return;
                (type == "mean" ? mean : name).assign(b.begin(), b.end());
            } else if (type == "data" && plen >= 8 && plen <= 512) {
                std::vector<unsigned char> b;
                if (!f.read(pos + 16, b, static_cast<size_t>(plen - 8))) return;
                value.assign(b.begin(), b.end());
            }
            pos += size;
        }
        if (mean == "com.apple.iTunes" && !name.empty()) apply_replaygain_tag(t.rg, name, value);
    }

    void item(const std::string &type, uint64_t body, uint64_t blen) {
        if (type == "----") {
            freeform(body, blen);
            return;
        }
        /* the 'data' child: size, 'data', type (1 UTF-8, 13 JPEG, 14 PNG, 0 binary), locale, payload */
        unsigned char d[16];
        if (blen < 16 || !f.read(body, d, 16) || std::memcmp(d + 4, "data", 4) != 0) return;
        const uint64_t dsize = be32(d);
        if (dsize < 16 || dsize > blen) return;
        const uint32_t dtype = be32(d + 8) & 0xFFFFFF;
        const uint64_t plen = dsize - 16, poff = body + 16;
        if (type == "covr") {
            t.has_picture = true;
            if (pic && plen <= max_pic && pic->data.empty()) {
                std::vector<unsigned char> b;
                if (f.read(poff, b, static_cast<size_t>(plen))) {
                    pic->data = std::move(b);
                    pic->mime = dtype == 14 ? "image/png" : dtype == 13 ? "image/jpeg" : "";
                    pic->type = 3;
                }
            }
            return;
        }
        if (type == "trkn" || type == "disk") {
            unsigned char n[8];
            if (plen >= 4 && f.read(poff, n, 4)) {
                const int v = be16(n + 2);
                if (type == "trkn" && !t.track) t.track = v;
                if (type == "disk" && !t.disc) t.disc = v;
            }
            return;
        }
        if (dtype != 1 || plen > 4096) return;
        std::vector<unsigned char> b;
        if (!f.read(poff, b, static_cast<size_t>(plen))) return;
        const std::string v(b.begin(), b.end());
        if (type == "\xA9nam") set_once(t.title, v);
        else if (type == "\xA9" "ART") set_once(t.artist, v);
        else if (type == "aART") set_once(t.album_artist, v);
        else if (type == "\xA9" "alb") set_once(t.album, v);
        else if (type == "\xA9" "day") set_once(t.date, v);
        else if (type == "\xA9" "gen") set_once(t.genre, v);
    }
};

void read_mp4(const File &f, TrackTags &t, Picture *pic, size_t max_pic) {
    t.codec = "AAC";
    Mp4 m{f, t, pic, max_pic};
    m.walk(0, f.size(), "");
    t.ok = true;
    if (t.duration_ms) t.bitrate_kbps = static_cast<unsigned>(f.size() * 8 / t.duration_ms);
}

/* ------------------------------------------------------------------------------------------------ WAV, AIFF */

void read_wav(const File &f, TrackTags &t, Picture *pic, size_t max_pic) {
    t.codec = "WAV";
    uint64_t pos = 12, data_len = 0;
    unsigned block = 0;
    while (pos + 8 <= f.size()) {
        unsigned char c[8];
        if (!f.read(pos, c, 8)) break;
        const uint64_t len = le32(c + 4);
        if (std::memcmp(c, "fmt ", 4) == 0 && len >= 16) {
            unsigned char m[16];
            if (f.read(pos + 8, m, 16)) {
                t.channels = le16(m + 2);
                t.rate = le32(m + 4);
                block = le16(m + 12);
                t.bits = le16(m + 14);
                t.ok = true;
            }
        } else if (std::memcmp(c, "data", 4) == 0) {
            data_len = (len == 0xFFFFFFFFu || pos + 8 + len > f.size()) ? f.size() - pos - 8 : len;
        } else if (std::memcmp(c, "LIST", 4) == 0 && len >= 4 && len < (1u << 20)) {
            std::vector<unsigned char> b;
            if (f.read(pos + 8, b, len) && std::memcmp(b.data(), "INFO", 4) == 0) {
                size_t q = 4;
                while (q + 8 <= b.size()) {
                    const std::string id(reinterpret_cast<const char *>(b.data() + q), 4);
                    const uint32_t l = le32(b.data() + q + 4);
                    q += 8;
                    if (l > b.size() - q) break;
                    size_t e = l;
                    while (e > 0 && b[q + e - 1] == 0) --e;
                    const std::string v(reinterpret_cast<const char *>(b.data() + q), e);
                    if (id == "INAM") set_once(t.title, v);
                    else if (id == "IART") set_once(t.artist, v);
                    else if (id == "IPRD") set_once(t.album, v);
                    else if (id == "ICRD") set_once(t.date, v);
                    else if (id == "IGNR") set_once(t.genre, v);
                    else if (id == "ITRK" && !t.track) t.track = leading_int(v);
                    q += l + (l & 1);
                }
            }
        } else if ((std::memcmp(c, "id3 ", 4) == 0 || std::memcmp(c, "ID3 ", 4) == 0) && len <= kMaxTagBytes) {
            std::vector<unsigned char> b;
            if (f.read(pos + 8, b, len)) parse_id3v2(b.data(), b.size(), t, pic, max_pic);
        }
        pos += 8 + len + (len & 1);
    }
    if (block && t.rate) t.duration_ms = data_len / block * 1000 / t.rate;
}

void read_aiff(const File &f, TrackTags &t, Picture *pic, size_t max_pic) {
    t.codec = "AIFF";
    uint64_t pos = 12;
    while (pos + 8 <= f.size()) {
        unsigned char c[8];
        if (!f.read(pos, c, 8)) break;
        const uint64_t len = be32(c + 4);
        if (std::memcmp(c, "COMM", 4) == 0 && len >= 18) {
            unsigned char m[18];
            if (f.read(pos + 8, m, 18)) {
                t.channels = be16(m);
                const uint32_t frames = be32(m + 2);
                t.bits = be16(m + 6);
                const int exp = ((m[8] & 0x7F) << 8 | m[9]) - 16383 - 63;
                uint64_t mant = 0;
                for (int i = 0; i < 8; ++i) mant = (mant << 8) | m[10 + i];
                double r = static_cast<double>(mant);
                for (int i = 0; i < exp && i < 64; ++i) r *= 2;
                for (int i = 0; i > exp && i > -64; --i) r /= 2;
                t.rate = static_cast<uint32_t>(r + 0.5);
                if (t.rate) t.duration_ms = uint64_t(frames) * 1000 / t.rate;
                t.ok = true;
            }
        } else if ((std::memcmp(c, "NAME", 4) == 0 || std::memcmp(c, "AUTH", 4) == 0) && len < 4096) {
            std::vector<unsigned char> b;
            if (f.read(pos + 8, b, len)) {
                const std::string v = latin1_to_utf8(b.data(), b.size());
                if (c[0] == 'N') set_once(t.title, v);
                else set_once(t.artist, v);
            }
        } else if ((std::memcmp(c, "ID3 ", 4) == 0 || std::memcmp(c, "id3 ", 4) == 0) && len <= kMaxTagBytes) {
            std::vector<unsigned char> b;
            if (f.read(pos + 8, b, len)) parse_id3v2(b.data(), b.size(), t, pic, max_pic);
        }
        pos += 8 + len + (len & 1);
    }
}

}  // namespace

bool read_replaygain(const std::string &path, ReplayGainTags &out) {
    TrackTags t;
    const bool ok = read_tags(path, t, nullptr);
    out = t.rg;
    return ok;
}

bool read_tags(const std::string &path, TrackTags &out, Picture *pic, size_t max_picture_bytes, uint64_t *bytes_read) {
    out = TrackTags();
    if (pic) *pic = Picture();
    if (bytes_read) *bytes_read = 0;
    File f(path);
    if (!f.ok()) return false;
    struct Count {
        const File &f;
        uint64_t *out;
        ~Count() {
            if (out) *out = f.bytes_read();
        }
    } count{f, bytes_read};
    unsigned char h[12] = {0};
    const bool head = f.read(0, h, std::min<uint64_t>(12, f.size()));
    const std::string ext = lower_ext(path);
    if (head && std::memcmp(h, "fLaC", 4) == 0) read_flac(f, out, pic, max_picture_bytes, 0);
    else if (head && std::memcmp(h, "OggS", 4) == 0) read_ogg(f, out, pic, max_picture_bytes);
    else if (head && (std::memcmp(h, "RIFF", 4) == 0 || std::memcmp(h, "RF64", 4) == 0) && std::memcmp(h + 8, "WAVE", 4) == 0)
        read_wav(f, out, pic, max_picture_bytes);
    else if (head && std::memcmp(h, "FORM", 4) == 0) read_aiff(f, out, pic, max_picture_bytes);
    else if (head && std::memcmp(h + 4, "ftyp", 4) == 0) read_mp4(f, out, pic, max_picture_bytes);
    else if (head && std::memcmp(h, "ID3", 3) == 0 && ext == "flac") {
        const size_t total = 10 + syncsafe(h + 6) + ((h[5] & 0x10) ? 10 : 0);
        read_flac(f, out, pic, max_picture_bytes, total);
    } else if (ext == "mp3" || ext == "mp2" || (head && (std::memcmp(h, "ID3", 3) == 0 || (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0))))
        read_mp3(f, out, pic, max_picture_bytes);
    else {
        static const char *const names[][2] = {{"wma", "WMA"}, {"ape", "APE"}, {"wv", "WavPack"}, {"tta", "TTA"},
                                               {"mpc", "Musepack"}, {"dsf", "DSD"}, {"dff", "DSD"}, {"aac", "AAC"},
                                               {"mka", "Matroska"}, {"ac3", "AC-3"}};
        out.codec = upper(ext);
        for (const auto &n : names)
            if (ext == n[0]) out.codec = n[1];
    }
    return true;
}

}  // namespace vinyl
