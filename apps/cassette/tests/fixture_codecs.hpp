/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette tests: two Encoder implementations that need no library and write streams with exactly the structure the
 * writer, the take list and the repair rely on (phase B of FLAC/Opus), so those are tested byte for byte everywhere:
 *   VerbatimFlac  a real FLAC stream (RFC 9639) with VERBATIM subframes: fLaC, STREAMINFO (totals 0 = unknown until
 *                 finish()), VORBIS_COMMENT, optional PADDING, frames of a fixed block size with valid CRC-8 / CRC-16;
 *                 finish() writes the last partial block and rewrites STREAMINFO in place (MD5 stays 0 = unknown), as
 *                 libFLAC does. Any FLAC decoder plays it (the codec suite checks that with libFLAC).
 *   PlainOggOpus  an Ogg Opus stream (RFC 7845) with valid page CRCs: OpusHead, OpusTags, then 20 ms packets (a TOC
 *                 byte and filler: the content is not meant to sound) in pages closed every `page_packets` packets or
 *                 at flush(); the coded stream is the pre-skip plus the input, granule positions count it at 48 kHz,
 *                 an end-of-stream page at finish() with the exact length (libopusfile reads the durations exactly,
 *                 the codec suite checks that). No decision delay.
 * decode_verbatim_flac() reads a VerbatimFlac file back to interleaved samples. Header-only; the CRCs come from
 * codec/stream_scan (their known-answer values are tested in test_stream_scan).
 */
#pragma once

#include "codec/byte_sink.hpp"
#include "codec/encoder.hpp"
#include "codec/stream_scan.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fx {

using namespace cassette;

/* The synthetic test signal (a sawtooth with a little noise, the core tests' sig()), sample i of the interleaved stream. */
inline int16_t tone(uint64_t i) {
    uint64_t x = (i + 1) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    const int noise = static_cast<int>(x & 0xFF) - 128;
    const int saw = (static_cast<int>(i % 200) - 100) * 250;
    return static_cast<int16_t>(saw + noise);
}

/* Frames first .. first + frames - 1 of the signal in the capture format (little endian; 24-bit adds low bits). */
inline std::vector<unsigned char> pcm(const AudioFormat &f, uint64_t first, uint64_t frames) {
    std::vector<unsigned char> b;
    b.reserve(static_cast<size_t>(frames) * f.channels * (f.bits / 8u));
    for (uint64_t i = 0; i < frames; ++i)
        for (unsigned c = 0; c < f.channels; ++c) {
            const int32_t v = tone((first + i) * f.channels + c);
            if (f.bits == 24) {
                const int32_t w = v * 256 + static_cast<int32_t>((first + i + c) % 251);
                b.push_back(static_cast<unsigned char>(w & 0xFF));
                b.push_back(static_cast<unsigned char>((w >> 8) & 0xFF));
                b.push_back(static_cast<unsigned char>((w >> 16) & 0xFF));
            } else {
                b.push_back(static_cast<unsigned char>(v & 0xFF));
                b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
            }
        }
    return b;
}

/* Little-endian PCM bytes back to signed samples. */
inline std::vector<int32_t> samples_of(const AudioFormat &f, const std::vector<unsigned char> &p) {
    std::vector<int32_t> s;
    const unsigned bytes = f.bits / 8;
    for (size_t i = 0; i + bytes <= p.size(); i += bytes) {
        int32_t v = 0;
        for (int k = static_cast<int>(bytes) - 1; k >= 0; --k) v = (v << 8) | p[i + static_cast<size_t>(k)];
        if (v & (1 << (f.bits - 1))) v -= (1 << f.bits);
        s.push_back(v);
    }
    return s;
}

inline void put_be(std::vector<unsigned char> &b, uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
}
inline void put_le(std::vector<unsigned char> &b, uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
}

class VerbatimFlac final : public Encoder {
public:
    explicit VerbatimFlac(const AudioFormat &f, uint32_t block = 4096, uint32_t padding = 0)
        : fmt_(f), block_(block), padding_(padding) {}

    int start(ByteSink &sink, const Tags &tags) override {
        if (st_.error) return st_.error;
        sink_ = &sink;
        std::vector<unsigned char> h = {'f', 'L', 'a', 'C'};
        h.push_back(0x00); /* STREAMINFO, not last */
        put_be(h, 34, 3);
        const std::vector<unsigned char> si = streaminfo(0);
        h.insert(h.end(), si.begin(), si.end());
        std::vector<unsigned char> vc;
        const std::string vendor = "cassette test";
        put_le(vc, vendor.size(), 4);
        vc.insert(vc.end(), vendor.begin(), vendor.end());
        put_le(vc, tags.size(), 4);
        for (const Tag &t : tags) {
            const std::string s = t.name + "=" + t.value;
            put_le(vc, s.size(), 4);
            vc.insert(vc.end(), s.begin(), s.end());
        }
        h.push_back(static_cast<unsigned char>(4 | (padding_ ? 0 : 0x80)));
        put_be(h, vc.size(), 3);
        h.insert(h.end(), vc.begin(), vc.end());
        if (padding_) {
            h.push_back(0x81);
            put_be(h, padding_, 3);
            h.insert(h.end(), padding_, 0);
        }
        return out(h, 0);
    }

    int write(const void *pcm, size_t frames) override {
        if (st_.error) return st_.error;
        if (!sink_ || st_.finished) return fail(-100103);
        const size_t fb = frame_bytes();
        const unsigned char *p = static_cast<const unsigned char *>(pcm);
        pending_.insert(pending_.end(), p, p + frames * fb);
        st_.frames_in += frames;
        while (pending_.size() >= block_ * fb) {
            const int r = emit(pending_.data(), block_);
            if (r < 0) return r;
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(block_ * fb));
        }
        return 0;
    }
    int flush() override { return st_.error; } /* a partial block stays until finish(), as in libFLAC */
    int finish() override {
        if (st_.error) return st_.error;
        if (st_.finished) return 0;
        st_.finished = true;
        const size_t fb = frame_bytes();
        if (!pending_.empty()) {
            const int r = emit(pending_.data(), static_cast<uint32_t>(pending_.size() / fb));
            if (r < 0) return r;
            pending_.clear();
        }
        const std::vector<unsigned char> si = streaminfo(st_.frames_out);
        int r = sink_->seek(8 + 10);
        if (r == 0) r = sink_->write(si.data() + 10, 8);
        if (r < 0) return fail(r);
        st_.bytes_out += 8;
        return 0;
    }
    EncoderStats stats() const override { return st_; }
    const AudioFormat &format() const override { return fmt_; }

private:
    size_t frame_bytes() const { return static_cast<size_t>(fmt_.channels) * (fmt_.bits / 8u); }
    int fail(int e) {
        if (!st_.error) st_.error = e;
        return st_.error;
    }
    int out(const std::vector<unsigned char> &b, uint64_t frames) {
        const int r = sink_->write(b.data(), b.size());
        if (r < 0) return fail(r);
        st_.bytes_out += b.size();
        st_.units_out++;
        st_.frames_out += frames;
        return 0;
    }
    std::vector<unsigned char> streaminfo(uint64_t total) const {
        std::vector<unsigned char> s;
        put_be(s, block_, 2);
        put_be(s, block_, 2);
        put_be(s, 0, 3);
        put_be(s, 0, 3);
        const uint32_t ch = fmt_.channels - 1u, bps = fmt_.bits - 1u;
        s.push_back(static_cast<unsigned char>(fmt_.rate >> 12));
        s.push_back(static_cast<unsigned char>((fmt_.rate >> 4) & 0xFF));
        s.push_back(static_cast<unsigned char>(((fmt_.rate & 0xF) << 4) | (ch << 1) | (bps >> 4)));
        s.push_back(static_cast<unsigned char>(((bps & 0xF) << 4) | ((total >> 32) & 0xF)));
        put_be(s, total & 0xFFFFFFFFull, 4);
        s.insert(s.end(), 16, 0); /* MD5 unknown */
        return s;
    }
    static unsigned rate_code(uint32_t r) {
        switch (r) {
        case 8000: return 4;
        case 16000: return 5;
        case 22050: return 6;
        case 24000: return 7;
        case 32000: return 8;
        case 44100: return 9;
        case 48000: return 10;
        case 96000: return 11;
        default: return 0;
        }
    }
    static unsigned block_code(uint32_t n) {
        if (n == 192) return 1;
        for (unsigned c = 2; c <= 5; ++c)
            if (n == (144u << c)) return c;
        for (unsigned c = 8; c <= 15; ++c)
            if (n == (1u << c)) return c;
        return 7;
    }
    int emit(const unsigned char *pcm, uint32_t n) {
        std::vector<unsigned char> f = {0xFF, 0xF8};
        const unsigned bc = block_code(n);
        f.push_back(static_cast<unsigned char>((bc << 4) | rate_code(fmt_.rate)));
        const unsigned ss = fmt_.bits == 16 ? 4 : fmt_.bits == 24 ? 6 : 0;
        f.push_back(static_cast<unsigned char>(((fmt_.channels - 1u) << 4) | (ss << 1)));
        const uint64_t v = frame_no_++;
        if (v < 0x80) {
            f.push_back(static_cast<unsigned char>(v));
        } else {
            int extra = v < 0x800 ? 1 : v < 0x10000 ? 2 : v < 0x200000 ? 3 : v < 0x4000000 ? 4 : 5;
            static const unsigned char lead[6] = {0, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC};
            f.push_back(static_cast<unsigned char>(lead[extra] | (v >> (6 * extra))));
            for (int k = extra - 1; k >= 0; --k) f.push_back(static_cast<unsigned char>(0x80 | ((v >> (6 * k)) & 0x3F)));
        }
        if (bc == 7) put_be(f, n - 1u, 2);
        f.push_back(flac_crc8(f.data(), f.size()));
        const unsigned bytes = fmt_.bits / 8u;
        for (unsigned c = 0; c < fmt_.channels; ++c) {
            f.push_back(0x02); /* VERBATIM, no wasted bits */
            for (uint32_t i = 0; i < n; ++i) {
                const unsigned char *s = pcm + (static_cast<size_t>(i) * fmt_.channels + c) * bytes;
                for (int k = static_cast<int>(bytes) - 1; k >= 0; --k) f.push_back(s[k]); /* little endian in, big out */
            }
        }
        put_be(f, flac_crc16(f.data(), f.size()), 2);
        return out(f, n);
    }

    AudioFormat fmt_;
    uint32_t block_, padding_;
    ByteSink *sink_ = nullptr;
    EncoderStats st_;
    std::vector<unsigned char> pending_;
    uint64_t frame_no_ = 0;
};

class PlainOggOpus final : public Encoder {
public:
    explicit PlainOggOpus(const AudioFormat &f, unsigned packet_bytes = 80, unsigned page_packets = 50,
                          uint32_t preskip = 312)
        : fmt_(f), packet_bytes_(packet_bytes), page_packets_(page_packets), preskip_(preskip) {}

    int start(ByteSink &sink, const Tags &tags) override {
        if (st_.error) return st_.error;
        sink_ = &sink;
        std::vector<unsigned char> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1,
                                           static_cast<unsigned char>(fmt_.channels)};
        put_le(head, preskip_, 2);
        put_le(head, fmt_.rate, 4);
        put_le(head, 0, 2);
        head.push_back(0);
        int r = page(0x02, 0, {head});
        if (r < 0) return r;
        std::vector<unsigned char> t = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
        const std::string vendor = "cassette test";
        put_le(t, vendor.size(), 4);
        t.insert(t.end(), vendor.begin(), vendor.end());
        put_le(t, tags.size(), 4);
        for (const Tag &g : tags) {
            const std::string s = g.name + "=" + g.value;
            put_le(t, s.size(), 4);
            t.insert(t.end(), s.begin(), s.end());
        }
        return page(0, 0, {t});
    }
    int write(const void *pcm, size_t frames) override {
        if (st_.error) return st_.error;
        if (!sink_ || st_.finished) return fail(-100103);
        (void)pcm;
        st_.frames_in += frames;
        /* as a real encoder: the coded stream is the pre-skip (the codec's look-ahead) plus the input, at 48 kHz */
        while (preskip_ + in48() - g48_ >= 960) {
            add_packet();
            if (cur_.size() >= page_packets_) {
                const int r = close_page(0);
                if (r < 0) return r;
            }
        }
        return 0;
    }
    int flush() override {
        if (st_.error) return st_.error;
        return cur_.empty() ? 0 : close_page(0);
    }
    int finish() override {
        if (st_.error) return st_.error;
        if (st_.finished) return 0;
        st_.finished = true;
        if (preskip_ + in48() > g48_) add_packet(); /* the last, partial 20 ms */
        end48_ = in48();
        return close_page(0x04);
    }
    EncoderStats stats() const override { return st_; }
    const AudioFormat &format() const override { return fmt_; }
    uint32_t preskip() const { return preskip_; }

private:
    int fail(int e) {
        if (!st_.error) st_.error = e;
        return st_.error;
    }
    void add_packet() {
        std::vector<unsigned char> p(packet_bytes_, 0);
        p[0] = static_cast<unsigned char>(0xF8 | (fmt_.channels == 2 ? 0x04 : 0)); /* CELT FB 20 ms, one frame */
        for (unsigned i = 1; i < packet_bytes_; ++i) p[i] = static_cast<unsigned char>((seed_ = seed_ * 1103515245u + 12345u) >> 16);
        cur_.push_back(p);
        g48_ += 960;
    }
    uint64_t in48() const { return st_.frames_in * 48000u / fmt_.rate; }
    /* RFC 7845: a page's granule position counts every sample of the packets that end on it, the pre-skip included;
     * the last one is the exact length plus the pre-skip (end trimming) */
    int close_page(unsigned type) {
        uint64_t g = g48_;
        if (type & 0x04) g = preskip_ + end48_;
        const int r = page(type, g, cur_);
        cur_.clear();
        if (r == 0 && g > preskip_) st_.frames_out = std::min<uint64_t>(st_.frames_in, (g - preskip_) * fmt_.rate / 48000u);
        return r;
    }
    int page(unsigned type, uint64_t granule, const std::vector<std::vector<unsigned char>> &packets) {
        std::vector<unsigned char> lacing, body;
        for (const std::vector<unsigned char> &p : packets) {
            size_t n = p.size();
            while (n >= 255) {
                lacing.push_back(255);
                n -= 255;
            }
            lacing.push_back(static_cast<unsigned char>(n));
            body.insert(body.end(), p.begin(), p.end());
        }
        std::vector<unsigned char> pg = {'O', 'g', 'g', 'S', 0, static_cast<unsigned char>(type)};
        put_le(pg, granule, 8);
        put_le(pg, 0x5A5A1234u, 4);
        put_le(pg, seq_++, 4);
        put_le(pg, 0, 4);
        pg.push_back(static_cast<unsigned char>(lacing.size()));
        pg.insert(pg.end(), lacing.begin(), lacing.end());
        pg.insert(pg.end(), body.begin(), body.end());
        const uint32_t crc = ogg_crc32(pg.data(), pg.size());
        for (int k = 0; k < 4; ++k) pg[22 + k] = static_cast<unsigned char>((crc >> (8 * k)) & 0xFF);
        const int r = sink_->write(pg.data(), pg.size());
        if (r < 0) return fail(r);
        st_.bytes_out += pg.size();
        st_.units_out++;
        return 0;
    }

    AudioFormat fmt_;
    unsigned packet_bytes_, page_packets_;
    uint32_t preskip_;
    ByteSink *sink_ = nullptr;
    EncoderStats st_;
    std::vector<std::vector<unsigned char>> cur_;
    uint64_t g48_ = 0, end48_ = 0;
    uint32_t seq_ = 0, seed_ = 1;
};

/* The interleaved samples of a VerbatimFlac stream (16-bit or 24-bit), up to the first frame that does not parse. */
inline std::vector<int32_t> decode_verbatim_flac(const std::vector<unsigned char> &d) {
    std::vector<int32_t> out;
    if (d.size() < 8 || std::memcmp(d.data(), "fLaC", 4) != 0) return out;
    size_t pos = 4;
    unsigned channels = 1, bits = 16;
    bool last = false;
    while (!last && pos + 4 <= d.size()) {
        last = d[pos] & 0x80;
        const size_t len = (static_cast<size_t>(d[pos + 1]) << 16) | (d[pos + 2] << 8) | d[pos + 3];
        if ((d[pos] & 0x7F) == 0 && pos + 4 + 34 <= d.size()) {
            const unsigned char *b = d.data() + pos + 4;
            channels = ((b[12] >> 1) & 7) + 1;
            bits = (((b[12] & 1) << 4) | (b[13] >> 4)) + 1;
        }
        pos += 4 + len;
    }
    const unsigned bytes = bits / 8;
    while (pos + 8 <= d.size() && d[pos] == 0xFF && d[pos + 1] == 0xF8) {
        const unsigned bc = d[pos + 2] >> 4;
        size_t i = pos + 4;
        const unsigned c = d[i];
        int extra = (c & 0x80) == 0 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : (c & 0xFC) == 0xF8 ? 4 : 5;
        i += 1 + static_cast<size_t>(extra);
        uint32_t n = 0;
        if (bc == 7) {
            n = ((d[i] << 8) | d[i + 1]) + 1u;
            i += 2;
        } else if (bc >= 8) {
            n = 1u << bc;
        } else if (bc >= 2) {
            n = 144u << bc;
        } else {
            n = 192;
        }
        i += 1; /* CRC-8 */
        const size_t end = i + channels * (1 + static_cast<size_t>(n) * bytes) + 2;
        if (end > d.size()) break;
        std::vector<int32_t> fr(static_cast<size_t>(n) * channels);
        for (unsigned ch = 0; ch < channels; ++ch) {
            i += 1;
            for (uint32_t s = 0; s < n; ++s) {
                int32_t v = 0;
                for (unsigned k = 0; k < bytes; ++k) v = (v << 8) | d[i++];
                if (v & (1 << (bits - 1))) v -= (1 << bits);
                fr[static_cast<size_t>(s) * channels + ch] = v;
            }
        }
        out.insert(out.end(), fr.begin(), fr.end());
        pos = end;
    }
    return out;
}

}  // namespace fx
