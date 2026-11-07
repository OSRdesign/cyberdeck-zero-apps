/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the GPS link (core/gps_link.cpp) on a fake port: nothing opened while off, a receiver found, the
 * strict baud sniff (Auto tries 9600, 4800, 38400, 115200; line noise and bad checksums never lock), busy and denied
 * ports, the 2 / 4 / 8 / 15 / 30 s backoff, hot-plug both ways, a receiver that goes silent, a chosen port, and the
 * bounded reads of one tick.
 */
#include "check.hpp"
#include "fake_gps_port.hpp"
#include "gps_link.hpp"

#include <algorithm>

using namespace atlas;

namespace {

/* one epoch of a 3D fix as text, the sentences of the demonstration feeds */
std::string epoch(int second = 30)
{
    char t[16];
    std::snprintf(t, sizeof t, "1015%02d.00", second);
    const std::string pos = "4853.37855,N,00216.19971,E";
    return nmea_frame(std::string("GNRMC,") + t + ",A," + pos + ",0.10,,150126,,,A,V") + "\r\n" +
           nmea_frame(std::string("GNGGA,") + t + "," + pos + ",1,09,1.10,41.5,M,47.3,M,,") + "\r\n" +
           nmea_frame("GNGSA,A,3,02,03,04,05,06,07,08,09,10,,,,3.12,1.10,2.64,1") + "\r\n";
}

/* runs the link from `t` to `t_end` (ms) in 100 ms ticks, feeding `dev` one epoch a second when `feed` */
long run(GpsLink &link, FakeGpsPorts &fake, long t, long t_end, int feed_dev = -1)
{
    for (; t <= t_end; t += 100) {
        fake.now = t;
        if (feed_dev >= 0 && t % 1000 == 0 && fake.devs[static_cast<std::size_t>(feed_dev)].present &&
            !fake.devs[static_cast<std::size_t>(feed_dev)].gone)
            fake.push(static_cast<std::size_t>(feed_dev), epoch(static_cast<int>((t / 1000) % 60)));
        link.tick(t);
    }
    return t;
}

GpsConfig on() { GpsConfig c; c.on = true; return c; }

} // namespace

void test_gps_link()
{
    /* off: nothing is listed, opened or read */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0");
        GpsLink link(fake);
        run(link, fake, 0, 5000, 0);
        CHECK(link.state() == GpsLinkState::Off);
        CHECK_EQ(fake.list_calls, 0);
        CHECK_EQ(fake.open_calls, 0);
        CHECK_EQ(static_cast<int>(fake.bytes_read), 0);
    }
    /* on with no device: "no device", looked at every 2 s */
    {
        FakeGpsPorts fake;
        GpsLink link(fake);
        link.configure(on());
        run(link, fake, 0, 6900);
        CHECK(link.state() == GpsLinkState::NoDevice);
        CHECK_EQ(fake.open_calls, 0);
        CHECK(fake.list_calls >= 3 && fake.list_calls <= 5);
        CHECK_EQ(chip_text(link.status(7000), false), std::string("No GPS device"));
    }
    /* a receiver at 9600: found, locked after two valid sentences, a 3D fix, the speed remembered */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 9600);
        GpsLink link(fake);
        std::vector<std::string> log;
        link.set_log([&log](const std::string &s) { log.push_back(s); });
        link.configure(on());
        fake.push(0, epoch(30));
        run(link, fake, 0, 300);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(link.port().path, std::string("/dev/ttyACM0"));
        CHECK_EQ(link.baud(), 9600);
        CHECK_EQ(link.last_good_baud(), 9600);
        CHECK_EQ(link.tracker().fix().kind, 3);
        const GpsStatus s = link.status(400);
        CHECK(s.kind == GpsStatus::Kind::Fix3D);
        CHECK_EQ(chip_text(s, false), std::string("3D fix 9 sats"));
        CHECK(!log.empty());
        /* the port is read-only, locked and was opened once; nothing but reading went on */
        CHECK_EQ(fake.count("open "), 1);
        CHECK_EQ(fake.count("config "), 1);
    }
    /* Auto speed: a receiver at 38400 is found after 9600 and 4800 (2.5 s each); the next time 38400 goes first */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 38400);
        GpsLink link(fake);
        link.configure(on());
        long t = run(link, fake, 0, 6000, 0);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(link.baud(), 38400);
        CHECK_EQ(fake.count("config /dev/ttyACM0 9600"), 1);
        CHECK_EQ(fake.count("config /dev/ttyACM0 4800"), 1);
        CHECK_EQ(fake.count("config /dev/ttyACM0 38400"), 1);
        CHECK_EQ(fake.count("config /dev/ttyACM0 115200"), 0);
        CHECK_EQ(link.last_good_baud(), 38400);
        /* unplug, replug: the first speed tried is the one that worked */
        fake.devs[0].present = false;
        fake.devs[0].gone = true;
        t = run(link, fake, t, t + 3000);
        CHECK(link.state() == GpsLinkState::NoDevice);
        fake.events.clear();
        fake.devs[0].present = true;
        fake.devs[0].gone = false;
        fake.devs[0].data.clear();
        run(link, fake, t, t + 6000, 0);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(fake.count("config /dev/ttyACM0 9600"), 0);
        CHECK_EQ(fake.count("config /dev/ttyACM0 38400"), 1);
    }
    /* Auto speed at 115200: all four are tried, in order */
    {
        FakeGpsPorts fake;
        fake.add("ttyUSB0", 115200);
        GpsLink link(fake);
        link.configure(on());
        run(link, fake, 0, 9000, 0);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(link.baud(), 115200);
        int order[4] = {0, 0, 0, 0};
        int n = 0;
        for (const std::string &e : fake.events) {
            if (e.compare(0, 7, "config ") != 0) continue;
            const int b = std::atoi(e.c_str() + e.rfind(' ') + 1);
            if (n < 4) order[n++] = b;
        }
        CHECK_EQ(n, 4);
        CHECK(order[0] == 9600 && order[1] == 4800 && order[2] == 38400 && order[3] == 115200);
        const std::vector<int> o = GpsLink::baud_order(38400);
        CHECK(o.size() == 4 && o[0] == 38400 && o[1] == 9600 && o[2] == 4800 && o[3] == 115200);
    }
    /* a fixed speed: only that one, a wrong one never locks */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 38400);
        GpsLink link(fake);
        GpsConfig c = on();
        c.baud = 9600;
        link.configure(c);
        run(link, fake, 0, 3000, 0);
        CHECK_EQ(fake.count("config /dev/ttyACM0 9600"), 1);
        CHECK_EQ(fake.count("config /dev/ttyACM0 4800"), 0);
        CHECK(link.state() != GpsLinkState::Locked);
        CHECK(link.state() == GpsLinkState::Searching);
        CHECK_EQ(chip_text(link.status(3000), false), std::string("Searching"));
        c.baud = 38400;
        link.configure(c); /* the setting changed: a new try at once */
        run(link, fake, 3100, 4500, 0);
        CHECK(link.state() == GpsLinkState::Locked);
    }
    /* strict: line noise at every speed, bad checksums, and one valid sentence are not a receiver */
    {
        FakeGpsPorts fake;
        fake.add("ttyUSB0", 1); /* no speed matches: noise at all four */
        GpsLink link(fake);
        link.configure(on());
        run(link, fake, 0, 12000);
        CHECK(link.state() != GpsLinkState::Locked);
        CHECK(!link.tracker().fix().kind);
        FakeGpsPorts bad;
        bad.add("ttyACM0", 9600);
        GpsLink l2(bad);
        l2.configure(on());
        std::string broken = epoch();
        for (std::size_t i = broken.find("\r\n"); i != std::string::npos; i = broken.find("\r\n", i + 2))
            broken[i - 1] = broken[i - 1] == '0' ? '1' : '0'; /* the last checksum digit of every sentence is wrong now */
        bad.push(0, broken + broken + broken);
        run(l2, bad, 0, 2400);
        CHECK(l2.state() == GpsLinkState::Searching);
        FakeGpsPorts one;
        one.add("ttyACM0", 9600);
        GpsLink l3(one);
        l3.configure(on());
        one.push(0, nmea_frame("GNGGA,101530.00,,,,,0,00,99.99,,,,,,") + "\r\n");
        run(l3, one, 0, 2400);
        CHECK(l3.state() == GpsLinkState::Searching); /* a single sentence is not enough in the window */
        one.push(0, nmea_frame("GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1") + "\r\n");
        run(l3, one, 2500, 2800);
        CHECK(l3.state() == GpsLinkState::Locked); /* two valid sentences are enough */
        CHECK(l3.status(2900).kind == GpsStatus::Kind::Searching); /* locked on the link, but no fix yet */
    }
    /* garbage first, valid sentences after it: the garbage is dropped */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 9600);
        GpsLink link(fake);
        link.configure(on());
        fake.push(0, std::string("\x7f\x01\xfe garbage $GPGGA,broken*00\r\n") + epoch());
        run(link, fake, 0, 300);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(link.tracker().fix().kind, 3);
    }
    /* a port that does not talk NMEA: the backoff 2, 4, 8, 15, 30 s */
    {
        FakeGpsPorts fake;
        fake.add("ttyUSB0", 1);
        GpsLink link(fake);
        link.configure(on());
        run(link, fake, 0, 120000);
        const std::vector<long> &o = fake.devs[0].open_times;
        CHECK(o.size() >= 5);
        /* each try lasts 4 x 2.5 s = 10 s; the pause after it is 2, 4, 8, 15, 30, 30 s (plus up to 2 s of scan) */
        const long want[] = {2000, 4000, 8000, 15000, 30000};
        for (std::size_t i = 1; i < o.size() && i <= 5; ++i) {
            const long gap = o[i] - o[i - 1] - 10000;
            CHECK(gap >= want[i - 1]);
            CHECK(gap <= want[i - 1] + 2300);
        }
        CHECK(o.size() <= 7); /* not a busy loop: in 120 s there are only a handful of tries */
        CHECK_EQ(GpsLink::backoff_ms(0), 2000L);
        CHECK_EQ(GpsLink::backoff_ms(1), 4000L);
        CHECK_EQ(GpsLink::backoff_ms(2), 8000L);
        CHECK_EQ(GpsLink::backoff_ms(3), 15000L);
        CHECK_EQ(GpsLink::backoff_ms(4), 30000L);
        CHECK_EQ(GpsLink::backoff_ms(40), 30000L);
        CHECK_EQ(GpsLink::backoff_ms(-1), 2000L);
    }
    /* busy and denied */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 9600).open_err = PortErr::Busy;
        GpsLink link(fake);
        link.configure(on());
        run(link, fake, 0, 3000);
        CHECK(link.state() == GpsLinkState::Searching); /* a device is there, another app holds it */
        fake.devs[0].open_err = PortErr::None;
        long t = run(link, fake, 3100, 12000, 0);
        CHECK(link.state() == GpsLinkState::Locked);
        (void)t;

        FakeGpsPorts den;
        den.add("ttyACM0", 9600).open_err = PortErr::Denied;
        GpsLink l2(den);
        l2.configure(on());
        run(l2, den, 0, 3000);
        CHECK(l2.state() == GpsLinkState::NoAccess);
        CHECK_EQ(chip_text(l2.status(3000), false), std::string("No access to GPS port"));
        /* a second port that works: found, the denied one does not matter */
        den.add("ttyUSB0", 9600);
        run(l2, den, 3100, 8000, 1);
        CHECK(l2.state() == GpsLinkState::Locked);
        CHECK_EQ(l2.port().path, std::string("/dev/ttyUSB0"));
    }
    /* hot-plug: nothing, then a receiver appears (found within the 2 s look), then it is pulled, then plugged again */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 9600).present = false;
        GpsLink link(fake);
        link.configure(on());
        long t = run(link, fake, 0, 3000);
        CHECK(link.state() == GpsLinkState::NoDevice);
        fake.devs[0].present = true;
        const long plugged = t;
        t = run(link, fake, t, t + 4000, 0);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK(!fake.devs[0].open_times.empty());
        CHECK(fake.devs[0].open_times[0] - plugged <= 2200);
        /* pulled: the read fails, the port is closed, "no device" */
        fake.devs[0].present = false;
        fake.devs[0].gone = true;
        t = run(link, fake, t, t + 300);
        CHECK(link.state() == GpsLinkState::NoDevice);
        CHECK_EQ(fake.count("close "), 1);
        CHECK(!link.status(t).has_fix());
        /* plugged again, as a new node: found at the next look, no backoff (the old port left the list) */
        fake.devs[0].present = true;
        fake.devs[0].gone = false;
        fake.devs[0].data.clear();
        const long replug = t;
        run(link, fake, t, t + 4000, 0);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK(fake.devs[0].open_times.back() - replug <= 2200);
        CHECK_EQ(fake.devs[0].opens, 2);
    }
    /* a locked receiver that goes silent for 10 s is lost and found again when it talks */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 9600);
        GpsLink link(fake);
        link.configure(on());
        long t = run(link, fake, 0, 3000, 0);
        CHECK(link.state() == GpsLinkState::Locked);
        t = run(link, fake, t, t + 9000); /* silent for 9 s: still locked (its fix goes stale on its own at 5 s) */
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK(link.status(t).stale);
        t = run(link, fake, t, t + 2500);
        CHECK(link.state() != GpsLinkState::Locked);
        CHECK(fake.count("close ") >= 1);
        t = run(link, fake, t, t + 20000, 0);
        CHECK(link.state() == GpsLinkState::Locked);
    }
    /* a chosen port: only that one, even when others talk; absent: no device */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 9600);
        fake.add("ttyUSB1", 9600);
        GpsLink link(fake);
        GpsConfig c = on();
        c.port = "/dev/ttyUSB1";
        link.configure(c);
        run(link, fake, 0, 3000, 1);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(link.port().path, std::string("/dev/ttyUSB1"));
        CHECK_EQ(fake.devs[0].opens, 0);
        c.port = "/dev/ttyUSB7";
        link.configure(c);
        run(link, fake, 3100, 7000);
        CHECK(link.state() == GpsLinkState::NoDevice);
        CHECK_EQ(fake.devs[0].opens, 0);
        CHECK_EQ(fake.count("close "), 1);
    }
    /* Auto: a u-blox is tried before the other known receivers (a Prolific puck), whatever the port numbers */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 1, 0x067b, "", 0x2303);   /* a Prolific puck that does not talk */
        fake.add("ttyUSB0", 9600, 0x1546);            /* the u-blox */
        GpsLink link(fake);
        link.configure(on());
        run(link, fake, 0, 1500, 1);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(link.port().path, std::string("/dev/ttyUSB0"));
        CHECK_EQ(fake.devs[0].opens, 0); /* never opened: the u-blox went first and talked */
        /* two known receivers, the first silent: the second is used after it (both are known) */
        FakeGpsPorts f2;
        f2.add("ttyUSB0", 1, 0x067b, "", 0x23a3);
        f2.add("ttyUSB1", 9600, 0x067b, "", 0x2303);
        GpsLink l2(f2);
        l2.configure(on());
        run(l2, f2, 0, 14000, 1);
        CHECK_EQ(l2.port().path, std::string("/dev/ttyUSB1"));
        CHECK_EQ(f2.devs[0].opens, 1);
    }
    /* THE RULE: Auto never opens a device that is no known receiver (a CH340 radar, CP210x, FTDI, an Espressif board, a
     * port with no USB id, another Prolific chip): no open() at all, "No GPS receiver found"; a known receiver that
     * appears later is found, the unknown ones still untouched; a port the user chose is opened whatever it is */
    {
        FakeGpsPorts fake;
        fake.add("ttyUSB0", 9600, 0x1a86, "", 0x7523);   /* CH340: the mmWave radar */
        fake.add("ttyUSB1", 9600, 0x10c4, "", 0xea60);   /* CP210x */
        fake.add("ttyUSB2", 9600, 0x0403, "", 0x6001);   /* FTDI */
        fake.add("ttyACM0", 9600, 0x303a, "", 0x1001);   /* Espressif native USB */
        fake.add("ttyACM1", 9600, 0, "usb-GPS_Receiver-if00"); /* no USB id, a GPS-like name: still no */
        fake.add("ttyUSB3", 9600, 0x067b, "", 0x2304);   /* another Prolific chip: not on the list */
        for (FakeDevice &d : fake.devs) d.info.product = "GPS GNSS receiver"; /* a name proves nothing */
        GpsLink link(fake);
        link.configure(on());
        run(link, fake, 0, 70000, -1);
        CHECK_EQ(fake.open_calls, 0);
        CHECK_EQ(fake.count("open "), 0);
        CHECK_EQ(fake.count("config "), 0);
        CHECK_EQ(static_cast<int>(fake.bytes_read), 0);
        CHECK(link.state() == GpsLinkState::NoReceiver);
        CHECK_EQ(chip_text(link.status(70000), false), std::string("No GPS receiver found"));
        CHECK(link.status(70000).kind == GpsStatus::Kind::NoReceiver);
        CHECK(!link.status(70000).has_fix());
        CHECK(fake.list_calls > 20); /* it keeps looking, every 2 s, but only looks */
        /* a known receiver is plugged in: found; the unknown ones are still never opened */
        fake.add("ttyUSB4", 9600, 0x1546);
        run(link, fake, 70100, 74500, 6);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(link.port().path, std::string("/dev/ttyUSB4"));
        for (std::size_t i = 0; i < 6; ++i) CHECK_EQ(fake.devs[i].opens, 0);
        /* the port the user chose is opened as chosen, a CH340 included */
        GpsConfig c = on();
        c.port = "/dev/ttyUSB0";
        link.configure(c);
        run(link, fake, 74600, 76000, 0);
        CHECK_EQ(fake.devs[0].opens, 1);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(link.port().path, std::string("/dev/ttyUSB0"));
        /* back to Auto: it is closed, and Auto goes to the u-blox again */
        link.configure(on());
        run(link, fake, 76100, 79000, 6);
        CHECK_EQ(link.port().path, std::string("/dev/ttyUSB4"));
        CHECK_EQ(fake.devs[0].opens, 1);
    }
    /* only unknown ports, no GPS at all: a chosen absent port is "no device", Auto sees ports but no receiver; no port: no device */
    {
        FakeGpsPorts fake;
        fake.add("ttyUSB0", 9600, 0x1a86, "", 0x7523);
        GpsLink link(fake);
        link.configure(on());
        run(link, fake, 0, 3000);
        CHECK(link.state() == GpsLinkState::NoReceiver);
        fake.devs[0].present = false;
        run(link, fake, 3100, 6000);
        CHECK(link.state() == GpsLinkState::NoDevice);
        GpsConfig c = on();
        c.port = "/dev/ttyUSB5";
        link.configure(c);
        fake.devs[0].present = true;
        run(link, fake, 6100, 9000);
        CHECK(link.state() == GpsLinkState::NoDevice); /* the chosen one is not there */
        CHECK_EQ(fake.open_calls, 0);
    }
    /* the first port does not talk (it is listened to for all four speeds), then the second is tried and found; the first is
     * left alone for its backoff */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 1);
        fake.add("ttyACM1", 9600);
        GpsLink link(fake);
        link.configure(on());
        run(link, fake, 0, 14000, 1);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(link.port().path, std::string("/dev/ttyACM1"));
        CHECK_EQ(fake.devs[0].opens, 1);
    }
    /* a tick reads a bounded amount: 100 KB waiting is taken over many ticks, never in one */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 9600);
        GpsLink link(fake);
        link.configure(on());
        std::string big;
        while (big.size() < 100000) big += epoch();
        fake.push(0, big);
        fake.now = 0;
        link.tick(0); /* opens */
        const std::size_t before = fake.bytes_read;
        link.tick(100);
        CHECK(fake.bytes_read - before <= 4 * 512);
        link.tick(200);
        CHECK(fake.bytes_read - before <= 8 * 512);
        CHECK(fake.bytes_read - before > 0);
    }
    /* switching off closes the port at once; configuring the same setting again changes nothing */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 9600);
        GpsLink link(fake);
        link.configure(on());
        run(link, fake, 0, 2000, 0);
        CHECK(link.state() == GpsLinkState::Locked);
        link.configure(on());
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK_EQ(fake.count("close "), 0);
        link.configure(GpsConfig());
        CHECK(link.state() == GpsLinkState::Off);
        CHECK_EQ(fake.count("close "), 1);
        const int opens = fake.open_calls;
        run(link, fake, 2100, 8000, 0);
        CHECK_EQ(fake.open_calls, opens);
        CHECK(!link.status(8000).has_fix());
    }
}
