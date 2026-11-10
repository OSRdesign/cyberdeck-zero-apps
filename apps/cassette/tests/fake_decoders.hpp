/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette tests: fake libFLAC stream decoder and libopus decoder tables (phase C), header-only so every suite that plays,
 * builds peaks or trims FLAC / Opus takes can use them without a library.
 *   FLAC: decodes the VerbatimFlac streams of tests/fixture_codecs.hpp (and any FLAC with VERBATIM / CONSTANT subframes)
 *         with libFLAC's calling conventions: init_stream with the read / seek / tell / length / eof / write / error
 *         callbacks, process_until_end_of_metadata, process_single, seek_absolute (which, as in libFLAC, hands the tail
 *         of the target frame to the write callback during the call), flush, finish, get_state. The stream is read
 *         through the read callback (so the FileOps under it see every read); a frame whose CRC-16 fails is skipped with
 *         the error callback, as libFLAC does.
 *   Opus: opus_decoder_create / opus_decode / opus_decoder_ctl. A packet decodes to its TOC duration at the decoder's
 *         rate; the samples are a function of the packet's bytes and the sample's index in it (no state: a seek gives
 *         exactly the samples a linear decode gives, so positions are checked to the sample). PLC (data null) gives 0.
 * State and switches are global (one fake at a time per test): fake_flac_dec_state(), fake_opus_dec_state().
 * expected_opus_pcm() is the independent reference: every packet of the file in order, the fake's samples, the pre-skip
 * dropped, cut at the stream's length.
 */
#pragma once

#include "codec/codec_libs.hpp"
#include "codec/ogg_opus.hpp"
#include "codec/stream_scan.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fakedec {

using namespace cassette;

/* ---------------------------------------------------------------- FLAC */

struct FlacDecState {
    int fail_seek_once_at = -1; /* this seek_absolute call (0-based, counted over all decoders) returns SEEK_ERROR once */
    int seeks = 0, news = 0, deletes = 0, inits = 0, finishes = 0, flushes = 0, frames_written = 0, crc_errors = 0;
    int md5 = -1;
};
inline FlacDecState &fake_flac_dec_state() {
    static FlacDecState s;
    return s;
}

struct FakeFlac {
    flac::ReadCb read = nullptr;
    flac::DecSeekCb seek = nullptr;
    flac::DecWriteCb write = nullptr;
    flac::ErrorCb error = nullptr;
    void *client = nullptr;
    int state = flac::kDecUninitialized;
    bool loaded = false;
    unsigned channels = 0, bits = 0;
    uint32_t block = 0;
    struct Frame {
        uint64_t first = 0;
        uint32_t n = 0;
        std::vector<int32_t> s; /* interleaved */
    };
    std::vector<Frame> frames;
    size_t next = 0;
};

inline FakeFlac *ff(flac::Decoder *d) { return reinterpret_cast<FakeFlac *>(d); }
inline const FakeFlac *ff(const flac::Decoder *d) { return reinterpret_cast<const FakeFlac *>(d); }

/* A big-endian bit reader for the subframes. */
struct BitIn {
    const std::vector<unsigned char> &d;
    uint64_t bp, end;
    bool get(unsigned n, uint64_t &v) {
        v = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (bp >= end) return false;
            v = (v << 1) | ((d[static_cast<size_t>(bp >> 3)] >> (7 - (bp & 7))) & 1u);
            ++bp;
        }
        return true;
    }
};

inline int32_t sext(uint64_t v, unsigned bits) {
    if (bits == 0) return 0;
    const uint64_t m = uint64_t(1) << (bits - 1);
    return static_cast<int32_t>(static_cast<int64_t>((v ^ m)) - static_cast<int64_t>(m));
}

/* Reads the whole stream through the callbacks and parses it (VERBATIM and CONSTANT subframes, independent channels). */
inline bool fake_flac_load(FakeFlac &f) {
    if (f.loaded) return true;
    std::vector<unsigned char> d;
    if (f.seek && f.seek(nullptr, 0, f.client) != flac::kDecSeekStatusOk) {
        f.state = flac::kDecAborted;
        return false;
    }
    for (;;) {
        unsigned char buf[4096];
        size_t n = sizeof buf;
        const int r = f.read(nullptr, buf, &n, f.client);
        if (r == flac::kReadAbort) {
            f.state = flac::kDecAborted;
            return false;
        }
        d.insert(d.end(), buf, buf + n);
        if (r == flac::kReadEndOfStream || n == 0) break;
    }
    f.loaded = true;
    if (d.size() < 8 || std::memcmp(d.data(), "fLaC", 4) != 0) {
        f.state = flac::kDecAborted;
        return false;
    }
    size_t pos = 4;
    bool last = false;
    while (!last && pos + 4 <= d.size()) {
        last = (d[pos] & 0x80) != 0;
        const size_t len = (static_cast<size_t>(d[pos + 1]) << 16) | (d[pos + 2] << 8) | d[pos + 3];
        if ((d[pos] & 0x7F) == 0 && pos + 4 + 34 <= d.size()) {
            const unsigned char *b = d.data() + pos + 4;
            f.block = (b[2] << 8) | b[3];
            f.channels = ((b[12] >> 1) & 7) + 1;
            f.bits = (((b[12] & 1) << 4) | (b[13] >> 4)) + 1;
        }
        pos += 4 + len;
    }
    /* frames: a header at pos, subframes, CRC-16; a frame that fails is skipped by searching the next sync */
    while (pos + 6 <= d.size()) {
        if (!(d[pos] == 0xFF && (d[pos + 1] & 0xFE) == 0xF8)) {
            ++pos;
            continue;
        }
        const bool variable = d[pos + 1] & 1;
        const unsigned bc = d[pos + 2] >> 4;
        size_t i = pos + 4;
        const unsigned c = d[i];
        int extra = (c & 0x80) == 0 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : (c & 0xFC) == 0xF8 ? 4 : (c & 0xFE) == 0xFC ? 5 : 6;
        uint64_t num = extra == 0 ? c : extra == 6 ? 0 : (c & (0x3F >> extra));
        if (i + 1 + static_cast<size_t>(extra) > d.size()) break;
        for (int k = 1; k <= extra; ++k) num = (num << 6) | (d[i + static_cast<size_t>(k)] & 0x3F);
        i += 1 + static_cast<size_t>(extra);
        uint32_t n = 0;
        if (bc == 6) {
            if (i + 1 > d.size()) break;
            n = d[i] + 1u;
            i += 1;
        } else if (bc == 7) {
            if (i + 2 > d.size()) break;
            n = ((d[i] << 8) | d[i + 1]) + 1u;
            i += 2;
        } else if (bc >= 8) {
            n = 1u << bc;
        } else if (bc >= 2) {
            n = 144u << bc;
        } else {
            n = 192;
        }
        const unsigned sr = d[pos + 2] & 15;
        if (sr == 12) i += 1;
        else if (sr == 13 || sr == 14) i += 2;
        if (i + 1 > d.size() || flac_crc8(d.data() + pos, i - pos) != d[i]) {
            ++pos;
            continue;
        }
        i += 1;
        FakeFlac::Frame fr;
        fr.first = variable ? num : num * f.block;
        fr.n = n;
        fr.s.assign(static_cast<size_t>(n) * f.channels, 0);
        BitIn bi{d, static_cast<uint64_t>(i) * 8, static_cast<uint64_t>(d.size()) * 8};
        bool ok = true;
        for (unsigned ch = 0; ch < f.channels && ok; ++ch) {
            uint64_t hdr;
            if (!bi.get(8, hdr)) {
                ok = false;
                break;
            }
            const unsigned type = static_cast<unsigned>((hdr >> 1) & 0x3F);
            uint64_t v;
            if (type == 0) { /* CONSTANT */
                if (!bi.get(f.bits, v)) ok = false;
                for (uint32_t s = 0; s < n && ok; ++s) fr.s[static_cast<size_t>(s) * f.channels + ch] = sext(v, f.bits);
            } else if (type == 1) { /* VERBATIM */
                for (uint32_t s = 0; s < n && ok; ++s) {
                    if (!bi.get(f.bits, v)) ok = false;
                    else fr.s[static_cast<size_t>(s) * f.channels + ch] = sext(v, f.bits);
                }
            } else {
                ok = false;
            }
        }
        if (!ok) {
            ++pos;
            continue;
        }
        const size_t end = static_cast<size_t>((bi.bp + 7) / 8);
        if (end + 2 > d.size() || flac_crc16(d.data() + pos, end - pos) != ((d[end] << 8) | d[end + 1])) {
            fake_flac_dec_state().crc_errors++;
            if (f.error) f.error(nullptr, 2 /* FRAME_CRC_MISMATCH */, f.client);
            ++pos;
            continue;
        }
        f.frames.push_back(std::move(fr));
        pos = end + 2;
    }
    return true;
}

inline int fake_flac_emit(FakeFlac &f, const FakeFlac::Frame &fr, uint32_t skip) {
    flac::FrameHeader h;
    std::memset(&h, 0, sizeof h);
    h.blocksize = fr.n - skip;
    h.channels = f.channels;
    h.bits_per_sample = f.bits;
    h.number_type = flac::kFrameNumberTypeSample;
    h.number.sample_number = fr.first + skip;
    std::vector<std::vector<int32_t>> ch(f.channels, std::vector<int32_t>(h.blocksize));
    for (uint32_t s = 0; s < h.blocksize; ++s)
        for (unsigned c = 0; c < f.channels; ++c) ch[c][s] = fr.s[static_cast<size_t>(s + skip) * f.channels + c];
    std::vector<const int32_t *> ptr(f.channels);
    for (unsigned c = 0; c < f.channels; ++c) ptr[c] = ch[c].data();
    fake_flac_dec_state().frames_written++;
    return f.write(nullptr, &h, ptr.data(), f.client);
}

inline FlacDecLib make_flac_dec_lib() {
    fake_flac_dec_state() = FlacDecState();
    FlacDecLib L;
    L.name = "fake";
    L.version = "fake 1.0";
    L.decoder_new = []() -> flac::Decoder * {
        fake_flac_dec_state().news++;
        return reinterpret_cast<flac::Decoder *>(new FakeFlac());
    };
    L.decoder_delete = [](flac::Decoder *d) {
        fake_flac_dec_state().deletes++;
        delete ff(d);
    };
    L.set_md5_checking = [](flac::Decoder *, flac::Bool v) -> flac::Bool {
        fake_flac_dec_state().md5 = v;
        return 1;
    };
    L.init_stream = [](flac::Decoder *d, flac::ReadCb r, flac::DecSeekCb s, flac::DecTellCb, flac::LengthCb, flac::EofCb,
                       flac::DecWriteCb w, flac::DecMetadataCb, flac::ErrorCb e, void *client) -> int {
        FakeFlac &f = *ff(d);
        if (!r || !w || !e) return 3; /* INVALID_CALLBACKS */
        f.read = r;
        f.seek = s;
        f.write = w;
        f.error = e;
        f.client = client;
        f.state = flac::kDecSearchForMetadata;
        fake_flac_dec_state().inits++;
        return flac::kDecInitOk;
    };
    L.process_until_end_of_metadata = [](flac::Decoder *d) -> flac::Bool {
        FakeFlac &f = *ff(d);
        if (!fake_flac_load(f)) return 0;
        f.state = flac::kDecSearchForFrameSync;
        return 1;
    };
    L.process_single = [](flac::Decoder *d) -> flac::Bool {
        FakeFlac &f = *ff(d);
        if (!fake_flac_load(f)) return 0;
        if (f.next >= f.frames.size()) {
            f.state = flac::kDecEndOfStream;
            return 1;
        }
        if (fake_flac_emit(f, f.frames[f.next++], 0) != flac::kWriteContinue) {
            f.state = flac::kDecAborted;
            return 0;
        }
        f.state = flac::kDecSearchForFrameSync;
        return 1;
    };
    L.seek_absolute = [](flac::Decoder *d, uint64_t sample) -> flac::Bool {
        FakeFlac &f = *ff(d);
        FlacDecState &st = fake_flac_dec_state();
        const int call = st.seeks++;
        if (call == st.fail_seek_once_at) {
            f.state = flac::kDecSeekError;
            return 0;
        }
        if (!fake_flac_load(f)) return 0;
        for (size_t i = 0; i < f.frames.size(); ++i) {
            const FakeFlac::Frame &fr = f.frames[i];
            if (sample >= fr.first && sample < fr.first + fr.n) {
                f.next = i + 1;
                if (fake_flac_emit(f, fr, static_cast<uint32_t>(sample - fr.first)) != flac::kWriteContinue) {
                    f.state = flac::kDecAborted;
                    return 0;
                }
                f.state = flac::kDecSearchForFrameSync;
                return 1;
            }
        }
        f.state = flac::kDecSeekError;
        return 0;
    };
    L.flush = [](flac::Decoder *d) -> flac::Bool {
        fake_flac_dec_state().flushes++;
        ff(d)->state = flac::kDecSearchForFrameSync;
        return 1;
    };
    L.get_state = [](const flac::Decoder *d) -> int { return ff(d)->state; };
    L.finish = [](flac::Decoder *d) -> flac::Bool {
        fake_flac_dec_state().finishes++;
        ff(d)->state = flac::kDecUninitialized;
        return 1;
    };
    return L;
}

/* ---------------------------------------------------------------- Opus */

struct OpusDecState {
    int fail_decode_at = -1; /* this decode call (0-based, all decoders) returns OPUS_INVALID_PACKET */
    int creates = 0, destroys = 0, decodes = 0, plc = 0, resets = 0;
    int32_t rate = 0;
    int channels = 0, gain = 0;
};
inline OpusDecState &fake_opus_dec_state() {
    static OpusDecState s;
    return s;
}

struct FakeOpus {
    int32_t rate = 48000;
    int channels = 1;
};

/* The fake's sample j (at 48 kHz steps of the packet) of channel c. */
inline int16_t fake_opus_sample(const unsigned char *p, size_t len, uint32_t j48, int c) {
    const uint32_t a = len > 2 ? (static_cast<uint32_t>(p[1]) << 8) | p[2] : 0x1234;
    const uint32_t b = len > 4 ? (static_cast<uint32_t>(p[3]) << 8) | p[4] : 0x4321;
    return static_cast<int16_t>((a + j48 * 37u + static_cast<uint32_t>(c) * b) & 0xFFFF);
}

inline int fake_opus_ctl(opus::Decoder *, int request, ...) {
    if (request == opus::kResetState) {
        fake_opus_dec_state().resets++;
        return opus::kOk;
    }
    if (request == opus::kSetGainRequest) {
        va_list ap;
        va_start(ap, request);
        fake_opus_dec_state().gain = va_arg(ap, int32_t);
        va_end(ap);
        return opus::kOk;
    }
    return opus::kBadArg;
}

inline OpusDecLib make_opus_dec_lib() {
    fake_opus_dec_state() = OpusDecState();
    OpusDecLib L;
    L.name = "fake";
    L.version = "fake opus";
    L.decoder_create = [](int32_t rate, int channels, int *err) -> opus::Decoder * {
        if (!(rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000) || channels < 1 ||
            channels > 2) {
            if (err) *err = opus::kBadArg;
            return nullptr;
        }
        fake_opus_dec_state().creates++;
        fake_opus_dec_state().rate = rate;
        fake_opus_dec_state().channels = channels;
        if (err) *err = opus::kOk;
        FakeOpus *f = new FakeOpus();
        f->rate = rate;
        f->channels = channels;
        return reinterpret_cast<opus::Decoder *>(f);
    };
    L.decoder_destroy = [](opus::Decoder *d) {
        fake_opus_dec_state().destroys++;
        delete reinterpret_cast<FakeOpus *>(d);
    };
    L.decode = [](opus::Decoder *d, const unsigned char *data, int32_t len, int16_t *pcm, int frame_size, int) -> int {
        FakeOpus &f = *reinterpret_cast<FakeOpus *>(d);
        OpusDecState &st = fake_opus_dec_state();
        const int call = st.decodes++;
        if (!data) {
            st.plc++;
            std::memset(pcm, 0, static_cast<size_t>(frame_size) * f.channels * sizeof(int16_t));
            return frame_size;
        }
        if (call == st.fail_decode_at) return opus::kInvalidPacket;
        const uint32_t n48 = oggopus::packet_samples48(data, static_cast<size_t>(len));
        if (n48 == 0) return opus::kInvalidPacket;
        const uint32_t k = 48000u / static_cast<uint32_t>(f.rate);
        const int n = static_cast<int>(n48 / k);
        if (n > frame_size) return opus::kBufferTooSmall;
        for (int j = 0; j < n; ++j)
            for (int c = 0; c < f.channels; ++c)
                pcm[j * f.channels + c] = fake_opus_sample(data, static_cast<size_t>(len), static_cast<uint32_t>(j) * k, c);
        return n;
    };
    L.decoder_ctl = &fake_opus_ctl;
    L.strerror = [](int) -> const char * { return "fake opus error"; };
    return L;
}

/* The expected output of the fake for an Ogg Opus file: every packet in order, the pre-skip dropped, the length kept;
 * interleaved int16 at the decoder's output rate. Independent of the decoder's seeking and buffering. */
inline std::vector<int16_t> expected_opus_pcm(FileOps &ops, const std::string &path) {
    std::vector<int16_t> out;
    oggopus::OggOpusFile f(ops);
    if (f.open(path) != 0) return out;
    const unsigned ch = f.head().channels;
    const uint32_t rate = oggopus::decode_rate(f.head().input_rate);
    const uint32_t k = 48000u / rate;
    oggopus::Timeline tl;
    tl.start48 = f.start48();
    tl.preskip = f.head().preskip;
    tl.k = k;
    const uint64_t total = tl.length(f.end48());
    std::vector<int16_t> all;
    oggopus::Packet p;
    while (f.next(p) == 1) {
        const uint32_t n = oggopus::packet_samples48(p.data.data(), p.data.size()) / k;
        for (uint32_t j = 0; j < n; ++j)
            for (unsigned c = 0; c < ch; ++c)
                all.push_back(fake_opus_sample(p.data.data(), p.data.size(), j * k, static_cast<int>(c)));
    }
    const size_t skip = static_cast<size_t>(tl.skip()) * ch;
    if (all.size() > skip) out.assign(all.begin() + static_cast<long>(skip), all.end());
    if (out.size() > total * ch) out.resize(static_cast<size_t>(total) * ch);
    return out;
}

}  // namespace fakedec
