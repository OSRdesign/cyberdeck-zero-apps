/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding tests: every UI action reaches its controller command, the binding's own state (boxes, highlight,
 * notices, the trim page's comings and goings) and leave(). A fake Port and an inline Worker (the jobs run at once).
 */
#include "check.hpp"
#include "fake_port.hpp"

#include "bind/binder.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

using namespace cassette;
using namespace cassette::bind;
using tb::base_snapshot;
using tb::FakePort;
using tb::row;

namespace {

constexpr int64_t kNow = 1794139200;

struct Rig {
    FakePort port;
    Worker worker{false};
    Binder binder;
    ui::UiActions act;
    int64_t t = 1000;
    explicit Rig(Binder::Options o = Binder::Options()) : binder(port, worker, (o.utc = true, o))
    {
        port.snap = base_snapshot();
        port.rows = {row("take_007_20261108-1432.wav", 42, 4ull << 20, "2026-11-08 14:32", true),
                     row("take_006_20261108-1105.wav", 728, 69ull << 20, "2026-11-08 11:05")};
        act = binder.actions();
        tick();
        tick(); /* the next take number arrives */
        port.take_calls();
    }
    bool tick(int64_t dt = 33)
    {
        t += dt;
        return binder.tick(t, kNow);
    }
    std::vector<std::string> calls() { return port.take_calls(); }
};

bool has(const std::vector<std::string> &v, const std::string &s) { return std::find(v.begin(), v.end(), s) != v.end(); }

void test_deck_keys()
{
    Rig r;
    r.port.snap.deck.stop.enabled = true;
    r.tick();
    r.act.deck_key(ui::KeyId::Rew, ui::KeyEdge::Down);
    r.act.deck_key(ui::KeyId::Rew, ui::KeyEdge::Up);
    r.act.deck_key(ui::KeyId::Rew, ui::KeyEdge::Click); /* nothing: REW acts on Down / Up */
    r.act.deck_key(ui::KeyId::Fwd, ui::KeyEdge::Down);
    r.act.deck_key(ui::KeyId::Fwd, ui::KeyEdge::Up);
    r.act.deck_key(ui::KeyId::PlayPause, ui::KeyEdge::Down); /* nothing: the others act on Click */
    r.act.deck_key(ui::KeyId::PlayPause, ui::KeyEdge::Up);
    r.act.deck_key(ui::KeyId::PlayPause, ui::KeyEdge::Click);
    r.act.deck_key(ui::KeyId::Stop, ui::KeyEdge::Click);
    r.act.deck_key(ui::KeyId::Rec, ui::KeyEdge::Down);
    r.act.deck_key(ui::KeyId::Rec, ui::KeyEdge::Click);
    r.act.countdown_cancel();
    const std::vector<std::string> want = {"key_event RewDown", "key_event RewUp",  "key_event FwdDown",
                                           "key_event FwdUp",   "key_event PlayPause", "key_event Stop",
                                           "key_event Rec",     "key_event TapOverlay"};
    CHECK(r.calls() == want);

    /* a dimmed key does nothing: REC without a sound input, STOP when idle; an Up always goes through */
    r.port.snap.card.present = false;
    r.port.snap.deck.stop.enabled = false;
    r.port.snap.deck.rew.enabled = false;
    r.tick();
    CHECK(!r.binder.vm().rec.keys[static_cast<int>(ui::KeyId::Rec)].enabled);
    r.act.deck_key(ui::KeyId::Rec, ui::KeyEdge::Click);
    r.act.deck_key(ui::KeyId::Stop, ui::KeyEdge::Click);
    r.act.deck_key(ui::KeyId::Rew, ui::KeyEdge::Down);
    r.act.deck_key(ui::KeyId::Rew, ui::KeyEdge::Up);
    const std::vector<std::string> only_up = {"key_event RewUp"};
    CHECK(r.calls() == only_up);
}

void test_steppers()
{
    Rig r;
    /* SOFT: from the controller's value at each job, so three taps add up */
    r.act.soft_gain(+1);
    r.act.soft_gain(+1);
    r.act.soft_gain(+1);
    r.act.soft_gain(-1);
    std::vector<std::string> c = r.calls();
    CHECK(c.size() == 4 && c[0] == "set_soft_gain_db 7.0" && c[2] == "set_soft_gain_db 9.0" && c[3] == "set_soft_gain_db 8.0");
    r.port.snap.card.soft_gain_db = 24;
    r.act.soft_gain(+1);
    CHECK(has(r.calls(), "set_soft_gain_db 24.0")); /* clamped */
    r.act.soft_gain(0);
    CHECK(r.calls().empty());

    /* CARD: one 1 dB step on a fine card */
    r.port.snap.card.level_db = 12.0;
    r.act.card_gain(+1);
    c = r.calls();
    CHECK(c.size() == 1 && c[0] == "set_level_db 13.0");
    /* a coarse card (3 dB steps): the step grows until the level moves */
    r.port.level_step = 3.0;
    r.port.snap.card.level_db = 12.0;
    r.act.card_gain(+1);
    c = r.calls();
    CHECK(c.size() == 2 && c[0] == "set_level_db 13.0" && c[1] == "set_level_db 14.0");
    CHECK(std::fabs(r.port.snap.card.level_db - 15.0) < 1e-9);
    r.act.card_gain(-1);
    CHECK(std::fabs(r.port.snap.card.level_db - 12.0) < 1e-9);
    /* at the top: one try, no loop */
    r.port.snap.card.level_db = 23.81;
    r.port.level_step = 0.01;
    r.calls();
    r.act.card_gain(+1);
    CHECK_EQ(r.calls().size(), 1u);
    /* no level control: nothing */
    r.port.snap.card.has_level = false;
    r.act.card_gain(+1);
    CHECK(r.calls().empty());
    r.tick();
    CHECK(!r.binder.vm().rec.card.shown);
}

/* The three pickers' entries. Codec: a heading, the six quick presets (WAV, FLAC, Opus 24, 32, 64, 128 kbps), a heading, then
 * WAV, FLAC, Opus; a preset sets the codec and its bit rate through the controller (rate and quality back to the defaults),
 * a plain codec sets the three stored values; one this card cannot record sets nothing and says why */
void test_format_choice()
{
    Rig r;
    r.act.setting_choice("codec", 2);
    CHECK(r.calls() == std::vector<std::string>{"set_format_choice flac"});
    r.act.setting_choice("codec", 4);
    CHECK(r.calls() == std::vector<std::string>{"set_format_choice opus speech32"});
    r.act.setting_choice("codec", 6);
    CHECK(r.calls() == std::vector<std::string>{"set_format_choice opus music128"});
    r.act.setting_choice("codec", 1);
    CHECK(r.calls() == std::vector<std::string>{"set_format_choice wav"});
    r.act.setting_choice("codec", 9);
    CHECK(r.calls() == std::vector<std::string>{"set_rec_params flac - 0 0"});
    r.act.setting_choice("codec", 10);
    CHECK(r.calls() == std::vector<std::string>{"set_rec_params opus - 0 0"});
    r.act.setting_choice("codec", 8);
    CHECK(r.calls() == std::vector<std::string>{"set_rec_params wav - 0 0"});
    r.act.setting_choice("codec", 0); /* a heading, out of range: nothing, no notice */
    r.act.setting_choice("codec", 7);
    r.act.setting_choice("codec", 11);
    r.act.setting_choice("codec", -1);
    CHECK(r.calls().empty());
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "");
    /* rate and quality: the entry's value */
    r.act.setting_choice("rate", 0);
    CHECK(r.calls() == std::vector<std::string>{"set_rec_params wav - 48000 0"});
    r.act.setting_choice("quality", 0);
    CHECK(r.calls() == std::vector<std::string>{"set_rec_params wav - 0 16"});
    r.act.setting_choice("quality", 1); /* this card has no 24-bit */
    CHECK(r.calls().empty());
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "Not available: this card records 16-bit only");
    /* a card with several rates and 24-bit: Opus bit rates, a codec change that moves the rate says so */
    Rig m;
    m.port.snap.format_inputs.rates = {16000, 44100, 48000};
    m.port.snap.format_inputs.has24 = true;
    m.port.snap.settings.rate = 44100;
    m.tick();
    m.act.setting_choice("codec", 10);
    CHECK(m.calls() == std::vector<std::string>{"set_rec_params opus - 48000 0"});
    m.tick();
    m.tick();
    CHECK_EQ(m.binder.vm().notice, "Sample rate set to 48 kHz: Opus cannot record at 44.1 kHz");
    m.port.snap.settings.format = RecFormat::Wav; /* the fake port took the choice: back to WAV at 44.1 kHz */
    m.port.snap.settings.rate = 44100;
    m.port.snap.settings.quality = 0;
    m.tick();
    m.act.setting_choice("quality", 1);
    CHECK(m.calls() == std::vector<std::string>{"set_rec_params wav - 44100 24"});
    m.port.snap.settings.format = RecFormat::Opus;
    m.tick();
    m.act.setting_choice("quality", 4); /* Opus: 64 kbps */
    CHECK(m.calls() == std::vector<std::string>{"set_rec_params opus - 44100 64"});
    /* the controller refuses (a take is running): a notice */
    r.act.notice_dismiss();
    r.tick();
    r.port.format_choice_result = false;
    r.act.setting_choice("codec", 2);
    r.tick();
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "The format cannot be changed now");
    r.act.notice_dismiss();
    r.tick();
    r.act.setting_choice("rate", 0);
    r.tick();
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "The format cannot be changed now");
    /* a stereo 44.1 kHz card: Opus is not available, nothing is set, the reason is the notice */
    Rig q;
    q.port.snap.format_inputs.capture.rate = 44100;
    q.port.snap.format_inputs.capture.channels = 2;
    q.tick();
    q.act.setting_choice("codec", 5);
    CHECK(q.calls().empty());
    q.tick();
    CHECK_EQ(q.binder.vm().notice, "Not available: Opus needs 8, 12, 16, 24 or 48 kHz; this card has 44.1 kHz");
    q.act.setting_choice("codec", 10);
    CHECK(q.calls().empty());
    q.act.setting_choice("codec", 2); /* FLAC works there */
    CHECK(q.calls() == std::vector<std::string>{"set_format_choice flac"});
}

void test_settings_actions()
{
    Rig r;
    r.act.setting_choice("input", 1);
    r.act.setting_choice("codec", 1);
    r.act.setting_choice("timer", 2);
    r.act.setting_choice("timer", 9); /* out of range: nothing */
    r.act.setting_choice("rewfwd", 1);
    r.act.setting_choice("rewfwd", 0);
    r.act.setting_choice("split", 1);
    r.act.setting_choice("split", 0);
    r.act.setting_choice("channels", 1); /* nothing to set */
    r.act.setting_toggle("agc", true);
    r.act.setting_toggle("lowcut", true);
    r.act.setting_toggle("limiter", false);
    r.act.setting_slider("level", 15);
    r.act.setting_slider("other", 3);
    r.act.setting_toggle("monitor", true);
    r.act.setting_slider("monitor_level", 7); /* steps of 5 %: 35 % */
    r.act.setting_slider("monitor_level", 40); /* out of range: clamped to the 20 steps */
    r.act.setting_slider("monitor_level", -2);
    r.act.setting_toggle("monitor", false);
    r.act.setting_text("prefix", "interview");
    r.act.setting_text("other", "x");
    const std::vector<std::string> want = {
        "select_card 1",      "set_format_choice wav",    "set_record_timer 10", "set_scan_mode jump", "set_scan_mode scan",
        "set_auto_split off", "set_auto_split on", "set_auto_gain on",    "set_low_cut on",     "set_limiter off",
        "set_level_db 15.0",  "set_monitor on",     "set_monitor_level 35", "set_monitor_level 100",
        "set_monitor_level 0", "set_monitor off",   "set_prefix interview"};
    const std::vector<std::string> got = r.calls();
    CHECK(got == want);
    if (got != want)
        for (const std::string &g : got) std::fprintf(stderr, "  got: %s\n", g.c_str());
    /* the folder choice: the row's entry */
    r.port.snap.settings.folder = "/media/usb/Rec";
    r.tick();
    r.act.setting_choice("folder", 1);
    CHECK(has(r.calls(), "set_folder ~/Recordings"));
    r.port.folder_result = false;
    r.act.setting_choice("folder", 0);
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "The folder cannot be changed now");
    /* a refused prefix: a notice */
    Rig q;
    q.act.setting_text("prefix", "bad name!");
    q.tick();
    CHECK_EQ(q.binder.vm().notice, "Prefix not changed: use letters, digits, space, - and _");
    CHECK(!has(q.calls(), "set_prefix bad name!"));
    /* spaces: inner ones pass, outer ones are trimmed before the check; empty and too long say so */
    Rig q2;
    q2.act.setting_text("prefix", "  my band  ");
    q2.tick();
    CHECK(has(q2.calls(), "set_prefix my band"));
    Rig q3;
    q3.act.setting_text("prefix", "    ");
    q3.tick();
    CHECK_EQ(q3.binder.vm().notice, "Prefix not changed: the name is empty");
    Rig q4;
    q4.act.setting_text("prefix", std::string(25, 'a'));
    q4.tick();
    CHECK_EQ(q4.binder.vm().notice, "Prefix not changed: up to 24 characters");
}

void test_take_number_action()
{
    Rig r;
    CHECK_EQ(r.binder.vm().rec.info.take, "TAKE 008");
    r.act.setting_number("takenum", 50);
    r.act.setting_number("other", 3);  /* not ours: nothing */
    r.act.setting_number("takenum", 0); /* out of range: nothing */
    std::vector<std::string> c = r.calls();
    CHECK(has(c, "set_next_take 50") && !has(c, "set_next_take 3") && !has(c, "set_next_take 0"));
    CHECK(has(c, "next_take_number")); /* the row's value is read back */
    r.tick();
    r.tick();
    CHECK_EQ(r.binder.vm().rec.info.take, "TAKE 050");
    const ui::SettingRow *row = nullptr;
    for (const ui::SettingRow &x : r.binder.vm().settings.rows)
        if (x.id == "takenum") row = &x;
    CHECK(row && row->value == "Next: 050" && row->pos == 50);
    /* the guard moved it on: a notice names both numbers */
    r.port.take_number_land = 12;
    r.act.setting_number("takenum", 3);
    r.tick();
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "Take 003 is in use: next is 012");
    CHECK_EQ(r.binder.vm().rec.info.take, "TAKE 012");
    /* a take is running: refused with a notice, the number stays */
    r.port.take_number_land = 0;
    r.port.take_number_result = -16; /* EBUSY */
    r.tick(4100); /* the previous notice has timed out */
    r.act.setting_number("takenum", 80);
    r.tick();
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "The take number cannot be changed now");
    CHECK_EQ(r.binder.vm().rec.info.take, "TAKE 012");
    r.port.take_number_result = -5; /* EIO */
    r.tick(4100);
    r.act.setting_number("takenum", 81);
    r.tick();
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "Take number not changed");
}

void test_pages_and_list()
{
    Rig r;
    r.act.open(ui::Screen::Settings);
    CHECK(has(r.calls(), "refresh_cards"));
    r.act.open(ui::Screen::List);
    CHECK(has(r.calls(), "refresh_takes"));
    CHECK_EQ(r.binder.vm().list.selected, 0); /* the current take */

    r.act.list_select(1);
    CHECK(r.calls().empty()); /* a tap only moves the highlight: nothing is loaded */
    r.tick();
    CHECK_EQ(r.binder.vm().list.selected, 1); /* the highlight stays where the user put it */
    r.act.list_select(7); /* out of range: nothing */
    CHECK(r.calls().empty());
    r.act.list_activate(1); /* Enter / a second tap: the preview starts */
    r.act.list_load(0);     /* LOAD */
    std::vector<std::string> c = r.calls();
    CHECK(c.size() == 2 && c[0] == "preview_take take_006_20261108-1105.wav" && c[1] == "load take_007_20261108-1432.wav");
    r.act.list_rename(1, "interview");
    CHECK(has(r.calls(), "rename_take take_006_20261108-1105.wav interview"));
    r.act.list_rename(1, "take"); /* unchanged: nothing */
    CHECK(r.calls().empty());
    r.port.rename_result = -EEXIST;
    r.act.list_rename(0, "x");
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "Not renamed: that name is taken");
    CHECK(has(r.calls(), "rename_take take_007_20261108-1432.wav x"));
    /* names with '_', '-' and inner spaces reach the port as typed; outer spaces are trimmed */
    r.port.rename_result = 0;
    r.act.list_rename(0, "my_take-2");
    CHECK(has(r.calls(), "rename_take take_007_20261108-1432.wav my_take-2"));
    r.act.list_rename(0, "  my band 2 ");
    CHECK(has(r.calls(), "rename_take take_007_20261108-1432.wav my band 2"));
    /* every refusal says its own cause */
    const struct { int code; const char *text; } causes[] = {
        {kRenameTooLong, "Not renamed: up to 24 characters"},
        {kRenameBadChar, "Not renamed: use letters, digits, space, - and _"},
        {kRenameEmpty, "Not renamed: the name is empty"},
        {kRenameNotTake, "Not renamed: this file is not a Cassette take and cannot be renamed"},
        {-EINVAL, "Not renamed: use letters, digits, space, - and _"},
        {-EBUSY, "Not renamed: the take is in use"},
    };
    r.tick(4100); /* the earlier notice is gone */
    for (const auto &k : causes) {
        r.port.rename_result = k.code;
        r.act.list_rename(0, "zz");
        r.tick();
        CHECK_EQ(r.binder.vm().notice, std::string(k.text));
        r.calls();
        r.tick(4100);
    }
    r.port.rename_result = 0;

    /* DELETE asks first */
    r.act.list_delete(1);
    CHECK(r.calls().empty());
    r.tick();
    CHECK(r.binder.vm().confirm.shown);
    CHECK_EQ(r.binder.vm().confirm.body, "take_006_20261108-1105.wav");
    r.act.confirm_answer(false);
    r.tick();
    CHECK(!r.binder.vm().confirm.shown);
    CHECK(r.calls().empty());
    r.act.list_delete(1);
    r.act.confirm_answer(true);
    CHECK(has(r.calls(), "delete_take take_006_20261108-1105.wav"));
    r.tick();
    CHECK(!r.binder.vm().confirm.shown);
    r.port.delete_result = -EBUSY;
    r.act.list_delete(0);
    r.act.confirm_answer(true);
    r.tick(5000); /* the rename notice ends */
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "Not deleted: the take is in use");

    /* opening the list again: the highlight goes back to the current take */
    r.act.open(ui::Screen::Recorder);
    r.act.open(ui::Screen::List);
    r.tick();
    CHECK_EQ(r.binder.vm().list.selected, 0);
}

/* The list's preview: the same row stops and rewinds, another row switches, closing the list stops it; LOAD. */
void test_preview_and_load()
{
    Rig r;
    const std::string a = "take_007_20261108-1432.wav", b = "take_006_20261108-1105.wav";
    r.act.open(ui::Screen::List);
    r.tick();
    r.calls();
    CHECK_EQ(r.binder.vm().list.previewing, -1);
    CHECK(r.binder.vm().list.can_load);
    /* start on row 1 */
    r.act.list_activate(1);
    CHECK(r.calls() == std::vector<std::string>{"preview_take " + b});
    r.tick();
    CHECK_EQ(r.binder.vm().list.previewing, 1);
    /* the same row again: stop (and rewind), no restart */
    r.act.list_activate(1);
    CHECK(r.calls() == std::vector<std::string>{"key_event Stop"});
    r.tick();
    CHECK_EQ(r.binder.vm().list.previewing, -1);
    CHECK_EQ(r.binder.vm().rec.transport, ui::Transport::Idle);
    /* another row while one previews: it switches */
    r.act.list_activate(1);
    r.act.list_activate(0);
    CHECK(r.calls() == (std::vector<std::string>{"preview_take " + b, "preview_take " + a}));
    r.tick();
    CHECK_EQ(r.binder.vm().list.previewing, 0);
    /* the cassette names the playing take, the loaded take is still the first */
    CHECK_EQ(r.binder.vm().rec.tape.label, "Take 007");
    r.act.list_activate(1);
    r.calls();
    r.tick();
    CHECK_EQ(r.binder.vm().rec.tape.label, "Take 006");
    CHECK_EQ(r.port.snapshot().take.name, std::string("")); /* nothing was loaded by previewing */
    /* Esc: the list closes and the preview stops */
    r.act.open(ui::Screen::Recorder);
    CHECK(r.calls() == std::vector<std::string>{"key_event Stop"});
    r.tick();
    CHECK_EQ(r.binder.vm().list.previewing, -1);
    /* a playback the recorder started goes on when the list is opened and closed */
    r.port.snap.deck.state = kDeckPlaying;
    r.port.snap.take.playing_name = a;
    r.act.open(ui::Screen::List);
    r.act.open(ui::Screen::Recorder);
    CHECK(!has(r.calls(), "key_event Stop"));
    r.port.snap.deck.state = kDeckIdle;
    r.port.snap.take.playing_name.clear();

    /* a preview and then S (the settings) also ends it */
    r.act.open(ui::Screen::List);
    r.act.list_activate(1);
    r.calls();
    r.act.open(ui::Screen::Settings);
    CHECK(has(r.calls(), "key_event Stop"));
    r.tick();

    /* LOAD: the take goes into the deck, the preview is not stopped by the binding (the controller does it) */
    r.act.open(ui::Screen::List);
    r.act.list_activate(1);
    r.calls();
    r.act.list_load(1);
    CHECK(r.calls() == std::vector<std::string>{"load " + b});
    r.act.open(ui::Screen::Recorder); /* the UI shows the recorder right after LOAD */
    CHECK(!has(r.calls(), "key_event Stop")); /* the controller's load stopped the preview already */
    r.tick();
    CHECK_EQ(r.binder.vm().rec.tape.label, "Take 006");
    CHECK_EQ(r.binder.vm().rec.tape.load_seq, 1u);
    CHECK_EQ(r.binder.vm().rec.transport, ui::Transport::Idle);
    CHECK(r.binder.vm().rec.hub_dps[0] == 0.0 && r.binder.vm().rec.hub_dps[1] == 0.0); /* the hubs do not turn until PLAY */
    CHECK_EQ(r.binder.vm().rec.counter_ms, 0);
    CHECK_EQ(r.binder.vm().list.previewing, -1);
    CHECK_EQ(r.binder.vm().list.selected, 1);
    /* the list opens on the loaded take */
    r.act.open(ui::Screen::List);
    r.tick();
    CHECK_EQ(r.binder.vm().list.selected, 1);

    /* LOAD out of range does nothing; a refusal is a notice */
    r.calls();
    r.act.list_load(9);
    CHECK(r.calls().empty());
    r.port.load_result = false;
    r.act.list_load(0);
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "The take could not be loaded");
    r.port.load_result = true;

    /* while recording, paused or counting down LOAD is dimmed (can_load false) and does nothing; so does a preview */
    for (int st : {static_cast<int>(kDeckCountdown), static_cast<int>(kDeckRecording), static_cast<int>(kDeckRecordingPaused)}) {
        r.port.snap.deck.state = st;
        r.tick();
        CHECK(!r.binder.vm().list.can_load);
        r.calls();
        r.act.list_load(0);
        r.act.list_activate(1);
        CHECK(r.calls().empty());
    }
    r.port.snap.deck.state = kDeckIdle;
    r.tick();
    CHECK(r.binder.vm().list.can_load);
}

void open_trim(Rig &r)
{
    r.act.open(ui::Screen::List);
    r.act.list_trim(1);
    r.port.snap.trim.open = true;
    r.port.snap.trim.take = "take_006_20261108-1105.wav";
    r.port.snap.trim.rate = 48000;
    r.port.snap.trim.frames = 48000 * 64;
    r.port.snap.trim.out_frame = 48000 * 64;
    r.port.snap.trim.visible_frames = 48000 * 64;
    r.port.snap.trim.max_zoom = 9;
    r.port.snap.trim.peaks_state = 2;
    r.tick();
}

void test_trim_actions()
{
    Rig r;
    r.act.open(ui::Screen::List);
    r.calls();
    r.act.list_trim(1);
    CHECK(has(r.calls(), "open_trim take_006_20261108-1105.wav"));
    open_trim(r);
    CHECK_EQ(r.binder.vm().trim.take, "TAKE 006");
    CHECK_EQ(r.binder.vm().trim.window_peaks.size(), 220u);
    CHECK_EQ(r.binder.vm().trim.overview_peaks.size(), 110u);
    std::vector<std::string> c = r.calls();
    CHECK(has(c, "peak_range 0 3072000 110"));
    CHECK(has(c, "peak_range -1536000 3072000 220")); /* centred on the marker at 0 */
    r.tick();
    CHECK(r.calls().empty()); /* the window did not move: no read */
    r.port.snap.trim.position = 48000;
    r.tick();
    CHECK(has(r.calls(), "peak_range -1488000 3072000 220"));

    r.act.trim_zoom(+1);
    CHECK(has(r.calls(), "set_zoom 1"));
    r.port.snap.trim.zoom = 5;
    r.act.trim_zoom(+1);
    CHECK(has(r.calls(), "set_zoom 5")); /* 2 s is the narrowest window */
    r.act.trim_zoom(-1);
    CHECK(has(r.calls(), "set_zoom 4"));
    r.port.snap.trim.zoom = 0;
    r.act.trim_zoom(-1);
    CHECK(has(r.calls(), "set_zoom 0"));
    r.act.trim_nudge(100);
    r.act.trim_nudge(-1000);
    c = r.calls();
    CHECK(c.size() == 2 && c[0] == "move_position 4800" && c[1] == "move_position -48000");
    r.act.trim_scrub(2500);
    CHECK(has(r.calls(), "set_position 120000"));
    r.act.trim_scrub(-50);
    CHECK(has(r.calls(), "set_position 0"));
    r.act.trim_set_in();
    r.act.trim_set_out();
    r.act.trim_split();
    r.act.trim_remove_split(0);
    r.act.trim_select_split(1);
    r.act.trim_select_split(-1);
    c = r.calls();
    const std::vector<std::string> want = {"set_in", "set_out", "add_split", "remove_split 0", "select_split 1"};
    CHECK(c == want);

    /* the flag shows selected after a tap, not after SPLIT */
    r.port.snap.trim.splits = {48000, 96000};
    r.port.snap.trim.selected_split = 1;
    r.port.snap.trim.position = 96000;
    r.act.trim_split();
    r.tick();
    CHECK_EQ(r.binder.vm().trim.selected_split, -1);
    r.act.trim_select_split(1);
    r.tick();
    CHECK_EQ(r.binder.vm().trim.selected_split, 1);
    r.act.trim_nudge(100);
    r.tick();
    CHECK_EQ(r.binder.vm().trim.selected_split, -1);

    /* PLAY toggles the preview */
    r.calls();
    r.act.trim_play();
    CHECK(has(r.calls(), "preview on"));
    r.tick();
    CHECK(r.binder.vm().trim.playing);
    r.act.trim_play();
    CHECK(has(r.calls(), "preview off"));
}

void test_save_flow()
{
    Rig r;
    open_trim(r);
    r.port.snap.trim.in_frame = 48000;
    r.port.snap.trim.out_frame = 48000 * 10;
    r.port.snap.trim.splits = {48000 * 3, 48000 * 6};
    r.tick();
    r.calls();
    r.act.trim_save();
    CHECK(has(r.calls(), "edit_names take_006_20261108-1105.wav 3"));
    r.tick();
    const ui::SaveVm &sv = r.binder.vm().save;
    CHECK(sv.shown);
    CHECK(sv.files.size() == 3);
    if (sv.files.size() == 3) {
        CHECK_EQ(sv.files[0].name, "take_006_20261108-1105-1.wav");
        CHECK_EQ(sv.files[0].length_ms, 2000);
        CHECK_EQ(sv.files[1].length_ms, 3000);
        CHECK_EQ(sv.files[2].name, "take_006_20261108-1105-3.wav");
        CHECK_EQ(sv.files[2].length_ms, 4000);
    }
    /* CANCEL */
    r.act.save_choose(ui::SaveMode::Cancel);
    r.tick();
    CHECK(!r.binder.vm().save.shown);
    CHECK(!has(r.calls(), "save new"));
    /* SAVE AS NEW FILES */
    r.act.trim_save();
    r.tick();
    r.act.save_choose(ui::SaveMode::NewFiles);
    CHECK(has(r.calls(), "save new"));
    r.tick();
    CHECK(!r.binder.vm().save.shown);
    /* the save runs, then ends: a notice */
    r.port.snap.trim.save_state = 1;
    r.tick();
    r.port.snap.trim.save_state = 2;
    r.port.snap.trim.save_outputs = {"a-1.wav", "a-2.wav", "a-3.wav"};
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "Saved 3 new files");
    r.port.snap.trim.save_state = 0;
    r.tick();
    /* REPLACE ORIGINAL: asks once more */
    r.act.trim_save();
    r.tick();
    r.act.save_choose(ui::SaveMode::Replace);
    r.tick();
    CHECK(r.binder.vm().confirm.shown);
    CHECK_EQ(r.binder.vm().confirm.title, "Delete the original take?");
    CHECK(r.binder.vm().save.shown);
    r.act.confirm_answer(false); /* no: the save box stays */
    r.tick();
    CHECK(!r.binder.vm().confirm.shown && r.binder.vm().save.shown);
    CHECK(!has(r.calls(), "save replace"));
    r.act.save_choose(ui::SaveMode::Replace);
    r.act.confirm_answer(true);
    CHECK(has(r.calls(), "save replace"));
    r.tick();
    CHECK(!r.binder.vm().confirm.shown && !r.binder.vm().save.shown);
    r.port.snap.trim.save_state = 1;
    r.tick();
    r.port.snap.trim.save_state = 3;
    r.port.snap.trim.save_message = "the disk is full";
    r.tick(5000);
    CHECK_EQ(r.binder.vm().notice, "Save failed: the disk is full");
    /* a refused save */
    r.port.snap.trim.save_state = 0;
    r.port.save_result = false;
    r.tick();
    r.act.trim_save();
    r.tick();
    r.act.save_choose(ui::SaveMode::NewFiles);
    r.tick(5000);
    CHECK_EQ(r.binder.vm().notice, "The save could not start");
    /* nothing to save: no box */
    r.port.snap.trim.out_frame = r.port.snap.trim.in_frame;
    r.tick();
    r.calls();
    r.act.trim_save();
    CHECK(r.calls().empty());
}

void test_leaving_trim()
{
    /* no save running: the preview stops and the trim page closes */
    Rig r;
    open_trim(r);
    r.calls();
    r.act.open(ui::Screen::List);
    std::vector<std::string> c = r.calls();
    CHECK(has(c, "preview off") && has(c, "close_trim"));

    /* a save running: the page closes only when it is done (leaving never cancels it) */
    Rig q;
    open_trim(q);
    q.port.snap.trim.save_state = 1;
    q.tick();
    q.calls();
    q.act.open(ui::Screen::List);
    c = q.calls();
    CHECK(has(c, "preview off") && !has(c, "close_trim"));
    q.tick();
    CHECK(!has(q.calls(), "close_trim"));
    q.port.snap.trim.save_state = 2;
    q.tick();
    CHECK(has(q.calls(), "close_trim"));

    /* TRIM on another take while a save runs: opened after it */
    Rig w;
    open_trim(w);
    w.port.snap.trim.save_state = 1;
    w.tick();
    w.act.open(ui::Screen::List);
    w.calls();
    w.act.list_trim(0);
    w.act.open(ui::Screen::Trim);
    CHECK(!has(w.calls(), "open_trim take_007_20261108-1432.wav"));
    w.tick();
    CHECK(w.calls().empty());
    w.port.snap.trim.save_state = 2;
    w.tick();
    c = w.calls();
    CHECK(has(c, "open_trim take_007_20261108-1432.wav") && !has(c, "close_trim"));

    /* TRIM refused (the take cannot be opened): back to the list */
    Rig f;
    f.port.open_trim_result = false;
    f.act.open(ui::Screen::List);
    f.act.list_trim(1);
    f.act.open(ui::Screen::Trim);
    f.tick();
    ui::Screen s = ui::Screen::Trim;
    CHECK(f.binder.take_nav(s) && s == ui::Screen::List);
    CHECK(!f.binder.take_nav(s));

    /* TRIM while recording: a notice, back to the list, open_trim not called */
    Rig g;
    g.port.snap.deck.state = kDeckRecording;
    g.tick();
    g.calls();
    g.act.list_trim(1);
    g.tick();
    CHECK(!has(g.calls(), "open_trim take_006_20261108-1105.wav"));
    CHECK_EQ(g.binder.vm().notice, "Stop the recording to trim a take");
    CHECK(g.binder.take_nav(s) && s == ui::Screen::List);

    /* TRIM while a take plays: STOP first */
    Rig p;
    p.port.snap.deck.state = kDeckPlaying;
    p.tick();
    p.calls();
    p.act.list_trim(1);
    c = p.calls();
    CHECK(c.size() == 2 && c[0] == "key_event Stop" && c[1] == "open_trim take_006_20261108-1105.wav");
}

void test_notices()
{
    Rig r;
    Notice a, b;
    a.kind = Notice::Repaired;
    a.text = "Recovered take_003 after an interrupted recording (41 s kept)";
    b.kind = Notice::CardLost;
    b.text = "The sound card was unplugged";
    r.port.snap.notices = {a, b};
    r.tick(); /* takes them (one job: read and ack) */
    CHECK(has(r.calls(), "ack_notices"));
    CHECK(r.port.snap.notices.empty());
    r.tick();
    CHECK_EQ(r.binder.vm().notice, a.text);
    r.tick(3000);
    CHECK_EQ(r.binder.vm().notice, a.text);
    r.tick(1100);
    CHECK_EQ(r.binder.vm().notice, b.text);
    r.tick(4100);
    CHECK_EQ(r.binder.vm().notice, "");
    CHECK(!has(r.calls(), "ack_notices")); /* nothing new: no ack */
    /* the binding's own */
    r.binder.push_notice("hello");
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "hello");
    r.binder.push_notice("");
    r.tick(4100);
    CHECK_EQ(r.binder.vm().notice, "");
}

void test_next_take_and_revision()
{
    Rig r;
    CHECK_EQ(r.binder.vm().rec.info.take, "TAKE 008"); /* FakePort::next */
    r.port.next = 9;
    r.tick();
    CHECK(!has(r.calls(), "next_take_number")); /* only when the takes changed */
    r.port.snap.takes_revision = 5;
    r.port.rows.push_back(row("take_008_nodate.wav", 3, 1 << 20, ""));
    r.tick();
    std::vector<std::string> c = r.calls();
    CHECK(has(c, "next_take_number"));
    r.tick();
    CHECK_EQ(r.binder.vm().rec.info.take, "TAKE 009");
    CHECK_EQ(r.binder.vm().list.take_count, 3);
    /* tick() says whether anything changed */
    CHECK(!r.tick());
    r.port.snap.meter.ch[0].bar = 3;
    CHECK(r.tick());
    CHECK(!r.tick());
}

void test_leave()
{
    Rig r;
    r.binder.leave();
    std::vector<std::string> c = r.calls();
    CHECK(c.size() == 2 && c[0] == "app_leave" && c[1] == "shutdown");
    r.binder.leave(); /* idempotent */
    CHECK(r.calls().empty());
    r.act.deck_key(ui::KeyId::Rec, ui::KeyEdge::Click); /* after leave: dropped */
    CHECK(r.calls().empty());

    /* the queued commands run before app_leave */
    FakePort port;
    port.snap = base_snapshot();
    Worker worker(true);
    Binder b(port, worker);
    ui::UiActions act = b.actions();
    std::atomic<bool> hold{true};
    worker.post([&hold] {
        while (hold.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    act.deck_key(ui::KeyId::Stop, ui::KeyEdge::Click);
    act.soft_gain(+1);
    std::thread release([&hold] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        hold.store(false);
    });
    b.leave();
    release.join();
    c = port.take_calls();
    CHECK(c.size() == 4 && c[0] == "key_event Stop" && c[1] == "set_soft_gain_db 7.0" && c[2] == "app_leave" &&
          c[3] == "shutdown");
}

} // namespace

/* T6d: Sync every, Mic mute, Output reach their commands; a notice is dismissed by a tap */
void test_new_settings_actions()
{
    Rig r;
    r.port.snap.card.has_mute = true;
    r.port.snap.card.output_labels = {"USB Audio Device", "Default output"};
    r.port.snap.card.output_devices = {"hw:CARD=Demo,DEV=0", "default"};
    r.port.snap.card.output_selected = 0;
    r.tick();
    r.act.setting_choice("sync", 0);
    r.act.setting_choice("sync", 3);
    r.act.setting_choice("sync", 4); /* out of range: nothing */
    r.act.setting_toggle("mute", true);
    r.act.setting_choice("output", 1);
    r.act.setting_choice("output", 2); /* out of range: nothing */
    const std::vector<std::string> want = {"set_sync_interval 5", "set_sync_interval 30", "set_mute on",
                                           "select_output default"};
    CHECK(r.calls() == want);
    r.port.output_result = false; /* a take runs: refused */
    r.act.setting_choice("output", 0);
    r.tick();
    CHECK_EQ(r.binder.vm().notice, "The output cannot be changed now");
    /* the rows follow the snapshot */
    bool mute = false, out = false;
    for (const ui::SettingRow &row : r.binder.vm().settings.rows) {
        mute |= row.id == "mute";
        out |= row.id == "output";
    }
    CHECK(mute && out);

    /* a tap on the notice dismisses it; the next one follows */
    Rig q;
    q.binder.push_notice("first");
    q.binder.push_notice("second");
    q.tick();
    CHECK_EQ(q.binder.vm().notice, "first");
    q.act.notice_dismiss();
    q.tick();
    CHECK_EQ(q.binder.vm().notice, "second");
    q.act.notice_dismiss();
    q.tick();
    CHECK_EQ(q.binder.vm().notice, "");
}

/* T6d: after a replace save the trim page closes onto the list, on the first result, with the notice */
void test_replace_save()
{
    auto run = [](std::vector<std::string> outs, const char *want_notice, bool replaced, bool expect_nav) {
        Rig r;
        open_trim(r);
        r.port.snap.trim.save_state = 1;
        r.tick();
        r.port.snap.trim.save_state = 2;
        r.port.snap.trim.save_replaced = replaced;
        r.port.snap.trim.save_outputs = outs;
        r.tick();
        CHECK_EQ(r.binder.vm().notice, want_notice);
        ui::Screen s = ui::Screen::Trim;
        const bool nav = r.binder.take_nav(s);
        CHECK_EQ(nav, expect_nav);
        if (!expect_nav) return;
        CHECK(s == ui::Screen::List);
        /* the UI shows the list: its open() call; the selection is the first result */
        r.port.rows = {row("take_007_20261108-1432.wav", 42, 4ull << 20, "2026-11-08 14:32", true),
                       row(outs[0], 2, 1ull << 20, "2026-11-08 11:30")};
        for (size_t i = 1; i < outs.size(); ++i) r.port.rows.push_back(row(outs[i], 3, 1ull << 20, "2026-11-08 11:30"));
        r.port.snap.takes_revision = 9;
        r.calls();
        r.act.open(ui::Screen::List);
        r.tick();
        CHECK(has(r.calls(), "close_trim"));
        CHECK_EQ(r.binder.vm().list.selected, 1);
        CHECK_EQ(r.binder.vm().list.takes[1].id, outs[0]);
    };
    run({"a-1.wav", "a-2.wav", "a-3.wav"}, "Saved 3 files, original removed", true, true);
    run({"a-1.wav"}, "Saved, original replaced", true, true);
    /* new files: the page stays */
    run({"a-1.wav", "a-2.wav"}, "Saved 2 new files", false, false);

    /* a small take saves between two ticks: the binding never sees "running", only the end */
    {
        Rig m;
        open_trim(m);
        m.act.trim_save();
        m.tick();
        m.act.save_choose(ui::SaveMode::Replace);
        m.act.confirm_answer(true);
        CHECK(has(m.calls(), "save replace"));
        m.port.snap.trim.save_state = 2; /* done at once */
        m.port.snap.trim.save_replaced = true;
        m.port.snap.trim.save_outputs = {"c-1.wav", "c-2.wav"};
        m.tick();
        CHECK_EQ(m.binder.vm().notice, "Saved 2 files, original removed");
        ui::Screen sc = ui::Screen::Trim;
        CHECK(m.binder.take_nav(sc) && sc == ui::Screen::List);
    }

    /* the page was left while the save ran: the notice only, no navigation */
    Rig q;
    open_trim(q);
    q.port.snap.trim.save_state = 1;
    q.tick();
    q.act.open(ui::Screen::List);
    q.port.snap.trim.save_state = 2;
    q.port.snap.trim.save_replaced = true;
    q.port.snap.trim.save_outputs = {"b-1.wav", "b-2.wav"};
    q.tick();
    ui::Screen s;
    CHECK(!q.binder.take_nav(s));
    CHECK_EQ(q.binder.vm().notice, "Saved 2 files, original removed");
}

void test_binder()
{
    test_new_settings_actions();
    test_replace_save();
    test_deck_keys();
    test_steppers();
    test_settings_actions();
    test_format_choice();
    test_pages_and_list();
    test_preview_and_load();
    test_trim_actions();
    test_save_flow();
    test_leaving_trim();
    test_notices();
    test_next_take_and_revision();
    test_take_number_action();
    test_leave();
}

void test_worker()
{
    /* order */
    Worker w(true);
    std::vector<int> seen;
    std::mutex mu;
    for (int i = 0; i < 200; ++i)
        w.post([i, &seen, &mu] {
            std::lock_guard<std::mutex> g(mu);
            seen.push_back(i);
        });
    w.wait_idle();
    bool ordered = seen.size() == 200;
    for (int i = 0; ordered && i < 200; ++i) ordered = seen[static_cast<size_t>(i)] == i;
    CHECK(ordered);
    CHECK_EQ(w.done(), 200ul);
    /* finish runs what is queued, then drops later posts */
    std::atomic<int> n{0};
    std::atomic<bool> hold{true};
    w.post([&hold] {
        while (hold.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    for (int i = 0; i < 5; ++i) w.post([&n] { ++n; });
    std::thread release([&hold] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        hold.store(false);
    });
    w.finish();
    release.join();
    CHECK_EQ(n.load(), 5);
    w.post([&n] { ++n; });
    w.wait_idle();
    CHECK_EQ(n.load(), 5);
    w.finish();
    /* inline */
    Worker in(false);
    int k = 0;
    in.post([&k] { k = 7; });
    CHECK_EQ(k, 7);
    in.wait_idle();
    in.finish();
    in.post([&k] { k = 9; });
    CHECK_EQ(k, 7);

    /* a drag: only the latest marker is sent while a job is queued */
    FakePort port;
    port.snap = base_snapshot();
    port.snap.trim.open = true;
    port.snap.trim.rate = 48000;
    port.snap.trim.frames = 48000 * 60;
    Worker worker(true);
    Binder b(port, worker);
    ui::UiActions act = b.actions();
    b.tick(1, kNow);
    worker.wait_idle();
    port.take_calls();
    std::atomic<bool> gate{true};
    worker.post([&gate] {
        while (gate.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    for (int ms = 100; ms <= 1000; ms += 100) act.trim_scrub(ms);
    gate.store(false);
    worker.wait_idle();
    std::vector<std::string> c = port.take_calls();
    CHECK(c.size() == 1 && c[0] == "set_position 48000");
    act.trim_scrub(2000);
    worker.wait_idle();
    c = port.take_calls();
    CHECK(c.size() == 1 && c[0] == "set_position 96000");
}
