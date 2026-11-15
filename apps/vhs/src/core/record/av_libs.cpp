/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: FFmpeg for recording (see av_libs.hpp).
 */
#include "av_libs.hpp"

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace vhs {
namespace av {

Offsets offsets_for(int lavf_major)
{
    Offsets o; /* the 61 set (FFmpeg 7.x) */
    if (lavf_major == 60) { /* FFmpeg 6.1 */
        o.par_width = 56;
        o.par_height = 60;
        o.ctx_time_base = 100;
        o.ctx_sample_fmt = 360;
        o.frame_sample_rate = 208;
        o.frame_ch_layout = 448;
    } else if (lavf_major == 62) { /* FFmpeg 8.0: AVFrame lost its deprecated fields */
        o.frame_sample_rate = 180;
        o.frame_ch_layout = 384;
    }
    return o;
}

const LibSet kLibSets[kLibSetCount] = {
    {"libavformat.so.61", "libavcodec.so.61", "libavutil.so.59", 61}, /* FFmpeg 7.x: Debian 13 */
    {"libavformat.so.62", "libavcodec.so.62", "libavutil.so.60", 62}, /* FFmpeg 8.x: Ubuntu 26.04 */
    {"libavformat.so.60", "libavcodec.so.60", "libavutil.so.58", 60}, /* FFmpeg 6.1: the build box, tests only */
};

} // namespace av

namespace {

std::atomic<bool> g_loaded{false};

template <typename Fn> void sym(void *h, Fn &fn, const char *name, bool &ok, std::string &why)
{
    void *p = ::dlsym(h, name);
    if (!p) {
        ok = false;
        if (why.empty()) why = std::string("lacks ") + name;
    }
    fn = reinterpret_cast<Fn>(p);
}

} // namespace

bool AvLibs::complete() const
{
    return log_set_level && frame_alloc && frame_free && frame_get_buffer && frame_make_writable && opt_set &&
           opt_set_int && opt_get_int && opt_set_chlayout && chlayout_default && chlayout_copy && chlayout_uninit &&
           dict_set && dict_free && strerror && alloc_output_context2 && free_context && new_stream && avio_open &&
           avio_closep && avio_flush && write_header && interleaved_write_frame && write_trailer && open_input &&
           find_stream_info && read_frame && close_input && find_encoder_by_name && alloc_context &&
           free_codec_context && open2 && send_frame && receive_packet && parameters_from_context && parameters_copy &&
           packet_alloc && packet_free && packet_unref;
}

std::string AvLibs::error_text(int err) const
{
    char buf[128] = {0};
    if (strerror && strerror(err, buf, sizeof buf) == 0) return buf;
    return "error " + std::to_string(err);
}

const AvLibs *real_av_libs(std::string *why_out)
{
    static std::once_flag once;
    static AvLibs lib;
    static bool ok = false;
    static std::string why;
    std::call_once(once, [] {
        for (const av::LibSet &s : av::kLibSets) {
            void *uh = ::dlopen(s.util, RTLD_NOW | RTLD_LOCAL);
            void *ch = uh ? ::dlopen(s.codec, RTLD_NOW | RTLD_LOCAL) : nullptr;
            void *fh = ch ? ::dlopen(s.fmt, RTLD_NOW | RTLD_LOCAL) : nullptr;
            if (!fh) {
                if (why.empty()) {
                    const char *e = ::dlerror();
                    why = std::string(s.fmt) + ": " + (e ? e : "not found");
                }
                continue;
            }
            bool all = true;
            std::string w;
            lib = AvLibs();
            lib.name = std::string(s.fmt) + " " + s.codec + " " + s.util;
            lib.lavf_major = s.major;
            lib.off = av::offsets_for(s.major);
            sym(uh, lib.log_set_level, "av_log_set_level", all, w);
            sym(uh, lib.frame_alloc, "av_frame_alloc", all, w);
            sym(uh, lib.frame_free, "av_frame_free", all, w);
            sym(uh, lib.frame_get_buffer, "av_frame_get_buffer", all, w);
            sym(uh, lib.frame_make_writable, "av_frame_make_writable", all, w);
            sym(uh, lib.opt_set, "av_opt_set", all, w);
            sym(uh, lib.opt_set_int, "av_opt_set_int", all, w);
            sym(uh, lib.opt_get_int, "av_opt_get_int", all, w);
            sym(uh, lib.opt_set_chlayout, "av_opt_set_chlayout", all, w);
            sym(uh, lib.chlayout_default, "av_channel_layout_default", all, w);
            sym(uh, lib.chlayout_copy, "av_channel_layout_copy", all, w);
            sym(uh, lib.chlayout_uninit, "av_channel_layout_uninit", all, w);
            sym(uh, lib.dict_set, "av_dict_set", all, w);
            sym(uh, lib.dict_free, "av_dict_free", all, w);
            sym(uh, lib.strerror, "av_strerror", all, w);
            sym(fh, lib.alloc_output_context2, "avformat_alloc_output_context2", all, w);
            sym(fh, lib.free_context, "avformat_free_context", all, w);
            sym(fh, lib.new_stream, "avformat_new_stream", all, w);
            sym(fh, lib.avio_open, "avio_open", all, w);
            sym(fh, lib.avio_closep, "avio_closep", all, w);
            sym(fh, lib.avio_flush, "avio_flush", all, w);
            sym(fh, lib.write_header, "avformat_write_header", all, w);
            sym(fh, lib.interleaved_write_frame, "av_interleaved_write_frame", all, w);
            sym(fh, lib.write_trailer, "av_write_trailer", all, w);
            sym(fh, lib.open_input, "avformat_open_input", all, w);
            sym(fh, lib.find_stream_info, "avformat_find_stream_info", all, w);
            sym(fh, lib.read_frame, "av_read_frame", all, w);
            sym(fh, lib.close_input, "avformat_close_input", all, w);
            sym(ch, lib.find_encoder_by_name, "avcodec_find_encoder_by_name", all, w);
            sym(ch, lib.alloc_context, "avcodec_alloc_context3", all, w);
            sym(ch, lib.free_codec_context, "avcodec_free_context", all, w);
            sym(ch, lib.open2, "avcodec_open2", all, w);
            sym(ch, lib.send_frame, "avcodec_send_frame", all, w);
            sym(ch, lib.receive_packet, "avcodec_receive_packet", all, w);
            sym(ch, lib.parameters_from_context, "avcodec_parameters_from_context", all, w);
            sym(ch, lib.parameters_copy, "avcodec_parameters_copy", all, w);
            sym(ch, lib.packet_alloc, "av_packet_alloc", all, w);
            sym(ch, lib.packet_free, "av_packet_free", all, w);
            sym(ch, lib.packet_unref, "av_packet_unref", all, w);
            if (!all) {
                why = lib.name + " " + w;
                continue;
            }
            lib.log_set_level(av::kLogError);
            ok = true;
            g_loaded = true;
            std::fprintf(stderr, "[vhs] ffmpeg: %s loaded\n", lib.name.c_str());
            return;
        }
        if (why.empty()) why = "no FFmpeg libraries";
    });
    if (!ok) {
        if (why_out) *why_out = why;
        return nullptr;
    }
    return &lib;
}

bool av_libs_loaded()
{
    return g_loaded.load();
}

} // namespace vhs
