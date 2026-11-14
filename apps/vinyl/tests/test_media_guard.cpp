/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: a drive that goes away takes its files out of the queue (owner ruling 2026-11-14, B50): the drive a path lives
 * on, the drives that are gone (a fake `present`), Queue::remove_under with the playing track on the gone drive, only some
 * entries on it, all of them, a shuffled queue, and a saved queue at start that refers to a drive that is not there.
 */
#include "check.hpp"

#include "library/media_guard.hpp"
#include "queue/queue.hpp"
#include "state/resume.hpp"

#include <set>

using namespace vinyl;

void test_media_guard() {
    const std::vector<DriveBase> bases = default_drive_bases();
    CHECK_EQ(drive_dir_of("/media/u/SANDISK/Album/1.mp3", bases), std::string("/media/u/SANDISK"));
    CHECK_EQ(drive_dir_of("/run/media/u/STICK/1.mp3", bases), std::string("/run/media/u/STICK"));
    CHECK_EQ(drive_dir_of("/mnt/stick/a/b.mp3", bases), std::string("/mnt/stick"));
    CHECK_EQ(drive_dir_of("/mnt/stick", bases), std::string("/mnt/stick"));
    CHECK_EQ(drive_dir_of("/home/o/Music/a.mp3", bases), std::string(""));
    CHECK_EQ(drive_dir_of("/media/u", bases), std::string(""));
    CHECK_EQ(drive_dir_of("/media/", bases), std::string(""));
    CHECK_EQ(drive_dir_of("/tmp/x/media/a/b.mp3", {{"/tmp/x/media/", 1}}), std::string("/tmp/x/media/a"));

    /* the gone drives: each once; a directory that holds music all the same is not one */
    const std::set<std::string> here = {"/media/u/A"};
    auto present = [&](const std::string &d) { return here.count(d) != 0; };
    std::vector<std::string> paths = {"/media/u/A/1.mp3", "/media/u/B/1.mp3", "/media/u/B/2.mp3", "/home/o/Music/x.mp3", "/mnt/gone/1.mp3", "/mnt/music/1.mp3"};
    std::vector<std::string> gone = gone_drive_dirs(paths, present, bases);
    CHECK_EQ(gone.size(), static_cast<size_t>(3));
    CHECK_EQ(gone[0], std::string("/media/u/B"));
    CHECK_EQ(gone[1], std::string("/mnt/gone"));
    CHECK_EQ(gone[2], std::string("/mnt/music"));
    gone = gone_drive_dirs(paths, present, bases, [](const std::string &p) { return p == "/mnt/music/1.mp3"; });
    CHECK_EQ(gone.size(), static_cast<size_t>(2)); /* /mnt/music is a folder of the system disk: its file is there */
    CHECK(gone_drive_dirs({"/home/o/a.mp3"}, present, bases).empty());

    /* Queue::remove_under: the playing track is on the gone drive, the next one that is left takes its place */
    const std::vector<std::string> q = {"/media/u/A/1.mp3", "/media/u/A/2.mp3", "/media/u/B/1.mp3", "/media/u/B/2.mp3", "/media/u/A/3.mp3", "/home/o/Music/h.mp3"};
    Queue a;
    a.set(q, 1, "x");
    Queue::RemoveResult r = a.remove_under({"/media/u/A"});
    CHECK_EQ(r.removed, 3);
    CHECK(r.current_removed);
    CHECK_EQ(a.size(), 3);
    CHECK(a.current_path() && *a.current_path() == "/media/u/B/1.mp3");
    /* only the other drive's entries: the playing track stays current */
    a.set(q, 1, "x");
    r = a.remove_under({"/media/u/B"});
    CHECK_EQ(r.removed, 2);
    CHECK(!r.current_removed);
    CHECK_EQ(a.size(), 4);
    CHECK(a.current_path() && *a.current_path() == "/media/u/A/2.mp3");
    CHECK_EQ(a.current(), 1);
    /* a track before the current one goes: the current keeps its track, its place moves */
    a.set(q, 5, "x");
    r = a.remove_under({"/media/u/A", "/media/u/B"});
    CHECK_EQ(r.removed, 5);
    CHECK(!r.current_removed);
    CHECK_EQ(a.size(), 1);
    CHECK_EQ(a.current(), 0);
    /* every entry on it: the queue is empty */
    a.set({"/media/u/A/1.mp3", "/media/u/A/2.mp3"}, 0, "x");
    r = a.remove_under({"/media/u/A"});
    CHECK_EQ(r.removed, 2);
    CHECK(r.current_removed);
    CHECK(a.empty());
    CHECK_EQ(a.current(), -1);
    CHECK(a.current_path() == nullptr);
    /* the playing one is last and the survivors are before it: the first survivor is current */
    a.set({"/home/o/h.mp3", "/media/u/A/1.mp3"}, 1, "x");
    r = a.remove_under({"/media/u/A"});
    CHECK(r.current_removed);
    CHECK_EQ(a.current(), 0);
    CHECK(a.current_path() && *a.current_path() == "/home/o/h.mp3");
    /* nothing under the drive, and a name that only starts like it: untouched */
    a.set({"/media/u/AB/1.mp3", "/media/u/A"}, 0, "x");
    r = a.remove_under({"/media/u/A"});
    CHECK_EQ(r.removed, 0);
    CHECK_EQ(a.size(), 2);
    CHECK_EQ(a.remove_under({}).removed, 0);
    /* shuffled: items and the play order stay a permutation, the playing track stays current when it survives */
    Queue s;
    s.set(q, 5, "x");
    s.set_shuffle(true);
    const std::string playing = *s.current_path();
    r = s.remove_under({"/media/u/A"});
    CHECK_EQ(r.removed, 3);
    CHECK_EQ(s.size(), 3);
    CHECK(!r.current_removed);
    CHECK(s.current_path() && *s.current_path() == playing);
    CHECK_EQ(s.items().size(), static_cast<size_t>(3));
    CHECK_EQ(s.order().size(), static_cast<size_t>(3));
    int seen = 0;
    for (int i = 0; i < s.size(); ++i) seen += s.at(i).compare(0, 11, "/media/u/A/") != 0;
    CHECK_EQ(seen, 3);

    /* a start: the saved queue refers to a drive that is not there: its entries go before the first screen; when all of them
     * were on it the queue is empty (the app opens on the Library hub), when some were not, the first of those is current */
    ResumeState st;
    st.items = {"/media/u/GONE/1.mp3", "/media/u/GONE/2.mp3"};
    st.order = {0, 1};
    st.current = 0;
    st.position_ms = 83000;
    Queue rq;
    queue_from_resume(st, rq);
    r = rq.remove_under(gone_drive_dirs(rq.items(), [](const std::string &) { return false; }, bases));
    CHECK_EQ(r.removed, 2);
    CHECK(rq.empty());
    CHECK_EQ(rq.current(), -1);
    st.items = {"/media/u/GONE/1.mp3", "/home/o/Music/a.mp3", "/media/u/GONE/2.mp3", "/home/o/Music/b.mp3"};
    st.order = {0, 1, 2, 3};
    st.current = 0;
    queue_from_resume(st, rq);
    r = rq.remove_under(gone_drive_dirs(rq.items(), [](const std::string &) { return false; }, bases));
    CHECK_EQ(r.removed, 2);
    CHECK(r.current_removed);
    CHECK(rq.current_path() && *rq.current_path() == "/home/o/Music/a.mp3");
    /* a drive that comes back later does not bring them back: the queue is what is left */
    CHECK_EQ(rq.size(), 2);
    CHECK(gone_drive_dirs(rq.items(), [](const std::string &) { return true; }, bases).empty());
}
