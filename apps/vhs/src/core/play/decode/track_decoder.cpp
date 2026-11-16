/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Vinyl 0.3.0 (apps/vinyl at fdad829) for VHS (brief 3.1, 10); VHS changes are marked VHS.
 *
 * Vinyl decoders: open_track and the open errors' sentences (see track_decoder.hpp). VHS: FFmpeg only; Vinyl's probe by
 * magic bytes, the tier-1 decoders and the WAV / AIFF reader are not here.
 */
#include "track_decoder.hpp"

#include <cerrno>
#include <cstring>

namespace vhs {

const char *codec_name(Codec c) {
    switch (c) {
    case Codec::Ffmpeg: return "FFmpeg";
    default: return "";
    }
}

DecodeLibs real_decode_libs() {
    DecodeLibs l;
    l.ffmpeg = [] { return real_ffmpeg(); };
    return l;
}

std::unique_ptr<TrackDecoder> open_track(const std::string &path, const DecodeLibs &libs, TrackInfo &info, int *err) {
    const FfmpegLib *ff = libs.ffmpeg ? libs.ffmpeg() : nullptr;
    if (!ff) {
        if (err) *err = kNeedsFfmpeg;
        return nullptr;
    }
    std::unique_ptr<TrackDecoder> d = make_ffmpeg_decoder(ff);
    const int r = d->open(path, info);
    if (r != 0) {
        if (err) *err = r;
        return nullptr;
    }
    if (err) *err = 0;
    return d;
}

std::string track_error_text(int err, const std::string &path) {
    (void)path;
    switch (err) {
    case kNeedsFfmpeg: return "The FFmpeg libraries could not be loaded. Reinstall VHS.";
    case kNotAudio: return "This file cannot be read.";
    case kNoAudioStream: return "This video has no sound.";
    case kDecodeFailed: return "The sound of this file is damaged or in a variant VHS cannot read.";
    default: break;
    }
    if (err == -ENOENT) return "The file is gone (moved, renamed or on a removed drive).";
    if (err == -EACCES || err == -EPERM) return "The file cannot be read (permission).";
    if (err < 0 && err > -4096) return std::string("The file cannot be read: ") + std::strerror(-err) + ".";
    return "The file cannot be played.";
}

}  // namespace vhs
