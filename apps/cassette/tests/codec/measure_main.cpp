/*
 * SPDX-License-Identifier: MIT
 *
 * cassette-codec-measure: the FLAC compression ratios and Opus sizes behind the size model (codec/formats.hpp), with
 * the real libraries (run it with LD_LIBRARY_PATH at a private root, tests/codec/run.sh builds it).
 *   cassette-codec-measure                 synthetic speech and music at several capture formats and noise floors
 *   cassette-codec-measure FILE.wav ...    WAV files (any WAV Cassette or arecord wrote, cut headers included)
 * Ratio = FLAC bytes after the header / PCM bytes. Opus: bytes per minute measured vs the model's target figure.
 * Encode speed is this PC's, not a board's.
 */
#include "fileops.hpp"
#include "flac_encoder.hpp"
#include "formats.hpp"
#include "opus_encoder.hpp"
#include "siggen.hpp"
#include "wav.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>

using namespace cassette;

namespace {

struct Result {
    double ratio = 0;   /* FLAC: stream bytes / PCM bytes; Opus: bytes per minute / model */
    double per_min = 0; /* bytes per minute after the header */
    double rt = 0;      /* real-time factor on this machine */
    bool ok = false;
};

double seconds_of(const AudioFormat &f, size_t frames) { return static_cast<double>(frames) / f.rate; }

Result run(Encoder &e, const sig::Bytes &pcm, size_t frames) {
    Result r;
    MemoryByteSink sink;
    if (e.start(sink, {{"TITLE", "measure"}}) != 0) return r;
    const uint64_t header = sink.size();
    const AudioFormat &f = e.format();
    const size_t fb = f.channels * (f.bits / 8u);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t at = 0; at < frames; at += 4800)
        if (e.write(pcm.data() + at * fb, std::min<size_t>(4800, frames - at)) != 0) return r;
    if (e.finish() != 0) return r;
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double stream = static_cast<double>(sink.size() - header);
    r.ratio = stream / (static_cast<double>(frames) * fb);
    r.per_min = stream / seconds_of(f, frames) * 60.0;
    r.rt = dt > 0 ? seconds_of(f, frames) / dt : 0;
    r.ok = true;
    return r;
}

Result flac(const FlacLib *lib, const AudioFormat &f, const sig::Bytes &pcm, size_t frames, unsigned level) {
    FlacConfig c;
    c.fmt = f;
    c.level = level;
    FlacEncoder e(lib, c);
    return run(e, pcm, frames);
}

Result opus(const OpusEncLib *lib, const AudioFormat &f, const sig::Bytes &pcm, size_t frames, const FormatChoice &ch) {
    OpusConfig c;
    c.fmt = f;
    c.bitrate_bps = ch.opus_bitrate_bps;
    c.application = ch.opus_app;
    c.force_mono = ch.opus_force_mono;
    OpusEncoder e(lib, c);
    Result r = run(e, pcm, frames);
    if (r.ok) r.ratio = r.per_min / static_cast<double>(opus_bytes_per_minute(ch.opus_bitrate_bps));
    return r;
}

const char *fmt_text(const AudioFormat &f) {
    static char b[64];
    std::snprintf(b, sizeof b, "%5.1f kHz %s %2u-bit", f.rate / 1000.0, f.channels == 1 ? "mono  " : "stereo",
                  static_cast<unsigned>(f.bits));
    return b;
}

void opus_rows(const OpusEncLib *lib, const AudioFormat &f, const sig::Bytes &pcm, size_t frames, const char *what) {
    if (!lib || !opus_rate_ok(f.rate)) return;
    FormatInputs in;
    in.state = CaptureState::Known;
    in.capture = f;
    in.codecs.flac = in.codecs.opus = true;
    for (const FormatChoice &ch : list_formats(in)) {
        if (ch.key.format != RecFormat::Opus) continue;
        const Result r = opus(lib, f, pcm, frames, ch);
        if (r.ok)
            std::printf("  %-26s %-8s %-14s %9.0f B/min measured, %9llu model, %.2f of the model, %.0fx rt\n",
                        fmt_text(f), what, ch.name.c_str(), r.per_min,
                        static_cast<unsigned long long>(opus_bytes_per_minute(ch.opus_bitrate_bps)), r.ratio, r.rt);
    }
}

void synthetic(const FlacLib *fl, const OpusEncLib *ol, double seconds) {
    const AudioFormat fmts[] = {{48000, 1, 16}, {48000, 2, 16}, {44100, 2, 16}, {16000, 1, 16}, {48000, 1, 24}, {48000, 2, 24}};
    const double noises[] = {-80.0, -70.0, -60.0};
    std::printf("FLAC level 5, synthetic signals (ratio = FLAC stream / PCM):\n");
    for (const AudioFormat &f : fmts) {
        const size_t frames = static_cast<size_t>(seconds * f.rate);
        for (int kind = 0; kind < 2; ++kind) {
            std::printf("  %-26s %-7s", fmt_text(f), kind ? "music" : "speech");
            for (double n : noises) {
                const sig::Bytes pcm = kind ? sig::music(f, frames, n) : sig::speech(f, frames, n);
                const Result r = flac(fl, f, pcm, frames, 5);
                std::printf("  noise %3.0f dBFS: %5.1f %%", n, r.ok ? 100 * r.ratio : -1.0);
            }
            std::printf("\n");
        }
    }
    std::printf("FLAC levels, 48 kHz mono 16-bit, noise -70 dBFS:\n");
    {
        const AudioFormat f{48000, 1, 16};
        const size_t frames = static_cast<size_t>(seconds * f.rate);
        for (int kind = 0; kind < 2; ++kind) {
            const sig::Bytes pcm = kind ? sig::music(f, frames) : sig::speech(f, frames);
            std::printf("  %-7s", kind ? "music" : "speech");
            for (unsigned level : {0u, 5u, 8u}) {
                const Result r = flac(fl, f, pcm, frames, level);
                std::printf("  level %u: %5.1f %% (%.0fx rt)", level, r.ok ? 100 * r.ratio : -1.0, r.rt);
            }
            std::printf("\n");
        }
    }
    std::printf("Opus presets, synthetic signals, noise -70 dBFS:\n");
    for (const AudioFormat &f : {AudioFormat{48000, 1, 16}, AudioFormat{48000, 2, 16}, AudioFormat{16000, 1, 16}}) {
        const size_t frames = static_cast<size_t>(seconds * f.rate);
        opus_rows(ol, f, sig::speech(f, frames), frames, "speech");
        opus_rows(ol, f, sig::music(f, frames), frames, "music");
    }
}

int one_file(const FlacLib *fl, const OpusEncLib *ol, const std::string &path) {
    FileOps &ops = posix_file_ops();
    const HeaderInfo h = parse_header(ops, path);
    if (!h.ok) {
        std::printf("%s: not a WAV file I can read (%s)\n", path.c_str(), h.error.c_str());
        return 1;
    }
    const uint64_t bytes = h.actual_data - h.actual_data % h.block_align;
    const size_t frames = static_cast<size_t>(bytes / h.block_align);
    sig::Bytes pcm(bytes);
    int fd = ops.open(path, O_RDONLY, 0);
    if (fd < 0 || ops.pread(fd, pcm.data(), pcm.size(), h.data_offset) != static_cast<ssize_t>(pcm.size())) {
        std::printf("%s: read failed\n", path.c_str());
        if (fd >= 0) ops.close(fd);
        return 1;
    }
    ops.close(fd);
    std::printf("%s: %s, %.1f s\n", path.c_str(), fmt_text(h.fmt), seconds_of(h.fmt, frames));
    if (fl) {
        std::printf("  FLAC");
        for (unsigned level : {0u, 5u, 8u}) {
            const Result r = flac(fl, h.fmt, pcm, frames, level);
            std::printf("  level %u: %5.1f %%", level, r.ok ? 100 * r.ratio : -1.0);
        }
        std::printf("\n");
    }
    opus_rows(ol, h.fmt, pcm, frames, "file");
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    std::string fwhy, owhy;
    const FlacLib *fl = real_flac(&fwhy);
    const OpusEncLib *ol = real_opusenc(&owhy);
    std::printf("libFLAC: %s\nlibopusenc: %s\n", fl ? (fl->name + " " + fl->version).c_str() : fwhy.c_str(),
                ol ? (ol->name + " " + ol->version).c_str() : owhy.c_str());
    if (!fl) return 2;
    double seconds = 60;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else files.push_back(argv[i]);
    }
    if (files.empty()) {
        synthetic(fl, ol, seconds);
        return 0;
    }
    int rc = 0;
    for (const std::string &f : files) rc |= one_file(fl, ol, f);
    return rc;
}
