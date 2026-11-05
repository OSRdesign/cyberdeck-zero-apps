/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the link (states, rates, loss, re-detection, hot-plug), against fake ports.
 */
#include "check.hpp"

#include "fake_sys.hpp"
#include "link.hpp"

using namespace mmw;

namespace {

std::string radar_stream() { return test_read_file(test_data_dir() + "ld2452-9600-walk-15s.bin"); }

void step(FakeSys &sys, Link &link, long ms, long dt = 30)
{
    const long end = sys.now_ms + ms;
    while (sys.now_ms < end) {
        sys.now_ms += dt;
        link.poll(sys.now_ms);
    }
}

void test_found_and_rates()
{
    FakeSys sys;
    sys.add("ttyUSB0").stream[9600] = radar_stream();
    Link link(sys);
    link.start(sys.now_ms);
    CHECK(link.state() == LinkState::Searching);
    step(sys, link, 1200);
    CHECK(link.state() == LinkState::Found);
    CHECK_EQ(link.port().tty, "ttyUSB0");
    CHECK_EQ(link.baud(), 9600);
    CHECK_EQ(link.port_title(), "ttyUSB0 (CH340)");
    CHECK(link.take_remembered_changed());
    CHECK(!link.take_remembered_changed());
    CHECK_EQ(link.remembered().baud, 9600);
    CHECK_EQ(link.remembered().port_id, "usb-1a86:7523");
    step(sys, link, 3000);
    const LinkStats s = link.stats(sys.now_ms);
    CHECK(s.fps > 17.5 && s.fps < 21.0); /* 19.3 Hz */
    CHECK(s.frames > 40);
    CHECK_EQ(s.bad_frames, 0u);
    CHECK(s.last_frame_age_ms >= 0 && s.last_frame_age_ms < 100);
    CHECK_EQ(link.tracker().frames(), s.frames);
    CHECK(link.last_frame().present_count() <= 1);
    CHECK_EQ(link.status(), "");
    /* clean close: restored, released */
    link.close();
    CHECK_EQ(sys.open_handles(), 0);
    CHECK_EQ(sys.by_tty("ttyUSB0")->restores, 1);
}

void test_found_restores_probe_only()
{
    /* the port that is kept is restored once, at close; the failed 256000 probe is not a separate open */
    FakeSys sys;
    FakePort &p = sys.add("ttyUSB0");
    p.stream[9600] = radar_stream();
    Link link(sys);
    link.start(0);
    step(sys, link, 1000);
    CHECK(link.state() == LinkState::Found);
    CHECK_EQ(p.opens, 1);
    CHECK_EQ(p.restores, 0);
    link.close();
    CHECK_EQ(p.restores, 1);
    CHECK_EQ(p.closes, 1);
    link.close(); /* twice is fine */
    CHECK_EQ(p.closes, 1);
}

void test_lost_and_redetect()
{
    FakeSys sys;
    FakePort &p = sys.add("ttyUSB0");
    p.stream[9600] = radar_stream();
    Link link(sys);
    link.start(0);
    step(sys, link, 2000);
    CHECK(link.state() == LinkState::Found);
    /* the frames stop (the port stays): lost after 2 s, re-detect finds it again when it talks again */
    sys.by_tty("ttyUSB0")->stopped = true;
    step(sys, link, 1800);
    CHECK(link.state() == LinkState::Found);
    step(sys, link, 400);
    CHECK(link.state() == LinkState::Lost);
    CHECK_EQ(sys.open_handles() <= 1, true);
    sys.by_tty("ttyUSB0")->stopped = false;
    step(sys, link, 9000);
    CHECK(link.state() == LinkState::Found);
    /* the remembered speed made the second detection a single try: 9600 only after the first full scan */
    const FakePort &q = *sys.by_tty("ttyUSB0");
    CHECK(q.bauds.size() >= 3);
    CHECK_EQ(q.bauds.back(), 9600);

    /* unplugged: a read error is "Lost" at once, the radar's slots expire */
    sys.unplug("ttyUSB0");
    step(sys, link, 700);
    CHECK(link.state() == LinkState::Lost);
    CHECK_EQ(sys.open_handles(), 0);
    CHECK(!link.tracker().slot(0).active);
    /* replugged on another number: found again by itself (hot-plug) */
    sys.add("ttyUSB1").stream[9600] = radar_stream();
    step(sys, link, 4000);
    CHECK(link.state() == LinkState::Found);
    CHECK_EQ(link.port().tty, "ttyUSB1");
}

void test_busy_and_hotplug()
{
    FakeSys sys;
    sys.add("ttyUSB0").lock_busy = true;
    Link link(sys);
    link.start(0);
    step(sys, link, 500);
    CHECK(link.state() == LinkState::Busy);
    CHECK_EQ(link.reports().size(), static_cast<std::size_t>(1));
    CHECK_EQ(link.reports()[0].text, "busy (held by another app)");
    /* the other app lets go: found by the retry */
    sys.by_tty("ttyUSB0")->lock_busy = false;
    sys.by_tty("ttyUSB0")->stream[9600] = radar_stream();
    step(sys, link, 6000);
    CHECK(link.state() == LinkState::Found);

    /* nothing plugged: Searching; a plug-in is found within a few seconds */
    FakeSys s2;
    Link l2(s2);
    l2.start(0);
    step(s2, l2, 3000);
    CHECK(l2.state() == LinkState::Searching);
    CHECK(l2.reports().empty());
    s2.add("ttyUSB0").stream[9600] = radar_stream();
    step(s2, l2, 4000);
    CHECK(l2.state() == LinkState::Found);

    /* denied: Error */
    FakeSys s3;
    s3.add("ttyUSB0").denied = true;
    Link l3(s3);
    l3.start(0);
    step(s3, l3, 300);
    CHECK(l3.state() == LinkState::Error);
}

void test_not_radar_is_left_alone()
{
    FakeSys sys;
    FakePort &g = sys.add("ttyACM0", 0x1234, 0x5678);
    g.stream[9600] = std::string("$GPGGA,1,2,3,4,5*00\r\n"); /* bad checksum on purpose: text */
    g.wrong_speed = "$GPGGA,1,2,3,4,5*00\r\n";
    g.rate_bps = 1000;
    Link link(sys);
    link.start(0);
    step(sys, link, 4000);
    CHECK(link.state() == LinkState::Searching);
    CHECK_EQ(link.reports().size(), static_cast<std::size_t>(1));
    const int opens = sys.by_tty("ttyACM0")->opens;
    step(sys, link, 20000); /* no more opens while the port list stays the same */
    CHECK_EQ(sys.by_tty("ttyACM0")->opens, opens);
    /* the user's retry scans it again */
    link.rescan(sys.now_ms);
    step(sys, link, 4000);
    CHECK(sys.by_tty("ttyACM0")->opens > opens);
    /* a radar plugged in next to it is found, the other still not reopened */
    const int opens2 = sys.by_tty("ttyACM0")->opens;
    sys.add("ttyUSB0").stream[9600] = radar_stream();
    step(sys, link, 4000);
    CHECK(link.state() == LinkState::Found);
    CHECK_EQ(sys.by_tty("ttyACM0")->opens, opens2);
}

} // namespace

void test_link()
{
    test_found_and_rates();
    test_found_restores_probe_only();
    test_lost_and_redetect();
    test_busy_and_hotplug();
    test_not_radar_is_left_alone();
}
