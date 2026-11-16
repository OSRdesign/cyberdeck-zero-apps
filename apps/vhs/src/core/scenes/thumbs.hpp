/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the row thumbnails (brief 9.1, 8): the first video packet of an MJPEG movie (its JPEG) or the photo, decoded with
 * TurboJPEG at the smallest scale that still covers the row's box and fitted to it (64x36 at 480x320, 56x32 at
 * 320x480, the class's size elsewhere), in the display's format; cached as
 * ~/.cache/vhs/thumbs/<name>-<size>-<mtime>.raw (a small header: magic, w, h, stride, format, box) and deleted with
 * their file. Movies of another codec: their first picture through FFmpeg's decoder (movie/video_decoder.hpp, M5). LVGL-free; the scenes worker runs it.
 */
#pragma once

#include "index.hpp"
#include "live/pipeline.hpp"

#include <string>

namespace vhs {

class FileOps;
struct AvLibs;

/* $XDG_CACHE_HOME/vhs, else ~/.cache/vhs. */
std::string cache_dir();
std::string thumb_path(const std::string &dir, const SceneEntry &e);

/* A cached thumbnail made for this box and format: false when absent, stale or made for another box. */
bool thumb_load(const std::string &path, int box_w, int box_h, PixFmt fmt, Picture &out);
int thumb_save(FileOps &ops, const std::string &path, const Picture &p);
/* Made from the file; false with why ("no thumbnail for H.264 in P1", "cannot read" ...). av may be null (photos). */
bool make_thumb(FileOps &ops, const AvLibs *av, const SceneEntry &e, int box_w, int box_h, PixFmt fmt, Picture &out,
                std::string *why);
/* Every cached thumbnail of a file name (all sizes and times). */
void thumb_remove(FileOps &ops, const std::string &dir, const std::string &name);

} // namespace vhs
