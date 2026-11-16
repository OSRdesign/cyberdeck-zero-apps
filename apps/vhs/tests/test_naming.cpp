/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: file names and the shared counter (brief 8): the grammar round trip, nodate, parts, the photo prefix,
 * foreign names; one counter for scenes and photos that never repeats a number after deletes or a lost counter (journals
 * count); the photo save (the name, the file, the folder's mode, no temp file left), the photo list newest first and the
 * review's meta line; the self-timer; the scenes worker.
 */
#include "check.hpp"

#include "camera/fake_camera.hpp"
#include "jpeg/turbojpeg.hpp"
#include "scenes/fileops.hpp"
#include "scenes/naming.hpp"
#include "scenes/photos.hpp"
#include "scenes/worker.hpp"
#include "timer/self_timer.hpp"

#include <sys/stat.h>
#include <utime.h>

#include <atomic>
#include <fstream>
#include <iterator>

using namespace vhs;

namespace {

void grammar()
{
    SceneName n;
    CHECK(parse_scene_name("scene_014_20261114-185200.mkv", n));
    CHECK_EQ(n.prefix, std::string("scene"));
    CHECK_EQ(n.number, 14);
    CHECK_EQ(n.when, std::string("20261114-185200"));
    CHECK_EQ(n.part, 1);
    CHECK(n.kind() == SceneKind::Movie);
    CHECK_EQ(format_scene_name(n), std::string("scene_014_20261114-185200.mkv"));
    CHECK(parse_scene_name("scene_014_20261114-185200_p2.mkv", n));
    CHECK_EQ(n.part, 2);
    CHECK_EQ(format_scene_name(n), std::string("scene_014_20261114-185200_p2.mkv"));
    CHECK(parse_scene_name("photo_013_20261114-184710.jpg", n));
    CHECK(n.kind() == SceneKind::Photo);
    CHECK_EQ(n.number, 13);
    CHECK(parse_scene_name("scene_015_nodate.mkv", n));
    CHECK_EQ(n.when, std::string("nodate"));
    /* a prefix with '_', '-' and spaces: parsed from the right */
    CHECK(parse_scene_name("Stopwatch test_a-b_1000_nodate.MKV", n));
    CHECK_EQ(n.prefix, std::string("Stopwatch test_a-b"));
    CHECK_EQ(n.number, 1000);
    CHECK_EQ(n.ext, std::string("mkv"));
    n.number = 7;
    n.prefix = "take";
    n.part = 1;
    CHECK_EQ(format_scene_name(n), std::string("take_007_nodate.mkv"));
    /* foreign names */
    for (const char *f : {"holiday.mkv", "scene_14_nodate.mkv", "scene_014_2026-11-14.mkv", "scene_014_nodate.txt",
                          "scene_014_nodate_p1.mkv", "take_003_nodate.jpg", ".scene_014_nodate.mkv",
                          "scene_014_20261114-1852.mkv", "_014_nodate.mkv", "photo_013_nodate_p2.jpg"})
        CHECK(!parse_scene_name(f, n));
    CHECK_EQ(when_text(0, false), std::string("nodate"));
    CHECK_EQ(when_text(1794682030, true, true), std::string("20261114-184710"));
    CHECK(!clock_synchronized("/nonexistent/timesync/synchronized"));
    CHECK_EQ(highest_number({"scene_014_nodate.mkv", "photo_013_nodate.jpg", "x.mkv", "scene_020_nodate.json"}), 20);
}

void counter()
{
    const std::string d = make_temp_dir("count");
    FileOps &ops = posix_file_ops();
    int n = 0;
    CHECK_EQ(next_scene_number(ops, d, n), 0);
    CHECK_EQ(n, 1);
    CHECK_EQ(next_scene_number(ops, d, n), 0);
    CHECK_EQ(n, 2);
    /* files with higher numbers (copied in, another app's run): past them */
    write_file(d + "/photo_013_20261114-184710.jpg", "x");
    CHECK_EQ(next_scene_number(ops, d, n), 0);
    CHECK_EQ(n, 14); /* scene_014 after photo_013: one counter */
    /* deleted files: the counter still remembers */
    std::remove((d + "/photo_013_20261114-184710.jpg").c_str());
    CHECK_EQ(next_scene_number(ops, d, n), 0);
    CHECK_EQ(n, 15);
    /* a lost counter: the highest number in the folder and the journals + 1 */
    std::remove((d + "/.counter").c_str());
    write_file(d + "/scene_009_nodate.mkv", "x");
    make_dirs(d + "/.journal");
    write_file(d + "/.journal/scene_021_nodate.json", "{}");
    CHECK_EQ(next_scene_number(ops, d, n), 0);
    CHECK_EQ(n, 22);
    /* a damaged counter counts as none */
    write_file(d + "/.counter", "garbage");
    CHECK_EQ(next_scene_number(ops, d, n), 0);
    CHECK_EQ(n, 22); /* the journal's 21 + 1 */
    /* a folder that cannot be written: no number handed out */
    int m = -5;
    CHECK(next_scene_number(ops, "/nonexistent-vhs/folder", m) < 0);
    CHECK_EQ(m, -5);
    rm_tree(d);
}

void save_and_list()
{
    if (!TurboJpeg::get().can_encode()) {
        SKIP("photo save: no libturbojpeg.so.0");
        return;
    }
    std::vector<std::uint8_t> card, jpeg;
    fake_test_card(640, 360, 0, 6, card);
    JpegEncoder enc;
    CHECK(enc.encode_bgrx(card.data(), 640, 360, 85, jpeg));
    const std::string base = make_temp_dir("photos");
    const std::string folder = base + "/VHS";
    FileOps &ops = posix_file_ops();
    ExifFields f;
    f.make = "Maker";
    f.model = "UGREEN FineCam 1080P CM969";
    f.software = kSoftware;
    /* synchronised: a dated name and EXIF dates */
    PhotoSave s = save_photo(ops, folder, jpeg.data(), jpeg.size(), f, 1794682030, true, true);
    CHECK(s.ok);
    CHECK_EQ(s.name, std::string("photo_001_20261114-184710.jpg"));
    struct stat st;
    CHECK(stat(folder.c_str(), &st) == 0);
    CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0700); /* the folder was made 0700 */
    std::ifstream in(s.path, std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_EQ(file.size(), s.bytes);
    ExifFields r;
    CHECK(exif_parse(file.data(), file.size(), r));
    CHECK_EQ(r.datetime, std::string("2026:11:14 18:47:10"));
    CHECK_EQ(r.datetime_original, std::string("2026:11:14 18:47:10"));
    CHECK_EQ(r.model, f.model);
    CHECK_EQ(r.width, 640u);
    /* not synchronised: nodate, no EXIF date */
    s = save_photo(ops, folder, jpeg.data(), jpeg.size(), f, 1794682031, false, true);
    CHECK(s.ok);
    CHECK_EQ(s.name, std::string("photo_002_nodate.jpg"));
    std::ifstream in2(s.path, std::ios::binary);
    const std::vector<std::uint8_t> file2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
    CHECK(exif_parse(file2.data(), file2.size(), r));
    CHECK(r.datetime.empty());
    /* no temp file left */
    std::vector<std::string> names;
    ops.list_dir(folder, names);
    for (const std::string &n : names) CHECK(n.find(".tmp") == std::string::npos);
    /* a broken frame: nothing written, no number handed out */
    const std::uint8_t junk[] = {1, 2, 3};
    s = save_photo(ops, folder, junk, sizeof junk, f, 0, false);
    CHECK(!s.ok);
    CHECK(!s.error.empty());
    /* the list: newest first (mtime, then number), foreign .jpg files too, the counter file never */
    write_file(folder + "/holiday.JPG", "not a photo");
    struct utimbuf old;
    old.actime = old.modtime = 1000;
    utime((folder + "/holiday.JPG").c_str(), &old);
    const std::vector<PhotoEntry> l = list_photos(ops, folder);
    CHECK_EQ(l.size(), static_cast<std::size_t>(3));
    if (l.size() == 3) {
        CHECK_EQ(l[0].name, std::string("photo_002_nodate.jpg"));
        CHECK_EQ(l[1].name, std::string("photo_001_20261114-184710.jpg"));
        CHECK_EQ(l[2].name, std::string("holiday.JPG"));
        CHECK_EQ(l[0].number, 2);
    }
    CHECK(list_photos(ops, base + "/none").empty());
    CHECK_EQ(photo_meta(1920, 1080, 100352, 1794595620, true), std::string("1920x1080, 98 KB, Nov 13 2026 18:47"));
    CHECK_EQ(photo_meta(1920, 1080, 3 * 1024 * 1024, 1793750400, true), std::string("1920x1080, 3.0 MB, Nov 4 2026 00:00"));
    CHECK_EQ(expand_home("/media/x"), std::string("/media/x"));
    CHECK(expand_home("~/VHS").find("/VHS") != std::string::npos);
    CHECK(expand_home("~/VHS")[0] != '~');
    rm_tree(base);
}

void self_timer()
{
    SelfTimer t;
    CHECK(!t.active());
    CHECK_EQ(t.shown(0), 0);
    CHECK(!t.fire(0));
    t.start(3, 1000);
    CHECK(t.active());
    CHECK_EQ(t.total(), 3);
    CHECK_EQ(t.shown(1000), 3);
    CHECK_EQ(t.shown(1999), 3);
    CHECK_EQ(t.shown(2001), 2);
    CHECK_EQ(t.shown(3999), 1);
    CHECK(!t.fire(3999));
    CHECK(t.fire(4000)); /* zero */
    CHECK(!t.active());
    CHECK(!t.fire(4100)); /* once */
    t.start(10, 0, TimerKind::Photo);
    t.cancel();
    CHECK(!t.active());
    CHECK(!t.fire(20000)); /* a cancel takes nothing */
    t.start(0, 0);
    CHECK(!t.active()); /* Off */
}

void worker()
{
    Worker w;
    w.start();
    std::atomic<int> ran{0};
    int done = 0;
    for (int i = 0; i < 5; ++i)
        w.post([&ran, &done]() -> std::function<void()> {
            ++ran;
            return [&done] { ++done; };
        });
    CHECK(wait_for(2000, [&] { return ran.load() == 5; }));
    CHECK(wait_for(2000, [&] { return w.pending() == 5; })); /* 5 completions waiting */
    CHECK_EQ(done, 0); /* completions run only in drain(), on the owner's thread */
    CHECK_EQ(w.drain(), 5);
    CHECK_EQ(done, 5);
    /* stop: the queued jobs still run */
    w.post([&ran]() -> std::function<void()> {
        ++ran;
        return {};
    });
    w.stop();
    CHECK_EQ(ran.load(), 6);
    w.stop(); /* twice: harmless */
}

} // namespace

void test_naming()
{
    grammar();
    counter();
    save_and_list();
    self_timer();
    worker();
}
