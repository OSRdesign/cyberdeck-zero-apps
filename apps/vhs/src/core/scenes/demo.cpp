/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the demo scenes folder (see demo.hpp).
 */
#include "demo.hpp"

#include "fileops.hpp"
#include "index.hpp"
#include "jpeg/turbojpeg.hpp"
#include "locks.hpp"
#include "photos.hpp"
#include "record/recorder.hpp"

#include <utime.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

namespace vhs {
namespace {

/* colour bars rotated in hue by `shift` (0..5), 1280x720 BGRX, as a JPEG */
bool bars_jpeg(int shift, int w, int h, std::vector<std::uint8_t> &out)
{
    static const std::uint8_t bars[7][3] = {{235, 235, 235}, {16, 235, 235}, {235, 235, 16}, {16, 235, 16},
                                            {235, 16, 235},  {16, 16, 235},  {235, 16, 16}}; /* BGR */
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t *p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            if (y < h * 2 / 3) {
                const std::uint8_t *c = bars[(x * 7 / w + shift) % 7];
                p[0] = c[0];
                p[1] = c[1];
                p[2] = c[2];
            } else {
                const std::uint8_t g = static_cast<std::uint8_t>(16 + 219 * x / w);
                p[0] = p[1] = p[2] = g;
            }
            p[3] = 255;
        }
    JpegEncoder enc;
    return enc.ok() && enc.encode_bgrx(px.data(), w, h, 70, out);
}

void set_mtime(const std::string &path, long t)
{
    struct utimbuf u;
    u.actime = static_cast<time_t>(t);
    u.modtime = static_cast<time_t>(t);
    utime(path.c_str(), &u);
}

bool write_bytes(const std::string &path, const std::vector<std::uint8_t> &b)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

bool movie(FileOps &ops, const AvLibs &av, const std::string &folder, const std::string &stem, const std::vector<std::uint8_t> &jpeg,
           int frames, int w, int h, bool sound = false)
{
    RecorderConfig rc;
    rc.av = &av;
    rc.ops = &ops;
    rc.folder = folder;
    rc.stem = stem;
    rc.video.w = w;
    rc.video.h = h;
    rc.ring_seconds = 60;
    if (sound) { /* a 440 Hz tone, 16 kHz mono, on the same clock as the frames (the playback fixture, M5) */
        rc.audio_rate = 16000;
        rc.audio_channels = 1;
    }
    Recorder r(rc);
    r.start();
    std::uint64_t pushed = 0;
    std::vector<std::int16_t> chunk(1024);
    const std::int64_t ta0_ns = 3000000LL * 1000 - 300000000LL; /* the microphone 0.3 s before the first frame */
    for (int i = 0; i < frames; ++i) {
        const std::int64_t ts_us = 3000000 + i * 33333LL;
        while (sound && ta0_ns + static_cast<std::int64_t>((pushed + 1024) * 1000000000ULL / 16000) <= ts_us * 1000 + 40000000LL) {
            for (int k = 0; k < 1024; ++k)
                chunk[static_cast<std::size_t>(k)] = static_cast<std::int16_t>(std::lround(3000.0 * std::sin(2 * M_PI * 440.0 * static_cast<double>(pushed + static_cast<std::uint64_t>(k)) / 16000)));
            r.write(chunk.data(), 1024);
            pushed += 1024;
            if (pushed % 32768 == 1024) r.audio_clock(ta0_ns + static_cast<std::int64_t>(pushed * 1000000000ULL / 16000), pushed);
        }
        r.push_video(jpeg.data(), jpeg.size(), ts_us);
    }
    for (int i = 0; i < 1000 && r.status().frames < static_cast<std::uint64_t>(frames); ++i) {
        struct timespec ts{0, 5000000};
        nanosleep(&ts, nullptr);
    }
    r.request_stop(RecStop::User);
    r.join();
    return r.status().frames > 0;
}

} // namespace

int make_demo_scenes(FileOps &ops, const std::string &folder, const AvLibs *av, const std::string &set)
{
    if (ensure_folder(ops, folder) != 0) return 0;
    int movies = 0, photos = 0;
    count_scenes(ops, folder, movies, photos);
    if (movies + photos > 0) return 0;
    struct Item {
        const char *name;
        bool is_movie;
        int frames;
        long when;
    };
    /* newest last in this table; times a few minutes to days apart (2026-11-12 .. 2026-11-14) */
    std::vector<Item> items = {
        {"scene_001_20261112-101500.mkv", true, 20, 1794478500},
        {"photo_002_20261112-103000.jpg", false, 0, 1794479400},
        {"scene_003_20261113-090000.mkv", true, 24, 1794560400},
        {"photo_004_20261113-091200.jpg", false, 0, 1794561120},
        {"scene_005_20261113-210044.mkv", true, 18, 1794603644},
        {"scene_005_20261113-210044_p2.mkv", true, 12, 1794603800},
        {"Stopwatch test_006_20261114-160500.mkv", true, 15, 1794672300},
        {"photo_007_20261114-170000.jpg", false, 0, 1794675600},
        {"scene_008_20261114-180000.mkv", true, 30, 1794679200},
        {"photo_009_20261114-184710.jpg", false, 0, 1794682030},
        {"photo_010_20261114-184800.jpg", false, 0, 1794682080},
        {"scene_011_20261114-185200.mkv", true, 36, 1794682320},
        {"Sunset_012_20261114-190000.mkv", true, 24, 1794682800}, /* renamed: its label is "Sunset" in every form */
        {"Trip_013_20261114-190500.mkv", true, 300, 1794683100}, /* the playback fixture (newest): 10 s with sound, "Trip" */
    };
    if (set == "cjk") {
        items.push_back({"東京の夜.mkv", true, 15, 1794300000});
        items.push_back({"家族旅行 2026.jpg", false, 0, 1794300100});
        items.push_back({"회의 녹화.mkv", true, 15, 1794300200});
        items.push_back({"旅行_写真.jpeg", false, 0, 1794300300});
    }
    int made = 0, shift = 0;
    for (const Item &it : items) {
        std::vector<std::uint8_t> jpeg;
        if (!bars_jpeg(shift++ % 7, it.is_movie ? 1280 : 1920, it.is_movie ? 720 : 1080, jpeg)) continue;
        const std::string path = folder + "/" + it.name;
        bool ok = false;
        if (it.is_movie) {
            if (!av) continue;
            const std::string name = it.name;
            const std::string stem = name.substr(0, name.size() - 4);
            ok = movie(ops, *av, folder, stem, jpeg, it.frames, 1280, 720, it.frames >= 300);
            if (ok) { /* the recorder's journal says "closed": keep only the files */
                ops.unlink(folder + "/.journal/" + stem + ".json");
            }
        } else {
            ok = write_bytes(path, jpeg);
        }
        if (!ok) continue;
        set_mtime(path, it.when);
        ++made;
    }
    set_locked(ops, folder, "Stopwatch test_006_20261114-160500.mkv", true);
    return made;
}

} // namespace vhs
