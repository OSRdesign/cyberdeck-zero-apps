/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the command line, the options shared by the page and the bench, and the headless commands (from the spike).
 *
 *   vhs                       the app (from the launcher, or by hand on the board's console)
 *   vhs --bench SECONDS ...   capture + decode + fit without a screen, a stats line a second, then a summary
 *   vhs --list                the cameras, the chosen one's identity, modes, control, the board's selection, the codec
 *                             nodes, TurboJPEG
 *   vhs --selftest            the pipeline against the fake camera's profiles: PASS / FAIL (no camera needed)
 *
 * Options (app and bench): --mode MJPG:1280x720:30, --scale auto|1|2|4|8, --device PATH, --format rgb565|xrgb8888,
 * --box WxH (bench: the picture box, default 352x198), --accurate (no TJFLAG_FASTDCT), --fake[=finecam|portrait|c270|
 * yuyv] (the fake camera), --fake-file F.mjpg (the fake camera looping a canned MJPEG file), --stats (the stats line
 * every second instead of every 10 s). The app reads them as environment variables (main() sets them, the scenes use
 * them): VHS_MODE, VHS_SCALE, VHS_DEVICE, VHS_DEV_ROOT, VHS_SYS_ROOT, VHS_FORMAT, VHS_ACCURATE, VHS_FAKE, VHS_FAKE_FILE,
 * VHS_STATS; the scenes' fake-camera fixtures VHS_FAKE_SCRIPT, VHS_FAKE_RESTART_MS, VHS_FAKE_BUS, VHS_FAKE_CTRL,
 * VHS_FAKE_UNDERVOLT, VHS_SOC (the SoC class instead of the detected one), VHS_SET (settings for the fake camera),
 * VHS_FOLDER, VHS_CLOCK_SYNCED; the scenes' VHS_DEMO_NOTICES; VHS_TURBOJPEG (a library path, turbojpeg.hpp); VHS_SETTINGS (settings.hpp).
 */
#pragma once

#include "app/controller.hpp"
#include "camera/camera.hpp"
#include "camera/fake_camera.hpp"
#include "convert.hpp"

#include <string>
#include <utility>
#include <vector>

namespace vhs {

struct Options {
    bool fake = false;
    FakeOptions fake_opt;
    std::vector<FakeOptions> fake_more; /* VHS_FAKE_CAMERAS (the fake camera): the cameras after the first (0.1.2) */
    std::string fake_script;
    bool fake_undervolt = false;
    std::string device;          /* "" = discovery */
    std::string dev_root = "/dev";
    std::string sys_root;        /* "" = the real system */
    std::string soc;             /* VHS_SOC (scenes): "bcm2712", "bcm2711", "bcm2837", "other"; "" = detected */
    std::string settings_overrides; /* VHS_SET (the fake camera's scenes): "photo_mode=light,self_timer_s=3" */
    std::string folder;          /* VHS_FOLDER: the folder instead of Settings > Folder */
    int clock_synced = -1;       /* VHS_CLOCK_SYNCED: 0 / 1 instead of systemd-timesyncd's marker */
    ModeReq mode;                /* not valid = the board's selection */
    int denom = 0;               /* 0 = the fit rule */
    bool fmt_set = false;        /* false: the page uses the display's format */
    PixFmt fmt = PixFmt::RGB565;
    int box_w = 352, box_h = 198;
    bool fastdct = true;
    bool stats = false;
    std::string fake_mic;        /* VHS_FAKE_MIC (the fake camera): tone | quiet | silent | busy | none; "" = tone */
    long fake_free_mb = -1;      /* VHS_FAKE_FREE_MB (the fake camera): the free space the recorder sees */
    std::string demo_scenes;     /* VHS_DEMO_SCENES (the fake camera): "1" or "cjk": a demo scenes folder */
};

struct Cli {
    enum class Cmd { Ui, Bench, List, Selftest, Help };
    Cmd cmd = Cmd::Ui;
    int seconds = 10;
    Options opt;
    std::vector<std::string> pass; /* arguments for the page's runner (--version) */
    /* the options given, as the page's VHS_* variables (main() exports them before the page starts) */
    std::vector<std::pair<std::string, std::string>> env;
};

/* argv[1..]: false with err on a bad argument. */
bool parse_cli(const std::vector<std::string> &args, Cli &out, std::string &err);
std::string cli_usage();

/* The app's options from VHS_* (warnings: bad values, ignored). */
Options options_from_env(std::string *warnings = nullptr);

bool parse_box(const std::string &s, int &w, int &h);

/* The controller's configuration for these options (the app sets the box, the pixel format and its thread). */
ControllerConfig controller_config(const Options &o);

int run_bench(const Cli &cli);
int run_list(const Cli &cli);
int run_selftest();

} // namespace vhs
