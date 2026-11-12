/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 */
#include "decoder.hpp"

#include "flac_decoder.hpp"
#include "opus_decoder.hpp"
#include "stream_scan.hpp"

#include <cstring>

namespace vinyl {

std::unique_ptr<Decoder> make_decoder(FileOps &ops, const std::string &path, const DecoderLibs &libs, DecodedInfo &info,
                                      int *err) {
    int e = 0;
    std::unique_ptr<Decoder> d;
    switch (container_of(ops, path)) {
    case Container::Flac:
        if (!libs.flac) e = kCodecMissing;
        else d.reset(new FlacDecoder(libs.flac));
        break;
    case Container::OggOpus:
        if (!libs.opus) e = kCodecMissing;
        else d.reset(new OpusFileDecoder(libs.opus));
        break;
    default: e = kCodecBadStream; break;
    }
    if (d) {
        e = d->open(ops, path, info);
        if (e < 0) d.reset();
    }
    if (err) *err = d ? 0 : e;
    return d;
}

bool is_compressed_take(FileOps &ops, const std::string &path) {
    Container c = container_of_name(path); /* the extension, as the take list does; the magic when there is none */
    if (c == Container::Unknown) c = container_of(ops, path);
    return c == Container::Flac || c == Container::OggOpus;
}

std::string decode_error_text(int err, const std::string &path) {
    const Container c = container_of_name(path);
    if (err == kCodecMissing) {
        if (c == Container::Flac) return "FLAC not installed (libflac14)";
        if (c == Container::OggOpus) return "Opus not installed (libopus0)";
        return codec_error_text(err);
    }
    if (err <= kCodecMissing && err >= kCodecBadStream) return codec_error_text(err);
    if (err < 0) return std::strerror(-err);
    return "no error";
}

}  // namespace vinyl
