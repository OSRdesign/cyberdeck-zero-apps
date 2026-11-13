/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the queue (order, shuffle, repeat, editing), the folder browser (roots, listing, natural order, M3U,
 * breadcrumb) and the resume file.
 */
#include "check.hpp"

#include "library/browse.hpp"
#include "nav/nav_rules.hpp"
#include "queue/queue.hpp"
#include "state/resume.hpp"

#include <algorithm>
#include <set>
#include <sys/stat.h>

using namespace vinyl;

namespace {
std::vector<std::string> names(int n) {
    std::vector<std::string> v;
    for (int i = 0; i < n; ++i) v.push_back("/m/t" + std::to_string(i) + ".mp3");
    return v;
}
}  // namespace

void test_queue() {
    /* design QA run 4: the modal takes Esc and the header's Esc until it is closed (D-30); the queue buttons that cannot
     * act are dimmed and do nothing (D-81) */
    CHECK(back_step(true, false) == BackStep::CloseModal);
    CHECK(back_step(true, true) == BackStep::CloseModal); /* a notice behind the dialog stays until the dialog is gone */
    CHECK(back_step(false, true) == BackStep::HideNoticeAndBack);
    CHECK(back_step(false, false) == BackStep::Back);
    CHECK_EQ(queue_buttons_enabled(0), 0u);
    CHECK_EQ(queue_buttons_enabled(1), kQueueRemove); /* only the playing track: Remove only */
    CHECK_EQ(queue_buttons_enabled(2), kQueueUp | kQueueDown | kQueueRemove | kQueueClear);
    CHECK_EQ(queue_buttons_enabled(20), kQueueUp | kQueueDown | kQueueRemove | kQueueClear);
    /* design QA run 5: the toolkit's dialog unrestyled: Enter and Esc cancel, Y clears, nothing else does anything */
    CHECK(modal_answer(kLvKeyEnter, 0) == ModalAnswer::Cancel);
    CHECK(modal_answer(kLvKeyEsc, 0) == ModalAnswer::Cancel);
    CHECK(modal_answer(0, kEvdevKeyY) == ModalAnswer::Confirm);
    for (uint32_t k : {17u, 18u, 19u, 20u, 9u, 11u, 99u}) CHECK(modal_answer(k, 0) == ModalAnswer::None); /* arrows, Tab, letters */
    CHECK(modal_answer(0, 46) == ModalAnswer::None); /* C again does not clear */
    Queue q;
    CHECK(q.empty());
    CHECK_EQ(q.next_pos(true), -1);
    q.set(names(5), 2, "/m");
    CHECK_EQ(q.size(), 5);
    CHECK_EQ(q.current(), 2);
    CHECK_EQ(q.at(2), std::string("/m/t2.mp3"));
    CHECK_EQ(q.next_pos(false), 3);
    CHECK_EQ(q.prev_pos(), 1);
    /* the end: repeat off stops, all wraps, one replays at a track end and moves on for the Next key */
    q.set_current(4);
    CHECK_EQ(q.next_pos(false), -1);
    q.set_repeat(Repeat::All);
    CHECK_EQ(q.next_pos(false), 0);
    q.set_repeat(Repeat::One);
    CHECK_EQ(q.next_pos(false), 4);
    CHECK_EQ(q.next_pos(true), 0);
    q.set_current(1);
    CHECK_EQ(q.next_pos(true), 2);
    CHECK(q.cycle_repeat() == Repeat::Off);
    CHECK(q.cycle_repeat() == Repeat::All);
    CHECK(q.cycle_repeat() == Repeat::One);
    q.set_repeat(Repeat::Off);
    q.set_current(0);
    CHECK_EQ(q.prev_pos(), -1);
    q.set_repeat(Repeat::All);
    CHECK_EQ(q.prev_pos(), 4);
    q.set_repeat(Repeat::Off);
    CHECK_EQ(q.next_after(3, false), 4);
    CHECK_EQ(q.next_after(4, false), -1);

    /* shuffle keeps the current track first and visits every track once */
    q.set(names(30), 7, "/m");
    q.seed(42);
    q.set_shuffle(true);
    CHECK(q.shuffle());
    CHECK_EQ(q.current(), 0);
    CHECK_EQ(q.at(0), std::string("/m/t7.mp3"));
    std::set<std::string> seen;
    for (int i = 0; i < q.size(); ++i) seen.insert(q.at(i));
    CHECK_EQ(static_cast<int>(seen.size()), 30);
    bool moved = false;
    for (int i = 1; i < q.size(); ++i) moved = moved || q.at(i) != "/m/t" + std::to_string(i) + ".mp3";
    CHECK(moved);
    /* shuffle off: back to the list order, the current track kept */
    q.set_current(5);
    const std::string cur = q.at(5);
    q.set_shuffle(false);
    CHECK_EQ(*q.current_path(), cur);
    CHECK_EQ(q.at(0), std::string("/m/t0.mp3"));

    /* editing: move up / down follow the current position */
    q.set(names(4), 1, "/m");
    CHECK(q.move_up(1));
    CHECK_EQ(q.current(), 0);
    CHECK_EQ(q.at(0), std::string("/m/t1.mp3"));
    CHECK(!q.move_up(0));
    CHECK(q.move_down(0));
    CHECK_EQ(q.current(), 1);
    CHECK(!q.move_down(3));
    /* remove before, at and after the current track */
    q.set(names(5), 2, "/m");
    q.remove(0);
    CHECK_EQ(q.current(), 1);
    CHECK_EQ(*q.current_path(), std::string("/m/t2.mp3"));
    q.remove(1); /* the current track: the next one takes its place */
    CHECK_EQ(q.current(), 1);
    CHECK_EQ(*q.current_path(), std::string("/m/t3.mp3"));
    q.remove(2);
    CHECK_EQ(q.size(), 2);
    q.set_current(1);
    q.remove(1); /* the last, current: none */
    CHECK_EQ(q.current(), -1);
    /* editing with shuffle on changes only the order */
    q.set(names(6), 0, "/m");
    q.set_shuffle(true);
    const std::string a = q.at(1), b = q.at(2);
    CHECK(q.move_up(2));
    CHECK_EQ(q.at(1), b);
    CHECK_EQ(q.at(2), a);
    q.remove(3);
    CHECK_EQ(q.size(), 5);
    for (int o : q.order()) CHECK(o >= 0 && o < 5);
    q.set_shuffle(false);
    /* clear all keeps the current track */
    q.set(names(6), 3, "/m");
    q.clear_except_current();
    CHECK_EQ(q.size(), 1);
    CHECK_EQ(q.current(), 0);
    CHECK_EQ(q.at(0), std::string("/m/t3.mp3"));
    q.set(names(3), -1, "/m");
    q.clear_except_current();
    CHECK(q.empty());
    /* restore: a bad order becomes the linear one */
    q.restore(names(3), {0, 0, 1}, 1, false, Repeat::All, "/m");
    CHECK_EQ(q.order().size(), static_cast<size_t>(3));
    CHECK_EQ(q.order()[2], 2);
    CHECK(q.repeat() == Repeat::All);
    q.restore(names(3), {2, 0, 1}, 9, true, Repeat::Off, "/m");
    CHECK_EQ(q.at(0), std::string("/m/t2.mp3"));
    CHECK_EQ(q.current(), -1);
}

void test_browse() {
    /* natural order */
    CHECK(natural_less("2 Song", "10 Song"));
    CHECK(!natural_less("10 Song", "2 Song"));
    CHECK(natural_less("abc", "ABD"));
    CHECK(natural_less("Track 02", "track 3"));
    CHECK(natural_less("a", "ab"));

    /* roots from /proc/mounts */
    const std::string mounts = "/dev/root / ext4 rw 0 0\n"
                               "proc /proc proc rw 0 0\n"
                               "/dev/sda1 /media/pi/USB\\040STICK vfat rw 0 0\n"
                               "tmpfs /run/user/1000 tmpfs rw 0 0\n"
                               "/dev/sdb1 /mnt/music exfat rw 0 0\n"
                               "/dev/sdc1 /media/pi/USB\\040STICK vfat rw 0 0\n";
    const std::vector<Root> roots = list_roots("/home/pi", "pi", mounts);
    CHECK_EQ(roots.size(), static_cast<size_t>(3));
    CHECK_EQ(roots[0].label, std::string("Music"));
    CHECK_EQ(roots[0].path, std::string("/home/pi/Music"));
    CHECK_EQ(roots[1].label, std::string("USB STICK"));
    CHECK_EQ(roots[1].path, std::string("/media/pi/USB STICK"));
    CHECK_EQ(roots[2].path, std::string("/mnt/music"));
    CHECK_EQ(breadcrumb(roots, "/home/pi/Music"), std::string("MUSIC"));
    CHECK_EQ(breadcrumb(roots, "/home/pi/Music/Phoenix"), std::string("MUSIC / PHOENIX"));
    CHECK_EQ(breadcrumb(roots, "/home/pi/Music/a/b/c/d"), std::string("MUSIC / ... / C / D"));
    CHECK_EQ(breadcrumb(roots, "/media/pi/USB STICK/Jazz"), std::string("USB STICK / JAZZ"));
    CHECK_EQ(parent_folder(roots, "/home/pi/Music/Phoenix"), std::string("/home/pi/Music"));
    CHECK_EQ(parent_folder(roots, "/home/pi/Music"), std::string(""));
    CHECK(root_of(roots, "/home/pi/Musical") == nullptr);

    /* a folder listing */
    const std::string dir = make_temp_dir("browse");
    CHECK(!dir.empty());
    ::mkdir((dir + "/Album 10").c_str(), 0755);
    ::mkdir((dir + "/Album 2").c_str(), 0755);
    ::mkdir((dir + "/.hidden").c_str(), 0755);
    for (const char *f : {"10 Last.flac", "2 Second.mp3", "1 First.MP3", "cover.jpg", "notes.txt", "mix.m3u", ".secret.mp3", "x.m4a"})
        write_file(dir + "/" + f, "x");
    std::vector<Entry> e;
    CHECK_EQ(list_folder(dir, false, e), 0);
    CHECK_EQ(e.size(), static_cast<size_t>(8));
    CHECK(e[0].kind == Entry::Kind::Up);
    CHECK(e[1].kind == Entry::Kind::Folder);
    CHECK_EQ(e[1].name, std::string("Album 2"));
    CHECK_EQ(e[2].name, std::string("Album 10"));
    CHECK(e[3].kind == Entry::Kind::Playlist);
    CHECK_EQ(e[4].name, std::string("1 First.MP3"));
    CHECK_EQ(e[5].name, std::string("2 Second.mp3"));
    CHECK_EQ(e[6].name, std::string("10 Last.flac"));
    CHECK_EQ(e[7].name, std::string("x.m4a"));
    CHECK_EQ(folder_tracks(e).size(), static_cast<size_t>(4));
    CHECK_EQ(list_folder(dir, true, e), 0);
    CHECK(e[0].kind == Entry::Kind::Folder);
    CHECK(list_folder(dir + "/missing", false, e) < 0);

    /* M3U: relative and absolute, comments, BOM, CRLF, Windows separators, missing files dropped */
    const std::string m3u = "\xEF\xBB\xBF#EXTM3U\r\n#EXTINF:123,Artist - First\r\n1 First.MP3\r\n  2 Second.mp3  \r\n"
                            "missing.mp3\r\nhttp://radio.example/stream\r\nAlbum 2\\..\\10 Last.flac\r\n" + dir + "/x.m4a\n";
    const std::vector<std::string> items = parse_m3u(m3u, dir, file_exists);
    CHECK_EQ(items.size(), static_cast<size_t>(4));
    if (items.size() == 4) {
        CHECK_EQ(items[0], dir + "/1 First.MP3");
        CHECK_EQ(items[1], dir + "/2 Second.mp3");
        CHECK_EQ(items[2], dir + "/Album 2/../10 Last.flac");
        CHECK_EQ(items[3], dir + "/x.m4a");
    }
    CHECK_EQ(parse_m3u("a.mp3\nb.mp3\n", "/x").size(), static_cast<size_t>(2));
    rm_tree(dir);
}

void test_resume() {
    ResumeState s;
    s.items = {"/m/a.mp3", "/m/b \"quoted\".flac", "/m/日本.opus"};
    s.order = {2, 0, 1};
    s.current = 1;
    s.shuffle = true;
    s.repeat = Repeat::One;
    s.source = "/m/list.m3u";
    s.position_ms = 83456;
    s.output = "bt:70:BF:92:53:6B:8B";
    s.browse = "/m";
    s.app_volume = 72;
    const std::string json = resume_to_json(s);
    ResumeState r;
    CHECK(resume_from_json(json, r));
    CHECK_EQ(r.items.size(), static_cast<size_t>(3));
    CHECK_EQ(r.items[1], s.items[1]);
    CHECK_EQ(r.items[2], s.items[2]);
    CHECK_EQ(r.order.size(), static_cast<size_t>(3));
    CHECK_EQ(r.current, 1);
    CHECK(r.shuffle);
    CHECK(r.repeat == Repeat::One);
    CHECK_EQ(r.position_ms, static_cast<uint64_t>(83456));
    CHECK_EQ(r.output, s.output);
    CHECK_EQ(r.app_volume, 72);
    /* damaged files mean a fresh start */
    CHECK(!resume_from_json("{not json", r));
    CHECK(r.items.empty());
    CHECK(!resume_from_json("{\"v\": 2}", r));
    CHECK(resume_from_json("{\"v\": 1, \"queue\": {\"items\": [\"/a\"], \"current\": 5}, \"app_volume\": 400}", r));
    CHECK_EQ(r.current, -1);
    CHECK_EQ(r.app_volume, 100);
    /* the file: written atomically, read back; a missing one is a fresh start */
    const std::string dir = make_temp_dir("resume");
    const std::string path = dir + "/sub/state.json";
    CHECK_EQ(save_resume(path, s), 0);
    CHECK(load_resume(path, r));
    CHECK_EQ(r.items[0], std::string("/m/a.mp3"));
    CHECK(!load_resume(dir + "/none.json", r));
    /* the queue round trip */
    Queue q;
    queue_from_resume(s, q);
    CHECK_EQ(q.size(), 3);
    CHECK_EQ(q.at(0), std::string("/m/日本.opus"));
    ResumeState back;
    resume_from_queue(q, back);
    CHECK_EQ(back.current, 1);
    CHECK(back.shuffle);
    rm_tree(dir);
}
