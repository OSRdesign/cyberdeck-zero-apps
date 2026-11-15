/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the hardware codec nodes (see codec_caps.hpp).
 */
#include "codec_caps.hpp"

#include "camera/camera.hpp"

#include <fstream>

namespace vhs {
namespace {

std::string first_line(const std::string &path)
{
    std::ifstream f(path);
    std::string s;
    if (f) std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

} // namespace

CodecCaps detect_codec_caps(const std::string &sys_root, const std::string &dev_root)
{
    CodecCaps c;
    const std::string dir = sys_root + "/sys/class/video4linux";
    for (const std::string &v : list_dir(dir)) {
        const std::string name = first_line(dir + "/" + v + "/name");
        const std::string node = dev_root + "/" + v;
        if (name == "bcm2835-codec-encode" && c.h264_encode.empty()) c.h264_encode = node;
        else if (name == "bcm2835-codec-decode" && c.h264_decode.empty()) c.h264_decode = node;
        else if (name == "rpi-hevc-dec" && c.hevc_decode.empty()) c.hevc_decode = node;
    }
    return c;
}

std::string codec_caps_text(const CodecCaps &c)
{
    auto v = [](const std::string &s) { return s.empty() ? std::string("none") : s; };
    return "codecs: h264 encode " + v(c.h264_encode) + ", h264 decode " + v(c.h264_decode) + ", hevc decode " +
           v(c.hevc_decode);
}

} // namespace vhs
