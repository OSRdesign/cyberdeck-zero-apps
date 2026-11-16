/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Vinyl 0.3.0 (apps/vinyl at fdad829) for VHS (brief 3.1, 10); VHS changes are marked VHS.
 *
 * Vinyl decoders: the production table of FFmpeg (see decode_libs.hpp): dlopen + dlsym at the first use (RTLD_NOW |
 * RTLD_LOCAL, every symbol checked, a readable reason when something is missing). The handles are never closed.
 * VHS: the mpg123 and vorbisfile tables are gone; AVFormatContext's start_time and duration (new here) sit 32 bytes
 * earlier in the 60 set than in the 61 / 62 sets.
 */
#include "decode_libs.hpp"

#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <mutex>

namespace vhs {

bool FfmpegLib::complete() const {
    return avutil_version && log_set_level && frame_alloc && frame_free && frame_unref && opt_get_int &&
           opt_get_sample_fmt && opt_get_chlayout && chlayout_default && chlayout_uninit && avformat_version && open_input &&
           find_stream_info && find_best_stream && close_input && read_frame && seek_frame && alloc_context &&
           free_context && parameters_to_context && open2 && send_packet && receive_frame && flush_buffers &&
           packet_alloc && packet_free && packet_unref && swr_alloc_set_opts2 && swr_init && swr_free && swr_convert;
}

namespace dav {
Offsets offsets_for(int lavf_major) {
    /* the same for libavformat 60 (FFmpeg 6.1: AVStream has its av_class already), 61 (7.x) and 62 (8.x): checked
     * against the 6.1, 7.1 and 8.0 headers by tests/abi; an older major is not loaded. VHS: AVFormatContext.duration
     * moved with the removal of the deprecated fields (72 in the 60 set, 104 after) */
    Offsets o;
    if (lavf_major == 60) {
        o.fmt_start_time = 64;
        o.fmt_duration = 72;
    }
    return o;
}

const LibSet kLibSets[kLibSetCount] = {
    {"libavformat.so.61", "libavcodec.so.61", "libavutil.so.59", "libswresample.so.5", 61}, /* FFmpeg 7.x: Debian 13 */
    {"libavformat.so.62", "libavcodec.so.62", "libavutil.so.60", "libswresample.so.6", 62}, /* FFmpeg 8.x: Ubuntu 26.04 */
    {"libavformat.so.60", "libavcodec.so.60", "libavutil.so.58", "libswresample.so.4", 60}, /* FFmpeg 6.1: the build box, tests only */
};
}  // namespace dav

namespace {

template <typename Fn> bool sym(void *h, Fn &fn, const char *name, const std::string &lib, std::string &why) {
    void *p = ::dlsym(h, name);
    fn = reinterpret_cast<Fn>(p);
    if (!p && why.empty()) why = lib + " lacks " + name;
    return p != nullptr;
}

struct FfmpegHolder {
    std::once_flag once;
    FfmpegLib lib;
    bool ok = false;
    std::string why;
    std::atomic<bool> loaded{false};
    void load() {
        for (const dav::LibSet &s : dav::kLibSets) {
            void *fh = ::dlopen(s.fmt, RTLD_NOW | RTLD_LOCAL);
            if (!fh) {
                const char *e = ::dlerror();
                why = std::string(s.fmt) + ": " + (e ? e : "dlopen failed");
                continue;
            }
            void *ch = ::dlopen(s.codec, RTLD_NOW | RTLD_LOCAL);
            void *uh = ::dlopen(s.util, RTLD_NOW | RTLD_LOCAL);
            void *sh = ::dlopen(s.swr, RTLD_NOW | RTLD_LOCAL);
            if (!ch || !uh || !sh) {
                const char *e = ::dlerror();
                why = std::string(s.fmt) + " set incomplete: " + (e ? e : "a library is missing");
                continue; /* the handles stay mapped (never closed): a rare case */
            }
            lib = FfmpegLib();
            lib.name = std::string(s.fmt) + " " + s.codec + " " + s.util + " " + s.swr;
            lib.lavf_major = s.major;
            lib.off = dav::offsets_for(s.major);
            why.clear();
            const std::string n = lib.name;
            sym(uh, lib.avutil_version, "avutil_version", n, why);
            sym(uh, lib.log_set_level, "av_log_set_level", n, why);
            sym(uh, lib.frame_alloc, "av_frame_alloc", n, why);
            sym(uh, lib.frame_free, "av_frame_free", n, why);
            sym(uh, lib.frame_unref, "av_frame_unref", n, why);
            sym(uh, lib.opt_get_int, "av_opt_get_int", n, why);
            sym(uh, lib.opt_get_sample_fmt, "av_opt_get_sample_fmt", n, why);
            sym(uh, lib.opt_get_chlayout, "av_opt_get_chlayout", n, why);
            sym(uh, lib.chlayout_default, "av_channel_layout_default", n, why);
            sym(uh, lib.chlayout_uninit, "av_channel_layout_uninit", n, why);
            sym(fh, lib.avformat_version, "avformat_version", n, why);
            sym(fh, lib.open_input, "avformat_open_input", n, why);
            sym(fh, lib.find_stream_info, "avformat_find_stream_info", n, why);
            sym(fh, lib.find_best_stream, "av_find_best_stream", n, why);
            sym(fh, lib.close_input, "avformat_close_input", n, why);
            sym(fh, lib.read_frame, "av_read_frame", n, why);
            sym(fh, lib.seek_frame, "av_seek_frame", n, why);
            sym(ch, lib.alloc_context, "avcodec_alloc_context3", n, why);
            sym(ch, lib.free_context, "avcodec_free_context", n, why);
            sym(ch, lib.parameters_to_context, "avcodec_parameters_to_context", n, why);
            sym(ch, lib.open2, "avcodec_open2", n, why);
            sym(ch, lib.send_packet, "avcodec_send_packet", n, why);
            sym(ch, lib.receive_frame, "avcodec_receive_frame", n, why);
            sym(ch, lib.flush_buffers, "avcodec_flush_buffers", n, why);
            sym(ch, lib.packet_alloc, "av_packet_alloc", n, why);
            sym(ch, lib.packet_free, "av_packet_free", n, why);
            sym(ch, lib.packet_unref, "av_packet_unref", n, why);
            sym(sh, lib.swr_alloc_set_opts2, "swr_alloc_set_opts2", n, why);
            sym(sh, lib.swr_init, "swr_init", n, why);
            sym(sh, lib.swr_free, "swr_free", n, why);
            sym(sh, lib.swr_convert, "swr_convert", n, why);
            ok = lib.complete();
            if (ok) {
                /* the major the library reports wins over the file name (a distribution could ship another) */
                const int major = static_cast<int>(lib.avformat_version() >> 16);
                if (major != s.major) {
                    lib.lavf_major = major;
                    lib.off = dav::offsets_for(major);
                }
                lib.log_set_level(dav::kLogQuiet);
                loaded.store(true);
                std::fprintf(stderr, "[vhs] FFmpeg loaded for playback: %s (libavformat %d)\n", lib.name.c_str(), lib.lavf_major);
                return;
            }
        }
    }
};

FfmpegHolder &ffmpeg_holder() {
    static FfmpegHolder h;
    return h;
}

}  // namespace

const FfmpegLib *real_ffmpeg(std::string *why) {
    FfmpegHolder &h = ffmpeg_holder();
    std::call_once(h.once, [&h] { h.load(); });
    if (!h.ok && why) *why = h.why;
    return h.ok ? &h.lib : nullptr;
}

bool ffmpeg_loaded() { return ffmpeg_holder().loaded.load(); }

}  // namespace vhs
