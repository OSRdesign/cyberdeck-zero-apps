/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the row thumbnails (see thumbs.hpp).
 */
#include "thumbs.hpp"

#include "atomic_file.hpp"
#include "fileops.hpp"
#include "movie/video_decoder.hpp"
#include "record/av_libs.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace vhs {
namespace {

constexpr std::uint32_t kMagic = 0x54534856; /* "VHST" */

struct Header {
    std::uint32_t magic, w, h, stride, fmt, box_w, box_h;
};

bool read_file(FileOps &ops, const std::string &path, std::string &out, std::size_t max)
{
    return read_small_file(ops, path, out, max) == 0;
}

/* the first video packet of a movie (its bytes) */
bool first_video_packet(const AvLibs &av, const std::string &path, std::string &out, int &codec)
{
    av::FormatContext *ic = nullptr;
    if (av.open_input(&ic, path.c_str(), nullptr, nullptr) < 0 || !ic) return false;
    int vs = -1;
    const unsigned n = av.fmt_nb_streams(ic);
    for (unsigned i = 0; i < n && vs < 0; ++i) {
        av::CodecParameters *p = av.stream_par(av.fmt_stream(ic, i));
        if (av.at<int>(p, av.off.par_codec_type) == av::kMediaVideo) {
            vs = static_cast<int>(i);
            codec = av.at<int>(p, av.off.par_codec_id);
        }
    }
    bool ok = false;
    if (vs >= 0 && codec == av::kCodecMjpeg) {
        av::Packet *pkt = av.packet_alloc();
        for (int tries = 0; tries < 64 && av.read_frame(ic, pkt) >= 0; ++tries) {
            const bool mine = av.at<int>(pkt, av.off.pkt_stream_index) == vs;
            if (mine) {
                const std::uint8_t *d = av.at<std::uint8_t *>(pkt, av.off.pkt_data);
                out.assign(reinterpret_cast<const char *>(d), static_cast<std::size_t>(av.at<int>(pkt, av.off.pkt_size)));
                ok = true;
            }
            av.packet_unref(pkt);
            if (mine) break;
        }
        av.packet_free(&pkt);
    }
    av.close_input(&ic);
    return ok;
}

} // namespace

std::string cache_dir()
{
    const char *x = std::getenv("XDG_CACHE_HOME");
    if (x && x[0] == '/') return std::string(x) + "/vhs";
    const char *home = std::getenv("HOME");
    return std::string(home && home[0] ? home : "/tmp") + "/.cache/vhs";
}

std::string thumb_path(const std::string &dir, const SceneEntry &e)
{
    return dir + "/thumbs/" + e.name + "-" + std::to_string(e.size) + "-" + std::to_string(e.mtime) + ".raw";
}

bool thumb_load(const std::string &path, int box_w, int box_h, PixFmt fmt, Picture &out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    Header h{};
    if (!in.read(reinterpret_cast<char *>(&h), sizeof h)) return false;
    if (h.magic != kMagic || h.box_w != static_cast<std::uint32_t>(box_w) || h.box_h != static_cast<std::uint32_t>(box_h) ||
        h.fmt != static_cast<std::uint32_t>(fmt) || h.w == 0 || h.h == 0 || h.w > 4096 || h.h > 4096 || h.stride < h.w)
        return false;
    out = Picture();
    out.fmt = fmt;
    out.w = static_cast<int>(h.w);
    out.h = static_cast<int>(h.h);
    out.stride = static_cast<int>(h.stride);
    out.box_w = box_w;
    out.box_h = box_h;
    out.px.resize(static_cast<std::size_t>(h.stride) * h.h);
    return static_cast<bool>(in.read(reinterpret_cast<char *>(out.px.data()), static_cast<std::streamsize>(out.px.size())));
}

int thumb_save(FileOps &ops, const std::string &path, const Picture &p)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) make_dirs(ops, path.substr(0, slash));
    Header h{kMagic, static_cast<std::uint32_t>(p.w), static_cast<std::uint32_t>(p.h), static_cast<std::uint32_t>(p.stride),
             static_cast<std::uint32_t>(p.fmt), static_cast<std::uint32_t>(p.box_w), static_cast<std::uint32_t>(p.box_h)};
    std::string bytes(reinterpret_cast<const char *>(&h), sizeof h);
    bytes.append(reinterpret_cast<const char *>(p.px.data()), p.px.size());
    return atomic_write_file(ops, path, bytes);
}

bool make_thumb(FileOps &ops, const AvLibs *av, const SceneEntry &e, int box_w, int box_h, PixFmt fmt, Picture &out,
                std::string *why)
{
    std::string jpeg;
    if (e.kind == SceneKind::Photo) {
        if (!read_file(ops, e.path, jpeg, 64u << 20)) {
            if (why) *why = "cannot read";
            return false;
        }
    } else {
        if (!av) {
            if (why) *why = "no FFmpeg";
            return false;
        }
        int codec = -1;
        if (!first_video_packet(*av, e.path, jpeg, codec)) {
            if (codec >= 0 && codec != av::kCodecMjpeg) { /* another codec: its first picture through FFmpeg (M5) */
                if (!first_picture(*av, e.path, box_w, box_h, fmt, out, why)) return false;
                out.box_w = box_w;
                out.box_h = box_h;
                return true;
            }
            if (why) *why = "no video packet";
            return false;
        }
    }
    std::string err;
    if (!decode_jpeg_fit(reinterpret_cast<const std::uint8_t *>(jpeg.data()), jpeg.size(), box_w, box_h, fmt, out, &err)) {
        if (why) *why = err.empty() ? "cannot decode" : err;
        return false;
    }
    out.box_w = box_w;
    out.box_h = box_h;
    return true;
}

void thumb_remove(FileOps &ops, const std::string &dir, const std::string &name)
{
    std::vector<std::string> names;
    if (ops.list_dir(dir + "/thumbs", names) != 0) return;
    const std::string pre = name + "-";
    for (const std::string &n : names)
        if (n.compare(0, pre.size(), pre) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".raw") == 0) {
            /* the name is followed by "-<digits>-<digits>.raw": not another file whose name starts the same */
            const std::string rest = n.substr(pre.size(), n.size() - pre.size() - 4);
            const std::size_t dash = rest.find('-');
            if (dash != std::string::npos && rest.find_first_not_of("0123456789-") == std::string::npos)
                ops.unlink(dir + "/thumbs/" + n);
        }
}

} // namespace vhs
