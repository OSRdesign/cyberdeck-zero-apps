/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests (P2a): the Menu pages' contents (core/menu: only built rows, owner decision D32; the row kinds of design QA
 * D-45; the selection skips headings), the Keys page against the key handler's table, the About page's THIS OUTPUT
 * line from the volume model, the Menu key on every layout (and its touch area among the others), the Menu pages'
 * geometry at every QA size, and the sleep timer with a fake clock (its modes, the fade, the end of a track or an
 * album, a timer that runs whatever page shows).
 */
#include "check.hpp"

#include "layout/layout.hpp"
#include "menu/menu_model.hpp"
#include "sleep/sleep_timer.hpp"

#include <cmath>
#include <set>

using namespace vinyl;
using namespace vinyl::menu;

namespace {

void test_rows() {
    State st;
    st.output = "Evolve 65";
    const std::vector<Row> m = menu_rows(st);
    /* TOOLS: Sleep timer; SETTINGS: Playback, Output, Keys, About; never a row of a later phase */
    CHECK_EQ(m.size(), static_cast<size_t>(7));
    CHECK(m[0].kind == Row::Kind::Heading && m[0].text == "TOOLS");
    CHECK(m[1].kind == Row::Kind::Edit && m[1].id == "sleep" && m[1].value == "Off");
    CHECK(m[2].kind == Row::Kind::Heading && m[2].text == "SETTINGS");
    CHECK(m[3].id == "playback" && m[3].kind == Row::Kind::Open && m[3].value.empty());
    CHECK(m[4].id == "output" && m[4].value == "Evolve 65");
    CHECK(m[5].id == "keys" && m[6].id == "about");
    for (const Row &r : m) {
        CHECK(r.text != "Equalizer" && r.text != "Library" && r.text.find("Speed") == std::string::npos);
        CHECK(r.kind != Row::Kind::Choice);
    }
    /* the sleep timer's value: the time left, or the track mode */
    st.sleep = SleepMode::Min30;
    st.sleep_left_ms = 29 * 60000 + 12000;
    CHECK_EQ(menu_rows(st)[1].value, std::string("29:12"));
    st.sleep = SleepMode::Track;
    CHECK_EQ(menu_rows(st)[1].value, std::string("This track"));
    st.sleep = SleepMode::Album;
    CHECK_EQ(menu_rows(st)[1].value, std::string("This album"));
    /* the Sleep timer page: six choices, the chosen one blue, its heading and status line */
    const std::vector<Row> sl = sleep_rows(SleepMode::Min30);
    CHECK_EQ(sl.size(), static_cast<size_t>(6));
    const char *words[] = {"Off", "15 minutes", "30 minutes", "60 minutes", "This track", "This album or queue"};
    int chosen = 0;
    for (size_t i = 0; i < sl.size(); ++i) {
        CHECK(sl[i].kind == Row::Kind::Choice);
        CHECK_EQ(sl[i].text, std::string(words[i]));
        chosen += sl[i].chosen ? 1 : 0;
        SleepMode back = SleepMode::Off;
        CHECK(sleep_from_id(sl[i].id, back) && back == kSleepModes[i]);
    }
    CHECK_EQ(chosen, 1);
    CHECK(sl[2].chosen);
    CHECK_EQ(initial_item(sl), 2); /* the page opens on the chosen row */
    CHECK_EQ(page_heading(Page::Sleep), std::string("STOP PLAYING AFTER"));
    State run;
    run.sleep = SleepMode::Min30;
    run.sleep_left_ms = 29 * 60000 + 12000;
    CHECK_EQ(page_hint(Page::Sleep, run), std::string("Stops in 29:12. The music fades out for the last 10 seconds."));
    CHECK_EQ(page_hint(Page::Sleep, State()), std::string("")); /* Off: no status line */
    /* Playback: Gapless, ReplayGain, Remember position in long files; values; no Speed, no Crossfade */
    Settings s;
    std::vector<Row> pb = playback_rows(s);
    CHECK_EQ(pb.size(), static_cast<size_t>(3));
    CHECK(pb[0].id == "gapless" && pb[0].value == "On" && pb[0].kind == Row::Kind::Edit);
    CHECK(pb[1].id == "replaygain" && pb[1].value == "Track");
    CHECK(pb[2].id == "remember" && pb[2].value == "On" && pb[2].text == "Remember position in long files");
    s.gapless = false;
    s.replaygain = ReplayGainMode::Album;
    s.remember_long = false;
    pb = playback_rows(s);
    CHECK(pb[0].value == "Off" && pb[1].value == "Album" && pb[2].value == "Off");
    CHECK_EQ(page_hint(Page::Playback, State()), std::string("ReplayGain evens out the loudness between tracks."));
    /* the ReplayGain pick list */
    const std::vector<Row> rg = replaygain_rows(ReplayGainMode::Off);
    CHECK_EQ(rg.size(), static_cast<size_t>(3));
    CHECK(rg[0].chosen && !rg[1].chosen && !rg[2].chosen);
    CHECK(rg[0].id == "off" && rg[1].id == "track" && rg[2].id == "album");
    CHECK(!page_hint(Page::ReplayGain, State()).empty());
    CHECK_EQ(std::string(page_title(Page::ReplayGain)), std::string("ReplayGain"));
    CHECK_EQ(std::string(page_title(Page::Sleep)), std::string("Sleep timer"));
    /* the selection skips the headings, stops at the ends */
    CHECK_EQ(first_item(m), 1);
    CHECK_EQ(step_item(m, 1, 1), 3);
    CHECK_EQ(step_item(m, 3, -1), 1);
    CHECK_EQ(step_item(m, 1, -1), 1);
    CHECK_EQ(step_item(m, 6, 1), 6);
    CHECK_EQ(step_item(m, 3, 2), 5);
    CHECK_EQ(step_item(m, -1, 1), 1);
    CHECK_EQ(initial_item(m), 1);
}

/* The Keys page is the key handler's own table: each letter it names does what it says (the controller dispatches
 * letters through letter_command only), and every letter command is on the page. */
void test_keys() {
    const std::vector<KeyHelp> rows = key_help();
    CHECK(rows.size() >= 8);
    /* the canvas's rows first, in its order (Keys-480x320) */
    CHECK_EQ(rows[0].keys, std::string("Space"));
    CHECK_EQ(rows[0].action, std::string("Play or pause"));
    CHECK_EQ(rows[1].keys, std::string("Left"));
    CHECK_EQ(rows[1].action, std::string("Restart, or previous; hold: rewind"));
    CHECK_EQ(rows[2].action, std::string("Next; hold: fast forward"));
    CHECK_EQ(rows[3].keys, std::string("Shift+Left, Right"));
    CHECK_EQ(rows[6].keys, std::string("Q, B, O, M"));
    CHECK_EQ(rows[6].action, std::string("Queue, Browse, Output, Menu"));
    CHECK_EQ(rows[7].keys, std::string("- and ="));
    std::set<int> listed;
    for (const KeyHelp &k : rows) {
        CHECK_EQ(k.codes.size(), k.cmds.size());
        for (size_t i = 0; i < k.codes.size() && i < k.cmds.size(); ++i) {
            CHECK(letter_command(k.codes[i], k.queue_page) == k.cmds[i]);
            if (k.queue_page) CHECK(letter_command(k.codes[i], false) == KeyCmd::None); /* the queue's letters act there only */
            listed.insert(static_cast<int>(k.cmds[i]));
        }
        CHECK(!k.keys.empty() && !k.action.empty());
    }
    /* every command a letter has is listed on the page */
    for (uint32_t code = 1; code < 128; ++code)
        for (bool q : {false, true}) {
            const KeyCmd c = letter_command(code, q);
            if (c != KeyCmd::None) CHECK(listed.count(static_cast<int>(c)) == 1);
        }
    /* M and T (P2a), by physical key code (design QA D-32) */
    CHECK(letter_command(kKeyM, false) == KeyCmd::Menu);
    CHECK(letter_command(kKeyT, false) == KeyCmd::Sleep);
    CHECK(letter_command(kKeyY, false) == KeyCmd::None); /* Y belongs to the dialog */
    bool esc = false, media = false;
    for (const KeyHelp &k : rows) {
        esc = esc || k.keys == "Esc";
        media = media || k.keys == "Media keys";
    }
    CHECK(esc && media);
}

void test_about() {
    /* About-480x320's line: the Sharkoon's model (-45..0 dB, 1 dB steps) */
    const OutputVolumeModel sharkoon = volume_model(-45, 0, 1);
    CHECK_EQ(about_output_line("Sharkoon Mobile DAC", sharkoon, true, false),
             std::string("Sharkoon Mobile DAC: -45.0 to 0.0 dB, 23 levels of 2.05 dB."));
    const OutputVolumeModel jack = volume_model(-102.39, 4.0, 0.01);
    CHECK_EQ(about_output_line("Headphones", jack, true, false), std::string("Headphones: -56.0 to 4.0 dB, 30 levels of 2.07 dB."));
    CHECK_EQ(about_output_line("Evolve 65", default_volume_model(), true, false),
             std::string("Evolve 65: -60.0 to 0.0 dB, 30 levels of 2.07 dB."));
    CHECK(about_output_line("HDMI", default_volume_model(), true, true).find("Vinyl's own volume") != std::string::npos);
    CHECK_EQ(about_output_line("Evolve 65", default_volume_model(), false, false), std::string("Evolve 65: the level is not known yet."));
    OutputVolumeModel raw;
    raw.db = false;
    raw.n = 23;
    CHECK_EQ(about_output_line("Card", raw, true, false), std::string("Card: 23 steps (no dB scale)."));
    const About a = about("0.2.0", "X: y.");
    CHECK_EQ(a.name, std::string("Vinyl 0.2.0"));
    CHECK(a.line.find("CyberdeckZero Launcher") != std::string::npos);
    CHECK(a.formats.find("FFmpeg") != std::string::npos);
    CHECK_EQ(a.output, std::string("X: y."));
    CHECK(a.libraries.find("libmpg123") != std::string::npos && a.libraries.find("LVGL") != std::string::npos);
    CHECK(a.licences.find("/usr/share/doc/vinyl/copyright") != std::string::npos);
}

/* the Menu key on every class, as the canvas places it */
void test_menu_key() {
    using namespace vinyl::layout;
    for (SizeClass c : {SizeClass::Small, SizeClass::Medium, SizeClass::Portrait, SizeClass::Square, SizeClass::Large}) {
        const NowGeom g = now_geom(c);
        int menus = 0, sleeps = 0, outputs = 0;
        const Btn *menu = nullptr;
        bool in_transport = false;
        for (const std::vector<Btn> *row : {&g.transport, &g.bottom})
            for (const Btn &b : *row) {
                CHECK(b.enabled); /* nothing dimmed: nothing unbuilt is drawn (D32) */
                if (b.id == "menu") {
                    ++menus;
                    menu = &b;
                    in_transport = row == &g.transport;
                }
                sleeps += b.id == "sleep";
                outputs += b.id == "output";
                if (opens_page(b)) CHECK(key_active(b, true)); /* the page keys act with an empty queue */
            }
        CHECK_EQ(menus, 1);
        CHECK_EQ(outputs, 0); /* the output button or chip opens Output */
        if (!menu) continue;
        switch (c) {
        case SizeClass::Small: /* the seventh transport key after Queue */
            CHECK(in_transport);
            CHECK_EQ(g.transport.size(), static_cast<size_t>(7));
            CHECK(g.transport[5].id == "queue" && g.transport[6].id == "menu");
            CHECK_NEAR(menu->box.x, 414, 1e-9);
            CHECK_NEAR(menu->box.w, 46, 1e-9);
            break;
        case SizeClass::Square: /* the sixth transport key, where the dimmed sleep key was */
            CHECK(in_transport);
            CHECK(g.transport.back().id == "menu");
            CHECK_EQ(sleeps, 0);
            break;
        case SizeClass::Portrait: /* an icon key at the left of the bottom row, then Queue, then the volume group */
            CHECK(!in_transport);
            CHECK(g.bottom[0].id == "menu" && g.bottom[1].id == "queue");
            CHECK(!g.bottom[0].label && !g.bottom[1].label);
            CHECK(g.bottom[1].box.right() < g.vol_minus.x);
            break;
        case SizeClass::Large: /* Browse | Menu | the volume group; the sleep key acts */
            CHECK(!in_transport);
            CHECK(g.bottom.size() == 2 && g.bottom[0].id == "browse" && g.bottom[1].id == "menu");
            CHECK_EQ(sleeps, 1);
            break;
        case SizeClass::Medium: /* Browse | Queue | Menu | the volume group, 128 wide each */
            CHECK(!in_transport);
            CHECK(g.bottom.size() == 3 && g.bottom[2].id == "menu");
            for (const Btn &b : g.bottom) CHECK_NEAR(b.box.w, 128, 1e-9);
            break;
        }
    }
}

/* the Menu pages at every QA size: inside the panel, under the header, rows never under LIST_ROW */
void test_menu_geom() {
    struct Q {
        int w, h, body, list_row;
    };
    const Q qs[] = {{640, 480, 56, 57}, {800, 480, 56, 46}, {480, 480, 56, 57}, {720, 720, 56, 50}, {480, 800, 56, 43},
                    {480, 640, 56, 45}, {480, 320, 40, 32}, {320, 480, 40, 32}, {1280, 720, 56, 46}, {1024, 600, 56, 46}};
    for (const Q &q : qs)
        for (double row : {34.0, 40.0})
            for (bool heading : {false, true})
                for (bool strip : {false, true}) {
                    const layout::MenuGeom m = layout::menu_geom(q.w, q.h, q.body, row, heading, strip, q.list_row);
                    CHECK(m.panel.y >= q.body);
                    CHECK(m.panel.bottom() <= q.h);
                    CHECK(m.panel.x >= 0 && m.panel.right() <= q.w);
                    CHECK(m.list.y >= m.panel.y && m.list.bottom() <= m.panel.bottom());
                    CHECK(m.row_h >= q.list_row);
                    CHECK(m.key_row_h >= q.list_row);
                    if (strip) CHECK(m.strip.y >= m.list.bottom() - 1e-9 && m.strip.bottom() <= m.panel.bottom());
                    if (heading) CHECK(m.heading.h > 0 && m.list.y >= m.heading.bottom() - 1e-9);
                    CHECK(m.portrait == (static_cast<double>(q.w) / q.h < 0.9));
                }
    /* the drawn sizes: Menu-480x320 rows 34, Playback 40, Menu-320x480 rows 44, Keys-320x480 rows 50 */
    CHECK_NEAR(layout::menu_geom(480, 320, 40, 34, false, false, 32).row_h, 34, 1e-9);
    CHECK_NEAR(layout::menu_geom(480, 320, 40, 40, false, true, 32).row_h, 40, 1e-9);
    CHECK_NEAR(layout::menu_geom(320, 480, 40, 34, false, false, 32).row_h, 44, 1e-9);
    CHECK_NEAR(layout::menu_geom(320, 480, 40, 32, false, false, 32).key_row_h, 50, 1e-9);
    CHECK(layout::menu_geom(320, 480, 40, 32, false, false, 32).keys_two_lines);
    const layout::MenuGeom sl = layout::menu_geom(480, 320, 40, 34, true, true, 32);
    CHECK_NEAR(sl.list.y, 44 + 30, 0.5);  /* under "STOP PLAYING AFTER" */
    CHECK_NEAR(sl.strip.y, 262, 0.5);     /* the canvas's status line at 218 + 44 */
    CHECK_NEAR(sl.strip.h, 42, 0.5);
}

void test_sleep_timer() {
    SleepTimer t;
    uint64_t now = 1000000; /* a fake monotonic clock */
    CHECK(!t.active());
    CHECK_NEAR(t.fade_gain(now), 1.0, 1e-9);
    t.set(SleepMode::Min15, now);
    CHECK(t.active() && t.timed());
    CHECK_EQ(t.remaining_ms(now), static_cast<uint64_t>(15 * 60000));
    CHECK_EQ(sleep_menu_value(t.mode(), t.remaining_ms(now)), std::string("15:00"));
    now += 14 * 60000 + 45000; /* 15 s left: no fade yet */
    CHECK(!t.expired(now));
    CHECK_NEAR(t.fade_gain(now), 1.0, 1e-9);
    CHECK_EQ(sleep_left_text(t.remaining_ms(now)), std::string("0:15"));
    now += 5000; /* 10 s left: the fade starts */
    CHECK_NEAR(t.fade_gain(now), 1.0, 1e-6);
    now += 5000; /* 5 s left: -30 dB */
    CHECK_NEAR(20 * std::log10(t.fade_gain(now)), -30.0, 1e-3);
    now += 4900; /* 0.1 s left: near -60 dB */
    CHECK(20 * std::log10(t.fade_gain(now)) < -59);
    CHECK(!t.expired(now));
    now += 100; /* the end: pause, then Off */
    CHECK(t.expired(now));
    CHECK_NEAR(t.fade_gain(now), 0.0, 1e-9);
    t.finish();
    CHECK(!t.active());
    CHECK_NEAR(t.fade_gain(now), 1.0, 1e-9); /* the next Play is at full gain */
    /* the fade is monotonic and linear in dB */
    float prev = 1.0f;
    for (uint64_t r = 10000; r > 0; r -= 250) {
        const float g = sleep_fade_gain(r);
        CHECK(g <= prev + 1e-7f);
        prev = g;
    }
    CHECK_NEAR(sleep_fade_gain(0), 0.0, 1e-9);
    CHECK_NEAR(sleep_fade_gain(20000), 1.0, 1e-9);
    /* set again: counted from the new choice; 60 minutes */
    t.set(SleepMode::Min60, now);
    CHECK_EQ(sleep_menu_value(t.mode(), t.remaining_ms(now)), std::string("60:00"));
    t.set(SleepMode::Off, now);
    CHECK(!t.active());
    CHECK(!t.expired(now + 3600000ull * 2));
    /* This track: stop at the end of whatever plays */
    t.set(SleepMode::Track, now);
    CHECK(t.active() && !t.timed());
    CHECK_EQ(t.remaining_ms(now), static_cast<uint64_t>(0));
    CHECK(!t.expired(now + 3600000)); /* only the track's end stops it */
    CHECK_NEAR(t.fade_gain(now), 1.0, 1e-9); /* the engine fades the track's end */
    CHECK(t.stop_after_track(true, true, false));
    CHECK(t.stop_after_track(false, false, true));
    /* This album or queue: stop before another album, or at the queue's end; a playlist only at its end */
    t.set(SleepMode::Album, now);
    CHECK(!t.stop_after_track(true, true, false));
    CHECK(t.stop_after_track(true, false, false));
    CHECK(t.stop_after_track(false, true, false));
    CHECK(!t.stop_after_track(true, false, true)); /* a playlist: on to its end */
    CHECK(t.stop_after_track(false, false, true));
    CHECK_EQ(sleep_status_text(SleepMode::Album, 0),
             std::string("Stops at the end of this album or queue. The music fades out for its last 10 seconds."));
    /* Off and the minute modes never stop at a track's end */
    t.set(SleepMode::Min30, now);
    CHECK(!t.stop_after_track(false, false, false));
    /* the timer is only its own state: whatever page shows (the controller owns it), the clock alone moves it */
    SleepTimer pages;
    pages.set(SleepMode::Min30, 0);
    bool all = true;
    for (uint64_t ms = 0; ms < 30 * 60000; ms += 100) all = all && pages.active() && !pages.expired(ms);
    CHECK(all);
    CHECK(pages.expired(30 * 60000));
    CHECK_EQ(std::string(kSleepEndedNotice), std::string("Sleep timer ended. Paused."));
}

}  // namespace

void test_menu() {
    test_rows();
    test_keys();
    test_about();
    test_menu_key();
    test_menu_geom();
    test_sleep_timer();
}
