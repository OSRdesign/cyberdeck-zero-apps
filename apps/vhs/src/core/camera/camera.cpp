/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: camera facts and discovery (see camera.hpp).
 */
#include "camera.hpp"

#include <linux/videodev2.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>

namespace vhs {
namespace {

constexpr std::uint32_t kCidCameraOrientation = 0x009a0922; /* V4L2_CID_CAMERA_ORIENTATION (camera class base + 34) */

bool ends_with(const std::string &s, const std::string &tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

bool listed(const std::vector<CamMode> &modes, std::uint32_t fourcc, int w, int h)
{
    for (const CamMode &m : modes)
        if (m.fourcc == fourcc && m.w == w && m.h == h) return true;
    return false;
}

std::string read_line(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return "";
    char buf[64] = {0};
    const char *r = std::fgets(buf, sizeof buf, f);
    std::fclose(f);
    if (!r) return "";
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

int video_number(const std::string &name)
{
    if (name.compare(0, 5, "video") != 0 || name.size() == 5) return -1;
    for (std::size_t i = 5; i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return -1;
    return std::atoi(name.c_str() + 5);
}

/* QUERYCAP and the formats of one node (opened and closed again). */
void probe(V4l2Api &api, Candidate &c)
{
    const int fd = api.open(c.ref.path.c_str());
    if (fd < 0) return;
    const DeviceInfo info = query_device(api, fd);
    c.capture = info.capture;
    c.ref.card = info.card;
    if (info.capture) {
        struct v4l2_control oc;
        std::memset(&oc, 0, sizeof oc);
        oc.id = kCidCameraOrientation;
        if (api.ioctl(fd, VIDIOC_G_CTRL, &oc) == 0 && oc.value >= 0 && oc.value <= 2) c.ref.orientation = oc.value;
        for (std::uint32_t i = 0; i < 64; ++i) {
            struct v4l2_fmtdesc f;
            std::memset(&f, 0, sizeof f);
            f.index = i;
            f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (api.ioctl(fd, VIDIOC_ENUM_FMT, &f) != 0) break;
            if (f.pixelformat == kMJPG) c.mjpg = true;
            if (f.pixelformat == kYUYV) c.yuyv = true;
        }
    }
    api.close(fd);
}

class SystemFinder final : public CameraFinder {
public:
    SystemFinder(V4l2Api &api, std::string dev_root, std::string sys_root)
        : api_(api), dev_root_(std::move(dev_root)), sys_root_(std::move(sys_root))
    {
    }
    bool find(CameraRef &out) override
    {
        std::string pref;
        {
            std::lock_guard<std::mutex> lock(m_);
            pref = prefer_;
        }
        const std::vector<Candidate> c = scan_cameras(api_, dev_root_, sys_root_);
        const int i = pick_camera(c, pref);
        if (i < 0) return false;
        out = c[static_cast<std::size_t>(i)].ref;
        if (camera_fell_back(c, pref)) {
            out.fallback = true;
            out.wanted = pref;
        }
        return true;
    }
    std::vector<CameraEntry> list() override
    {
        return camera_entries(scan_cameras(api_, dev_root_, sys_root_));
    }
    void prefer(const std::string &key) override
    {
        std::lock_guard<std::mutex> lock(m_);
        prefer_ = key;
    }

private:
    V4l2Api &api_;
    std::string dev_root_, sys_root_;
    std::mutex m_;
    std::string prefer_;
};

class FixedFinder final : public CameraFinder {
public:
    FixedFinder(std::string path, std::string sys_root) : path_(std::move(path)), sys_root_(std::move(sys_root)) {}
    bool find(CameraRef &out) override
    {
        struct stat st;
        if (stat(path_.c_str(), &st) != 0) return false;
        out = CameraRef();
        out.path = path_;
        out.name = path_.substr(path_.find_last_of('/') + 1);
        out.id = identity_for_node(sys_root_, path_);
        return true;
    }

private:
    std::string path_, sys_root_;
};

} // namespace

bool CamMode::has_fps(int f) const
{
    return std::find(fps.begin(), fps.end(), f) != fps.end();
}

std::string fourcc_text(std::uint32_t f)
{
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((f >> (8 * i)) & 0xFF);
        s[static_cast<std::size_t>(i)] = std::isprint(static_cast<unsigned char>(c)) ? c : '?';
    }
    return s;
}

std::string mode_text(const ModeReq &m)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s %dx%d %d fps", fourcc_text(m.fourcc).c_str(), m.w, m.h, m.fps);
    return buf;
}

std::string mode_arg(const ModeReq &m)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s:%dx%d:%d", fourcc_text(m.fourcc).c_str(), m.w, m.h, m.fps);
    return buf;
}

bool parse_mode(const std::string &text, ModeReq &out)
{
    if (text.size() < 8 || text[4] != ':') return false;
    std::string f = text.substr(0, 4);
    for (char &c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    ModeReq m;
    m.fourcc = make_fourcc(f[0], f[1], f[2], f[3]);
    if (m.fourcc != kMJPG && m.fourcc != kYUYV) return false;
    const std::string rest = text.substr(5);
    int w = 0, h = 0, fps = 30, n = 0;
    char sep = 0;
    const int got = std::sscanf(rest.c_str(), "%dx%d%c%d%n", &w, &h, &sep, &fps, &n);
    if (got == 2) {
        int n2 = 0;
        std::sscanf(rest.c_str(), "%dx%d%n", &w, &h, &n2);
        if (static_cast<std::size_t>(n2) != rest.size()) return false;
        fps = 30;
    } else if (got == 4) {
        if ((sep != ':' && sep != '@') || static_cast<std::size_t>(n) != rest.size()) return false;
    } else {
        return false;
    }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || fps <= 0 || fps > 240) return false;
    m.w = w;
    m.h = h;
    m.fps = fps;
    out = m;
    return true;
}

int interval_fps(std::uint32_t num, std::uint32_t den)
{
    if (num == 0 || den == 0) return 0;
    return static_cast<int>((static_cast<std::uint64_t>(den) + num / 2) / num);
}

bool resolve_mode(const std::vector<CamMode> &modes, const ModeReq &want, ModeReq &out)
{
    for (const CamMode &m : modes) {
        if (m.fourcc != want.fourcc || m.w != want.w || m.h != want.h) continue;
        ModeReq r = want;
        if (!m.fps.empty() && !m.has_fps(want.fps)) {
            int best = m.fps[0];
            for (int f : m.fps)
                if (std::abs(f - want.fps) < std::abs(best - want.fps)) best = f;
            r.fps = best;
        }
        if (r.fps <= 0) r.fps = 30;
        out = r;
        return true;
    }
    return false;
}

std::vector<CamMode> enumerate_modes(V4l2Api &api, int fd)
{
    std::vector<CamMode> out;
    for (std::uint32_t fi = 0; fi < 64; ++fi) {
        struct v4l2_fmtdesc fmt;
        std::memset(&fmt, 0, sizeof fmt);
        fmt.index = fi;
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (api.ioctl(fd, VIDIOC_ENUM_FMT, &fmt) != 0) break;
        for (std::uint32_t si = 0; si < 64; ++si) {
            struct v4l2_frmsizeenum fs;
            std::memset(&fs, 0, sizeof fs);
            fs.index = si;
            fs.pixel_format = fmt.pixelformat;
            if (api.ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) != 0) break;
            if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) break; /* step-wise: not a UVC camera's way */
            CamMode m;
            m.fourcc = fmt.pixelformat;
            m.w = static_cast<int>(fs.discrete.width);
            m.h = static_cast<int>(fs.discrete.height);
            if (listed(out, m.fourcc, m.w, m.h)) continue;
            for (std::uint32_t ii = 0; ii < 32; ++ii) {
                struct v4l2_frmivalenum iv;
                std::memset(&iv, 0, sizeof iv);
                iv.index = ii;
                iv.pixel_format = fmt.pixelformat;
                iv.width = fs.discrete.width;
                iv.height = fs.discrete.height;
                if (api.ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &iv) != 0) break;
                if (iv.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
                const int fps = interval_fps(iv.discrete.numerator, iv.discrete.denominator);
                if (fps > 0 && !m.has_fps(fps)) m.fps.push_back(fps);
            }
            std::sort(m.fps.begin(), m.fps.end(), [](int a, int b) { return a > b; });
            out.push_back(m);
        }
    }
    return out;
}

bool lists_format(const std::vector<CamMode> &modes, std::uint32_t fourcc)
{
    for (const CamMode &m : modes)
        if (m.fourcc == fourcc) return true;
    return false;
}

DeviceInfo query_device(V4l2Api &api, int fd)
{
    DeviceInfo d;
    struct v4l2_capability cap;
    std::memset(&cap, 0, sizeof cap);
    if (api.ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) return d;
    const std::uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    d.capture = (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING) &&
                !(caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE));
    d.card.assign(reinterpret_cast<const char *>(cap.card), strnlen(reinterpret_cast<const char *>(cap.card), sizeof cap.card));
    d.driver.assign(reinterpret_cast<const char *>(cap.driver),
                    strnlen(reinterpret_cast<const char *>(cap.driver), sizeof cap.driver));
    d.bus.assign(reinterpret_cast<const char *>(cap.bus_info),
                 strnlen(reinterpret_cast<const char *>(cap.bus_info), sizeof cap.bus_info));
    return d;
}

std::vector<std::string> list_dir(const std::string &dir)
{
    std::vector<std::string> out;
    DIR *d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent *e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.empty() || n == "." || n == "..") continue;
        out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

int pick_camera(const std::vector<Candidate> &c, const std::string &remembered)
{
    if (!remembered.empty())
        for (std::size_t i = 0; i < c.size(); ++i)
            if (c[i].qualifies() && c[i].ref.key() == remembered) return static_cast<int>(i);
    int best = -1;
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (!c[i].qualifies()) continue;
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const Candidate &b = c[static_cast<std::size_t>(best)];
        if ((c[i].by_id && !b.by_id) || (c[i].by_id == b.by_id && c[i].ref.name < b.ref.name)) best = static_cast<int>(i);
    }
    return best;
}

std::vector<Candidate> scan_cameras(V4l2Api &api, const std::string &dev_root, const std::string &sys_root)
{
    std::vector<Candidate> out;
    std::vector<std::string> seen; /* video names already found through a by-id link */
    const std::string by_id = dev_root + "/v4l/by-id";
    for (const std::string &n : list_dir(by_id)) {
        if (!ends_with(n, "-video-index0")) continue;
        Candidate c;
        c.by_id = true;
        c.ref.path = by_id + "/" + n;
        c.ref.name = n;
        const std::string video = video_name_of(c.ref.path);
        seen.push_back(video);
        c.ref.id = identity_for_node(sys_root, c.ref.path);
        probe(api, c);
        out.push_back(c);
    }
    std::vector<std::string> nodes = list_dir(sys_root + "/sys/class/video4linux");
    std::sort(nodes.begin(), nodes.end(), [](const std::string &a, const std::string &b) {
        return video_number(a) < video_number(b);
    });
    for (const std::string &v : nodes) {
        if (video_number(v) < 0 || std::find(seen.begin(), seen.end(), v) != seen.end()) continue;
        if (node_driver(sys_root, v) != "uvcvideo") continue;
        const std::string index = read_line(sys_root + "/sys/class/video4linux/" + v + "/index");
        if (!index.empty() && index != "0") continue;
        Candidate c;
        c.ref.path = dev_root + "/" + v;
        c.ref.name = v;
        c.ref.id = identity_for_node(sys_root, c.ref.path);
        probe(api, c);
        out.push_back(c);
    }
    return out;
}

std::unique_ptr<CameraFinder> make_system_finder(V4l2Api &api, std::string dev_root, std::string sys_root)
{
    return std::make_unique<SystemFinder>(api, std::move(dev_root), std::move(sys_root));
}

std::unique_ptr<CameraFinder> make_fixed_finder(std::string path, std::string sys_root)
{
    return std::make_unique<FixedFinder>(std::move(path), std::move(sys_root));
}

} // namespace vhs

namespace vhs {

std::string readable_camera_name(const std::string &card)
{
    const std::size_t at = card.find(": ");
    if (at != std::string::npos && card.compare(0, at, card, at + 2, std::string::npos) == 0) return card.substr(0, at);
    return card;
}

bool camera_fell_back(const std::vector<Candidate> &c, const std::string &remembered)
{
    if (remembered.empty()) return false;
    int n = 0;
    for (const Candidate &x : c) {
        if (!x.qualifies()) continue;
        if (x.ref.key() == remembered) return false;
        ++n;
    }
    return n >= 2;
}

std::vector<CameraEntry> camera_entries(const std::vector<Candidate> &c)
{
    std::vector<CameraEntry> out;
    for (const Candidate &x : c) {
        if (!x.qualifies()) continue;
        CameraEntry e;
        e.key = x.ref.key();
        if (e.key.empty()) e.key = "node:" + x.ref.name; /* no identity at all (a bare node): the discovery name */
        bool dup = false;
        for (const CameraEntry &o : out) dup = dup || o.key == e.key;
        if (dup) continue; /* a second node of one device */
        e.name = readable_camera_name(!x.ref.id.product.empty() ? x.ref.id.product : x.ref.card);
        if (e.name.empty()) e.name = "Camera";
        e.path = x.ref.path;
        e.orientation = x.ref.orientation;
        const std::string &dp = x.ref.id.devpath;
        const std::size_t dash = dp.find('-');
        if (dash != std::string::npos && dash + 1 < dp.size()) e.port = dp.substr(dash + 1);
        out.push_back(e);
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::vector<std::size_t> same;
        for (std::size_t j = 0; j < out.size(); ++j)
            if (out[j].name == out[i].name) same.push_back(j);
        out[i].label = out[i].name;
        if (same.size() < 2) continue;
        auto distinct = [&](auto field) {
            for (std::size_t a = 0; a < same.size(); ++a)
                for (std::size_t b = a + 1; b < same.size(); ++b)
                    if (field(out[same[a]]) == field(out[same[b]])) return false;
            return true;
        };
        bool all_loc = true, all_port = true;
        for (std::size_t a : same) {
            all_loc = all_loc && out[a].orientation >= 0;
            all_port = all_port && !out[a].port.empty();
        }
        const bool loc = all_loc && distinct([](const CameraEntry &e) { return e.orientation; });
        const bool port = all_port && distinct([](const CameraEntry &e) { return e.port; });
        if (loc) {
            const char *w = out[i].orientation == 0 ? "front" : (out[i].orientation == 1 ? "back" : "external");
            out[i].label += std::string(" (") + w + ")";
        } else if (port) {
            out[i].label += " (USB " + out[i].port + ")";
        } else {
            std::size_t n = 1;
            for (std::size_t a : same) {
                if (a == i) break;
                ++n;
            }
            out[i].label += " #" + std::to_string(n);
        }
    }
    return out;
}

} // namespace vhs
