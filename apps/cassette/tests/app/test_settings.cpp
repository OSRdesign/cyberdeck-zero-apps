/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: settings applied live, persisted, and applied again after a restart (the per-card mixer state included).
 */
#include "helpers.hpp"

using namespace ta;

namespace {

void reboot_mixer(Rig &r) {
    r.alsa.with([](FakeAlsa &a) {
        for (auto &kv : a.cards_[0].values) kv.second = kv.first.find("Capture Volume") != std::string::npos ? 0 : 1;
    });
}

void t_persistence() {
    Rig r(one_card());
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    CHECK(c.set_level_db(12.0));
    CHECK(c.set_auto_gain(false));
    CHECK(c.set_mute(false));
    CHECK(c.set_soft_gain_db(6.5));
    CHECK(c.set_prefix("band"));
    CHECK(!c.set_prefix("../evil"));
    CHECK(!c.set_prefix(""));
    CHECK(c.set_sync_interval(20));
    CHECK(!c.set_sync_interval(3));
    CHECK(c.set_auto_split(false));
    CHECK(c.set_record_timer(10));
    CHECK(c.set_low_cut(true));
    CHECK(c.set_wind_mode(true));
    CHECK(c.set_limiter(true));
    CHECK(c.set_scan_mode(ScanMode::Jump));
    CHECK(c.set_format(RecFormat::Wav));
    CHECK(!c.set_format(RecFormat::Flac));
    CHECK(c.set_folder("~/Sessions"));
    Snapshot s = r.snap();
    CHECK_EQ(s.settings.prefix, "band");
    CHECK_EQ(s.folder, r.home + "/Sessions");
    CHECK_EQ(s.card.soft_gain_db, 6.5);
    /* a recording lands in the new folder with the new prefix */
    CHECK(c.set_record_timer(0));
    c.key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    CHECK(!c.set_folder("~/Elsewhere")); /* not while a take runs */
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.4; }));
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    CHECK(c.set_record_timer(10));
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) CHECK(rows[0].name.compare(0, 9, "band_001_") == 0);
    r.close();

    /* a restart (the board rebooted: the mixer is back at its defaults) */
    reboot_mixer(r);
    CHECK(r.alsa.mixer_value("Device", "Mic Capture Volume") == 0);
    r.open();
    CHECK(r.wait_meter());
    s = r.snap();
    CHECK_EQ(s.settings.prefix, "band");
    CHECK_EQ(s.settings.sync_interval_s, 20);
    CHECK(!s.settings.auto_split);
    CHECK_EQ(s.settings.record_timer_s, 10);
    CHECK(s.settings.low_cut && s.settings.limiter);
    CHECK(s.settings.rew_fwd == ScanMode::Jump);
    CHECK_EQ(s.settings.folder, "~/Sessions");
    CHECK_EQ(s.folder, r.home + "/Sessions");
    CHECK_EQ(s.card.soft_gain_db, 6.5);
    CHECK(r.alsa.mixer_value("Device", "Mic Capture Volume") == 8);
    CHECK(r.alsa.mixer_value("Device", "Auto Gain Control") == 0);
    CHECK(std::abs(s.card.level_db - 11.9) < 0.2);
    CHECK(!s.card.auto_gain && !s.card.mute);
    CHECK_EQ(r.ctl().takes().size(), 1u);
    /* the deck got the saved record timer */
    r.ctl().key_event(KeyEvent::Rec);
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckCountdown));
    r.ctl().key_event(KeyEvent::Stop);
    /* mute is a saved choice too */
    CHECK(r.ctl().set_mute(true));
    r.close();
    reboot_mixer(r);
    r.open();
    CHECK(r.wait_meter());
    CHECK(r.snap().card.mute);
    CHECK(r.alsa.mixer_value("Device", "Mic Capture Switch") == 0);
}

void t_corrupt_settings() {
    Rig r(one_card());
    posix_file_ops().mkdir(r.home + "/.config");
    posix_file_ops().mkdir(r.home + "/.config/cassette");
    FILE *f = std::fopen((r.home + "/.config/cassette/settings.json").c_str(), "wb");
    std::fputs("{ this is not json", f);
    std::fclose(f);
    r.open();
    CHECK(r.wait_meter()); /* defaults, no crash */
    CHECK_EQ(r.snap().settings.prefix, "take");
    CHECK(r.ctl().set_prefix("ok"));
    r.close();
    r.open();
    CHECK_EQ(r.snap().settings.prefix, "ok");
}

/* T6d: the snapshot lists every output with its device and the one in use; choosing one by its device sticks */
void t_outputs() {
    Rig r(one_card());
    r.open();
    CHECK(r.wait_meter());
    Snapshot s = r.snap();
    CHECK(s.card.output_labels.size() >= 2); /* the card's own playback side and the default output */
    CHECK_EQ(s.card.output_devices.size(), s.card.output_labels.size());
    CHECK(s.card.output_selected >= 0 && s.card.output_selected < static_cast<int>(s.card.output_labels.size()));
    if (s.card.output_devices.size() < 2 || s.card.output_selected < 0) return;
    const int other = 1 - s.card.output_selected;
    CHECK(r.ctl().select_output(s.card.output_devices[static_cast<size_t>(other)]));
    s = r.snap();
    CHECK_EQ(s.card.output_selected, other);
    CHECK_EQ(s.card.output_label, s.card.output_labels[static_cast<size_t>(other)]);
}

/* The Take number setting on the real controller: set, record, restart, collision guard, busy refusal. */
void t_take_number() {
    Rig r(one_card());
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    uint64_t landed = 0;
    CHECK(c.set_record_timer(0));
    CHECK_EQ(c.set_next_take_number(0), -EINVAL);
    CHECK_EQ(c.set_next_take_number(10000), -EINVAL);
    CHECK_EQ(c.set_next_take_number(50, &landed), 0);
    CHECK_EQ(landed, static_cast<uint64_t>(50));
    c.key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    CHECK_EQ(c.set_next_take_number(70), -EBUSY); /* not while a take runs */
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.3; }));
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) CHECK(rows[0].name.compare(0, 9, "take_050_") == 0);
    r.close();

    /* a restart keeps the counter: the next take is 051 */
    r.open();
    CHECK(r.wait_meter());
    CHECK(r.ctl().set_record_timer(0));
    r.ctl().key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.3; }));
    r.ctl().key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    int n51 = 0;
    for (const std::string &w : r.wavs())
        if (w.compare(0, 9, "take_051_") == 0) ++n51;
    CHECK(n51 >= 1);
    /* the guard: 050 and 051 exist, so 50 lands on 052; 1 is free and is taken as it is */
    CHECK_EQ(r.ctl().set_next_take_number(50, &landed), 0);
    CHECK_EQ(landed, static_cast<uint64_t>(52));
    CHECK_EQ(r.ctl().set_next_take_number(1, &landed), 0);
    CHECK_EQ(landed, static_cast<uint64_t>(1));
    r.close();
}

/* A prefix with spaces on the real controller: the setting sticks across a restart, a recording and its journal and peak
 * names carry it, a rename (with '_' and '-') works and says why when refused. */
std::vector<std::string> posix_file_ops_list(const std::string &dir) {
    std::vector<std::string> n;
    posix_file_ops().list_dir(dir, n);
    return n;
}

int c_rename(Rig &r, const std::string &name, const std::string &prefix, std::string *nn, RenameRefusal *why) {
    std::string out;
    const int rc = r.ctl().rename_take(name, prefix, &out, why);
    if (nn && rc == 0) *nn = out;
    return rc;
}

void t_prefix_spaces() {
    Rig r(one_card());
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    CHECK(c.set_prefix("my  band 2"));
    CHECK(!c.set_prefix(" lead"));
    CHECK(!c.set_prefix("trail "));
    CHECK(!c.set_prefix(std::string(25, 'a')));
    CHECK(c.set_prefix(std::string(10, 'a') + " " + std::string(13, 'b'))); /* 24 */
    CHECK(c.set_prefix("my  band 2"));
    CHECK(c.set_record_timer(0));
    c.key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.4; }));
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (rows.empty()) return;
    const std::string name = rows[0].name;
    CHECK(name.compare(0, 16, "my  band 2_001_2") == 0 || name.compare(0, 18, "my  band 2_001_nod") == 0);
    TakeName tn;
    CHECK(parse_take_name(name, tn));
    CHECK_EQ(tn.prefix, "my  band 2");
    bool journal = false;
    for (const std::string &f : posix_file_ops_list(r.rec() + "/.journal"))
        if (f.compare(0, 10, "my  band 2") == 0) journal = true;
    CHECK(journal);
    /* restart: the prefix is back, the next take uses it */
    r.close();
    r.open();
    CHECK_EQ(r.snap().settings.prefix, "my  band 2");
    /* rename with '_' and '-', and the reasons */
    std::string nn;
    RenameRefusal why = RenameRefusal::None;
    CHECK_EQ(c_rename(r, name, "my_take-2", &nn, &why), 0);
    CHECK_EQ(nn.compare(0, 14, "my_take-2_001_"), 0);
    CHECK_EQ(c_rename(r, nn, "a b", &nn, &why), 0);
    CHECK_EQ(nn.compare(0, 8, "a b_001_"), 0);
    CHECK_EQ(c_rename(r, nn, std::string(25, 'x'), nullptr, &why), -EINVAL);
    CHECK(why == RenameRefusal::TooLong);
    CHECK_EQ(c_rename(r, nn, "no!", nullptr, &why), -EINVAL);
    CHECK(why == RenameRefusal::BadChar);
    CHECK_EQ(c_rename(r, nn, "", nullptr, &why), -EINVAL);
    CHECK(why == RenameRefusal::Empty);
}

}  // namespace

void test_settings() {
    t_outputs();
    t_persistence();
    t_corrupt_settings();
    t_take_number();
    t_prefix_spaces();
}
