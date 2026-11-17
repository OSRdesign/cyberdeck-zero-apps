/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: M4 (brief 15.1 "Scenes": listing, sort, filter, lock, rename, delete (locked refused), thumbnail cache keyed
 * by mtime; 6.3 the repair at start), the Scenes page's geometry, and the REC start indicator (M4 item 1: the blink's
 * phase from a passed-in clock; the controller's starting state; a stop before the first frame leaves nothing).
 */
#include "check.hpp"

#include "app/controller.hpp"
#include "app/rec_indicator.hpp"
#include "jpeg/turbojpeg.hpp"
#include "layout/scenes_layout.hpp"
#include "live/clock.hpp"
#include "record/av_libs.hpp"
#include "record/journal.hpp"
#include "record/recorder.hpp"
#include "scenes/atomic_file.hpp"
#include "scenes/fileops.hpp"
#include "scenes/index.hpp"
#include "scenes/locks.hpp"
#include "scenes/thumbs.hpp"

#include <sys/stat.h>
#include <utime.h>

#include <chrono>
#include <fstream>
#include <thread>

using namespace vhs;

namespace {

bool exists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

void touch(const std::string &path, const std::string &bytes, std::int64_t mtime)
{
    write_file(path, bytes);
    struct utimbuf t;
    t.actime = static_cast<time_t>(mtime);
    t.modtime = static_cast<time_t>(mtime);
    utime(path.c_str(), &t);
}

std::vector<std::string> fixture_jpegs()
{
    std::vector<std::string> out;
    const std::string p = fixture("testsrc-720.mjpg");
    if (p.empty()) return out;
    std::ifstream in(p, std::ios::binary);
    const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t at = 0;
    while (true) {
        const std::size_t s = all.find("\xFF\xD8", at);
        if (s == std::string::npos) break;
        const std::size_t e = all.find("\xFF\xD9", s + 2);
        if (e == std::string::npos) break;
        out.push_back(all.substr(s, e + 2 - s));
        at = e + 2;
    }
    return out;
}

void indicator()
{
    /* 500 ms on, 500 ms off, lit first (the press) */
    CHECK(rec_dot_lit(1000, 1000));
    CHECK(rec_dot_lit(1499, 1000));
    CHECK(!rec_dot_lit(1500, 1000));
    CHECK(!rec_dot_lit(1999, 1000));
    CHECK(rec_dot_lit(2000, 1000));
    CHECK(!rec_dot_lit(3700, 1000));
    CHECK(rec_dot_lit(900, 1000)); /* a clock read before the press: lit */
    /* solid once recording */
    CHECK(rec_dot_shown(false, 1600, 1000));
    CHECK(!rec_dot_shown(true, 1600, 1000));
    CHECK(rec_dot_shown(true, 2100, 1000));
}

void layout()
{
    /* the canvas boards (TARGET 44: canvas pixels) */
    ScenesLayoutIn in;
    in.base.w = 480;
    in.base.h = 320;
    in.base.body_y = 40;
    in.base.target = 44;
    in.row = 42;
    in.list_row = 30;
    ScenesLayout l = scenes_layout(in);
    CHECK(!l.portrait);
    CHECK_EQ(l.panel.x, 8);
    CHECK_EQ(l.panel.w, 464);
    CHECK_EQ(l.chip[0].x, 8 + 250); /* canvas chips(250, 3, ..., 60, 24) in the panel */
    CHECK_EQ(l.chip[0].y, l.panel.y + 3);
    CHECK_EQ(l.chip[0].w, 60);
    CHECK_EQ(l.chip[0].h, 24);
    CHECK_EQ(l.chip[1].x, 8 + 316);
    CHECK(l.chip_hit[0].h >= 42 && l.chip_hit[0].y == l.panel.y);
    CHECK_EQ(l.list.y, l.panel.y + 30);
    CHECK_EQ(l.row_h, 52);
    CHECK(l.thumb.x == 12 && l.thumb.y == 8 && l.thumb.w == 64 && l.thumb.h == 36);
    CHECK_EQ(l.name_x, 88);
    CHECK_EQ(l.meta_y, 28);
    CHECK_EQ(l.date_r, 36);
    CHECK_EQ(l.sheet.x, 16);
    CHECK_EQ(l.sheet.y, l.panel.y + 30); /* canvas: the sheet at (8, 30) in the panel */
    CHECK_EQ(l.sheet.w, 448);
    CHECK_EQ(l.sheet_row_h, 31);
    CHECK(l.sheet.y + l.sheet.h <= l.panel.y + l.panel.h);
    in.base.w = 320;
    in.base.h = 480;
    l = scenes_layout(in);
    CHECK(l.portrait);
    CHECK_EQ(l.chip[0].x, 8 + 112); /* canvas chips(112, 4, ..., 56, 24) */
    CHECK_EQ(l.chip[0].w, 56);
    CHECK_EQ(l.row_h, 56);
    CHECK(l.thumb.x == 10 && l.thumb.y == 10 && l.thumb.w == 56 && l.thumb.h == 32);
    CHECK_EQ(l.name_x, 76);
    CHECK_EQ(l.date_r, 0);
    CHECK_EQ(l.list.y, l.panel.y + 34);
    /* a dense class: rows never under LIST_ROW, chips' hit boxes at least ROW */
    in.base.w = 800;
    in.base.h = 480;
    in.base.target = 80;
    in.row = 76;
    in.list_row = 57;
    l = scenes_layout(in);
    CHECK(l.row_h >= 57 && l.sheet_row_h >= 57 && l.chip_hit[2].h >= 76);
    CHECK(l.sheet.y + l.sheet.h <= l.panel.y + l.panel.h); /* the sheet fits; its rows scroll when they must */
    CHECK(l.sheet_note.y + l.sheet_note.h <= l.sheet.h);
    CHECK(l.sheet_rows_h >= l.sheet_row_h);
    /* the Square class has its own form (the 720x720 canvas times k; more in test_layout): the date column stays */
    in.base.w = 480;
    in.base.h = 480;
    in.base.square = true;
    l = scenes_layout(in);
    CHECK(l.square && !l.portrait && l.date_r > 0 && l.name_w > 100);
    CHECK(l.chip[2].x + l.chip[2].w <= l.panel.x + l.panel.w);
}

void listing()
{
    FileOps &ops = posix_file_ops();
    const std::string d = make_temp_dir("scn");
    touch(d + "/scene_014_20261114-185200.mkv", std::string(1000, 'm'), 1763145120);
    touch(d + "/scene_014_20261114-185200_p2.mkv", std::string(900, 'm'), 1763145200);
    touch(d + "/photo_013_20261114-184710.jpg", "x", 1763144830);
    touch(d + "/Stopwatch test_012_20261114-160500.mkv", "x", 1763136300);
    touch(d + "/holiday.MP4", "x", 1763000000);        /* foreign: listed, never renamed */
    touch(d + "/.scene_015_nodate.repair.mkv", "x", 1763145300); /* dot file: skipped */
    touch(d + "/notes.txt", "x", 1763145300);          /* not a scene */
    make_dirs(d + "/.journal");
    CHECK_EQ(set_locked(ops, d, "Stopwatch test_012_20261114-160500.mkv", true), 0);
    Journal j;
    j.state = "closed";
    j.repaired = true;
    j.file = "scene_014_20261114-185200.mkv";
    j.parts = {j.file, "scene_014_20261114-185200_p2.mkv"};
    CHECK_EQ(journal_write(ops, d, "scene_014_20261114-185200", j), 0);
    const std::vector<SceneEntry> v = list_scenes(ops, d);
    CHECK_EQ(v.size(), static_cast<std::size_t>(5));
    if (v.size() == 5) {
        CHECK_EQ(v[0].name, std::string("scene_014_20261114-185200_p2.mkv")); /* newest first */
        CHECK_EQ(v[1].name, std::string("scene_014_20261114-185200.mkv"));
        CHECK_EQ(v[2].name, std::string("photo_013_20261114-184710.jpg"));
        CHECK(v[2].kind == SceneKind::Photo);
        CHECK(v[3].locked && v[3].parsed);
        CHECK(!v[4].parsed);
        CHECK(v[0].repaired && v[1].repaired && !v[2].repaired);
        /* the labels: the stem; a renamed scene's prefix alone (12.4 item 4); parts " p2"; portrait short */
        CHECK_EQ(scene_label(v[1], false, "scene"), std::string("scene_014_20261114-185200"));
        CHECK_EQ(scene_label(v[0], false, "scene"), std::string("scene_014_20261114-185200 p2"));
        CHECK_EQ(scene_label(v[0], true, "scene"), std::string("scene_014 p2"));
        CHECK_EQ(scene_label(v[2], true, "scene"), std::string("photo_013"));
        CHECK_EQ(scene_label(v[3], false, "scene"), std::string("Stopwatch test"));
        CHECK_EQ(scene_label(v[4], false, "scene"), std::string("holiday"));
        CHECK_EQ(scene_label(v[1], false, "take"), std::string("scene_014_20261114-185200")); /* "scene" itself: not renamed */
    }
    int movies = 0, photos = 0;
    count_scenes(ops, d, movies, photos);
    CHECK_EQ(movies, 4);
    CHECK_EQ(photos, 1);
    /* the texts of the canvas rows */
    SceneEntry m;
    m.kind = SceneKind::Movie;
    m.size = 1400000000ull;
    m.mtime = 1763145120; /* 2025-11-14 18:32 UTC */
    m.meta.ok = true;
    m.meta.duration_ms = 754000;
    m.meta.codec = "MJPEG";
    m.meta.w = 1280;
    m.meta.h = 720;
    CHECK_EQ(scene_meta_text(m, false, true), std::string("12:34 - 1.4 GB - MJPEG 720p"));
    CHECK_EQ(scene_meta_text(m, true, true), std::string("12:34 - 1.4 GB - 720p - Nov 14"));
    m.repaired = true;
    CHECK_EQ(scene_meta_text(m, false, true), std::string("12:34 - 1.4 GB - MJPEG 720p - repaired"));
    SceneEntry p;
    p.kind = SceneKind::Photo;
    p.size = 98000;
    p.meta.ok = true;
    p.meta.w = 1920;
    p.meta.h = 1080;
    CHECK_EQ(scene_meta_text(p, false, true), std::string("1920x1080 - 98 KB"));
    CHECK_EQ(scene_date_text(1763145120, true), std::string("Nov 14 18:32"));
    CHECK_EQ(duration_text(31000), std::string("00:31"));
    CHECK_EQ(duration_text(3723000), std::string("1:02:03"));
    CHECK_EQ(duration_text(-1), std::string("--:--"));
    CHECK_EQ(video_size_label(1920, 1080), std::string("1080p"));
    CHECK_EQ(video_size_label(1280, 960), std::string("1280x960"));
    CHECK_EQ(video_size_label(720, 1280), std::string("720p"));
    CHECK_EQ(codec_label(av::kCodecH264, 61), std::string("H.264"));
    CHECK_EQ(codec_label(225, 61), std::string("AV1"));
    CHECK_EQ(codec_label(226, 60), std::string("AV1"));
    CHECK_EQ(codec_label(99999, 61), std::string("video"));

    /* the metadata cache: by name + size + mtime */
    MetaCache mc;
    mc.load(ops, d + "/cache/index.json");
    SceneMeta got;
    CHECK(!mc.get(v[1], got));
    SceneMeta sm;
    sm.ok = true;
    sm.duration_ms = 5000;
    sm.codec = "MJPEG";
    sm.w = 1280;
    sm.h = 720;
    mc.put(v[1], sm);
    CHECK_EQ(mc.save(ops), 0);
    CHECK_EQ(mc.save(ops), 1); /* unchanged: not written again */
    MetaCache mc2;
    mc2.load(ops, d + "/cache/index.json");
    CHECK(mc2.get(v[1], got) && got.duration_ms == 5000 && got.codec == "MJPEG" && got.w == 1280);
    SceneEntry changed = v[1];
    changed.mtime += 1;
    CHECK(!mc2.get(changed, got));

    /* rename: the prefix only, Cassette's refusals; the lock entry and the journal follow */
    std::string to, why;
    CHECK(rename_scene(ops, d, "holiday.MP4", "x", to, why) < 0);
    CHECK_EQ(why, std::string("this file is not a VHS scene and cannot be renamed"));
    CHECK(rename_scene(ops, d, "photo_013_20261114-184710.jpg", "garden", to, why) < 0);
    CHECK_EQ(why, std::string("photos keep the name photo"));
    CHECK(rename_scene(ops, d, "scene_014_20261114-185200.mkv", "", to, why) < 0);
    CHECK_EQ(why, std::string("the name is empty"));
    CHECK(rename_scene(ops, d, "scene_014_20261114-185200.mkv", std::string(25, 'a'), to, why) < 0);
    CHECK_EQ(why, std::string("up to 24 characters"));
    CHECK(rename_scene(ops, d, "scene_014_20261114-185200.mkv", "a/b", to, why) < 0);
    CHECK_EQ(why, std::string("use letters, digits, space, - and _"));
    touch(d + "/garden_014_20261114-185200.mkv", "y", 1763000000);
    CHECK_EQ(rename_scene(ops, d, "scene_014_20261114-185200.mkv", "garden", to, why), -EEXIST);
    CHECK_EQ(why, std::string("that name is taken"));
    ops.unlink(d + "/garden_014_20261114-185200.mkv");
    CHECK_EQ(rename_scene(ops, d, "Stopwatch test_012_20261114-160500.mkv", "Lap 2", to, why), 0);
    CHECK_EQ(to, std::string("Lap 2_012_20261114-160500.mkv"));
    CHECK(read_locks(ops, d).count("Lap 2_012_20261114-160500.mkv") == 1);
    CHECK_EQ(rename_scene(ops, d, "scene_014_20261114-185200_p2.mkv", "Trip", to, why), 0);
    Journal jr;
    CHECK(journal_read(ops, journal_path(d, "scene_014_20261114-185200"), jr));
    CHECK(jr.parts.size() == 2 && jr.parts[1] == "Trip_014_20261114-185200_p2.mkv");

    /* delete: refused on a locked file; else the lock entry and the journal part go */
    CHECK_EQ(delete_scene(ops, d, "Lap 2_012_20261114-160500.mkv", why), -EPERM);
    CHECK_EQ(why, std::string("the file is locked"));
    CHECK(exists(d + "/Lap 2_012_20261114-160500.mkv"));
    CHECK_EQ(set_locked(ops, d, "Lap 2_012_20261114-160500.mkv", false), 0);
    CHECK(!exists(d + "/.locked")); /* no lock left: no file */
    CHECK_EQ(delete_scene(ops, d, "Lap 2_012_20261114-160500.mkv", why), 0);
    CHECK(!exists(d + "/Lap 2_012_20261114-160500.mkv"));
    CHECK_EQ(delete_scene(ops, d, "Trip_014_20261114-185200_p2.mkv", why), 0);
    CHECK(journal_read(ops, journal_path(d, "scene_014_20261114-185200"), jr) && jr.parts.size() == 1);
    CHECK_EQ(delete_scene(ops, d, "scene_014_20261114-185200.mkv", why), 0);
    CHECK(!exists(journal_path(d, "scene_014_20261114-185200"))); /* the last part: the journal goes */

    /* open journals are counted without FFmpeg */
    Journal o;
    o.file = "scene_020_nodate.mkv";
    o.parts = {o.file};
    CHECK_EQ(journal_write(ops, d, "scene_020_nodate", o), 0);
    CHECK_EQ(open_journal_count(ops, d), 1);
    rm_tree(d);
}

/* an MJPEG MKV of n frames made by the recorder (video only) */
bool make_mkv(const AvLibs &av, const std::vector<std::string> &jpegs, const std::string &folder, const std::string &stem, int n)
{
    RecorderConfig rc;
    rc.av = &av;
    rc.ops = &posix_file_ops();
    rc.folder = folder;
    rc.stem = stem;
    rc.video.w = 1280;
    rc.video.h = 720;
    rc.ring_seconds = 60;
    Recorder r(rc);
    r.start();
    for (int i = 0; i < n; ++i) {
        const std::string &f = jpegs[static_cast<std::size_t>(i) % jpegs.size()];
        r.push_video(reinterpret_cast<const std::uint8_t *>(f.data()), f.size(), 7000000 + i * 33333);
    }
    wait_for(5000, [&] { return r.status().frames == static_cast<std::uint64_t>(n); });
    r.request_stop(RecStop::User);
    r.join();
    return r.status().frames == static_cast<std::uint64_t>(n);
}

void probes_and_thumbs()
{
    const std::vector<std::string> jpegs = fixture_jpegs();
    std::string why;
    const AvLibs *av = real_av_libs(&why);
    if (jpegs.empty() || !av || !TurboJpeg::get().available()) {
        SKIP("probes and thumbnails: needs the testsrc fixture (ffmpeg), FFmpeg's libraries and libturbojpeg");
        return;
    }
    FileOps &ops = posix_file_ops();
    const std::string d = make_temp_dir("thm");
    CHECK(make_mkv(*av, jpegs, d, "scene_001_nodate", 60));
    write_file(d + "/photo_002_nodate.jpg", jpegs[0]);
    int w = 0, h = 0;
    CHECK(jpeg_size(reinterpret_cast<const std::uint8_t *>(jpegs[0].data()), jpegs[0].size(), w, h));
    CHECK(w == 1280 && h == 720);
    const std::vector<SceneEntry> v = list_scenes(ops, d);
    CHECK_EQ(v.size(), static_cast<std::size_t>(2));
    for (const SceneEntry &e0 : v) {
        SceneEntry e = e0;
        e.meta = e.kind == SceneKind::Photo ? probe_photo(ops, e.path) : probe_movie(*av, e.path);
        CHECK(e.meta.ok);
        CHECK(e.meta.w == 1280 && e.meta.h == 720);
        if (e.kind == SceneKind::Movie) {
            CHECK_EQ(e.meta.codec, std::string("MJPEG"));
            CHECK(std::llabs(e.meta.duration_ms - 2000) <= 40); /* 60 frames at 30 fps */
        }
        Picture p;
        std::string err;
        CHECK(make_thumb(ops, av, e, 64, 36, PixFmt::RGB565, p, &err));
        CHECK(p.w == 64 && p.h == 36); /* 16:9 into 64x36 */
        const std::string tp = thumb_path(d + "/cache", e);
        CHECK_EQ(thumb_save(ops, tp, p), 0);
        Picture back;
        CHECK(thumb_load(tp, 64, 36, PixFmt::RGB565, back));
        CHECK(back.px == p.px && back.w == 64);
        CHECK(!thumb_load(tp, 56, 32, PixFmt::RGB565, back)); /* made for another box */
        CHECK(!thumb_load(tp, 64, 36, PixFmt::XRGB8888, back));
        SceneEntry later = e;
        later.mtime += 5; /* the file changed: another cache name */
        CHECK(!thumb_load(thumb_path(d + "/cache", later), 64, 36, PixFmt::RGB565, back));
        thumb_remove(ops, d + "/cache", e.name);
        CHECK(!exists(tp));
    }
    rm_tree(d);
}

/* ------------------------------------------------------------------------------------------------ the controller */

ControllerConfig cfg(const std::string &folder)
{
    ControllerConfig c;
    c.fake = true;
    c.fake_opt.frames = 2;
    c.persist = false;
    c.log_every = 1000;
    c.ui_tid = current_tid();
    c.folder = folder;
    c.soc_override = "bcm2837";
    c.clock_synced = 0;
    c.utc = true;
    return c;
}

bool step_until(Controller &ctl, int ms, const std::function<bool(const Snapshot &)> &pred, Snapshot *last = nullptr)
{
    Snapshot s;
    const bool ok = wait_for(ms, [&] {
        s = ctl.step(mono_ms());
        return pred(s);
    });
    if (last) *last = s;
    return ok;
}

void start_indicator()
{
    std::string why;
    if (!real_av_libs(&why) || !TurboJpeg::get().can_encode()) {
        SKIP("the start indicator: needs FFmpeg's libraries and libturbojpeg");
        return;
    }
    const std::string d = make_temp_dir("sti");
    ControllerConfig c = cfg(d + "/VHS");
    c.fake_opt.start_delay_ms = 900; /* a board camera's slow start after the switch */
    Controller ctl(c);
    ctl.start();
    CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const std::int64_t t0 = mono_ms();
    ctl.record(t0);
    Snapshot s = ctl.step(mono_ms());
    CHECK(s.recording && s.rec_starting && !s.rec_started);
    CHECK(s.rec_since_ms == t0);
    CHECK(s.rec_ms == 0);
    /* a stop during the start: nothing was written, so nothing stays (no file, no journal, no scene, no toast) */
    ctl.record(mono_ms());
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return !x.recording; }, &s));
    CHECK_EQ(s.scenes_recorded, 0);
    CHECK(s.notice.text.empty());
    CHECK(!exists(d + "/VHS/scene_001_nodate.mkv"));
    CHECK(!exists(journal_path(d + "/VHS", "scene_001_nodate")));
    /* a full start: starting until the first frame, then solid */
    std::this_thread::sleep_for(std::chrono::milliseconds(1500)); /* back to the preview */
    ctl.record(mono_ms());
    CHECK(ctl.step(mono_ms()).rec_starting);
    CHECK(step_until(ctl, 5000, [](const Snapshot &x) { return x.rec_started; }, &s));
    CHECK(!s.rec_starting);
    ctl.record(mono_ms());
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return !x.recording; }, &s));
    CHECK_EQ(s.scenes_recorded, 1);
    ctl.stop();
    rm_tree(d);
}

void controller_scenes()
{
    const std::vector<std::string> jpegs = fixture_jpegs();
    std::string why;
    const AvLibs *av = real_av_libs(&why);
    if (jpegs.empty() || !av || !TurboJpeg::get().available()) {
        SKIP("controller scenes: needs the testsrc fixture, FFmpeg's libraries and libturbojpeg");
        return;
    }
    FileOps &ops = posix_file_ops();
    const std::string d = make_temp_dir("csc");
    const std::string f = d + "/VHS";
    make_dirs(f);
    CHECK(make_mkv(*av, jpegs, f, "scene_001_nodate", 30));
    touch(f + "/photo_002_nodate.jpg", jpegs[1], 1763145000);
    touch(f + "/photo_003_nodate.jpg", jpegs[2], 1763146000);
    /* a scene left open by a crash: cut in the middle, its journal open */
    CHECK(make_mkv(*av, jpegs, f, "scene_004_nodate", 150));
    {
        std::ifstream in(f + "/scene_004_nodate.mkv", std::ios::binary);
        const std::string whole((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        write_file(f + "/scene_004_nodate.mkv", whole.substr(0, whole.size() * 6 / 10));
        Journal j;
        j.file = "scene_004_nodate.mkv";
        j.parts = {j.file};
        CHECK_EQ(journal_write(ops, f, "scene_004_nodate", j), 0);
    }
    Controller ctl(cfg(f));
    /* the repair at start, on the worker; then the count */
    CHECK(step_until(ctl, 5000, [](const Snapshot &x) { return x.scene_count == 4; }));
    Journal j;
    CHECK(journal_read(ops, journal_path(f, "scene_004_nodate"), j));
    CHECK(j.state == "closed" && j.repaired);
    ctl.scenes_open(64, 36, false);
    CHECK(step_until(ctl, 5000, [&](const Snapshot &) {
        const ScenesView &v = ctl.scenes();
        if (v.loading || v.rows.size() != 4) return false;
        for (const SceneRow &r : v.rows)
            if (!r.thumb) return false;
        return true;
    }));
    const ScenesView &v = ctl.scenes();
    CHECK_EQ(v.movies, 2);
    CHECK_EQ(v.photos, 2);
    bool repaired_row = false;
    for (const SceneRow &r : v.rows)
        if (r.e.name == "scene_004_nodate.mkv") repaired_row = r.meta.find("repaired") != std::string::npos;
    CHECK(repaired_row);
    CHECK(exists(d + "/vhs-cache/index.json")); /* the fake's cache: next to its folder, never the user's */
    ctl.scenes_set_filter(SceneFilter::Photos);
    CHECK_EQ(ctl.scenes().rows.size(), static_cast<std::size_t>(2));
    CHECK_EQ(ctl.scenes().rows[0].e.name, std::string("photo_003_nodate.jpg"));
    ctl.scenes_set_filter(SceneFilter::Movies);
    CHECK_EQ(ctl.scenes().rows.size(), static_cast<std::size_t>(2));
    ctl.scenes_set_filter(SceneFilter::All);
    /* lock, then delete refused with the toast; unlock, delete */
    ctl.scene_toggle_lock("photo_002_nodate.jpg");
    CHECK(step_until(ctl, 3000, [&](const Snapshot &) {
        for (const SceneRow &r : ctl.scenes().rows)
            if (r.e.name == "photo_002_nodate.jpg") return r.e.locked;
        return false;
    }));
    ctl.scene_delete("photo_002_nodate.jpg");
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.notice.text == "Not deleted: the file is locked."; }));
    CHECK(exists(f + "/photo_002_nodate.jpg"));
    ctl.dismiss_notice(mono_ms());
    ctl.scene_toggle_lock("photo_002_nodate.jpg");
    ctl.scene_rename("scene_001_nodate.mkv", "garden");
    CHECK(step_until(ctl, 3000, [&](const Snapshot &) {
        for (const SceneRow &r : ctl.scenes().rows)
            if (r.e.name == "garden_001_nodate.mkv") return r.label == "garden" && r.thumb != nullptr;
        return false;
    }));
    ctl.scene_rename("garden_001_nodate.mkv", "bad/name");
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.notice.text == "Not renamed: use letters, digits, space, - and _."; }));
    ctl.dismiss_notice(mono_ms());
    ctl.scene_delete("photo_002_nodate.jpg");
    CHECK(step_until(ctl, 3000, [&](const Snapshot &x) { return ctl.scenes().rows.size() == 3 && x.scene_count == 3; }));
    CHECK(!exists(f + "/photo_002_nodate.jpg"));
    /* Play on a photo: the review at that photo */
    ctl.scenes_close();
    ctl.review_open(352, 198, "photo_003_nodate.jpg");
    CHECK(step_until(ctl, 3000, [&](const Snapshot &) { return ctl.review().count == 1 && !ctl.review().loading; }));
    CHECK_EQ(ctl.review().name, std::string("photo_003_nodate.jpg"));
    ctl.review_close();
    ctl.stop();
    rm_tree(d);
}

} // namespace

void test_scenes()
{
    indicator();
    layout();
    listing();
    probes_and_thumbs();
    start_indicator();
    controller_scenes();
}
