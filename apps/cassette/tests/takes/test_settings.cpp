/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette takes tests: the settings store.
 */
#include "check.hpp"

#include "json.hpp"
#include "settings.hpp"

#include <cerrno>

using namespace cassette;
using namespace tk;

void test_settings() {
    TempDir d;
    FailOps ops;
    const std::string path = default_settings_path(d.path); /* <home>/.config/cassette/settings.json */
    CHECK_EQ(path, d.path + "/.config/cassette/settings.json");

    /* missing file: defaults */
    SettingsStore s(ops, path);
    CHECK_EQ(s.load(), -ENOENT);
    const Settings def;
    CHECK(s.values().format == RecFormat::Wav);
    CHECK_EQ(s.values().folder, std::string("~/Recordings"));
    CHECK_EQ(s.values().prefix, std::string("take"));
    CHECK_EQ(s.values().sync_interval_s, 10);
    CHECK(s.values().auto_split);
    CHECK_EQ(s.values().record_timer_s, 0);
    CHECK(s.values().rew_fwd == ScanMode::Fast);
    CHECK(!s.values().low_cut);
    CHECK(!s.values().limiter);
    CHECK_EQ(s.values().soft_gain_db, 0.0);
    CHECK(!s.values().monitor); /* the live monitor is off by default, at 50 % */
    CHECK_EQ(s.values().monitor_level, 50);
    CHECK(!s.values().wave_fit); /* the trim waveform opens in TRUE */
    CHECK(s.values().card.empty() && s.values().output_device.empty());

    /* round trip, the folder is created */
    Settings &v = s.values();
    v.format = RecFormat::Flac;
    v.folder = "/media/stick/rec";
    v.prefix = "band";
    v.sync_interval_s = 20;
    v.auto_split = false;
    v.record_timer_s = 10;
    v.rew_fwd = ScanMode::Jump;
    v.low_cut = true;
    v.limiter = true;
    v.soft_gain_db = 12.5;
    v.monitor = true;
    v.monitor_level = 35;
    v.wave_fit = true;
    v.card = "UC02||";
    v.output_device = "hw:CARD=UC02,DEV=0";
    /* two cards with the same name, different by-id; the first has no USB identity */
    const std::string c1 = card_descriptor("UC02", "usb-UC02_UC02_UC02-00", "0000:0000");
    const std::string c2 = card_descriptor("UC02", "usb-UC02_UC02_UC02-01", "0d8c:0014");
    CHECK_EQ(c1, std::string("UC02|usb-UC02_UC02_UC02-00|"));
    CHECK_EQ(c2, std::string("UC02|usb-UC02_UC02_UC02-01|0d8c:0014"));
    CHECK(c1 != c2);
    CardState a;
    a.has_level = true;
    a.level_db = 17.9;
    a.auto_gain = false;
    a.mute = true;
    CardState b;
    b.has_level = false; /* a card without a level control */
    b.auto_gain = true;
    s.set_card_state(c1, a);
    s.set_card_state(c2, b);
    CHECK_EQ(s.save(), 0);

    SettingsStore s2(ops, path);
    CHECK_EQ(s2.load(), 0);
    CHECK(s2.values().format == RecFormat::Flac);
    CHECK_EQ(s2.values().folder, std::string("/media/stick/rec"));
    CHECK_EQ(s2.values().prefix, std::string("band"));
    CHECK_EQ(s2.values().sync_interval_s, 20);
    CHECK(!s2.values().auto_split);
    CHECK_EQ(s2.values().record_timer_s, 10);
    CHECK(s2.values().rew_fwd == ScanMode::Jump);
    CHECK(s2.values().low_cut);
    CHECK(s2.values().limiter);
    CHECK_EQ(s2.values().soft_gain_db, 12.5);
    CHECK(s2.values().monitor);
    CHECK_EQ(s2.values().monitor_level, 35);
    CHECK(s2.values().wave_fit);
    CHECK_EQ(s2.values().card, "UC02||");
    CHECK_EQ(s2.values().output_device, "hw:CARD=UC02,DEV=0");
    CardState r;
    CHECK(s2.card_state(c1, r));
    CHECK(r.has_level);
    CHECK_EQ(r.level_db, 17.9);
    CHECK(!r.auto_gain);
    CHECK(r.mute);
    CHECK(s2.card_state(c2, r));
    CHECK(!r.has_level);
    CHECK(r.auto_gain);
    CHECK(!r.mute);
    CHECK(!s2.card_state(card_descriptor("UC02", "usb-other", ""), r));
    CHECK_EQ(s2.cards().size(), static_cast<size_t>(2));
    {
        json::Value root;
        CHECK(json::parse(read_text(path), root));
        CHECK(root.get("version") && root.get("version")->n == 1.0);
    }

    /* unknown keys survive a write, at the top level and inside a card */
    write_text(path, "{\"version\": 1, \"prefix\": \"x\", \"future\": {\"k\": [1, 2]}, "
                     "\"cards\": {\"A|b|\": {\"level_db\": 3, \"eq\": \"warm\"}}}");
    SettingsStore s3(ops, path);
    CHECK_EQ(s3.load(), 0);
    CHECK_EQ(s3.values().prefix, std::string("x"));
    CardState c;
    CHECK(s3.card_state("A|b|", c));
    c.level_db = 4;
    s3.set_card_state("A|b|", c);
    CHECK_EQ(s3.save(), 0);
    {
        json::Value root;
        CHECK(json::parse(read_text(path), root));
        const json::Value *fu = root.get("future");
        CHECK(fu && fu->is_obj() && fu->get("k") && fu->get("k")->arr.size() == 2);
        const json::Value *cards = root.get("cards");
        const json::Value *e = cards ? cards->get("A|b|") : nullptr;
        CHECK(e && e->get("eq") && e->get("eq")->s == "warm");
        CHECK(e && e->get("level_db") && e->get("level_db")->n == 4.0);
    }
    /* forget a card */
    CHECK(s3.forget_card("A|b|"));
    CHECK_EQ(s3.save(), 0);
    SettingsStore s3b(ops, path);
    s3b.load();
    CHECK(!s3b.card_state("A|b|", c));

    /* the monitor level is clamped to 0..100 in steps of 5; a wrong type is ignored */
    for (const auto &pr : std::vector<std::pair<std::string, int>>{{"37", 35}, {"38", 40}, {"250", 100}, {"-20", 0}, {"0", 0}, {"100", 100}, {"\"loud\"", 50}}) {
        write_text(path, "{\"version\": 1, \"monitor\": true, \"monitor_level\": " + pr.first + "}");
        SettingsStore sm(ops, path);
        CHECK_EQ(sm.load(), 0);
        CHECK(sm.values().monitor);
        CHECK_EQ(sm.values().monitor_level, pr.second);
    }
    /* wave_fit: written only when true (back to TRUE removes the key); an older file without it opens in TRUE; a wrong type is ignored */
    {
        SettingsStore sw(ops, path);
        sw.load();
        sw.values().wave_fit = true;
        CHECK_EQ(sw.save(), 0);
        CHECK(read_text(path).find("wave_fit") != std::string::npos);
        sw.values().wave_fit = false;
        CHECK_EQ(sw.save(), 0);
        CHECK(read_text(path).find("wave_fit") == std::string::npos);
        SettingsStore sw2(ops, path);
        CHECK_EQ(sw2.load(), 0);
        CHECK(!sw2.values().wave_fit);
    }
    write_text(path, "{\"version\": 1, \"format\": \"flac\", \"monitor\": true}"); /* an old file */
    {
        SettingsStore so(ops, path);
        CHECK_EQ(so.load(), 0);
        CHECK(!so.values().wave_fit);
        CHECK(so.values().format == RecFormat::Flac && so.values().monitor);
    }
    write_text(path, "{\"version\": 1, \"wave_fit\": \"yes\"}");
    {
        SettingsStore sw(ops, path);
        CHECK_EQ(sw.load(), 0);
        CHECK(!sw.values().wave_fit);
    }
    write_text(path, "{\"version\": 1, \"wave_fit\": true}");
    {
        SettingsStore sw(ops, path);
        CHECK_EQ(sw.load(), 0);
        CHECK(sw.values().wave_fit);
    }
    write_text(path, "{\"version\": 1, \"monitor\": \"yes\"}");
    {
        SettingsStore sm(ops, path);
        CHECK_EQ(sm.load(), 0);
        CHECK(!sm.values().monitor);
    }

    /* corrupt files give defaults and never crash */
    for (const std::string &junk : {std::string("{\"prefix\": "), std::string("[1,2,3]"), std::string("null"),
                                    std::string("\x00\xff\xfe garbage", 11), std::string()}) {
        write_text(path, junk);
        SettingsStore sc(ops, path);
        CHECK_EQ(sc.load(), -EINVAL);
        CHECK_EQ(sc.values().prefix, def.prefix);
        CHECK_EQ(sc.values().sync_interval_s, def.sync_interval_s);
        CHECK(sc.cards().empty());
    }

    /* invalid values give defaults, numbers are clamped */
    write_text(path, "{\"format\": \"mp3\", \"folder\": 7, \"prefix\": \"../evil\", \"sync_interval_s\": 99, "
                     "\"auto_split\": \"yes\", \"record_timer_s\": 12, \"rew_fwd\": \"warp\", \"low_cut\": 1, "
                     "\"soft_gain_db\": 40, \"cards\": {\"X||\": {\"level_db\": 1e9, \"mute\": \"no\"}, \"Y||\": 5}}");
    SettingsStore sv(ops, path);
    CHECK_EQ(sv.load(), 0);
    CHECK(sv.values().format == RecFormat::Wav);
    CHECK_EQ(sv.values().folder, def.folder);
    CHECK_EQ(sv.values().prefix, def.prefix);
    CHECK_EQ(sv.values().sync_interval_s, 30);
    CHECK(sv.values().auto_split);
    CHECK_EQ(sv.values().record_timer_s, 10);
    CHECK(sv.values().rew_fwd == ScanMode::Fast);
    CHECK(!sv.values().low_cut);
    CHECK_EQ(sv.values().soft_gain_db, 24.0);
    CHECK(sv.card_state("X||", c));
    CHECK_EQ(c.level_db, kCardLevelMax);
    CHECK(!c.mute);
    CHECK(!sv.card_state("Y||", c));
    write_text(path, "{\"sync_interval_s\": 1, \"soft_gain_db\": -3, \"record_timer_s\": -5}");
    CHECK_EQ(sv.load(), 0);
    CHECK_EQ(sv.values().sync_interval_s, 5);
    CHECK_EQ(sv.values().soft_gain_db, 0.0);
    CHECK_EQ(sv.values().record_timer_s, 0);
    Settings cl;
    cl.record_timer_s = 16;
    cl.prefix = "a/b";
    cl.folder = "";
    clamp_settings(cl);
    CHECK_EQ(cl.record_timer_s, 20);
    CHECK_EQ(cl.prefix, std::string("take"));
    CHECK_EQ(cl.folder, std::string("~/Recordings"));

    /* crash safety: a failure between the temp write and the rename keeps the old file */
    SettingsStore sa(ops, path);
    sa.load();
    sa.values().prefix = "old";
    CHECK_EQ(sa.save(), 0);
    const std::string before = read_text(path);
    sa.values().prefix = "new";
    ops.rename_errno = EIO;
    CHECK_EQ(sa.save(), -EIO);
    ops.rename_errno = 0;
    CHECK_EQ(read_text(path), before);
    CHECK(!exists(path + ".tmp"));
    ops.fsync_errno = ENOSPC;
    CHECK_EQ(sa.save(), -ENOSPC);
    ops.fsync_errno = 0;
    CHECK_EQ(read_text(path), before);
    SettingsStore sb(ops, path);
    CHECK_EQ(sb.load(), 0);
    CHECK_EQ(sb.values().prefix, std::string("old"));
    CHECK_EQ(sa.save(), 0);
    CHECK_EQ(sb.load(), 0);
    CHECK_EQ(sb.values().prefix, std::string("new"));
}
