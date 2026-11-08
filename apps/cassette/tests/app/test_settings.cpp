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

}  // namespace

void test_settings() {
    t_persistence();
    t_corrupt_settings();
}
