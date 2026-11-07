/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: position.state (core/position_state.cpp): the exact text of the file for a 3D fix, a 2D fix, no
 * fix and a stale fix; the writer (atomic: a temporary file renamed, never a half file; at most once a second; only
 * when the content changed; removed when the GPS goes off); the folder it goes to.
 */
#include "check.hpp"
#include "gps_fix.hpp"
#include "position_state.hpp"

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

using namespace atlas;

namespace {

NmeaMsg msg(const std::string &body) { return parse_nmea(nmea_frame(body)); }

GpsStatus status3d(std::int64_t at, const char *speed = "0.10", const char *course = "")
{
    FixTracker t;
    const std::string pos = "4853.37855,N,00216.19971,E";
    t.on_msg(msg(std::string("GNRMC,101530.00,A,") + pos + "," + speed + "," + course + ",150126,,,A,V"), at);
    t.on_msg(msg("GNGGA,101530.00," + pos + ",1,09,1.10,41.5,M,47.3,M,,"), at);
    t.on_msg(msg("GNGSA,A,3,02,03,04,05,06,07,08,09,10,,,,3.12,1.10,2.64,1"), at);
    t.on_msg(msg("GPGSV,1,1,12,02,20,030,28"), at);
    return make_status(GpsLinkState::Locked, t, at + 100);
}

std::string coord(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    return buf;
}

bool exists(const std::string &p)
{
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

std::string line_of(const std::string &text, const std::string &key)
{
    const std::string k = "\n" + key + "=";
    const std::size_t i = ("\n" + text).find(k);
    if (i == std::string::npos) return "<absent>";
    const std::size_t a = i + k.size() - 1;
    const std::size_t e = text.find('\n', a);
    return text.substr(a, e - a);
}

} // namespace

void test_position_state()
{
    PositionStateInfo info;
    info.pid = 2310;
    info.gen = 812;
    info.source = "usb:1546:01a8";
    info.now_ms = 123999;

    /* a 3D fix, moving: every documented key, the extra ones, in the documented order */
    {
        const GpsStatus s = status3d(123456, "10.10", "70.0");
        const std::string t = format_position_state(s, info);
        CHECK_EQ(t, std::string("v=1\ngen=812\npid=2310\nwriter=deck-atlas\nfix=3\nlat=" + coord(48.0 + 53.37855 / 60.0) +
                                "\nlon=" + coord(2.0 + 16.19971 / 60.0) + "\nalt_m=41.5\n"
                                "speed_mps=5.20\ncourse_deg=70.0\nacc_m=5.5\nhdop=1.10\nsats_used=9\nsats_view=12\n"
                                "utc=2026-01-15T10:15:30Z\nmono_ms=123456\nsource=usb:1546:01a8\n"));
    }
    /* a standstill: the course is unknown, so the key is absent (an absent key means unknown) */
    {
        const std::string t = format_position_state(status3d(1000), info);
        CHECK_EQ(line_of(t, "course_deg"), std::string("<absent>"));
        CHECK_EQ(line_of(t, "speed_mps"), std::string("0.05"));
        CHECK_EQ(line_of(t, "fix"), std::string("3"));
    }
    /* 2D: no altitude */
    {
        FixTracker t;
        t.on_msg(msg("GNRMC,101530.00,A,4853.37855,N,00216.19971,E,0.0,,150126,,,A,V"), 50);
        t.on_msg(msg("GNGGA,101530.00,4853.37855,N,00216.19971,E,1,04,4.00,,M,,M,,"), 50);
        t.on_msg(msg("GNGSA,A,2,02,03,04,05,,,,,,,,,5.00,4.00,3.00,1"), 50);
        const std::string text = format_position_state(make_status(GpsLinkState::Locked, t, 100), info);
        CHECK_EQ(line_of(text, "fix"), std::string("2"));
        CHECK_EQ(line_of(text, "alt_m"), std::string("<absent>"));
        CHECK_EQ(line_of(text, "acc_m"), std::string("20.0"));
        CHECK_EQ(line_of(text, "sats_used"), std::string("4"));
    }
    /* southern and western positions keep their sign */
    {
        FixTracker t;
        t.on_msg(msg("GNRMC,101530.00,A,3351.12345,S,07012.50000,W,0.0,,150126,,,A,V"), 50);
        const std::string text = format_position_state(make_status(GpsLinkState::Locked, t, 100), info);
        CHECK_EQ(line_of(text, "lat"), coord(-(33.0 + 51.12345 / 60.0)));
        CHECK_EQ(line_of(text, "lon"), coord(-(70.0 + 12.5 / 60.0)));
        CHECK(line_of(text, "lat")[0] == '-' && line_of(text, "lon")[0] == '-');
    }
    /* no fix, no device, stale: fix=0 and nothing that could be taken for a position */
    {
        FixTracker none;
        for (GpsLinkState l : {GpsLinkState::Searching, GpsLinkState::NoDevice, GpsLinkState::NoAccess, GpsLinkState::Locked}) {
            const std::string t = format_position_state(make_status(l, none, 500), info);
            CHECK_EQ(t, std::string("v=1\ngen=812\npid=2310\nwriter=deck-atlas\nfix=0\nmono_ms=123999\nsource=usb:1546:01a8\n"));
        }
        FixTracker t;
        t.on_msg(msg("GNRMC,101530.00,A,4853.37855,N,00216.19971,E,0.0,,150126,,,A,V"), 1000);
        const GpsStatus s = make_status(GpsLinkState::Locked, t, 1000 + kStaleMs + 1);
        CHECK(s.stale);
        const std::string text = format_position_state(s, info);
        CHECK_EQ(line_of(text, "fix"), std::string("0"));
        CHECK_EQ(line_of(text, "lat"), std::string("<absent>"));
        PositionStateInfo nosrc = info;
        nosrc.source.clear();
        CHECK_EQ(line_of(format_position_state(s, nosrc), "source"), std::string("<absent>"));
    }
    /* the folder: the launcher's state folder, else the runtime folder's applaunch, else none */
    {
        const char *old_state = std::getenv("APPLAUNCH_STATE_DIR");
        const char *old_xdg = std::getenv("XDG_RUNTIME_DIR");
        const std::string keep_state = old_state ? old_state : "", keep_xdg = old_xdg ? old_xdg : "";
        ::setenv("APPLAUNCH_STATE_DIR", "/tmp/st", 1);
        ::setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
        CHECK_EQ(position_state_dir(), std::string("/tmp/st"));
        ::unsetenv("APPLAUNCH_STATE_DIR");
        CHECK_EQ(position_state_dir(), std::string("/run/user/1000/applaunch"));
        ::unsetenv("XDG_RUNTIME_DIR");
        CHECK_EQ(position_state_dir(), std::string(""));
        if (old_state) ::setenv("APPLAUNCH_STATE_DIR", keep_state.c_str(), 1);
        if (old_xdg) ::setenv("XDG_RUNTIME_DIR", keep_xdg.c_str(), 1);
        PositionStateWriter off("", 1);
        off.update(status3d(0), "x", 10000);
        CHECK(!off.enabled());
        CHECK_EQ(off.writes(), 0);
    }
    /* the writer on a real folder: the file, atomic, rate-limited, change-driven, removed */
    {
        const std::string dir = test_temp_dir() + "/state"; /* not there yet: the writer makes it */
        PositionStateWriter w(dir, 4242);
        CHECK(w.enabled());
        CHECK_EQ(w.path(), dir + "/position.state");
        const GpsStatus s1 = status3d(1000);
        w.update(s1, "usb:1546:01a8", 1000);
        CHECK_EQ(w.writes(), 1);
        std::string text = test_read_file(w.path());
        CHECK_EQ(line_of(text, "v"), std::string("1"));
        CHECK_EQ(line_of(text, "pid"), std::string("4242"));
        CHECK_EQ(line_of(text, "gen"), std::string("1"));
        CHECK_EQ(line_of(text, "writer"), std::string("deck-atlas"));
        CHECK_EQ(line_of(text, "fix"), std::string("3"));
        CHECK_EQ(line_of(text, "mono_ms"), std::string("1000"));
        CHECK(!exists(w.path() + ".tmp")); /* the temporary file was renamed away */
        struct stat st;
        CHECK_EQ(::stat(w.path().c_str(), &st), 0);
        CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
        /* the same content again: nothing is written */
        w.update(s1, "usb:1546:01a8", 1500);
        w.update(s1, "usb:1546:01a8", 3000);
        CHECK_EQ(w.writes(), 1);
        /* a new fix 400 ms later: held back until a second has passed since the last write, then written once */
        const GpsStatus s2 = status3d(1400, "10.10", "70.0");
        w.update(s2, "usb:1546:01a8", 1400);
        CHECK_EQ(w.writes(), 1);
        w.update(s2, "usb:1546:01a8", 1900);
        CHECK_EQ(w.writes(), 1);
        w.update(s2, "usb:1546:01a8", 2000);
        CHECK_EQ(w.writes(), 2);
        text = test_read_file(w.path());
        CHECK_EQ(line_of(text, "gen"), std::string("2"));
        CHECK_EQ(line_of(text, "course_deg"), std::string("70.0"));
        CHECK_EQ(line_of(text, "mono_ms"), std::string("1400"));
        w.update(s2, "usb:1546:01a8", 2100);
        CHECK_EQ(w.writes(), 2);
        /* the receiver is lost: fix=0 is written (after a second), once */
        FixTracker nothing;
        const GpsStatus lost = make_status(GpsLinkState::NoDevice, nothing, 2600);
        w.update(lost, "", 2600);
        CHECK_EQ(w.writes(), 2); /* a second has not passed since the write at 2000 */
        w.update(lost, "", 3000);
        CHECK_EQ(w.writes(), 3);
        text = test_read_file(w.path());
        CHECK_EQ(line_of(text, "fix"), std::string("0"));
        CHECK_EQ(line_of(text, "lat"), std::string("<absent>"));
        w.update(lost, "", 9000);
        CHECK_EQ(w.writes(), 3);
        /* switched off: the file goes */
        w.update(make_status(GpsLinkState::Off, nothing, 10000), "", 10000);
        CHECK(!exists(w.path()));
        w.update(make_status(GpsLinkState::Off, nothing, 11000), "", 11000); /* again: nothing to remove */
        /* back on: written again from gen 4 on */
        w.update(s2, "usb:1546:01a8", 12000);
        CHECK(exists(w.path()));
        CHECK_EQ(line_of(test_read_file(w.path()), "gen"), std::string("4"));
        w.clear();
        CHECK(!exists(w.path()));
        ::rmdir(dir.c_str());
        test_remove_tree(dir.substr(0, dir.rfind('/')));
    }
    /* a failing write does not crash and is retried a second later */
    {
        int calls = 0;
        PositionStateWriter w("/proc/deck-atlas-test/state", 1, [&calls](const std::string &, const std::string &) {
            ++calls;
            return false;
        });
        const GpsStatus s = status3d(100);
        w.update(s, "", 100);
        w.update(s, "", 600);
        CHECK_EQ(calls, 1);
        w.update(s, "", 1200);
        CHECK_EQ(calls, 2);
        CHECK_EQ(w.writes(), 0);
    }
}
