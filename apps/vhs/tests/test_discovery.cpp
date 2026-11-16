/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: camera discovery and identity (brief 3.4, 5.4, 5.5) on fixture trees: the USB identity from sysfs (vid:pid
 * + serial, else the device path), the host controller's driver (dwc_otg on a Pi 3A+ / Zero 2 W, xhci on a Pi 4B / 5),
 * the capability filter (a metadata node, an M2M codec node and a node without MJPG or YUYV are skipped), the by-id
 * preference, a uvcvideo node without a by-id link, the remembered identity, and the fixed node (--device).
 */
#include "check.hpp"

#include "camera/camera.hpp"
#include "camera/identity.hpp"

#include <linux/videodev2.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <unistd.h>

using namespace vhs;

namespace {

/* A V4l2Api answering QUERYCAP and ENUM_FMT for named nodes (what discovery asks). */
class FakeNodes final : public V4l2Api {
public:
    struct Node {
        std::uint32_t caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        std::vector<std::uint32_t> fmts;
        std::string card;
    };
    std::map<std::string, Node> nodes;
    int opens = 0;
    int open(const char *path) override
    {
        auto it = nodes.find(path);
        if (it == nodes.end()) {
            errno = ENOENT;
            return -1;
        }
        ++opens;
        open_.push_back(&it->second);
        return static_cast<int>(open_.size()) + 99;
    }
    int close(int) override { return 0; }
    int ioctl(int fd, unsigned long req, void *arg) override
    {
        const std::size_t i = static_cast<std::size_t>(fd - 100);
        if (i >= open_.size()) {
            errno = EBADF;
            return -1;
        }
        const Node &n = *open_[i];
        if (req == VIDIOC_QUERYCAP) {
            auto *c = static_cast<v4l2_capability *>(arg);
            std::memset(c, 0, sizeof *c);
            std::strncpy(reinterpret_cast<char *>(c->card), n.card.c_str(), sizeof c->card - 1);
            std::strncpy(reinterpret_cast<char *>(c->driver), "uvcvideo", sizeof c->driver - 1);
            c->device_caps = n.caps;
            c->capabilities = n.caps | V4L2_CAP_DEVICE_CAPS;
            return 0;
        }
        if (req == VIDIOC_ENUM_FMT) {
            auto *f = static_cast<v4l2_fmtdesc *>(arg);
            if (f->index >= n.fmts.size()) {
                errno = EINVAL;
                return -1;
            }
            f->pixelformat = n.fmts[f->index];
            return 0;
        }
        errno = ENOTTY;
        return -1;
    }
    void *map(std::size_t, int, off_t) override { return nullptr; }
    int unmap(void *, std::size_t) override { return 0; }
    int poll_in(int, int) override { return 0; }

private:
    std::vector<Node *> open_;
};

void link(const std::string &target, const std::string &at)
{
    make_dirs(at.substr(0, at.find_last_of('/')));
    CHECK(symlink(target.c_str(), at.c_str()) == 0);
}

/* A USB camera at <hc>/usbN/<path>/<dev>; its interface <dev>:1.0 driven by uvcvideo; /sys/class/video4linux/<video>. */
void usb_camera(const std::string &root, const std::string &hc, const std::string &hc_driver, const std::string &chain,
                const std::string &dev, const std::string &video, const std::string &vid, const std::string &pid,
                const std::string &serial, const std::string &product, const std::string &index = "0")
{
    const std::string sys = root + "/sys";
    make_dirs(sys + "/bus/drivers/" + hc_driver);
    make_dirs(sys + "/bus/usb/drivers/uvcvideo");
    const std::string hcdir = sys + "/devices/" + hc;
    make_dirs(hcdir);
    if (access((hcdir + "/driver").c_str(), F_OK) != 0) link(sys + "/bus/drivers/" + hc_driver, hcdir + "/driver");
    /* every directory of the chain is a USB device (root hub, hubs, the camera) */
    std::string at = hcdir;
    std::string rest = chain + "/" + dev;
    std::size_t pos = 0;
    while (pos <= rest.size()) {
        const std::size_t s = rest.find('/', pos);
        const std::string part = rest.substr(pos, s == std::string::npos ? std::string::npos : s - pos);
        at += "/" + part;
        make_dirs(at);
        if (access((at + "/idVendor").c_str(), F_OK) != 0) write_file(at + "/idVendor", "1d6b\n");
        if (s == std::string::npos) break;
        pos = s + 1;
    }
    write_file(at + "/idVendor", vid + "\n");
    write_file(at + "/idProduct", pid + "\n");
    if (!serial.empty()) write_file(at + "/serial", serial + "\n");
    write_file(at + "/manufacturer", "Maker\n");
    write_file(at + "/product", product + "\n");
    const std::string intf = at + "/" + dev + ":1.0";
    make_dirs(intf);
    link(sys + "/bus/usb/drivers/uvcvideo", intf + "/driver");
    const std::string cls = sys + "/class/video4linux/" + video;
    make_dirs(cls);
    link(intf, cls + "/device");
    write_file(cls + "/index", index + "\n");
}

void identity_and_bus()
{
    const std::string root = make_temp_dir("sys");
    usb_camera(root, "platform/soc/3f980000.usb", "dwc_otg", "usb1/1-1", "1-1.2", "video0", "a108", "2240", "SN1",
               "UGREEN FineCam 1080P CM969");
    usb_camera(root, "platform/axi/1000120000.pcie/1f00200000.usb/xhci-hcd.0", "xhci-hcd", "usb3", "3-1", "video2", "046d",
               "0825", "", "C270 HD WEBCAM");
    make_dirs(root + "/dev");
    write_file(root + "/dev/video0", "");
    CamIdentity a = identity_for_node(root, root + "/dev/video0");
    CHECK(a.valid());
    CHECK_EQ(a.vid, std::string("a108"));
    CHECK_EQ(a.pid, std::string("2240"));
    CHECK_EQ(a.serial, std::string("SN1"));
    CHECK_EQ(a.product, std::string("UGREEN FineCam 1080P CM969"));
    CHECK_EQ(a.manufacturer, std::string("Maker"));
    CHECK_EQ(a.devpath, std::string("1-1.2"));
    CHECK_EQ(a.hc_driver, std::string("dwc_otg"));
    CHECK_EQ(a.key(), std::string("a108:2240:SN1"));
    CHECK(bus_needs_replug(a.hc_driver));
    /* no serial: the device path; the xhci bus of a Pi 5 */
    CamIdentity b = identity_for_node(root, "video2");
    CHECK_EQ(b.key(), std::string("046d:0825@3-1"));
    CHECK_EQ(b.hc_driver, std::string("xhci-hcd"));
    CHECK(!bus_needs_replug(b.hc_driver));
    CHECK(!bus_needs_replug(""));
    CHECK(bus_needs_replug("dwc2"));
    CHECK(a.same(identity_for_node(root, "video0")));
    CHECK(!a.same(b));
    /* not on USB: invalid */
    CamIdentity none = identity_for_node(root, "video9");
    CHECK(!none.valid());
    CHECK_EQ(none.key(), std::string());
    CHECK(!none.same(none));
    CHECK_EQ(node_driver(root, "video0"), std::string("uvcvideo"));
    CHECK_EQ(video_name_of("/nonexistent/video7"), std::string("video7"));
    rm_tree(root);
}

void scan_and_pick()
{
    const std::string root = make_temp_dir("disc");
    const std::string dev = root + "/dev";
    const std::string fine = "usb-Ingenic_Semiconductor_Co._Ltd_UGREEN_FineCam_1080P_CM969_Ucamera001-video-index0";
    const std::string meta = "usb-Ingenic_Semiconductor_Co._Ltd_UGREEN_FineCam_1080P_CM969_Ucamera001-video-index1";
    const std::string c920 = "usb-046d_HD_Pro_Webcam_C920-video-index0";
    usb_camera(root, "platform/scb/fd500000.pcie/pci0000:00/0000:00:00.0/0000:01:00.0", "xhci_hcd", "usb1/1-1", "1-1.3",
               "video0", "a108", "2240", "SN1", "UGREEN FineCam 1080P CM969");
    usb_camera(root, "platform/scb/fd500000.pcie/pci0000:00/0000:00:00.0/0000:01:00.0", "xhci_hcd", "usb1/1-1", "1-1.4",
               "video2", "046d", "082d", "C920SN", "HD Pro Webcam C920");
    usb_camera(root, "platform/scb/fd500000.pcie/pci0000:00/0000:00:00.0/0000:01:00.0", "xhci_hcd", "usb1/1-1", "1-1.1",
               "video4", "046d", "0825", "", "C270 HD WEBCAM");
    /* the codec node: not uvcvideo, never opened */
    make_dirs(root + "/sys/class/video4linux/video10");
    make_dirs(dev);
    for (const char *v : {"video0", "video1", "video2", "video3", "video4", "video10"}) write_file(dev + "/" + v, "");
    link(dev + "/video0", dev + "/v4l/by-id/" + fine);
    link(dev + "/video1", dev + "/v4l/by-id/" + meta);
    link(dev + "/video2", dev + "/v4l/by-id/" + c920);

    FakeNodes api;
    FakeNodes::Node cam;
    cam.fmts = {kMJPG, kYUYV};
    cam.card = "UGREEN FineCam";
    FakeNodes::Node metadata;
    metadata.caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
    FakeNodes::Node h264only;
    h264only.fmts = {make_fourcc('H', '2', '6', '4')};
    FakeNodes::Node yuyv;
    yuyv.fmts = {kYUYV};
    FakeNodes::Node m2m;
    m2m.caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
    m2m.fmts = {kYUYV};
    api.nodes[dev + "/v4l/by-id/" + fine] = cam;
    api.nodes[dev + "/v4l/by-id/" + meta] = metadata;
    api.nodes[dev + "/v4l/by-id/" + c920] = h264only;
    api.nodes[dev + "/video4"] = yuyv;
    api.nodes[dev + "/video10"] = m2m;

    std::vector<Candidate> c = scan_cameras(api, dev, root);
    CHECK_EQ(c.size(), static_cast<std::size_t>(3)); /* the two index0 links and video4 (no link, uvcvideo, index 0) */
    if (c.size() == 3) {
        CHECK(c[0].by_id && c[1].by_id && !c[2].by_id);
        CHECK_EQ(c[2].ref.name, std::string("video4"));
        CHECK(c[2].qualifies());
        CHECK_EQ(c[2].ref.id.key(), std::string("046d:0825@1-1.1"));
    }
    /* the C920 here offers H.264 only: skipped; the FineCam: the first by-id that qualifies */
    int i = pick_camera(c, "");
    CHECK(i >= 0);
    if (i >= 0) {
        CHECK_EQ(c[static_cast<std::size_t>(i)].ref.name, fine);
        CHECK_EQ(c[static_cast<std::size_t>(i)].ref.id.key(), std::string("a108:2240:SN1"));
        CHECK_EQ(c[static_cast<std::size_t>(i)].ref.card, std::string("UGREEN FineCam"));
    }
    /* remembered: the C270 (no by-id link) wins over the by-id FineCam */
    i = pick_camera(c, "046d:0825@1-1.1");
    CHECK(i >= 0 && c[static_cast<std::size_t>(i)].ref.name == "video4");
    /* a remembered camera that is not there: the usual order */
    i = pick_camera(c, "dead:beef:X");
    CHECK(i >= 0 && c[static_cast<std::size_t>(i)].ref.name == fine);
    /* the codec node was never opened */
    bool codec_opened = false;
    for (const Candidate &x : c) codec_opened |= x.ref.name == "video10";
    CHECK(!codec_opened);

    /* the finder: the same pick, the preference set later */
    std::unique_ptr<CameraFinder> f = make_system_finder(api, dev, root);
    CameraRef ref;
    CHECK(f->find(ref));
    CHECK_EQ(ref.name, fine);
    f->prefer("046d:0825@1-1.1");
    CHECK(f->find(ref));
    CHECK_EQ(ref.name, std::string("video4"));
    /* by name order between two by-id cameras that qualify */
    std::vector<Candidate> two(2);
    two[0].by_id = two[1].by_id = true;
    two[0].capture = two[1].capture = true;
    two[0].mjpg = two[1].mjpg = true;
    two[0].ref.name = "usb-b-video-index0";
    two[1].ref.name = "usb-a-video-index0";
    CHECK_EQ(pick_camera(two, ""), 1);
    CHECK_EQ(pick_camera({}, ""), -1);
    /* nothing at all */
    const std::string empty = make_temp_dir("nodev");
    std::unique_ptr<CameraFinder> nf = make_system_finder(api, empty + "/dev", empty);
    CHECK(!nf->find(ref));
    rm_tree(empty);
    /* the fixed node: found while it exists, its identity from sysfs */
    std::unique_ptr<CameraFinder> fx = make_fixed_finder(dev + "/video0", root);
    CHECK(fx->find(ref));
    CHECK_EQ(ref.id.key(), std::string("a108:2240:SN1"));
    std::unique_ptr<CameraFinder> gone = make_fixed_finder(dev + "/video99", root);
    CHECK(!gone->find(ref));
    rm_tree(root);
}

} // namespace

void test_discovery()
{
    identity_and_bus();
    scan_and_pick();
}
