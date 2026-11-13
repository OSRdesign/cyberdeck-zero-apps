/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests (P2a): the settings file (defaults, round trip, unknown keys kept, written only when a setting changes),
 * the write policy of state.json and positions.json with a fake clock and a fake file that counts writes (report 060
 * finding 3: 0.1.0 rewrote state.json every 5 s while playing), and the positions in long files.
 */
#include "check.hpp"

#include "fileops.hpp"
#include "settings/settings.hpp"
#include "state/positions.hpp"
#include "state/write_policy.hpp"

using namespace vinyl;

namespace {

/* the real files, every finished atomic write (its rename) counted */
class CountingOps final : public FileOps {
public:
    int renames = 0;
    int open(const std::string &p, int f, int m) override { return real().open(p, f, m); }
    int close(int fd) override { return real().close(fd); }
    ssize_t write(int fd, const void *b, size_t n) override { return real().write(fd, b, n); }
    ssize_t pwrite(int fd, const void *b, size_t n, uint64_t o) override { return real().pwrite(fd, b, n, o); }
    ssize_t pread(int fd, void *b, size_t n, uint64_t o) override { return real().pread(fd, b, n, o); }
    int fdatasync(int fd) override { return real().fdatasync(fd); }
    int fsync(int fd) override { return real().fsync(fd); }
    int ftruncate(int fd, uint64_t l) override { return real().ftruncate(fd, l); }
    int fstat(int fd, StatInfo &st) override { return real().fstat(fd, st); }
    int stat(const std::string &p, StatInfo &st) override { return real().stat(p, st); }
    int rename(const std::string &a, const std::string &b) override {
        ++renames;
        return real().rename(a, b);
    }
    int unlink(const std::string &p) override { return real().unlink(p); }
    int mkdir(const std::string &p) override { return real().mkdir(p); }
    int fsync_dir(const std::string &p) override { return real().fsync_dir(p); }
    int list_dir(const std::string &p, std::vector<std::string> &n) override { return real().list_dir(p, n); }
    int free_space(const std::string &p, uint64_t &a) override { return real().free_space(p, a); }

private:
    static FileOps &real() { return posix_file_ops(); }
};

void test_settings_file() {
    const std::string dir = make_temp_dir("settings");
    const std::string path = dir + "/vinyl/settings.json";
    CountingOps ops;
    SettingsStore st(ops);
    st.load(path); /* no file: the defaults (D25: ReplayGain Track; gapless and long-file positions on) */
    CHECK(st.get().gapless);
    CHECK(st.get().replaygain == ReplayGainMode::Track);
    CHECK(st.get().remember_long);
    CHECK_EQ(ops.renames, 0); /* reading writes nothing */
    CHECK_EQ(st.set(st.get()), 1); /* unchanged: no write */
    CHECK_EQ(ops.renames, 0);
    Settings s = st.get();
    s.replaygain = ReplayGainMode::Album;
    s.gapless = false;
    CHECK_EQ(st.set(s), 0);
    CHECK_EQ(ops.renames, 1);
    CHECK_EQ(st.set(s), 1);
    CHECK_EQ(ops.renames, 1);
    /* read back */
    SettingsStore again(ops);
    again.load(path);
    CHECK(again.get() == s);
    /* unknown keys (a newer Vinyl's) survive a change */
    write_file(path, "{\"v\": 1, \"gapless\": true, \"replaygain\": \"off\", \"eq\": {\"preset\": \"Rock\"}, \"crossfade_s\": 4}\n");
    SettingsStore third(ops);
    third.load(path);
    CHECK(third.get().replaygain == ReplayGainMode::Off);
    CHECK(third.get().remember_long); /* absent: the default */
    Settings t = third.get();
    t.remember_long = false;
    CHECK_EQ(third.set(t), 0);
    const std::string text = read_file(path);
    CHECK(text.find("\"eq\"") != std::string::npos);
    CHECK(text.find("Rock") != std::string::npos);
    CHECK(text.find("\"crossfade_s\"") != std::string::npos);
    CHECK(text.find("\"remember_long\": false") != std::string::npos);
    /* a damaged file: the defaults */
    write_file(path, "{not json");
    SettingsStore bad(ops);
    bad.load(path);
    CHECK(bad.get() == Settings());
    /* an unknown mode keeps the default */
    Settings u;
    CHECK(settings_from_json("{\"replaygain\": \"loud\"}", u, nullptr));
    CHECK(u.replaygain == ReplayGainMode::Track);
    rm_tree(dir);
}

/* The write policy with a fake clock: ten minutes of playing, the controller's 100 ms tick marking progress each time,
 * write at most once a minute; every event written at the next tick. */
void test_write_policy() {
    struct FakeFile {
        int writes = 0;
    } file;
    WritePolicy p;
    uint64_t t = 0;
    p.wrote(t); /* the file as read at start */
    auto tick = [&](bool playing) {
        if (playing) p.progress();
        if (p.due(t)) {
            ++file.writes;
            p.wrote(t);
        }
        t += 100;
    };
    for (int i = 0; i < 6000; ++i) tick(true); /* 10 minutes */
    CHECK_EQ(file.writes, 9);                  /* at 1, 2 ... 9 min (the 10th minute ends at 599.9 s) */
    tick(true);
    CHECK_EQ(file.writes, 10);
    /* 0.1.0 wrote every 5 s while playing: 120 writes for the same 10 minutes */
    int old_writes = 0;
    uint64_t last = 0;
    for (uint64_t ms = 0; ms < 600000; ms += 100)
        if (ms - last > 5000) {
            ++old_writes;
            last = ms;
        }
    CHECK(old_writes >= 110);
    /* an event (a pause, a track change, a setting): at the next tick, then back to once a minute */
    const int before = file.writes;
    p.event();
    tick(false);
    CHECK_EQ(file.writes, before + 1);
    for (int i = 0; i < 100; ++i) tick(true); /* 10 s of playing after it: nothing */
    CHECK_EQ(file.writes, before + 1);
    /* paused: no progress, nothing written however long */
    for (int i = 0; i < 20000; ++i) tick(false);
    CHECK(!p.pending());
    /* two events in one tick: one write */
    p.event();
    p.event();
    tick(false);
    CHECK_EQ(file.writes, before + 3); /* the playing minute after the event wrote once too */
}

void test_positions() {
    Positions p;
    const uint64_t hour = 3600000;
    CHECK(!is_long_file(20 * 60 * 1000)); /* longer than 20 minutes */
    CHECK(is_long_file(20 * 60 * 1000 + 1000));
    CHECK(!p.note("/m/short.mp3", 60000, 240000)); /* a short track: nothing kept */
    CHECK_EQ(p.get("/m/short.mp3"), static_cast<uint64_t>(0));
    CHECK(p.note("/m/mix.mp3", 20 * 60000, hour));
    CHECK_EQ(p.get("/m/mix.mp3"), static_cast<uint64_t>(20 * 60000));
    CHECK(!p.note("/m/mix.mp3", 20 * 60000, hour)); /* the same place: no change */
    CHECK(p.note("/m/mix.mp3", 21 * 60000, hour));
    CHECK(p.note("/m/mix.mp3", 3000, hour)); /* the first 5 s: no position (it existed: changed) */
    CHECK_EQ(p.get("/m/mix.mp3"), static_cast<uint64_t>(0));
    CHECK(p.note("/m/mix.mp3", 30 * 60000, hour));
    CHECK(p.note("/m/mix.mp3", hour - 10000, hour)); /* the last 15 s: played to the end, forgotten */
    CHECK_EQ(p.get("/m/mix.mp3"), static_cast<uint64_t>(0));
    /* at most 500, the oldest goes */
    for (int i = 0; i < 520; ++i) p.note("/m/book" + std::to_string(i) + ".m4b", 60000 + i, hour);
    CHECK_EQ(p.size(), kMaxPositions);
    CHECK_EQ(p.get("/m/book0.m4b"), static_cast<uint64_t>(0));
    CHECK_EQ(p.get("/m/book519.m4b"), static_cast<uint64_t>(60519));
    /* a file of its own, read back */
    const std::string dir = make_temp_dir("positions");
    CountingOps ops;
    CHECK_EQ(save_positions(ops, dir + "/vinyl/positions.json", p), 0);
    CHECK_EQ(ops.renames, 1);
    Positions q;
    CHECK(load_positions(ops, dir + "/vinyl/positions.json", q));
    CHECK_EQ(q.size(), kMaxPositions);
    CHECK_EQ(q.get("/m/book300.m4b"), static_cast<uint64_t>(60300));
    CHECK(read_file(dir + "/vinyl/positions.json").size() < 64 * 1024);
    Positions none;
    CHECK(!load_positions(ops, dir + "/missing.json", none));
    CHECK_EQ(none.size(), static_cast<size_t>(0));
    rm_tree(dir);
    /* B-test case of report 060 (B34): a one-hour mix at 20:00, another track, back: 20:00; a 4-minute track: 0:00 */
    Positions b;
    b.note("/m/mix.flac", 20 * 60000, hour);
    b.note("/m/song.flac", 2 * 60000, 4 * 60000);
    CHECK_EQ(b.get("/m/mix.flac"), static_cast<uint64_t>(20 * 60000));
    CHECK_EQ(b.get("/m/song.flac"), static_cast<uint64_t>(0));
}

}  // namespace

void test_settings_state() {
    test_settings_file();
    test_write_policy();
    test_positions();
}
