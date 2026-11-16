/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the hardware codec nodes, found by name under /sys/class/video4linux/ * /name (brief 4.3, plan 3.2). P1 records
 * MJPEG copy everywhere and only logs these (and prints them in --list); P2's codec picker uses them (hardware H.264
 * where bcm2835-codec-encode exists: Zero 2 W, Pi 3A+, Pi 4B; x264 elsewhere). No node is opened. LVGL-free.
 */
#pragma once

#include <string>

namespace vhs {

struct CodecCaps {
    std::string h264_encode; /* "/dev/video11" for bcm2835-codec-encode, "" when absent */
    std::string h264_decode; /* bcm2835-codec-decode */
    std::string hevc_decode; /* rpi-hevc-dec */
};

/* sys_root "" = the real system. */
CodecCaps detect_codec_caps(const std::string &sys_root = "", const std::string &dev_root = "/dev");
/* "codecs: h264 encode /dev/video11, h264 decode /dev/video10, hevc decode none" */
std::string codec_caps_text(const CodecCaps &c);

} // namespace vhs
