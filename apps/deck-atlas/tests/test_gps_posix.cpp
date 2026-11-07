/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the real port layer (core/gps_posix.cpp) on a pseudo-terminal and on a fake /dev and /sys tree: the
 * list of ports with their USB identity, open (read-only, locked, exclusive), the speed, reading, the hung-up port, and
 * the whole link over a pty: sentences written to the master end come out as a 3D fix.
 */
#include "check.hpp"
#include "gps_ids.hpp"
#include "gps_link.hpp"
#include "gps_posix.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cstring>

using namespace atlas;

namespace {

void make_file(const std::string &path, const std::string &text)
{
    const std::size_t slash = path.rfind('/');
    std::string dir = path.substr(0, slash);
    std::string cur;
    for (std::size_t i = 0; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '/') {
            if (!cur.empty()) ::mkdir(cur.c_str(), 0755);
            if (i < dir.size()) cur += '/';
        } else {
            cur += dir[i];
        }
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

} // namespace

void test_gps_posix()
{
    CHECK(gps_baud_supported(9600));
    CHECK(gps_baud_supported(4800));
    CHECK(gps_baud_supported(38400));
    CHECK(gps_baud_supported(115200));
    CHECK(!gps_baud_supported(0));
    CHECK(!gps_baud_supported(19200));

    /* the list: only ttyACM* and ttyUSB*, natural order, the USB identity from sysfs, the by-id name */
    {
        const std::string root = test_temp_dir();
        GpsRoots roots;
        roots.dev = root + "/dev";
        roots.sys = root + "/sys";
        for (const char *n : {"ttyACM0", "ttyACM10", "ttyACM2", "ttyUSB0", "ttyS0", "tty1", "null"}) make_file(roots.dev + "/" + n, "");
        make_file(root + "/sys/devices/usb1/1-2/idVendor", "1a86\n"); /* a CH340 on ttyUSB0 */
        make_file(root + "/sys/devices/usb1/1-2/idProduct", "7523\n");
        make_file(root + "/sys/devices/usb1/1-2/1-2:1.0/ttyUSB0/dummy", "");
        ::mkdir((root + "/sys/class").c_str(), 0755);
        ::mkdir((root + "/sys/class/tty").c_str(), 0755);
        ::mkdir((root + "/sys/class/tty/ttyUSB0").c_str(), 0755);
        ::symlink((root + "/sys/devices/usb1/1-2/1-2:1.0").c_str(), (root + "/sys/class/tty/ttyUSB0/device").c_str());
        make_file(root + "/sys/devices/usb1/1-1/idVendor", "1546\n");
        make_file(root + "/sys/devices/usb1/1-1/idProduct", "01a8\n");
        make_file(root + "/sys/devices/usb1/1-1/product", "u-blox 7 - GPS/GNSS Receiver\n");
        make_file(root + "/sys/devices/usb1/1-1/1-1:1.0/ttyACM0/dummy", "");
        ::mkdir((root + "/sys/class").c_str(), 0755);
        ::mkdir((root + "/sys/class/tty").c_str(), 0755);
        ::mkdir((root + "/sys/class/tty/ttyACM0").c_str(), 0755);
        ::symlink((root + "/sys/devices/usb1/1-1/1-1:1.0").c_str(), (root + "/sys/class/tty/ttyACM0/device").c_str());
        ::mkdir((roots.dev + "/serial").c_str(), 0755);
        ::mkdir((roots.dev + "/serial/by-id").c_str(), 0755);
        ::symlink((roots.dev + "/ttyACM0").c_str(), (roots.dev + "/serial/by-id/usb-u-blox_AG_u-blox_7-if00").c_str());
        PosixGpsPorts ports(roots);
        const std::vector<PortInfo> l = ports.list_ports();
        CHECK_EQ(l.size(), static_cast<std::size_t>(4));
        if (l.size() == 4) {
            CHECK_EQ(l[0].tty, std::string("ttyACM0"));
            CHECK_EQ(l[1].tty, std::string("ttyACM2"));
            CHECK_EQ(l[2].tty, std::string("ttyACM10"));
            CHECK_EQ(l[3].tty, std::string("ttyUSB0"));
            CHECK_EQ(static_cast<int>(l[0].vid), 0x1546);
            CHECK_EQ(static_cast<int>(l[0].pid), 0x01a8);
            CHECK_EQ(l[0].product, std::string("u-blox 7 - GPS/GNSS Receiver"));
            CHECK_EQ(l[0].by_id, std::string("usb-u-blox_AG_u-blox_7-if00"));
            CHECK_EQ(l[1].vid, static_cast<std::uint16_t>(0));
            /* the identity decides what Auto may open: the u-blox yes, the CH340, a port with no id no */
            CHECK(gps_known_receiver(l[0].vid, l[0].pid));
            CHECK_EQ(static_cast<int>(l[3].vid), 0x1a86);
            CHECK_EQ(static_cast<int>(l[3].pid), 0x7523);
            CHECK(!gps_known_receiver(l[3].vid, l[3].pid));
            CHECK(!gps_known_receiver(l[1].vid, l[1].pid));
            CHECK_EQ(l[0].path, roots.dev + "/ttyACM0");
        }
        /* a regular file is no tty: Other; a missing node: Gone */
        PortErr err = PortErr::None;
        CHECK_EQ(ports.open(roots.dev + "/ttyACM0", err), -1);
        CHECK(err == PortErr::Other);
        CHECK_EQ(ports.open(roots.dev + "/ttyACM99", err), -1);
        CHECK(err == PortErr::Gone);
        test_remove_tree(root);
    }

    /* a pseudo-terminal: open (read-only, exclusive), speeds, reading, the hung-up port */
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(master >= 0);
    if (master < 0) return;
    CHECK_EQ(::grantpt(master), 0);
    CHECK_EQ(::unlockpt(master), 0);
    const char *slave_name = ::ptsname(master);
    CHECK(slave_name != nullptr);
    if (!slave_name) {
        ::close(master);
        return;
    }
    const std::string slave = slave_name;
    {
        PosixGpsPorts ports;
        PortErr err = PortErr::None;
        const int h = ports.open(slave, err);
        CHECK(h >= 0);
        CHECK(err == PortErr::None);
        /* the descriptor is read-only: the kernel refuses a send */
        CHECK((::fcntl(h, F_GETFL) & O_ACCMODE) == O_RDONLY);
        /* locked and exclusive: a second open (of ours, or another app's) is refused as busy */
        PortErr err2 = PortErr::None;
        PosixGpsPorts other;
        CHECK_EQ(other.open(slave, err2), -1);
        CHECK(err2 == PortErr::Busy);
        CHECK(ports.configure(h, 9600));
        CHECK(ports.configure(h, 4800));
        CHECK(ports.configure(h, 38400));
        CHECK(ports.configure(h, 115200));
        CHECK(!ports.configure(h, 12345));
        CHECK(ports.configure(h, 9600));
        struct termios t;
        CHECK_EQ(::tcgetattr(h, &t), 0);
        CHECK(cfgetispeed(&t) == B9600);
        CHECK((t.c_cflag & CSIZE) == CS8);
        CHECK((t.c_cflag & PARENB) == 0);
        CHECK((t.c_cflag & CSTOPB) == 0);
        CHECK_EQ(static_cast<int>(t.c_cc[VMIN]), 0);
        CHECK_EQ(static_cast<int>(t.c_cc[VTIME]), 0);
        /* nothing yet: 0, at once (never blocks) */
        std::uint8_t buf[64];
        CHECK_EQ(ports.read(h, buf, sizeof buf), 0L);
        const std::string line = nmea_frame("GNGGA,101530.00,4853.37855,N,00216.19971,E,1,09,1.10,41.5,M,47.3,M,,") + "\r\n";
        CHECK_EQ(static_cast<long>(::write(master, line.data(), line.size())), static_cast<long>(line.size()));
        long got = 0;
        std::string text;
        for (int i = 0; i < 50 && got < static_cast<long>(line.size()); ++i) {
            const long n = ports.read(h, buf, sizeof buf);
            if (n > 0) {
                text.append(reinterpret_cast<char *>(buf), static_cast<std::size_t>(n));
                got += n;
            } else {
                ::usleep(10000);
            }
        }
        CHECK_EQ(text, line);
        /* the master end goes away (the adapter pulled out): the port reads as gone */
        ::close(master);
        long r = 0;
        for (int i = 0; i < 20 && r >= 0; ++i) {
            r = ports.read(h, buf, sizeof buf);
            if (r == 0) ::usleep(10000);
        }
        CHECK(r < 0);
        ports.close(h);
        /* after close the slave can be opened again by another (the lock and exclusive mode are gone), but the master
         * is gone too: nothing more to test here */
    }

    /* the whole link over a new pty: found, sniffed at 9600, locked, a 3D fix */
    {
        const int m2 = ::posix_openpt(O_RDWR | O_NOCTTY);
        CHECK(m2 >= 0);
        if (m2 < 0) return;
        ::grantpt(m2);
        ::unlockpt(m2);
        const std::string s2 = ::ptsname(m2);
        const std::string empty_root = test_temp_dir(); /* no real /dev/tty* is a candidate: only the pty */
        GpsRoots roots;
        roots.dev = empty_root;
        roots.sys = empty_root;
        PosixGpsPorts ports(roots);
        ports.add_extra_port(s2);
        GpsLink link(ports);
        /* Auto: the pty has no USB id, so it is no known receiver and is never opened */
        GpsConfig a;
        a.on = true;
        link.configure(a);
        for (long t = 0; t < 4000; t += 100) link.tick(t);
        CHECK(link.state() == GpsLinkState::NoReceiver);
        GpsConfig c;
        c.on = true;
        c.port = s2; /* the port chosen by hand is opened whatever its id */
        link.configure(c);
        const std::string pos = "4853.37855,N,00216.19971,E";
        const std::string feed = nmea_frame("GNRMC,101530.00,A," + pos + ",0.10,,150126,,,A,V") + "\r\n" +
                                 nmea_frame("GNGGA,101530.00," + pos + ",1,09,1.10,41.5,M,47.3,M,,") + "\r\n" +
                                 nmea_frame("GNGSA,A,3,02,03,04,05,06,07,08,09,10,,,,3.12,1.10,2.64,1") + "\r\n";
        long now = 0;
        for (int i = 0; i < 40 && link.state() != GpsLinkState::Locked; ++i, now += 100) {
            link.tick(now);
            if (link.state() == GpsLinkState::Searching && i == 1) {
                /* the link opened the pty at 9600: the receiver talks */
                CHECK_EQ(static_cast<long>(::write(m2, feed.data(), feed.size())), static_cast<long>(feed.size()));
            }
            ::usleep(5000);
        }
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(link.baud(), 9600);
        CHECK_EQ(link.tracker().fix().kind, 3);
        CHECK_EQ(link.port().path, s2);
        link.configure(GpsConfig()); /* off: closed */
        ::close(m2);
        test_remove_tree(empty_root);
    }
}
