/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding tests, end to end without LVGL: the real Controller on the scripted sound system of the demo mode
 * (demo/demo_alsa.hpp: a generated tone, no hardware, nothing played), the real file operations in a temporary home, the
 * real Binder with its threaded Worker, ticked here at about 50 Hz as the LVGL timer would. Record a take, list it, trim
 * it to two splits and save new files (-1, -2, -3 on disk), the record countdown, an unplug and replug, no input, a card
 * without controls, two cards. SYNTHETIC audio only.
 */
#include "check.hpp"

#include "bind/binder.hpp"
#include "demo/demo_alsa.hpp"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <ftw.h>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace cassette;
using namespace cassette::bind;

namespace {

int rm_entry(const char *path, const struct stat *, int, struct FTW *) { return ::remove(path); }

void rm_rf(const std::string &dir)
{
    if (!dir.empty()) nftw(dir.c_str(), rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

std::string temp_dir()
{
    const char *tmp = std::getenv("TMPDIR");
    std::string t = std::string(tmp && *tmp ? tmp : "/tmp") + "/cassette-bind-test-XXXXXX";
    std::vector<char> b(t.begin(), t.end());
    b.push_back(0);
    return mkdtemp(b.data()) ? std::string(b.data()) : std::string();
}

bool file_exists(const std::string &p)
{
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

int64_t mono_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool starts_with(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string &s, const std::string &e)
{
    return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

struct E2E {
    std::string home;
    std::unique_ptr<AlsaApi> api;
    std::unique_ptr<Controller> ctl;
    std::unique_ptr<Port> port;
    std::unique_ptr<Worker> worker;
    std::unique_ptr<Binder> binder;
    ui::UiActions act;
    std::vector<std::string> notices_seen;

    explicit E2E(const demo::Script &script)
    {
        home = temp_dir();
        api = demo::make_demo_alsa(script);
        ControllerConfig cfg;
        cfg.api = api.get();
        cfg.home = home;
        cfg.min_free_bytes = 16ull << 20;
        cfg.replug_retry_ms = 300;
        ctl.reset(new Controller(cfg));
        ctl->start();
        port = make_controller_port(*ctl, posix_file_ops());
        worker.reset(new Worker(true));
        Binder::Options o;
        o.notice_ms = 300;
        binder.reset(new Binder(*port, *worker, o));
        act = binder->actions();
        tick();
    }
    ~E2E()
    {
        binder->leave();
        binder.reset();
        worker.reset();
        port.reset();
        ctl.reset();
        api.reset();
        rm_rf(home);
    }
    const ui::ViewModel &vm() const { return binder->vm(); }
    void tick()
    {
        binder->tick(mono_ms(), static_cast<int64_t>(std::time(nullptr)));
        const std::string &n = binder->vm().notice;
        if (!n.empty() && (notices_seen.empty() || notices_seen.back() != n)) notices_seen.push_back(n);
    }
    /* ticks every 20 ms until pred holds or ms elapse */
    bool until(const std::function<bool(const ui::ViewModel &)> &pred, int ms)
    {
        const int64_t end = mono_ms() + ms;
        for (;;) {
            tick();
            if (pred(vm())) return true;
            if (mono_ms() > end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    void run_for(int ms)
    {
        until([](const ui::ViewModel &) { return false; }, ms);
    }
    bool saw_notice(const std::string &part) const
    {
        for (const std::string &n : notices_seen)
            if (n.find(part) != std::string::npos) return true;
        return false;
    }
    std::string recordings() const { return home + "/Recordings"; }
};

void click(E2E &e, ui::KeyId k)
{
    e.act.deck_key(k, ui::KeyEdge::Down);
    e.act.deck_key(k, ui::KeyEdge::Up);
    e.act.deck_key(k, ui::KeyEdge::Click);
}

const ui::SettingRow *find_row(const ui::ViewModel &vm, const std::string &id)
{
    for (const ui::SettingRow &r : vm.settings.rows)
        if (r.id == id) return &r;
    return nullptr;
}

bool idle(const ui::ViewModel &v) { return v.rec.transport == ui::Transport::Idle; }

void test_signal_and_env()
{
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < 600; ++i) {
        const double d = demo::tone_level_db(i * 0.01);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    CHECK(lo < -18.0 && lo > -21.0); /* below the target zone ... */
    CHECK(hi > -6.0 && hi < -3.0);   /* ... and above it: the level sweeps through */

    setenv("CASSETTE_DEMO", "1", 1);
    CHECK(demo::wanted());
    setenv("CASSETTE_DEMO", "idle", 1);
    CHECK(!demo::wanted());
    unsetenv("CASSETTE_DEMO");
    CHECK(!demo::wanted());

    setenv("CASSETTE_DEMO_CARD", "both", 1);
    setenv("CASSETTE_DEMO_UNPLUG_MS", "1500", 1);
    setenv("CASSETTE_DEMO_REPLUG_MS", "x", 1);
    demo::Script s = demo::script_from_env();
    CHECK_EQ(s.cards, "both");
    CHECK_EQ(s.unplug_ms, 1500);
    CHECK_EQ(s.replug_ms, -1);
    setenv("CASSETTE_DEMO_CARD", "bogus", 1);
    CHECK_EQ(demo::script_from_env().cards, "level");
    unsetenv("CASSETTE_DEMO_CARD");
    unsetenv("CASSETTE_DEMO_UNPLUG_MS");
    unsetenv("CASSETTE_DEMO_REPLUG_MS");

    const std::string base = temp_dir();
    setenv("APPLAUNCH_STATE_DIR", base.c_str(), 1);
    setenv("CASSETTE_DEMO_HOME", "demo-home/x", 1);
    std::string err;
    std::string h = demo::demo_home(&err);
    CHECK_EQ(h, base + "/demo-home/x");
    struct stat st;
    CHECK(::stat(h.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    unsetenv("CASSETTE_DEMO_HOME");
    unsetenv("APPLAUNCH_STATE_DIR");
    setenv("TMPDIR", base.c_str(), 1);
    h = demo::demo_home(&err);
    CHECK(starts_with(h, base + "/cassette-demo-"));
    CHECK(::stat(h.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    unsetenv("TMPDIR");
    rm_rf(base);
}

void test_take_trim_save()
{
    demo::Script s;
    E2E e(s);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.input_present; }, 2000));
    CHECK(e.vm().rec.card.shown);                   /* the level card */
    CHECK(find_row(e.vm(), "level") && find_row(e.vm(), "agc"));
    CHECK_EQ(e.vm().rec.tape.label, "Take 001");
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.meter.ch[0].bar > 0; }, 2000)); /* the tone moves the meter */
    CHECK(!e.vm().rec.keys[static_cast<int>(ui::KeyId::Stop)].enabled);
    CHECK(e.vm().rec.keys[static_cast<int>(ui::KeyId::Rec)].enabled);

    /* CARD moves the card's level */
    const double before = e.vm().rec.card.db;
    e.act.card_gain(+1);
    CHECK(e.until([before](const ui::ViewModel &v) { return v.rec.card.db > before + 0.5; }, 1000));
    e.act.soft_gain(+1);
    CHECK(e.until([](const ui::ViewModel &v) { return std::fabs(v.rec.soft.db - 1.0) < 1e-6; }, 1000));
    e.act.soft_gain(-1);

    /* REC, 1.6 s, STOP */
    click(e, ui::KeyId::Rec);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.transport == ui::Transport::Recording; }, 1000));
    CHECK(e.vm().rec.keys[static_cast<int>(ui::KeyId::Rec)].latched);
    CHECK_EQ(e.vm().rec.tape.label, "Take 001");
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.counter_ms >= 1600; }, 4000));
    click(e, ui::KeyId::Stop);
    CHECK(e.until([](const ui::ViewModel &v) { return idle(v) && v.list.take_count == 1; }, 3000));
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.tape.label == "Take 002"; }, 1000)); /* the next take */
    CHECK_EQ(e.vm().list.takes[0].name, "Take 001");
    CHECK_EQ(e.vm().list.takes[0].sticker, "001");
    CHECK(e.vm().list.takes[0].length_ms >= 1500);
    const std::string take = e.vm().list.takes[0].id;
    CHECK(starts_with(take, "take_001_") && ends_with(take, ".wav"));
    CHECK(file_exists(e.recordings() + "/" + take));

    /* the list, TRIM */
    e.act.open(ui::Screen::List);
    e.act.list_trim(0);
    e.act.open(ui::Screen::Trim);
    CHECK(e.until([&take](const ui::ViewModel &v) { return v.trim.take_id == take && !v.trim.window_peaks.empty(); }, 3000));
    CHECK_EQ(e.vm().trim.take, "TAKE 001");
    CHECK_EQ(e.vm().trim.marker_ms, 0);
    float top = 0;
    for (float p : e.vm().trim.overview_peaks) top = std::max(top, p);
    CHECK(top > 0.05f && top < 0.9f); /* the tone, -20 to -4 dBFS */
    CHECK_EQ(e.vm().trim.window_peaks.size(), 220u);

    /* two splits at 0.5 s and 1.0 s */
    e.act.trim_nudge(500);
    CHECK(e.until([](const ui::ViewModel &v) { return v.trim.marker_ms == 500; }, 1000));
    e.act.trim_split();
    CHECK(e.until([](const ui::ViewModel &v) { return v.trim.splits.size() == 1; }, 1000));
    CHECK_EQ(e.vm().trim.selected_split, -1); /* SPLIT stays SPLIT */
    e.act.trim_nudge(500);
    CHECK(e.until([](const ui::ViewModel &v) { return v.trim.marker_ms == 1000; }, 1000));
    e.act.trim_split();
    CHECK(e.until([](const ui::ViewModel &v) { return v.trim.splits.size() == 2; }, 1000));
    CHECK(e.vm().trim.splits[0] == 500 && e.vm().trim.splits[1] == 1000);

    /* SAVE: the box names the three files */
    e.act.trim_save();
    CHECK(e.until([](const ui::ViewModel &v) { return v.save.shown; }, 1000));
    const std::vector<ui::SaveFile> files = e.vm().save.files;
    CHECK_EQ(files.size(), 3u);
    const std::string stem = take.substr(0, take.size() - 4);
    if (files.size() == 3) {
        CHECK_EQ(files[0].name, stem + "-1.wav");
        CHECK_EQ(files[1].name, stem + "-2.wav");
        CHECK_EQ(files[2].name, stem + "-3.wav");
        CHECK_EQ(files[0].length_ms, 500);
        CHECK_EQ(files[1].length_ms, 500);
        CHECK(files[2].length_ms >= 500);
    }
    e.act.save_choose(ui::SaveMode::NewFiles);
    CHECK(e.until([](const ui::ViewModel &v) { return !v.save.shown && v.list.take_count == 4; }, 5000));
    CHECK(e.until([&e](const ui::ViewModel &) { return e.saw_notice("Saved 3 new files"); }, 2000));

    /* back to the list: the trim page closes, the four takes listed, the files on disk */
    e.act.open(ui::Screen::List);
    CHECK(e.until([](const ui::ViewModel &v) { return v.trim.take_id.empty(); }, 1000));
    const ui::ListVm l = e.vm().list;
    CHECK_EQ(l.takes.size(), 4u);
    if (l.takes.size() == 4) {
        CHECK_EQ(l.takes[0].name, "Take 001");
        CHECK_EQ(l.takes[1].name, "Take 001-1");
        CHECK_EQ(l.takes[2].name, "Take 001-2");
        CHECK_EQ(l.takes[3].name, "Take 001-3");
        CHECK_EQ(l.takes[1].length_ms, 500);
        CHECK_EQ(l.takes[2].length_ms, 500);
    }
    for (int i = 1; i <= 3; ++i) CHECK(file_exists(e.recordings() + "/" + stem + "-" + std::to_string(i) + ".wav"));
    CHECK(file_exists(e.recordings() + "/" + take)); /* the original kept */
    /* the take number did not move: the next take is still 002 */
    e.act.open(ui::Screen::Recorder);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.tape.label == "Take 002"; }, 1000));

    /* PLAY the take from the list, then STOP */
    e.act.list_play(0);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.transport == ui::Transport::Playing; }, 1000));
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.counter_ms > 200; }, 2000));
    CHECK_EQ(e.vm().rec.tape.label, "Take 001");
    click(e, ui::KeyId::Stop);
    CHECK(e.until(idle, 1000));

    /* DELETE -3 after the confirm box */
    e.act.open(ui::Screen::List);
    e.tick();
    e.act.list_delete(3);
    e.tick();
    CHECK(e.vm().confirm.shown);
    e.act.confirm_answer(true);
    CHECK(e.until([](const ui::ViewModel &v) { return v.list.take_count == 3; }, 2000));
    CHECK(!file_exists(e.recordings() + "/" + stem + "-3.wav"));
}

void test_countdown()
{
    demo::Script s;
    E2E e(s);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.input_present; }, 2000));
    e.act.setting_choice("timer", 1);
    CHECK(e.until([](const ui::ViewModel &v) { return find_row(v, "timer") && find_row(v, "timer")->value == "3 s"; }, 1000));
    click(e, ui::KeyId::Rec);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.transport == ui::Transport::Countdown; }, 1000));
    CHECK(e.vm().rec.countdown_left >= 2 && e.vm().rec.countdown_left <= 3);
    CHECK(e.vm().rec.countdown_fraction > 0.5f);
    e.run_for(300);
    e.act.countdown_cancel();
    CHECK(e.until(idle, 1000));
    e.run_for(300);
    CHECK_EQ(e.vm().list.take_count, 0); /* nothing written */
    /* and to the end: the take starts at zero */
    click(e, ui::KeyId::Rec);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.transport == ui::Transport::Countdown; }, 1000));
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.transport == ui::Transport::Recording; }, 4500));
    click(e, ui::KeyId::Stop);
    CHECK(e.until([](const ui::ViewModel &v) { return idle(v) && v.list.take_count == 1; }, 3000));
    e.act.setting_choice("timer", 0);
}

void test_unplug_replug()
{
    demo::Script s;
    s.unplug_ms = 1500;
    s.replug_ms = 3200;
    E2E e(s);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.input_present; }, 1000));
    click(e, ui::KeyId::Rec);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.transport == ui::Transport::Recording; }, 1000));
    CHECK(e.until([](const ui::ViewModel &v) { return !v.rec.input_present; }, 3000));
    CHECK(e.until([](const ui::ViewModel &v) { return idle(v) && v.list.take_count == 1; }, 2000)); /* the take kept */
    CHECK(!e.vm().rec.tape.loaded);
    CHECK_EQ(e.vm().rec.info.take, "NO INPUT");
    CHECK(!e.vm().rec.keys[static_cast<int>(ui::KeyId::Rec)].enabled);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.input_present; }, 4000)); /* found again by itself */
    CHECK(e.until([&e](const ui::ViewModel &) { return e.saw_notice("is back"); }, 2000));
    CHECK(e.saw_notice("unplugged"));
    CHECK(e.vm().list.takes.size() == 1 && e.vm().list.takes[0].length_ms > 800);
}

void test_no_input_and_cards()
{
    {
        demo::Script s;
        s.cards = "none";
        E2E e(s);
        e.run_for(200);
        CHECK(!e.vm().rec.input_present);
        CHECK(!e.vm().rec.tape.loaded);
        CHECK_EQ(e.vm().rec.info.take, "NO INPUT");
        CHECK(!e.vm().rec.card.shown);
        CHECK(!e.vm().rec.keys[static_cast<int>(ui::KeyId::Rec)].enabled);
        click(e, ui::KeyId::Rec);
        e.run_for(300);
        CHECK(idle(e.vm()));
        CHECK_EQ(e.vm().list.take_count, 0);
    }
    {
        demo::Script s;
        s.cards = "plain";
        E2E e(s);
        CHECK(e.until([](const ui::ViewModel &v) { return v.rec.input_present; }, 2000));
        CHECK(!e.vm().rec.card.shown); /* no level control: SOFT alone */
        CHECK(!find_row(e.vm(), "level") && !find_row(e.vm(), "agc"));
        click(e, ui::KeyId::Rec);
        CHECK(e.until([](const ui::ViewModel &v) { return v.rec.transport == ui::Transport::Recording; }, 1000));
        e.run_for(400);
        click(e, ui::KeyId::Stop);
        CHECK(e.until([](const ui::ViewModel &v) { return idle(v) && v.list.take_count == 1; }, 3000));
    }
    {
        demo::Script s;
        s.cards = "both";
        E2E e(s);
        CHECK(e.until([](const ui::ViewModel &v) { return v.rec.input_present; }, 2000));
        const ui::SettingRow *in = find_row(e.vm(), "input");
        CHECK(in && in->choices.size() == 2 && in->value == "Demo Microphone");
        CHECK(e.vm().rec.card.shown);
        e.act.setting_choice("input", 1);
        CHECK(e.until(
            [](const ui::ViewModel &v) {
                const ui::SettingRow *r = find_row(v, "input");
                return r && r->value == "Demo Line Input" && v.rec.input_present && !v.rec.card.shown;
            },
            2000));
    }
}

/* The speech headset of the demo: mono, 16000 Hz only. Record, list, trim with two splits, save: every millisecond figure
 * on the way is frames / 16000, and the new files are cut at the right byte offsets. */
void test_speech_card()
{
    demo::Script s;
    s.cards = "speech";
    E2E e(s);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.input_present; }, 2000));
    CHECK_EQ(e.vm().rec.info.format, "16k/16 WAV");
    CHECK_EQ(e.vm().rec.tape.format, "WAV 16k");
    const ui::SettingRow *fmt = find_row(e.vm(), "format");
    CHECK(fmt && fmt->choices.size() == 1 && fmt->choices[0] == "WAV 16 kHz 16-bit");
    CHECK(e.vm().rec.card.shown);              /* it has a level */
    CHECK(!find_row(e.vm(), "agc"));           /* no auto gain */
    CHECK(find_row(e.vm(), "mute"));
    click(e, ui::KeyId::Rec);
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.transport == ui::Transport::Recording; }, 1000));
    CHECK(e.until([](const ui::ViewModel &v) { return v.rec.counter_ms >= 1600; }, 4000));
    click(e, ui::KeyId::Stop);
    CHECK(e.until([](const ui::ViewModel &v) { return idle(v) && v.list.take_count == 1; }, 3000));
    const std::string take = e.vm().list.takes[0].id;
    CHECK(e.vm().list.takes[0].length_ms >= 1500 && e.vm().list.takes[0].length_ms < 3500);

    e.act.open(ui::Screen::List);
    e.act.list_trim(0);
    e.act.open(ui::Screen::Trim);
    CHECK(e.until([&take](const ui::ViewModel &v) { return v.trim.take_id == take && !v.trim.window_peaks.empty(); }, 3000));
    e.act.trim_nudge(500);
    CHECK(e.until([](const ui::ViewModel &v) { return v.trim.marker_ms == 500; }, 1000));
    e.act.trim_split();
    CHECK(e.until([](const ui::ViewModel &v) { return v.trim.splits.size() == 1; }, 1000));
    e.act.trim_nudge(500);
    CHECK(e.until([](const ui::ViewModel &v) { return v.trim.marker_ms == 1000; }, 1000));
    e.act.trim_split();
    CHECK(e.until([](const ui::ViewModel &v) { return v.trim.splits.size() == 2; }, 1000));
    e.act.trim_save();
    CHECK(e.until([](const ui::ViewModel &v) { return v.save.shown; }, 1000));
    const std::vector<ui::SaveFile> files = e.vm().save.files;
    CHECK_EQ(files.size(), 3u);
    if (files.size() == 3) {
        CHECK_EQ(files[0].length_ms, 500);
        CHECK_EQ(files[1].length_ms, 500);
    }
    e.act.save_choose(ui::SaveMode::NewFiles);
    CHECK(e.until([](const ui::ViewModel &v) { return !v.save.shown && v.list.take_count == 4; }, 5000));
    e.act.open(ui::Screen::List);
    CHECK(e.until([](const ui::ViewModel &v) { return v.trim.take_id.empty(); }, 1000));
    const ui::ListVm l = e.vm().list;
    if (l.takes.size() == 4) {
        CHECK_EQ(l.takes[1].length_ms, 500);
        CHECK_EQ(l.takes[2].length_ms, 500);
    }
    /* the byte length of a 500 ms piece at 16000 Hz mono S16 is 16,000 bytes of audio */
    const std::string stem = take.substr(0, take.size() - 4);
    struct stat st;
    CHECK(::stat((e.recordings() + "/" + stem + "-1.wav").c_str(), &st) == 0);
    CHECK_EQ(static_cast<long long>(st.st_size), 5120LL + 16000LL);
}

} // namespace

void test_demo()
{
    test_signal_and_env();
    test_take_trim_save();
    test_countdown();
    test_unplug_replug();
    test_no_input_and_cards();
    test_speech_card();
}
