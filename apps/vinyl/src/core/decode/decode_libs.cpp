/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl decoders: the production tables of libmpg123, libvorbisfile and FFmpeg (see decode_libs.hpp): dlopen + dlsym at
 * the first use (RTLD_NOW | RTLD_LOCAL, every symbol checked, a readable reason when something is missing), as Cassette's
 * codec_libs.cpp. The handles are never closed.
 */
#include "decode_libs.hpp"

#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <mutex>

namespace vinyl {

bool Mpg123Lib::complete() const {
    return create && destroy && param && format_none && format && open && close && getformat && read && seek && tell &&
           length && scan && strerror;
}

bool VorbisFileLib::complete() const {
    return open_callbacks && clear && info && pcm_total && read && pcm_seek && pcm_tell && bitrate;
}

bool FfmpegLib::complete() const {
    return avutil_version && log_set_level && frame_alloc && frame_free && frame_unref && opt_get_int &&
           opt_get_sample_fmt && opt_get_chlayout && chlayout_default && chlayout_uninit && avformat_version && open_input &&
           find_stream_info && find_best_stream && close_input && read_frame && seek_frame && alloc_context &&
           free_context && parameters_to_context && open2 && send_packet && receive_frame && flush_buffers &&
           packet_alloc && packet_free && packet_unref && swr_alloc_set_opts2 && swr_init && swr_free && swr_convert;
}

namespace av {
Offsets offsets_for(int lavf_major) {
    /* the same for libavformat 60 (FFmpeg 6.1: AVStream has its av_class already), 61 (7.x) and 62 (8.x): checked
     * against the 6.1 and 7.1 headers by tests/abi; an older major is not loaded */
    (void)lavf_major;
    return Offsets();
}
}  // namespace av

namespace {

void *open_first(const char *const *names, size_t n, std::string &loaded, std::string &why) {
    for (size_t i = 0; i < n; ++i) {
        if (void *h = ::dlopen(names[i], RTLD_NOW | RTLD_LOCAL)) {
            loaded = names[i];
            return h;
        }
        const char *e = ::dlerror();
        why = std::string(names[i]) + ": " + (e ? e : "dlopen failed");
    }
    return nullptr;
}

template <typename Fn> bool sym(void *h, Fn &fn, const char *name, const std::string &lib, std::string &why) {
    void *p = ::dlsym(h, name);
    fn = reinterpret_cast<Fn>(p);
    if (!p && why.empty()) why = lib + " lacks " + name;
    return p != nullptr;
}

struct Mpg123Holder {
    std::once_flag once;
    Mpg123Lib lib;
    bool ok = false;
    std::string why;
    void load() {
        static const char *const names[] = {"libmpg123.so.0"};
        void *h = open_first(names, 1, lib.name, why);
        if (!h) return;
        why.clear();
        const std::string &n = lib.name;
        sym(h, lib.init, "mpg123_init", n, why);
        why.clear(); /* optional */
        sym(h, lib.create, "mpg123_new", n, why);
        sym(h, lib.destroy, "mpg123_delete", n, why);
        sym(h, lib.param, "mpg123_param", n, why);
        sym(h, lib.format_none, "mpg123_format_none", n, why);
        sym(h, lib.format, "mpg123_format", n, why);
        sym(h, lib.open, "mpg123_open", n, why);
        sym(h, lib.close, "mpg123_close", n, why);
        sym(h, lib.getformat, "mpg123_getformat", n, why);
        sym(h, lib.read, "mpg123_read", n, why);
        sym(h, lib.seek, "mpg123_seek", n, why);
        sym(h, lib.tell, "mpg123_tell", n, why);
        sym(h, lib.length, "mpg123_length", n, why);
        sym(h, lib.scan, "mpg123_scan", n, why);
        sym(h, lib.strerror, "mpg123_strerror", n, why);
        ok = lib.complete();
        if (ok && lib.init) lib.init();
    }
};

struct VorbisHolder {
    std::once_flag once;
    VorbisFileLib lib;
    bool ok = false;
    std::string why;
    void load() {
        static const char *const names[] = {"libvorbisfile.so.3"};
        void *h = open_first(names, 1, lib.name, why);
        if (!h) return;
        why.clear();
        const std::string &n = lib.name;
        sym(h, lib.open_callbacks, "ov_open_callbacks", n, why);
        sym(h, lib.clear, "ov_clear", n, why);
        sym(h, lib.info, "ov_info", n, why);
        sym(h, lib.pcm_total, "ov_pcm_total", n, why);
        sym(h, lib.read, "ov_read", n, why);
        sym(h, lib.pcm_seek, "ov_pcm_seek", n, why);
        sym(h, lib.pcm_tell, "ov_pcm_tell", n, why);
        sym(h, lib.bitrate, "ov_bitrate", n, why);
        ok = lib.complete();
    }
};

struct FfmpegHolder {
    std::once_flag once;
    FfmpegLib lib;
    bool ok = false;
    std::string why;
    std::atomic<bool> loaded{false};
    void load() {
        struct Set {
            const char *fmt, *codec, *util, *swr;
            int major;
        };
        static const Set sets[] = {
            {"libavformat.so.61", "libavcodec.so.61", "libavutil.so.59", "libswresample.so.5", 61},
            {"libavformat.so.62", "libavcodec.so.62", "libavutil.so.60", "libswresample.so.6", 62},
            {"libavformat.so.60", "libavcodec.so.60", "libavutil.so.58", "libswresample.so.4", 60},
        };
        for (const Set &s : sets) {
            std::string loaded_name, w;
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
            lib.off = av::offsets_for(s.major);
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
                    lib.off = av::offsets_for(major);
                }
                lib.log_set_level(av::kLogQuiet);
                loaded.store(true);
                std::fprintf(stderr, "[vinyl] FFmpeg loaded for a tier-2 file: %s (libavformat %d)\n", lib.name.c_str(),
                             lib.lavf_major);
                return;
            }
        }
    }
};

Mpg123Holder &mpg_holder() {
    static Mpg123Holder h;
    return h;
}
VorbisHolder &vorbis_holder() {
    static VorbisHolder h;
    return h;
}
FfmpegHolder &ffmpeg_holder() {
    static FfmpegHolder h;
    return h;
}

}  // namespace

const Mpg123Lib *real_mpg123(std::string *why) {
    Mpg123Holder &h = mpg_holder();
    std::call_once(h.once, [&h] { h.load(); });
    if (!h.ok && why) *why = h.why;
    return h.ok ? &h.lib : nullptr;
}

const VorbisFileLib *real_vorbisfile(std::string *why) {
    VorbisHolder &h = vorbis_holder();
    std::call_once(h.once, [&h] { h.load(); });
    if (!h.ok && why) *why = h.why;
    return h.ok ? &h.lib : nullptr;
}

const FfmpegLib *real_ffmpeg(std::string *why) {
    FfmpegHolder &h = ffmpeg_holder();
    std::call_once(h.once, [&h] { h.load(); });
    if (!h.ok && why) *why = h.why;
    return h.ok ? &h.lib : nullptr;
}

bool ffmpeg_loaded() { return ffmpeg_holder().loaded.load(); }

}  // namespace vinyl
