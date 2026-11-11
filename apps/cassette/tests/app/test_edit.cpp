/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: the trim page through the controller: peak windows, marker, IN/OUT/splits, zoom, preview, save to new
 * files and in place, the app closing during a save, and the in-memory peak fallback. Sources are synthetic ramps.
 */
#include "helpers.hpp"
#include "atomic_file.hpp"

#include <algorithm>

using namespace ta;

namespace {

const char *kTake = "take_001_20261108-1105.wav";

bool wait_peaks(Rig &r) {
    return r.wait([](const Snapshot &x) { return x.trim.open && x.trim.peaks_state == 2; }, 8000);
}
bool wait_save(Rig &r, int state) {
    return r.wait([state](const Snapshot &x) { return x.trim.save_state == state; }, 8000);
}

/* the visible files; hidden entries (.journal, .edits: the numbers a cancelled save reserved stay reserved) are left out */
std::vector<std::string> visible(std::vector<std::string> v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](const std::string &n) { return !n.empty() && n[0] == '.'; }), v.end());
    return v;
}

bool same_files(std::vector<std::string> a, std::vector<std::string> b) {
    a = visible(a);
    b = visible(b);
    if (a == b) return true;
    for (const std::string &n : a) std::fprintf(stderr, "  now: %s\n", n.c_str());
    for (const std::string &n : b) std::fprintf(stderr, "  was: %s\n", n.c_str());
    return false;
}

std::vector<std::string> without_peaks(std::vector<std::string> v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](const std::string &n) { return n.size() > 6 && n.compare(n.size() - 6, 6, ".peaks") == 0; }),
            v.end());
    return v;
}

void t_trim_state() {
    Rig r(one_card());
    make_take(r.rec(), kTake, 10.0);
    r.open();
    Controller &c = r.ctl();
    CHECK(!c.set_in()); /* no page open */
    CHECK(!c.save(SaveKind::NewFiles));
    CHECK(c.open_trim(kTake));
    Snapshot s = r.snap();
    CHECK(s.trim.open);
    CHECK_EQ(s.trim.frames, 480000u);
    CHECK_EQ(s.trim.in_frame, 0u);
    CHECK_EQ(s.trim.out_frame, 480000u);
    CHECK_EQ(s.trim.rate, 48000u);
    CHECK(wait_peaks(r));
    CHECK(!r.snap().trim.peaks_in_memory);
    CHECK_EQ(r.snap().trim.peaks_progress, 1.f);

    /* the waveform: 100 bars over the whole take, a rising ramp */
    std::vector<PeakBar> bars = c.peak_window(100);
    CHECK_EQ(bars.size(), 100u);
    if (bars.size() == 100) {
        CHECK(bars[0].min < -29900 && bars[0].min > -30100);
        CHECK(bars[0].max < -21000 && bars[0].max > -25400); /* the bar spans whole blocks of the pyramid */
        for (const PeakBar &b : bars) CHECK(b.min <= b.max);
    }

    /* the trim page reads its bars on the take's grid (binder: ui::bar_grid): a read starting on a multiple of the bar
     * size gives every grid bar the same pair as any other such read, and bars before frame 0 are empty */
    {
        const uint64_t fpb = 4801; /* not a multiple of the pyramid's blocks */
        const std::vector<PeakBar> a = c.peak_range(-3 * static_cast<int64_t>(fpb), 12 * fpb, 12);
        const std::vector<PeakBar> b = c.peak_range(0, 9 * fpb, 9);
        const std::vector<PeakBar> e = c.peak_range(5 * static_cast<int64_t>(fpb), 7 * fpb, 7);
        CHECK(a.size() == 12 && b.size() == 9 && e.size() == 7);
        if (a.size() == 12 && b.size() == 9 && e.size() == 7) {
            for (int i = 0; i < 3; ++i) CHECK(a[static_cast<size_t>(i)].min == 0 && a[static_cast<size_t>(i)].max == 0);
            int bad = 0;
            for (size_t k = 0; k < 9; ++k)
                if (a[k + 3].min != b[k].min || a[k + 3].max != b[k].max) ++bad;
            for (size_t k = 5; k < 9; ++k)
                if (e[k - 5].min != b[k].min || e[k - 5].max != b[k].max) ++bad;
            CHECK_EQ(bad, 0);
        }
    }

    /* zoom: whole take, then closer; the window follows the marker */
    CHECK(c.set_zoom(3));
    s = r.snap();
    CHECK_EQ(s.trim.visible_frames, 60000u);
    CHECK_EQ(s.trim.window_start, 0u);
    CHECK(c.set_position(300000));
    s = r.snap();
    CHECK(s.trim.window_start > 200000 && s.trim.window_start + s.trim.visible_frames <= 480000);
    CHECK(s.trim.position >= s.trim.window_start && s.trim.position <= s.trim.window_start + s.trim.visible_frames);
    CHECK(c.set_zoom(99));
    s = r.snap();
    CHECK_EQ(s.trim.zoom, s.trim.max_zoom);
    CHECK_EQ(s.trim.max_zoom, 6);
    CHECK_EQ(s.trim.visible_frames, 7500u);
    CHECK_EQ(c.peak_window(50).size(), 50u);
    CHECK(c.set_zoom(0));
    CHECK_EQ(r.snap().trim.visible_frames, 480000u);
    CHECK_EQ(r.snap().trim.window_start, 0u);

    /* IN, OUT, splits: only the marker moves */
    CHECK(c.set_position(48000));
    CHECK(c.set_in());
    CHECK(c.set_position(0));
    CHECK(c.set_position(432000));
    CHECK(c.set_out());
    CHECK(!c.set_in()); /* the marker sits on OUT: IN must stay before it */
    CHECK_EQ(r.snap().trim.in_frame, 48000u);
    CHECK_EQ(r.snap().trim.out_frame, 432000u);
    CHECK(c.set_position(144000));
    CHECK(c.add_split());
    CHECK(!c.add_split()); /* already there */
    CHECK(c.set_position(288000));
    CHECK(c.add_split());
    CHECK(c.set_position(48000));
    CHECK(!c.add_split()); /* not inside (IN, OUT) */
    CHECK(c.set_position(432000));
    CHECK(!c.add_split());
    s = r.snap();
    CHECK(s.trim.splits == (std::vector<uint64_t>{144000, 288000}));
    CHECK_EQ(s.trim.kept_seconds, 8.0);
    CHECK(c.select_split(0));
    CHECK_EQ(r.snap().trim.position, 144000u);
    CHECK_EQ(r.snap().trim.selected_split, 0);
    CHECK(!c.select_split(2));
    CHECK(c.remove_split(1));
    CHECK(r.snap().trim.splits == (std::vector<uint64_t>{144000}));
    CHECK(!c.remove_split(1));
    CHECK(c.set_position(288000));
    CHECK(c.add_split());
    CHECK(c.move_position(-1000));
    CHECK_EQ(r.snap().trim.position, 287000u);
    CHECK(c.move_position(1000));
    CHECK(c.move_position(10000000));
    CHECK_EQ(r.snap().trim.position, 480000u);
    CHECK(c.move_position(-10000000));
    CHECK_EQ(r.snap().trim.position, 0u);
    /* moving IN past a split drops it */
    CHECK(c.set_position(200000));
    CHECK(c.set_in());
    CHECK(r.snap().trim.splits == (std::vector<uint64_t>{288000}));
    CHECK(c.set_position(48000));
    CHECK(c.set_in());
    CHECK(c.set_position(144000));
    CHECK(c.add_split());

    /* preview plays the kept part from the marker and stops at OUT */
    r.sink.sleep_us = 5000;
    r.sink.clear_heard();
    CHECK(c.set_position(288000));
    CHECK(c.preview(true));
    CHECK(r.snap().trim.previewing);
    CHECK(r.wait([](const Snapshot &x) { return !x.trim.previewing; }, 6000));
    CHECK_EQ(r.snap().trim.position, 432000u);
    CHECK(r.sink.heard_frames() >= 144000 && r.sink.heard_frames() < 144000 + 4096);
    const std::vector<int32_t> hv = r.sink.heard();
    CHECK(!hv.empty() && hv[0] == tp::ramp(288000));
    CHECK(c.preview(false));
    /* the deck is not available to the page while a key is busy; recording is not offered while previewing */
    CHECK(c.preview(true));
    c.key_event(KeyEvent::Stop);
    CHECK(c.preview(false));
    c.close_trim();
    CHECK(!r.snap().trim.open);
}

void t_save_new_and_replace() {
    Rig r(one_card());
    make_take(r.rec(), kTake, 10.0);
    r.open();
    Controller &c = r.ctl();
    const std::string orig = r.rec() + "/" + kTake;
    const std::string before = slurp(orig);
    CHECK(c.open_trim(kTake));
    CHECK(wait_peaks(r));
    c.set_position(48000);
    c.set_in();
    c.set_position(432000);
    c.set_out();
    c.set_position(144000);
    c.add_split();
    c.set_position(288000);
    c.add_split();
    CHECK(c.save(SaveKind::NewFiles));
    CHECK(wait_save(r, 2));
    Snapshot s = r.snap();
    CHECK(s.trim.save_outputs == (std::vector<std::string>{"take_001_20261108-1105-1.wav", "take_001_20261108-1105-2.wav",
                                                          "take_001_20261108-1105-3.wav"}));
    CHECK(!s.trim.save_replaced);
    CHECK_EQ(slurp(orig), before); /* the original is untouched */
    const uint64_t want[3] = {96000, 144000, 144000};
    const uint64_t first[3] = {48000, 144000, 288000};
    for (int i = 0; i < 3; ++i) {
        WavData w = read_wav(r.rec() + "/" + s.trim.save_outputs[static_cast<size_t>(i)]);
        CHECK(w.ok && w.consistent);
        CHECK_EQ(w.frames(), want[i]);
        if (w.frames() == want[i] && w.frames() > 0) {
            CHECK_EQ(w.s.front(), tp::ramp(first[i]));
            CHECK_EQ(w.s.back(), tp::ramp(first[i] + want[i] - 1));
        }
    }
    CHECK_EQ(c.takes().size(), 4u);
    CHECK_EQ(c.takes()[0].label, "001"); /* the original first, then -1, -2, -3 */
    c.close_trim();

    /* replace the original with a trimmed copy (one part: the name stays) */
    const std::vector<std::string> names_before = without_peaks(r.files());
    CHECK(c.open_trim(kTake));
    CHECK(wait_peaks(r));
    CHECK(!r.snap().trim.peaks_in_memory);
    c.set_position(24000);
    c.set_in();
    c.set_position(456000);
    c.set_out();
    CHECK(c.save(SaveKind::ReplaceOriginal));
    CHECK(wait_save(r, 2));
    CHECK(r.snap().trim.save_replaced);
    WavData w = read_wav(orig);
    CHECK(w.ok && w.consistent);
    CHECK_EQ(w.frames(), 432000u);
    if (!w.s.empty()) CHECK_EQ(w.s.front(), tp::ramp(24000));
    CHECK(without_peaks(r.files()) == names_before);
    c.close_trim();
    /* the peaks of the trimmed take are rebuilt, not taken from the stale cache */
    CHECK(c.open_trim(kTake));
    CHECK(wait_peaks(r));
    CHECK_EQ(r.snap().trim.frames, 432000u);
    c.close_trim();

    /* a split with ReplaceOriginal: the original goes, the parts come */
    make_take(r.rec(), "take_002_20261108-1106.wav", 6.0);
    c.refresh_takes();
    CHECK(c.open_trim("take_002_20261108-1106.wav"));
    CHECK(wait_peaks(r));
    c.set_position(96000);
    c.add_split();
    c.set_position(192000);
    c.add_split();
    CHECK(c.save(SaveKind::ReplaceOriginal));
    CHECK(wait_save(r, 2));
    CHECK_EQ(r.snap().trim.save_outputs.size(), 3u);
    CHECK(!path_exists(posix_file_ops(), r.rec() + "/take_002_20261108-1106.wav"));
    CHECK(path_exists(posix_file_ops(), r.rec() + "/take_002_20261108-1106-1.wav"));
    CHECK(path_exists(posix_file_ops(), r.rec() + "/take_002_20261108-1106-3.wav"));
    c.close_trim();
}

void t_leave_during_save() {
    Rig r(one_card());
    make_take(r.rec(), kTake, 120.0);
    r.cfg.save_block_bytes = 16384;
    r.open();
    Controller &c = r.ctl();
    const std::string orig = r.rec() + "/" + kTake;
    const std::string before = slurp(orig);
    CHECK(c.open_trim(kTake));
    CHECK(wait_peaks(r));
    const std::vector<std::string> names_before = r.files();
    c.set_position(48000);
    c.set_in();
    c.set_position(48000 * 100);
    c.set_out();
    c.set_position(48000 * 50);
    c.add_split();
    r.ops.tmp_delay_us.store(1500);
    CHECK(c.save(SaveKind::NewFiles));
    CHECK(r.wait([](const Snapshot &x) { return x.trim.save_state == 1 && x.trim.save_progress > 0.05f; }, 8000));
    CHECK(r.snap().trim.save_progress < 1.f);
    const auto t0 = std::chrono::steady_clock::now();
    c.app_leave(); /* the app closes: the save is cancelled */
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(took < 2.0);
    Snapshot s = r.snap();
    CHECK_EQ(s.trim.save_state, 4);
    CHECK_EQ(s.trim.save_message, "Cancelled");
    r.ops.tmp_delay_us.store(0);
    CHECK_EQ(slurp(orig), before);
    CHECK(same_files(r.files(), names_before)); /* no result, no temporary left */
    r.close();

    /* the same when the controller is simply destroyed during a save */
    Rig r2(one_card(), r.home);
    r2.cfg.save_block_bytes = 16384;
    r2.open();
    CHECK(r2.ctl().open_trim(kTake));
    CHECK(wait_peaks(r2));
    r2.ctl().set_position(48000 * 90);
    r2.ctl().set_out();
    r2.ops.tmp_delay_us.store(1500);
    CHECK(r2.ctl().save(SaveKind::ReplaceOriginal));
    CHECK(r2.wait([](const Snapshot &x) { return x.trim.save_progress > 0.05f; }, 8000));
    r2.close();
    CHECK_EQ(slurp(orig), before); /* replace mode too: the original is untouched */
    CHECK(same_files(r2.files(), names_before));

    /* cancel_save() by itself (the UI's Cancel button) */
    Rig r3(one_card(), r.home);
    r3.cfg.save_block_bytes = 16384;
    r3.open();
    CHECK(r3.ctl().open_trim(kTake));
    CHECK(wait_peaks(r3));
    r3.ctl().set_position(48000 * 90);
    r3.ctl().set_out();
    r3.ops.tmp_delay_us.store(1500);
    CHECK(r3.ctl().save(SaveKind::NewFiles));
    CHECK(r3.wait([](const Snapshot &x) { return x.trim.save_progress > 0.05f; }, 8000));
    r3.ctl().cancel_save();
    CHECK(wait_save(r3, 4));
    r3.ops.tmp_delay_us.store(0);
    CHECK(same_files(r3.files(), names_before));
    /* and a save can be started again afterwards */
    CHECK(r3.ctl().save(SaveKind::NewFiles));
    CHECK(wait_save(r3, 2));
    CHECK_EQ(r3.ctl().takes().size(), 2u);
}

void t_peaks_in_memory() {
    Rig r(one_card());
    make_take(r.rec(), kTake, 5.0);
    r.ops.fail_peak_rename.store(true); /* the cache file cannot be stored */
    r.open();
    CHECK(r.ctl().open_trim(kTake));
    CHECK(wait_peaks(r));
    CHECK(r.snap().trim.peaks_in_memory);
    CHECK_EQ(r.ctl().peak_window(64).size(), 64u);
    std::vector<PeakBar> b = r.ctl().peak_window(64);
    if (b.size() == 64) CHECK(b[63].max > b[0].max);
    for (const std::string &n : r.files()) CHECK(n.find(".peaks") == std::string::npos);
    CHECK(r.ctl().set_zoom(2));
    CHECK_EQ(r.ctl().peak_window(32).size(), 32u);
}

void t_take_operations() {
    Rig r(one_card());
    make_take(r.rec(), kTake, 2.0);
    make_take(r.rec(), "take_002_20261108-1106.wav", 2.0);
    r.open();
    Controller &c = r.ctl();
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 2u);
    CHECK(rows[0].current); /* the newest is the current take */
    CHECK(c.select_take(kTake));
    CHECK(c.takes()[1].current && !c.takes()[0].current);
    CHECK(!c.select_take("nope.wav"));
    std::string nn;
    CHECK_EQ(c.rename_take(kTake, "band", &nn), 0);
    CHECK_EQ(nn, "band_001_20261108-1105.wav");
    CHECK(c.takes()[1].current); /* the current take follows the rename */
    CHECK_EQ(c.rename_take(kTake, "x", nullptr), -ENOENT);
    CHECK_EQ(c.rename_take("band_001_20261108-1105.wav", "bad/name", nullptr), -EINVAL);
    CHECK(c.open_trim(nn));
    CHECK_EQ(c.delete_take(nn), -EBUSY); /* open for trimming */
    c.close_trim();
    CHECK_EQ(c.delete_take(nn), 0);
    rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    CHECK(!rows[0].current); /* the loaded take was deleted: the deck is empty, nothing else is loaded for the user */
    CHECK(r.snap().take.name.empty());
    CHECK_EQ(c.delete_take(rows[0].name), 0);
    CHECK(c.takes().empty());
    CHECK(!r.snap().deck.play_pause.enabled); /* nothing left to play */
}

}  // namespace

void test_edit() {
    t_trim_state();
    t_save_new_and_replace();
    t_leave_during_save();
    t_peaks_in_memory();
    t_take_operations();
}
