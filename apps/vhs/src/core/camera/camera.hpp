/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: camera facts and discovery. The mode list is read from the camera at every open (ENUM_FMT / ENUM_FRAMESIZES /
 * ENUM_FRAMEINTERVALS, plan 062 16.2 C2): the CM969 lists other modes in each switch position, so nothing here assumes
 * one list.
 *
 * Discovery (brief 3.4) is by capability, not by name (the spike matched "UGREEN" only): every V4L2 node with
 * V4L2_CAP_VIDEO_CAPTURE in its device caps, streaming I/O, no M2M, offering MJPG or YUYV. Candidates are the
 * /dev/v4l/by-id/ *-video-index0 links (preferred), then the index-0 nodes that uvcvideo drives (sysfs) without such a
 * link. When several qualify: the remembered identity (settings camera_id), else the first by-id name in sort order.
 * The Pi's codec and ISP nodes are never opened (they are not uvcvideo and have no by-id link).
 */
#pragma once

#include "identity.hpp"
#include "v4l2_api.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vhs {

constexpr std::uint32_t make_fourcc(char a, char b, char c, char d)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}
constexpr std::uint32_t kMJPG = make_fourcc('M', 'J', 'P', 'G');
constexpr std::uint32_t kYUYV = make_fourcc('Y', 'U', 'Y', 'V');

std::string fourcc_text(std::uint32_t fourcc); /* "MJPG" */

/* One size of one format, with the frame rates the camera lists for it (whole fps, highest first). */
struct CamMode {
    std::uint32_t fourcc = 0;
    int w = 0, h = 0;
    std::vector<int> fps;
    bool has_fps(int f) const;
};

/* A mode asked for / in use. */
struct ModeReq {
    std::uint32_t fourcc = 0;
    int w = 0, h = 0;
    int fps = 30;
    bool valid() const { return fourcc != 0 && w > 0 && h > 0 && fps > 0; }
    bool same(const ModeReq &o) const { return fourcc == o.fourcc && w == o.w && h == o.h && fps == o.fps; }
};

std::string mode_text(const ModeReq &m);     /* "MJPG 1280x720 30 fps" */
std::string mode_arg(const ModeReq &m);      /* "MJPG:1280x720:30" (what parse_mode reads) */
/* "MJPG:1280x720:30", "yuyv:640x360" (30 fps), "MJPG:1280x720@30"; false on anything else. */
bool parse_mode(const std::string &text, ModeReq &out);

/* The fps of a frame interval num/den (1/30 -> 30; 333333/10000000 -> 30), 0 when invalid. */
int interval_fps(std::uint32_t num, std::uint32_t den);

/* The listed mode matching want's format and size, at want's fps when listed, else the listed rate nearest to it.
 * False when the camera lists no such format and size. */
bool resolve_mode(const std::vector<CamMode> &listed, const ModeReq &want, ModeReq &out);

/* Reads every format, size and rate the device lists (discrete sizes; a step-wise range is skipped). */
std::vector<CamMode> enumerate_modes(V4l2Api &api, int fd);
bool lists_format(const std::vector<CamMode> &modes, std::uint32_t fourcc);

/* QUERYCAP: a video capture device with streaming I/O, not a memory-to-memory codec. card / driver / bus: its names. */
struct DeviceInfo {
    bool capture = false;
    std::string card, driver, bus;
};
DeviceInfo query_device(V4l2Api &api, int fd);

/* Lists the names in a directory, sorted ("." and ".." skipped); empty when it cannot be read. */
std::vector<std::string> list_dir(const std::string &dir);

/* ------------------------------------------------------------------------------------------------- discovery */

/* A camera to open. */
struct CameraRef {
    std::string path; /* the node to open: a by-id link, /dev/videoN, or the fake camera's path */
    std::string name; /* the by-id name, else "videoN" */
    CamIdentity id;
    std::string card; /* QUERYCAP card name, when discovery opened it */
    int orientation = -1; /* 0.1.2: V4L2_CID_CAMERA_ORIENTATION when the kernel exposes it: 0 front, 1 back, 2 external; -1 unknown */
    /* 0.1.2: the finder had to leave the remembered camera (settings camera_id, `wanted`: not plugged in) for this one,
     * with two or more cameras present (with one the swap is silent, as in 0.1.1) */
    bool fallback = false;
    std::string wanted;
    std::string key() const { return id.stable_key(); }
};

/* One node as discovery saw it (the pick below is pure, unit-tested). */
struct Candidate {
    CameraRef ref;
    bool by_id = false;   /* found through /dev/v4l/by-id */
    bool capture = false; /* QUERYCAP: capture + streaming, not M2M */
    bool mjpg = false, yuyv = false;
    bool qualifies() const { return capture && (mjpg || yuyv); }
};

/* 0.1.2: a camera as the chooser lists it. `key` is the stable id (identity.hpp stable_key(): vid:pid + serial, else the USB
 * path, else the sysfs path: never /dev/videoN); `label` is distinct among the listed cameras. */
struct CameraEntry {
    std::string key;
    std::string name;     /* the product (else the card name), "USB FHD Webcam: USB FHD Webcam" cut to one half */
    std::string label;    /* name, plus " (front)" / " (back)" or " (USB 9.3)" or " #2" when the names repeat */
    std::string path;     /* what to open (not stored) */
    std::string port;     /* "9.3" (the USB ports under the bus), "" when unknown */
    int orientation = -1; /* 0 front, 1 back, 2 external, -1 unknown */
    bool operator==(const CameraEntry &o) const { return key == o.key && label == o.label; }
};

/* The qualifying candidates as chooser rows, in discovery order, one per identity (a second node of one device is
 * skipped), every label distinct. Pure. */
std::vector<CameraEntry> camera_entries(const std::vector<Candidate> &candidates);
/* "USB FHD Webcam: USB FHD Webcam" -> "USB FHD Webcam"; other names unchanged. */
std::string readable_camera_name(const std::string &card);

/* The candidate to use: of those that qualify, the one whose identity key is `remembered` when there is one; else the
 * first by-id one by name; else the first other one by name. -1 when none qualifies. */
int pick_camera(const std::vector<Candidate> &candidates, const std::string &remembered);
/* True when `remembered` is set, no qualifying candidate has it and two or more qualify (the fallback is announced). */
bool camera_fell_back(const std::vector<Candidate> &candidates, const std::string &remembered);

/* The system's candidates: the "-video-index0" links in dev_root/v4l/by-id, then the uvcvideo nodes of index 0 in
 * sys_root/sys/class/video4linux without a link (opened as dev_root/videoN). Each is opened for QUERYCAP and ENUM_FMT
 * and closed again (opening a camera that streams elsewhere is harmless; EBUSY comes only at S_FMT). */
std::vector<Candidate> scan_cameras(V4l2Api &api, const std::string &dev_root, const std::string &sys_root);

class CameraFinder {
public:
    virtual ~CameraFinder() = default;
    /* The camera to open now; false when there is none. Called from the capture thread. */
    virtual bool find(CameraRef &out) = 0;
    /* The identity to prefer when several cameras qualify (settings camera_id). Any thread. */
    virtual void prefer(const std::string &key) { (void)key; }
    /* 0.1.2: the usable cameras now (a scan: the nodes are opened for QUERYCAP and closed again). Any thread. */
    virtual std::vector<CameraEntry> list() { return {}; }
};

/* scan_cameras + pick_camera. */
std::unique_ptr<CameraFinder> make_system_finder(V4l2Api &api, std::string dev_root, std::string sys_root);
/* One fixed node (--device PATH): found while the path exists. */
std::unique_ptr<CameraFinder> make_fixed_finder(std::string path, std::string sys_root);

} // namespace vhs
