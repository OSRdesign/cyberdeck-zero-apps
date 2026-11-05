/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the real port layer (PosixSys) on a fake sysfs/dev/proc tree and on a pseudo-terminal that plays
 * the radar. Linux only.
 */
#include "check.hpp"

#include "detect.hpp"
#include "posix_sys.hpp"

#include <fcntl.h>
#include <pty.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace mmw;

namespace {

std::string make_tmp()
{
    const char *t = std::getenv("TMPDIR");
    std::string pattern = std::string(t && t[0] ? t : "/tmp") + "/mmw-test-XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    return mkdtemp(buf.data()) ? std::string(buf.data()) : std::string("/tmp/mmw-test-fallback");
}

void sh(const std::string &cmd)
{
    if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "command failed: %s\n", cmd.c_str());
}
void mk(const std::string &p) { sh("mkdir -p '" + p + "'"); }
void touch(const std::string &p, const std::string &content = "")
{
    std::ofstream(p) << content;
}
void mklink(const std::string &target, const std::string &name)
{
    if (::symlink(target.c_str(), name.c_str()) != 0) std::perror("symlink");
}

void test_tree()
{
    const std::string root = make_tmp();
    mk(root + "/dev/serial/by-id");
    for (const char *n : {"ttyUSB0", "ttyUSB10", "ttyUSB2", "ttyACM0", "ttyS0", "tty1"}) touch(root + "/dev/" + n);
    mklink("../../ttyUSB0", root + "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0");
    /* ttyUSB0 is a CH340 behind a USB interface directory; ttyACM0 has no sysfs entry */
    mk(root + "/sys/devices/usb1/1-1/1-1:1.0");
    touch(root + "/sys/devices/usb1/1-1/idVendor", "1a86\n");
    touch(root + "/sys/devices/usb1/1-1/idProduct", "7523\n");
    touch(root + "/sys/devices/usb1/1-1/product", "USB Serial\n");
    mk(root + "/sys/devices/usb1/1-1/1-1:1.0/driver_dir/ch341");
    mk(root + "/sys/class/tty/ttyUSB0");
    mklink(root + "/sys/devices/usb1/1-1/1-1:1.0", root + "/sys/class/tty/ttyUSB0/device");
    /* a process that has ttyUSB0 open */
    mk(root + "/proc/4242/fd");
    mklink(root + "/dev/ttyUSB0", root + "/proc/4242/fd/3");
    mk(root + "/proc/self_like");

    SysRoots r;
    r.sys = root + "/sys";
    r.dev = root + "/dev";
    r.proc = root + "/proc";
    PosixSys sys(r);
    const std::vector<PortMeta> ports = sys.list_ports();
    CHECK_EQ(ports.size(), static_cast<std::size_t>(4)); /* ttyUSB* and ttyACM*, never ttyS0 or tty1 */
    CHECK_EQ(ports[0].tty, "ttyACM0");
    CHECK_EQ(ports[1].tty, "ttyUSB0");
    CHECK_EQ(ports[2].tty, "ttyUSB2");
    CHECK_EQ(ports[3].tty, "ttyUSB10"); /* numeric order */
    CHECK_EQ(ports[1].vid, 0x1a86);
    CHECK_EQ(ports[1].pid, 0x7523);
    CHECK_EQ(ports[1].product, "USB Serial");
    CHECK_EQ(ports[1].by_id, "usb-1a86_USB_Serial-if00-port0");
    CHECK_EQ(port_id(ports[1]), "usb-1a86_USB_Serial-if00-port0");
    CHECK_EQ(port_title(ports[1]), "ttyUSB0 (CH340)");
    CHECK_EQ(ports[0].vid, 0);
    CHECK_EQ(port_id(ports[0]), "ttyACM0");
    CHECK(sys.held_by_other(root + "/dev/ttyUSB0"));
    CHECK(!sys.held_by_other(root + "/dev/ttyUSB2"));
    sh("rm -rf '" + root + "'");
}

void test_pty_radar()
{
    int master = -1, slave = -1;
    char name[128];
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) {
        std::fprintf(stderr, "openpty failed: pty test skipped\n");
        return;
    }
    const std::string stream = test_read_file(test_data_dir() + "ld2452-9600-walk-15s.bin");
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
    std::size_t sent = 0;
    auto radar_sends_a_frame = [&]() { /* the radar side: one frame per call, from the recorded walk */
        if (::write(master, stream.data() + (sent % stream.size()), 30) == 30) sent += 30;
    };
    ::close(slave); /* the detector opens it by name; keep the master */

    SysRoots r;
    r.dev = "/nonexistent";
    PosixSys sys(r);
    sys.add_extra_port(name);
    const std::vector<PortMeta> ports = sys.list_ports();
    CHECK_EQ(ports.size(), static_cast<std::size_t>(1));

    /* a second opener finds it locked */
    OpenErr e = OpenErr::None;
    const int h = sys.open(name, e);
    CHECK(h >= 0);
    OpenErr e2 = OpenErr::None;
    PosixSys other(r);
    const int h2 = other.open(name, e2);
    CHECK_EQ(h2, -1);
    CHECK(e2 == OpenErr::Busy);
    /* configure any speed, read, restore */
    CHECK(sys.configure(h, 256000));
    CHECK(sys.configure(h, 9600));
    sys.flush_input(h);
    sys.restore(h);
    sys.close(h);
    /* after close, a new opener can have it */
    const int h3 = other.open(name, e2);
    CHECK(h3 >= 0);
    if (h3 < 0) std::fprintf(stderr, "reopen err %d\n", static_cast<int>(e2));
    other.close(h3);

    /* the whole detector on the real layer: the pty delivers the same bytes at any speed, so it is found at 256000 */
    Detector det(sys);
    det.start(0, Remembered());
    long t = 0;
    while (det.running() && t < 3000) {
        t += 20;
        radar_sends_a_frame();
        det.poll(t);
        usleep(2000);
    }
    CHECK(det.found());
    CHECK(det.reports().size() == 1 && det.reports()[0].verdict == Verdict::Radar);
    RadarPort rp = det.take_radar();
    CHECK(rp.handle >= 0);
    /* while we hold it, another app gets "busy" */
    CHECK_EQ(other.open(name, e2), -1);
    CHECK(e2 == OpenErr::Busy);
    sys.restore(rp.handle);
    sys.close(rp.handle);
    /* unplug: the master goes away, reads fail */
    OpenErr e3;
    const int h4 = sys.open(name, e3);
    CHECK(h4 >= 0);
    sys.configure(h4, 9600);
    ::close(master);
    std::uint8_t buf[64];
    long n = 0;
    for (int i = 0; i < 5 && n >= 0; ++i) n = sys.read(h4, buf, sizeof(buf));
    /* a pty slave only reads 0 once its master is gone; a real USB unplug hangs the tty up (its ioctls give EIO), which
     * read() reports as < 0: that path needs the board test */
    CHECK(n <= 0);
    sys.close(h4);
    /* a node that is not there */
    CHECK_EQ(sys.open("/nonexistent/ttyUSB9", e3), -1);
    CHECK(e3 == OpenErr::Gone);
}

} // namespace

void test_posix()
{
    test_tree();
    test_pty_radar();
}
