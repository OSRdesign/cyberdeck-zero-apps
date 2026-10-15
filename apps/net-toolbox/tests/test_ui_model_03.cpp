/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3 tests: what the speed test, Wake-on-LAN and radar pages say and compute (src/ui/view_model_03.cpp and the
 * demonstration's snapshots, src/ui/demo_data_03.cpp, compiled in here: neither has LVGL): the dial's scale steps, the
 * needle's angle, the tick numbers, the easing, the grade lines, the result rows, the status lines, the layout of the live
 * block, the wake status and strip, the wording of a bad name or MAC, the radar's blip angle, radius, brightness and sweep,
 * the Bluetooth problems with their fix, the details of a device.
 */
#include "check.hpp"

#include "../src/ui/demo_data_03.cpp"
#include "../src/ui/view_model_03.cpp"

#include <set>

using namespace nt;
using namespace nt::ui;

namespace {

const RowDef *row_of(const std::vector<RowDef> &rows, const std::string &label)
{
    for (const RowDef &r : rows)
        if (r.label == label) return &r;
    return nullptr;
}

SpeedSnapshot speed(const char *scenario) { return demo_speed(scenario, SpeedConfig()); }

WolRequest wol_request(const char *host)
{
    WolRequest req;
    req.machine.name = "Desk PC";
    req.machine.mac = "b8:27:eb:10:20:30";
    req.machine.host = host;
    const auto src = make_fixture_source(fixture_dir("deck"));
    req.net = read_net_state(*src);
    return req;
}

WolSnapshot wake(const char *scenario, const char *host = "192.168.68.20") { return demo_wol(scenario, wol_request(host)); }

BleSnapshot ble(const char *scenario) { return demo_ble(scenario, BleConfig()); }

bool overlap(const Box &a, const Box &b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

bool ends_with_dots(const std::string &s) { return s.size() >= 3 && s.compare(s.size() - 3, 3, "...") == 0; }

void test_gauge()
{
    const std::vector<double> &steps = gauge_steps();
    CHECK_EQ(steps.size(), std::size_t(7));
    CHECK_NEAR(steps.front(), 10, 1e-9);
    CHECK_NEAR(steps.back(), 1000, 1e-9);
    CHECK_NEAR(gauge_scale_for(0), 10, 1e-9);
    CHECK_NEAR(gauge_scale_for(9.2), 10, 1e-9);
    CHECK_NEAR(gauge_scale_for(9.3), 25, 1e-9);
    CHECK_NEAR(gauge_scale_for(23), 25, 1e-9);
    CHECK_NEAR(gauge_scale_for(24), 50, 1e-9);
    CHECK_NEAR(gauge_scale_for(46), 50, 1e-9);
    CHECK_NEAR(gauge_scale_for(47), 100, 1e-9);
    CHECK_NEAR(gauge_scale_for(93), 250, 1e-9);
    CHECK_NEAR(gauge_scale_for(612), 1000, 1e-9);
    CHECK_NEAR(gauge_scale_for(5000), 1000, 1e-9);

    GaugeScaler s;
    s.reset();
    CHECK_NEAR(s.scale, 10, 1e-9);
    CHECK(!s.feed(3));
    CHECK(s.feed(30));
    CHECK_NEAR(s.scale, 50, 1e-9);
    CHECK(!s.feed(4)); /* never back down during a run */
    CHECK(!s.feed(31));
    CHECK_NEAR(s.scale, 50, 1e-9);
    CHECK(s.feed(612));
    CHECK_NEAR(s.scale, 1000, 1e-9);
    CHECK(!s.feed(std::nan("")));
    CHECK_NEAR(s.scale, 1000, 1e-9);
    s.reset();
    CHECK_NEAR(s.scale, 10, 1e-9);
    CHECK_NEAR(s.max_seen, 0, 1e-9);

    /* the needle: -110 degrees at 0, straight up at half, +110 at the full scale, never beyond */
    CHECK_NEAR(gauge_needle_deg(0, 100), -110, 1e-9);
    CHECK_NEAR(gauge_needle_deg(50, 100), 0, 1e-9);
    CHECK_NEAR(gauge_needle_deg(100, 100), 110, 1e-9);
    CHECK_NEAR(gauge_needle_deg(25, 100), -55, 1e-9);
    CHECK_NEAR(gauge_needle_deg(500, 100), 110, 1e-9);
    CHECK_NEAR(gauge_needle_deg(-5, 100), -110, 1e-9);
    CHECK_NEAR(gauge_needle_deg(std::nan(""), 100), -110, 1e-9);
    CHECK_NEAR(gauge_needle_deg(5, 0), -110, 1e-9);
    CHECK_NEAR(gauge_fraction(23.4, 50), 0.468, 1e-9);

    /* the numbers: six, from 0 to the scale in five equal steps, with a minor tick between each pair */
    std::vector<GaugeTick> t = gauge_ticks(10);
    CHECK_EQ(t.size(), std::size_t(11));
    std::vector<std::string> labels;
    for (const GaugeTick &k : t)
        if (k.major) labels.push_back(k.label);
        else CHECK(k.label.empty());
    CHECK_EQ(labels.size(), std::size_t(6));
    CHECK_EQ(labels[0], std::string("0"));
    CHECK_EQ(labels[1], std::string("2"));
    CHECK_EQ(labels[5], std::string("10"));
    CHECK_NEAR(t[1].value, 1.0, 1e-9);
    t = gauge_ticks(25);
    CHECK_EQ(t[2].label, std::string("5"));
    CHECK_EQ(t[10].label, std::string("25"));
    t = gauge_ticks(1000);
    CHECK_EQ(t[4].label, std::string("400"));
    CHECK_EQ(t[10].label, std::string("1000"));
    for (const double scale : steps) {
        const std::vector<GaugeTick> k = gauge_ticks(scale);
        CHECK_NEAR(k.front().value, 0, 1e-9);
        CHECK_NEAR(k.back().value, scale, 1e-9);
        for (std::size_t i = 1; i < k.size(); ++i) CHECK(k[i].value > k[i - 1].value);
    }

    /* the easing */
    CHECK_NEAR(ease_out(0, 10, 0, 180), 0, 1e-9);
    CHECK_NEAR(ease_out(0, 10, 180, 180), 10, 1e-9);
    CHECK_NEAR(ease_out(0, 10, 500, 180), 10, 1e-9);
    CHECK_NEAR(ease_out(4, 4, 50, 180), 4, 1e-9);
    double prev = 0;
    for (int ms = 0; ms <= 180; ms += 20) {
        const double v = ease_out(0, 10, ms, 180);
        CHECK(v >= prev - 1e-12);
        prev = v;
    }
    CHECK(ease_out(0, 10, 90, 180) > 5.0); /* ease-out: more than half done at half the time */
    CHECK(ease_out(10, 2, 90, 180) < 6.0);
    CHECK_NEAR(ease_out(1, 2, 10, 0), 2, 1e-9);

    /* the points around the dial */
    double x, y;
    polar_point(100, 100, 50, 0, x, y);
    CHECK_NEAR(x, 100, 1e-9);
    CHECK_NEAR(y, 50, 1e-9);
    polar_point(100, 100, 50, 90, x, y);
    CHECK_NEAR(x, 150, 1e-9);
    CHECK_NEAR(y, 100, 1e-9);
    polar_point(100, 100, 50, 180, x, y);
    CHECK_NEAR(y, 150, 1e-9);
    polar_point(100, 100, 50, -90, x, y);
    CHECK_NEAR(x, 50, 1e-9);

    for (const int outer : {60, 100, 128, 175, 290}) {
        const GaugeRadii r = gauge_radii(outer);
        CHECK(r.inner < r.outer);
        CHECK(r.needle < r.inner);
        CHECK(r.label < r.inner - r.tick_major);
        CHECK(r.hub < r.label);
        CHECK(r.tick_minor <= r.tick_major);
    }
}

void test_speed_texts()
{
    CHECK_EQ(speed_value_text(23.44), std::string("23.4"));
    CHECK_EQ(speed_value_text(4.2), std::string("4.2"));
    CHECK_EQ(speed_value_text(234.4), std::string("234"));
    CHECK_EQ(speed_value_text(99.97), std::string("100"));
    CHECK_EQ(speed_value_text(0.4), std::string("0.40"));
    CHECK_EQ(speed_value_text(-1), std::string("--"));
    CHECK_EQ(ms_text(18.4), std::string("18 ms"));
    CHECK_EQ(ms_text(3.24), std::string("3.2 ms"));
    CHECK_EQ(ms_text(-1), std::string("--"));
    CHECK_EQ(ms_delta_text(34.4), std::string("+34 ms"));
    CHECK_EQ(ms_delta_text(-2.2), std::string("-2 ms"));
    CHECK_EQ(speed_note(SpeedConfig().max_bytes),
             std::string("Uses up to about 150 MB each way. Start it yourself; on a phone hotspot that is mobile data."));
    CHECK_EQ(speed_note(50000000), std::string("Uses up to about 50 MB each way. Start it yourself; on a phone hotspot that is mobile data."));

    CHECK_EQ(speed_phase_label(SpeedSnapshot()), std::string("Ready"));
    CHECK_EQ(speed_phase_label(speed("latency")), std::string("Latency"));
    CHECK_EQ(speed_phase_label(speed("down-mid")), std::string("Download"));
    CHECK_EQ(speed_phase_label(speed("up")), std::string("Upload"));
    CHECK_EQ(speed_phase_label(speed("done-a")), std::string("Finished"));
    CHECK_EQ(speed_phase_label(demo_speed_stop(speed("stopped"))), std::string("Stopped"));
    CHECK_EQ(speed_phase_label(speed("e-tls")), std::string("Failed"));

    CHECK_NEAR(speed_progress(speed("down-mid"), 8), 4.5 / 8, 1e-9);
    CHECK_EQ(speed_progress_text(speed("down-mid"), 8), std::string("4 s of 8 s"));
    CHECK_NEAR(speed_progress(speed("latency"), 8), 0, 1e-9);
    CHECK_EQ(speed_progress_text(speed("latency"), 8), std::string("Latency"));
    CHECK_NEAR(speed_progress(speed("done-a"), 8), 0, 1e-9);
    CHECK_EQ(speed_progress_text(speed("done-a"), 8), std::string(""));
    SpeedSnapshot over = speed("down-mid");
    over.phase_s = 12;
    CHECK_NEAR(speed_progress(over, 8), 1.0, 1e-9);
    CHECK_EQ(speed_progress_text(over, 8), std::string("8 s of 8 s"));
}

void test_speed_grade()
{
    CHECK_EQ(grade_line(BloatGrade::A), std::string("A: latency stays flat under load"));
    CHECK_EQ(grade_line(BloatGrade::F), std::string("F: latency explodes under load: heavy buffering in the router"));
    for (const BloatGrade g : {BloatGrade::A, BloatGrade::B, BloatGrade::C, BloatGrade::D, BloatGrade::F}) {
        CHECK(grade_line(g).rfind(grade_letter(g), 0) == 0); /* the line starts with its letter */
        CHECK(!ends_with_dots(grade_line(g)));
    }
    CHECK(grade_tone(BloatGrade::A) == Tone::Ok);
    CHECK(grade_tone(BloatGrade::B) == Tone::Ok);
    CHECK(grade_tone(BloatGrade::C) == Tone::Warn);
    CHECK(grade_tone(BloatGrade::D) == Tone::Bad);
    CHECK(grade_tone(BloatGrade::F) == Tone::Bad);
    CHECK(grade_tone(BloatGrade::Unknown) == Tone::Normal);
    CHECK(grade_mark(BloatGrade::A) == Mark::Check);
    CHECK(grade_mark(BloatGrade::C) == Mark::Bang);
    CHECK(grade_mark(BloatGrade::F) == Mark::Cross);
    CHECK(grade_mark(BloatGrade::Unknown) == Mark::Dash);

    const SpeedGrade a = speed_grade(speed("done-a").result);
    CHECK_EQ(a.letter, std::string("A"));
    CHECK_EQ(a.line, std::string("A: latency stays flat under load"));
    CHECK(a.tone == Tone::Ok);
    const SpeedGrade c = speed_grade(speed("done-c").result);
    CHECK_EQ(c.letter, std::string("C")); /* the worse direction: +120 ms down, +45 ms up */
    CHECK(c.tone == Tone::Warn && c.mark == Mark::Bang);
    const SpeedGrade f = speed_grade(speed("done-f").result);
    CHECK_EQ(f.letter, std::string("F"));
    CHECK(f.tone == Tone::Bad && f.mark == Mark::Cross);
    const SpeedGrade none = speed_grade(speed("done-noping").result);
    CHECK_EQ(none.letter, std::string("-"));
    CHECK_EQ(none.line, std::string("Not measured: No ping target: the latency under load is not measured"));
    CHECK(!ends_with_dots(none.line));
}

void test_speed_rows()
{
    const SpeedSnapshot c = speed("done-c");
    const std::vector<RowDef> rows = speed_rows(c);
    CHECK_EQ(rows.size(), std::size_t(5));
    CHECK_EQ(rows[0].label, std::string("Latency (idle)"));
    CHECK_EQ(rows[0].value, std::string("18 ms"));
    CHECK_EQ(rows[1].label, std::string("Jitter"));
    const RowDef *down = row_of(rows, "Latency while downloading");
    const RowDef *up = row_of(rows, "Latency while uploading");
    CHECK(down && up);
    if (down && up) {
        CHECK(down->value.find("(+") != std::string::npos); /* the difference is in the words */
        CHECK(down->value.find(" ms)") != std::string::npos);
        CHECK(down->tone == Tone::Warn); /* +120 ms is a C */
        CHECK(up->tone == Tone::Warn || up->tone == Tone::Ok);
    }
    const RowDef *server = row_of(rows, "Server");
    CHECK(server && server->value == "Cloudflare, CDG");
    CHECK_EQ(speed_server_text(SpeedResult()), std::string(""));

    /* a run stopped during the download keeps what it had: the latency, no loaded row for a direction not measured */
    const SpeedSnapshot stopped = demo_speed_stop(speed("stopped"));
    const std::vector<RowDef> part = speed_rows(stopped);
    CHECK(row_of(part, "Latency (idle)") != nullptr);
    CHECK(row_of(part, "Latency while downloading") == nullptr);
    /* nothing measured: no rows at all */
    CHECK(speed_rows(SpeedSnapshot()).empty());
    CHECK(speed_rows(speed("e-tls")).empty());
    /* no ping: a note says why, no loaded latency numbers */
    const std::vector<std::string> notes = speed_notes(speed("done-noping"));
    CHECK_EQ(notes.size(), std::size_t(1));
    CHECK_EQ(notes[0], std::string("No ping target: the latency under load is not measured."));
    CHECK(speed_notes(speed("done-a")).empty());
}

void test_speed_band()
{
    CHECK_EQ(speed_band(SpeedSnapshot(), 8).text, std::string("Press Start to test your connection"));
    CHECK_EQ(speed_band(speed("latency"), 8).text, std::string("Measuring latency"));
    CHECK_EQ(speed_band(speed("down-mid"), 8).text, std::string("Downloading, 4 s of 8 s"));
    CHECK_EQ(speed_band(speed("up"), 8).text, std::string("Uploading, 3 s of 8 s"));
    CHECK_EQ(speed_band(speed("done-a"), 8).text, std::string("Done: 23.4 Mbit/s down, 31.2 up, grade A"));
    CHECK(speed_band(speed("done-a"), 8).tone == Tone::Ok);
    CHECK_EQ(speed_band(speed("done-f"), 8).text, std::string("Done: 23.4 Mbit/s down, 31.2 up, grade F"));
    CHECK(speed_band(speed("done-f"), 8).tone == Tone::Bad);
    CHECK_EQ(speed_band(speed("done-noping"), 8).text, std::string("Done: 23.4 Mbit/s down, 31.2 up"));
    CHECK_EQ(speed_band(demo_speed_stop(speed("stopped")), 8).text, std::string("Stopped"));
    CHECK_EQ(speed_band(speed("e-network"), 8).text, std::string("No connection"));
    CHECK_EQ(speed_band(speed("e-tls"), 8).text, std::string("Secure connection failed"));
    CHECK_EQ(speed_band(speed("e-refused"), 8).text, std::string("Cloudflare refused the test"));
    CHECK(speed_band(speed("e-tls"), 8).tone == Tone::Bad);
    CHECK_EQ(speed_error_title("something odd"), std::string("Speed test failed"));
    CHECK_EQ(speed_error_title("Cloudflare's certificate was not accepted. Is the board's clock right?"), std::string("Certificate not accepted"));
    for (const char *sc : {"latency", "down-low", "down-mid", "down-high", "up", "done-a", "done-b", "done-c", "done-d", "done-f",
                           "done-noping", "stopped", "e-network", "e-tls", "e-refused"}) {
        const SpeedSnapshot s = speed(sc);
        CHECK(!ends_with_dots(speed_band(s, 8).text));
        CHECK(!ends_with_dots(speed_band(demo_speed_stop(s), 8).text));
        CHECK(!ends_with_dots(speed_grade(s.result).line));
        for (const std::string &n : speed_notes(s)) CHECK(!ends_with_dots(n));
    }
    /* the error words of the core are plain and complete */
    CHECK(speed("e-network").error.find("speed.cloudflare.com") != std::string::npos);
    CHECK_EQ(speed("e-tls").error, std::string("The secure (TLS) connection to Cloudflare failed."));
    CHECK_EQ(speed("e-refused").error, std::string("Cloudflare refused the request (HTTP 403)."));
}

void test_speed_layout()
{
    struct Size {
        int w, h, lh, big;
    };
    /* the list areas of the five screens as the app builds them (roughly): width, height, caption line, big number */
    const Size sizes[] = {{560, 252, 22, 40}, {720, 252, 22, 40}, {640, 470, 22, 40}, {944, 360, 24, 44}, {960, 480, 26, 48}};
    for (const Size &z : sizes) {
        const SpeedLayout L = speed_layout(z.w, z.h, z.lh, z.big, 8);
        CHECK(L.radius >= 90);
        CHECK(L.gauge.x >= 0 && L.gauge.y >= 0 && L.gauge.x + L.gauge.w <= z.w && L.gauge.y + L.gauge.h <= z.h);
        CHECK(L.chart.x >= 0 && L.chart.y >= 0 && L.chart.x + L.chart.w <= z.w && L.chart.y + L.chart.h <= z.h);
        CHECK(L.progress.x >= 0 && L.progress.y >= 0 && L.progress.x + L.progress.w <= z.w && L.progress.y + L.progress.h <= z.h);
        CHECK(L.cx - L.radius >= L.gauge.x && L.cx + L.radius <= L.gauge.x + L.gauge.w);
        CHECK(L.cy - L.radius >= 0);
        /* the texts under the hub fit under the dial's centre */
        CHECK(L.cy + static_cast<int>(0.08 * L.radius) + 2 * z.lh + z.big <= z.h);
        CHECK(!overlap(L.chart, L.progress));
        if (L.side) CHECK(L.gauge.x + L.gauge.w <= L.chart.x);
        else CHECK(L.gauge.y + L.gauge.h <= L.chart.y);
        CHECK(L.chart.w >= 120 && L.chart.h >= 2 * z.lh);
    }
    CHECK(speed_layout(560, 252, 22, 40, 8).side);   /* 640x480: side by side, the dial as large as the height allows */
    CHECK(!speed_layout(640, 470, 22, 40, 8).side);  /* a tall area: the chart under the dial */
    CHECK(speed_layout(960, 480, 26, 48, 8).side);
}

void test_wake()
{
    /* the sent line */
    const WolSnapshot awake = wake("awake");
    CHECK_EQ(wol_sent_text(awake.send), std::string("Packet sent on wlan0"));
    WolSendResult two;
    two.sent = 4;
    for (const char *iface : {"wlan0", "wlan1", "wlan0", "wlan1"}) {
        WolPacket p;
        p.dest.iface = iface;
        p.words = "sent";
        two.packets.push_back(p);
    }
    CHECK_EQ(wol_sent_text(two), std::string("Packet sent on wlan0, wlan1"));
    two.errors = 1;
    two.sent = 3;
    two.packets[3].error = 101;
    two.packets[3].words = "network unreachable";
    CHECK_EQ(wol_sent_text(two), std::string("Packet sent on wlan0, wlan1. Failed on wlan1: network unreachable"));
    WolSendResult none;
    none.errors = 2;
    none.summary = "Nothing could be sent: network unreachable.";
    CHECK_EQ(wol_sent_text(none), std::string("Nothing could be sent: network unreachable."));

    /* the status: a shape and a word each */
    const WakeStatus idle = wake_status(WolSnapshot());
    CHECK(idle.kind == WakeKind::None);
    CHECK_EQ(idle.word, std::string("Not sent yet"));
    const WakeStatus sent = wake_status(wake("sent"));
    CHECK(sent.kind == WakeKind::Sent && sent.mark == Mark::Arrow);
    CHECK_EQ(sent.word, std::string("Sent"));
    CHECK_EQ(sent.detail, std::string("Packet sent on wlan0"));
    const WakeStatus waking = wake_status(wake("waking"));
    CHECK(waking.kind == WakeKind::Waking && waking.spinning);
    CHECK_EQ(waking.word, std::string("Waking... 37 s"));
    const WakeStatus ok = wake_status(awake);
    CHECK(ok.kind == WakeKind::Awake && ok.tone == Tone::Ok && ok.mark == Mark::Check);
    CHECK_EQ(ok.word, std::string("Awake after 22 s"));
    const WakeStatus once = wake_status(wake("once"));
    CHECK_EQ(once.word, std::string("Already awake"));
    CHECK(once.mark == Mark::Check);
    const WakeStatus no = wake_status(wake("noanswer"));
    CHECK(no.kind == WakeKind::NoAnswer && no.tone == Tone::Warn && no.mark == Mark::Dash);
    CHECK_EQ(no.word, std::string("No answer yet"));
    CHECK_EQ(no.detail, std::string("No answer after 120 s"));
    const WakeStatus bad = wake_status(wake("failed"));
    CHECK(bad.kind == WakeKind::Failed && bad.tone == Tone::Bad && bad.mark == Mark::Cross);
    CHECK_EQ(bad.word, std::string("Not sent"));
    const WakeStatus stopped = wake_status(demo_wol_stop(wake("waking")));
    CHECK(stopped.kind == WakeKind::Stopped);
    CHECK_EQ(stopped.word, std::string("Watch stopped"));
    /* no host to watch: the packet is sent, nothing more is said */
    CHECK(wake_status(wake("awake", "")).kind == WakeKind::Sent);

    /* the strip: one cell per second, grey while waiting, green from the first answer */
    std::vector<StripCell> strip = wake_strip(wake("waking"));
    CHECK_EQ(strip.size(), std::size_t(kWakeStripCells));
    CHECK(strip[0] == StripCell::Waiting && strip[36] == StripCell::Waiting);
    CHECK(strip[37] == StripCell::Future && strip[119] == StripCell::Future);
    strip = wake_strip(awake);
    CHECK(strip[0] == StripCell::Waiting && strip[21] == StripCell::Waiting);
    CHECK(strip[22] == StripCell::Awake && strip[119] == StripCell::Awake);
    strip = wake_strip(wake("noanswer"));
    for (const StripCell c : strip) CHECK(c == StripCell::Waiting);
    strip = wake_strip(WolSnapshot());
    for (const StripCell c : strip) CHECK(c == StripCell::Future);
    strip = wake_strip(wake("once"));
    CHECK(strip[0] == StripCell::Awake);
    CHECK_EQ(wake_strip(awake, 60).size(), std::size_t(60));

    /* the list's last status */
    const std::time_t at = 1792072920; /* any time: the text is the board's local time */
    char want[32];
    struct tm tm{};
    localtime_r(&at, &tm);
    std::strftime(want, sizeof(want), "Sent %H:%M", &tm);
    CHECK_EQ(wake_last_text(wake_last_of(wake("sent"), at)), std::string(want));
    CHECK_EQ(wake_last_text(wake_last_of(wake("waking"), at)), std::string(want));
    CHECK_EQ(wake_last_text(wake_last_of(awake, at)), std::string("Awake after 22 s"));
    CHECK_EQ(wake_last_text(wake_last_of(wake("once"), at)), std::string("Already awake"));
    CHECK_EQ(wake_last_text(wake_last_of(wake("noanswer"), at)), std::string("No answer yet"));
    CHECK_EQ(wake_last_text(wake_last_of(wake("failed"), at)), std::string("Not sent"));
    CHECK_EQ(wake_last_text(WakeLast()), std::string("Not woken yet"));
    CHECK(wake_last_tone(wake_last_of(awake, at)) == Tone::Ok);
    CHECK(wake_last_tone(wake_last_of(wake("noanswer"), at)) == Tone::Warn);
    CHECK(wake_last_tone(wake_last_of(wake("failed"), at)) == Tone::Bad);
    CHECK(wake_last_tone(wake_last_of(wake("sent"), at)) == Tone::Normal);

    /* the status line */
    CHECK_EQ(wol_band(WolSnapshot(), "Desk PC").text, std::string("Press Wake now to send the magic packet"));
    CHECK_EQ(wol_band(wake("waking"), "Desk PC").text, std::string("Packet sent, waiting for 192.168.68.20: 37 s"));
    CHECK_EQ(wol_band(awake, "Desk PC").text, std::string("192.168.68.20 answers after 22 s"));
    CHECK(wol_band(awake, "Desk PC").tone == Tone::Ok);
    CHECK_EQ(wol_band(wake("once"), "Desk PC").text, std::string("192.168.68.20 answered at once: probably awake already"));
    CHECK_EQ(wol_band(wake("noanswer"), "Desk PC").text, std::string("No answer from 192.168.68.20 after 120 s"));
    CHECK(wol_band(wake("noanswer"), "Desk PC").tone == Tone::Warn);
    CHECK_EQ(wol_band(wake("sent"), "Desk PC").text, std::string("Magic packet sent"));
    CHECK_EQ(wol_band(wake("failed"), "Desk PC").text, std::string("Nothing was sent"));
    CHECK_EQ(wol_band(demo_wol_stop(wake("waking")), "Desk PC").text, std::string("Stopped"));
    for (const char *sc : {"sent", "waking", "awake", "once", "noanswer", "failed"}) {
        const WolSnapshot s = wake(sc);
        CHECK(!ends_with_dots(wol_band(s, "x").text));
        CHECK(!ends_with_dots(wake_status(s).detail));
    }
    /* a long host does not run the line over */
    WolSnapshot longhost = wake("waking");
    longhost.watch_target = std::string(60, 'h') + ".example.org";
    CHECK_EQ(wol_band(longhost, "x").text, std::string("Packet sent, waiting for the machine: 37 s"));
}

void test_wake_input()
{
    Settings st;
    for (const WolMachine &m : demo_wol_machines(3)) CHECK(st.save_machine(m));
    CHECK_EQ(wol_name_problem("", st, ""), std::string("Type a name for the machine"));
    CHECK_EQ(wol_name_problem("   ", st, ""), std::string("Type a name for the machine"));
    CHECK_EQ(wol_name_problem("Garage", st, ""), std::string(""));
    CHECK(!wol_name_problem(std::string(33, 'x'), st, "").empty());
    CHECK(!wol_name_problem("a|b", st, "").empty());
    CHECK_EQ(wol_name_problem("NAS", st, ""), std::string("A machine called NAS is already saved"));
    CHECK_EQ(wol_name_problem("nas", st, ""), std::string("A machine called NAS is already saved"));
    CHECK_EQ(wol_name_problem("NAS", st, "NAS"), std::string("")); /* editing a machine keeps its own name */
    CHECK_EQ(wol_name_problem("nas", st, "NAS"), std::string(""));
    CHECK_EQ(wol_name_problem("NAS", st, "Desk PC"), std::string("A machine called NAS is already saved"));

    CHECK_EQ(wol_mac_problem("aa:bb:cc:dd:ee:ff"), std::string(""));
    CHECK_EQ(wol_mac_problem("AA-BB-CC-DD-EE-FF"), std::string(""));
    CHECK_EQ(wol_mac_problem("aabbccddeeff"), std::string(""));
    CHECK_EQ(wol_mac_problem("  aa:bb:cc:dd:ee:ff  "), std::string(""));
    for (const char *bad : {"", "aa:bb:cc", "aa:bb:cc:dd:ee:gg", "aa:bb-cc:dd-ee:ff", "aabbccddeeff00", "00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff",
                            "01:00:5e:00:00:01"}) {
        const std::string why = wol_mac_problem(bad);
        CHECK(!why.empty());
        CHECK(!ends_with_dots(why));
    }
    CHECK_EQ(wol_host_problem(""), std::string(""));
    CHECK_EQ(wol_host_problem("192.168.68.20"), std::string(""));
    CHECK_EQ(wol_host_problem("nas.local"), std::string(""));
    CHECK(!wol_host_problem("a b").empty());
    CHECK(!wol_host_problem("bad host!").empty());
    CHECK_EQ(std::string(kWolFullText), std::string("You can save up to 20 machines: delete one first."));
    CHECK_EQ(demo_wol_machines(25).size(), std::size_t(20));
    Settings full;
    for (const WolMachine &m : demo_wol_machines(20)) CHECK(full.save_machine(m));
    WolMachine extra = demo_wol_machines(1)[0];
    extra.name = "One too many";
    std::string why;
    CHECK(!full.save_machine(extra, &why));
    CHECK(!why.empty());
}

void test_ble_texts()
{
    CHECK_EQ(ble_band(BleSnapshot()).text, std::string("Press Start to scan for Bluetooth devices"));
    CHECK_EQ(ble_band(ble("scan25")).text, std::string("Scanning, 25 devices, 37 s left"));
    CHECK_EQ(ble_band(ble("scan6")).text, std::string("Scanning, 6 devices, 37 s left"));
    CHECK_EQ(ble_band(ble("scan0")).text, std::string("Scanning, 0 devices, 37 s left"));
    BleSnapshot one = ble("scan6");
    one.live = 1;
    CHECK_EQ(ble_band(one).text, std::string("Scanning, 1 device, 37 s left"));
    one.elapsed_s = 70;
    CHECK_EQ(ble_band(one).text, std::string("Scanning, 1 device, 0 s left"));
    BleSnapshot checking;
    checking.state = BleSnapshot::State::Checking;
    CHECK_EQ(ble_band(checking).text, std::string("Checking Bluetooth"));
    CHECK_EQ(ble_band(demo_ble_stop(ble("scan25"))).text, std::string("Stopped, 29 devices seen"));
    CHECK_EQ(ble_band(ble("done25")).text, std::string("Finished, 29 devices seen"));
    BleSnapshot seen1 = demo_ble_stop(ble("scan6"));
    seen1.seen = 1;
    CHECK_EQ(ble_band(seen1).text, std::string("Stopped, 1 device seen"));

    /* the reasons, with the one-line fix */
    BlePlain p = ble_problem(ble("off").error);
    CHECK_EQ(p.headline, std::string("Bluetooth is switched off"));
    CHECK_EQ(p.fix, std::string("Turn Bluetooth on in Settings > Bluetooth"));
    p = ble_problem(ble("rfkill").error);
    CHECK_EQ(p.headline, std::string("Bluetooth is blocked by rfkill"));
    CHECK_EQ(p.fix, std::string("Turn Bluetooth on in Settings > Bluetooth"));
    p = ble_problem(ble("none").error);
    CHECK_EQ(p.headline, std::string("No Bluetooth adapter"));
    CHECK(!p.fix.empty());
    p = ble_problem(ble("bluez").error);
    CHECK_EQ(p.headline, std::string("Bluetooth is not installed"));
    CHECK(p.fix.find("bluez") != std::string::npos);
    p = ble_problem("Bluetooth is switched off by a hardware switch (rfkill hard block).");
    CHECK_EQ(p.headline, std::string("Bluetooth is blocked by a switch"));
    p = ble_problem("The Bluetooth service (bluetoothd) is not running.");
    CHECK_EQ(p.headline, std::string("Bluetooth service is not running"));
    p = ble_problem("BlueZ sees no Bluetooth adapter.");
    CHECK_EQ(p.headline, std::string("No Bluetooth adapter"));
    p = ble_problem("something odd");
    CHECK_EQ(p.headline, std::string("The scan could not run"));
    CHECK(p.fix.empty());
    CHECK_EQ(ble_band(ble("off")).text, std::string("Bluetooth is switched off"));
    CHECK(ble_band(ble("off")).tone == Tone::Warn);
    for (const char *sc : {"off", "rfkill", "none", "bluez", "scan25", "scan0"}) {
        CHECK(!ends_with_dots(ble_band(ble(sc)).text));
        CHECK(!ends_with_dots(ble_problem(ble(sc).error).fix));
    }
    CHECK_EQ(std::string(kBleWarnText), std::string("Wi-Fi and Bluetooth share one radio: a Bluetooth keyboard may stutter while scanning."));
    CHECK_EQ(std::string(kBleLegend), std::string("Signal strength, not distance"));
}

void test_radar()
{
    /* the angle: stable, from the address only, in [0, 360) */
    CHECK_NEAR(blip_angle_deg("A4:C1:38:5E:01:7B"), blip_angle_deg("a4:c1:38:5e:01:7b"), 1e-9);
    std::set<int> angles;
    for (const BleDevice &d : demo_ble_devices(25)) {
        const double a = blip_angle_deg(d.address);
        CHECK(a >= 0.0 && a < 360.0);
        CHECK_NEAR(a, blip_angle_deg(d.address), 1e-12);
        angles.insert(static_cast<int>(a / 15.0));
    }
    CHECK(angles.size() >= 11); /* spread round the dial: 24 sectors of 15 degrees, 25 devices */
    CHECK(blip_angle_deg("A4:C1:38:5E:01:7B") != blip_angle_deg("A4:C1:38:5E:01:7C"));

    /* the radius: near inside the inner ring, medium between, far outside; stronger is nearer inside a class */
    CHECK(blip_radius_frac(-45) > 0 && blip_radius_frac(-45) < 1.0 / 3.0);
    CHECK(blip_radius_frac(-60) < 1.0 / 3.0);
    CHECK(blip_radius_frac(-61) > 1.0 / 3.0 && blip_radius_frac(-61) < 2.0 / 3.0);
    CHECK(blip_radius_frac(-80) > 1.0 / 3.0 && blip_radius_frac(-80) < 2.0 / 3.0);
    CHECK(blip_radius_frac(-81) > 2.0 / 3.0 && blip_radius_frac(-81) <= 0.95);
    CHECK(blip_radius_frac(-120) <= 0.95);
    CHECK(blip_radius_frac(-10) > 0);
    for (double dbm = -30; dbm > -100; dbm -= 1) CHECK(blip_radius_frac(dbm - 1) >= blip_radius_frac(dbm) - 1e-12);
    for (const BleDevice &d : demo_ble_devices(25)) { /* the ring agrees with the class word */
        const double f = blip_radius_frac(d.smooth_dbm);
        if (d.distance == DistanceClass::Near) CHECK(f < 1.0 / 3.0);
        else if (d.distance == DistanceClass::Medium) CHECK(f > 1.0 / 3.0 && f < 2.0 / 3.0);
        else CHECK(f > 2.0 / 3.0);
    }

    /* the brightness: gone after the expiry, fainter the longer ago, a pulse when the sweep has just passed */
    CHECK_EQ(blip_alpha(20000, 20000, 100, 50), 0);
    CHECK_EQ(blip_alpha(30000, 20000, 100, 50), 0);
    CHECK(blip_alpha(19000, 20000, 100, 50) > 0);
    CHECK(blip_alpha(1000, 20000, 100, 50) > blip_alpha(15000, 20000, 100, 50));
    CHECK(blip_alpha(1000, 20000, 60, 50) > blip_alpha(1000, 20000, 250, 50)); /* just passed (10 deg ago) beats long ago */
    CHECK(blip_alpha(1000, 20000, 60, 50) > blip_alpha(1000, 20000, -1, 50));
    CHECK(blip_alpha(1000, 20000, -1, 50) > blip_alpha(1000, 20000, 250, 50));
    CHECK(blip_alpha(1000, 20000, 10, 350) > blip_alpha(1000, 20000, 200, 350)); /* the sweep wraps past 360 */
    for (int age = 0; age < 20000; age += 1000)
        for (int sweep = -1; sweep < 360; sweep += 40) {
            const int a = blip_alpha(age, 20000, sweep, 123);
            CHECK(a >= 0 && a <= 255);
        }
    CHECK(blip_alpha(0, 20000, 100, 50) <= 255);
    CHECK_EQ(blip_alpha(100, 0, 100, 50), 0);

    CHECK_NEAR(sweep_deg_at(0), 0, 1e-9);
    CHECK_NEAR(sweep_deg_at(750), 90, 1e-9);
    CHECK_NEAR(sweep_deg_at(1500), 180, 1e-9);
    CHECK_NEAR(sweep_deg_at(3000), 0, 1e-9);
    CHECK_NEAR(sweep_deg_at(4500), 180, 1e-9);
    CHECK_NEAR(sweep_deg_at(kDemoClockMs), 50.4, 1e-6);
    CHECK(sweep_deg_at(-100) >= 0 && sweep_deg_at(-100) < 360);
}

void test_ble_rows()
{
    const std::vector<BleDevice> devices = demo_ble_devices(25);
    CHECK_EQ(devices.size(), std::size_t(25));
    for (std::size_t i = 1; i < devices.size(); ++i) CHECK(devices[i].smooth_dbm <= devices[i - 1].smooth_dbm); /* strongest first */
    CHECK_EQ(ble_row_label(devices[0]), std::string("Galaxy Buds2"));
    CHECK_EQ(ble_row_label(devices[3]), std::string("5B:12:F7:90:C4:08")); /* no name: the address */
    CHECK_EQ(ble_row_value(devices[0]), std::string("-43 dBm, near"));
    CHECK_EQ(ble_row_value(devices[7]), std::string("-71 dBm, medium"));
    CHECK_EQ(ble_row_value(devices[12]), std::string("-79 dBm, medium"));
    CHECK_EQ(ble_row_value(devices[13]), std::string("-81 dBm, far"));
    BleDevice quiet;
    CHECK_EQ(ble_row_value(quiet), std::string("no signal"));

    CHECK_EQ(ago_text(0), std::string("just now"));
    CHECK_EQ(ago_text(999), std::string("just now"));
    CHECK_EQ(ago_text(2100), std::string("2 s ago"));
    CHECK_EQ(ago_text(59999), std::string("59 s ago"));
    CHECK_EQ(ago_text(65000), std::string("1 min 5 s ago"));
    CHECK_EQ(ago_text(120000), std::string("2 min ago"));

    const std::vector<RowDef> rows = ble_detail_rows(devices[0], kDemoClockMs);
    CHECK(row_of(rows, "Name") && row_of(rows, "Name")->value == "Galaxy Buds2");
    CHECK(row_of(rows, "Address") && row_of(rows, "Address")->value == "A4:C1:38:5E:01:7B");
    CHECK(row_of(rows, "Address type") && row_of(rows, "Address type")->value == "public");
    CHECK(row_of(rows, "Vendor") && row_of(rows, "Vendor")->value == "Samsung");
    CHECK(row_of(rows, "Signal") && row_of(rows, "Signal")->value == "-45 dBm");
    CHECK(row_of(rows, "Smoothed signal") && row_of(rows, "Smoothed signal")->value == "-43 dBm, near");
    CHECK(row_of(rows, "TX power") && row_of(rows, "TX power")->value == "0 dBm");
    CHECK(row_of(rows, "Last seen") && row_of(rows, "Last seen")->value == "just now");
    CHECK(row_of(rows, "Heard") && row_of(rows, "Heard")->value == "188 times");
    const std::vector<RowDef> bare = ble_detail_rows(devices[3], kDemoClockMs); /* no name, no TX power */
    CHECK(row_of(bare, "Name") == nullptr);
    CHECK(row_of(bare, "TX power") == nullptr);
    CHECK(row_of(bare, "Last seen") && row_of(bare, "Last seen")->value == "2 s ago");
    const std::vector<RowDef> nameless = ble_detail_rows(devices[24], kDemoClockMs);
    CHECK(row_of(nameless, "Last seen")->value == "18 s ago");
    for (const RowDef &r : rows) CHECK(!ends_with_dots(r.value));
}

void test_ble_layout()
{
    /* 640x480: side by side, the radar at least 250 px, the list at least 3 rows */
    BleLayout L = ble_layout(560, 257, 56, 8);
    CHECK(L.side);
    CHECK(L.radar.w >= 250 && L.radar.w == L.radar.h);
    CHECK(L.list.h >= 3 * 56);
    CHECK(L.radar.x + L.radar.w + 8 <= L.list.x);
    CHECK(L.list.x + L.list.w <= 560);
    CHECK(L.radar.y >= 0 && L.radar.y + L.radar.h <= 257);
    /* 800x480 and the wide screens */
    L = ble_layout(720, 257, 56, 8);
    CHECK(L.side && L.radar.w >= 250 && L.list.w >= 240);
    L = ble_layout(944, 360, 56, 8);
    CHECK(L.side && L.radar.w >= 300 && L.list.w >= 240);
    L = ble_layout(960, 480, 56, 8);
    CHECK(L.side && L.radar.w >= 400);
    /* 720x720: stacked, the radar above the list, the list still 3 rows */
    L = ble_layout(640, 497, 56, 8);
    CHECK(!L.side);
    CHECK(L.radar.w >= 250 && L.radar.w == L.radar.h);
    CHECK(L.radar.y + L.radar.h + 8 <= L.list.y);
    CHECK(L.list.h >= 3 * 56);
    CHECK(L.list.y + L.list.h <= 497);
    CHECK(L.radar.x >= 0 && L.radar.x + L.radar.w <= 640);
    CHECK_EQ(L.list.w, 640);
}

} // namespace

void test_ui_model_03()
{
    test_gauge();
    test_speed_texts();
    test_speed_grade();
    test_speed_rows();
    test_speed_band();
    test_speed_layout();
    test_wake();
    test_wake_input();
    test_ble_texts();
    test_radar();
    test_ble_rows();
    test_ble_layout();
}
