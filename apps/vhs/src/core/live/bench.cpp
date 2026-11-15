/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the command line and the headless commands (see bench.hpp).
 */
#include "bench.hpp"

#include "app/controller.hpp"
#include "camera/capture.hpp"
#include "camera/controls.hpp"
#include "clock.hpp"
#include "jpeg/turbojpeg.hpp"
#include "modes/codec_caps.hpp"
#include "modes/select.hpp"
#include "modes/soc.hpp"
#include "pipeline.hpp"

#include <linux/videodev2.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace vhs {
namespace {

std::atomic<bool> g_bench_stop{false};

void on_bench_signal(int)
{
    g_bench_stop = true;
}

bool take_value(const std::vector<std::string> &args, std::size_t &i, const std::string &name, std::string &value,
                std::string &err)
{
    const std::string &a = args[i];
    if (a.size() > name.size() && a.compare(0, name.size() + 1, name + "=") == 0) {
        value = a.substr(name.size() + 1);
        return true;
    }
    if (i + 1 >= args.size()) {
        err = name + " needs a value";
        return false;
    }
    value = args[++i];
    return true;
}

bool is_opt(const std::string &a, const std::string &name)
{
    return a == name || (a.size() > name.size() && a.compare(0, name.size() + 1, name + "=") == 0);
}

void print_table_row(const char *stage, const char *fps, const Summary &s, bool has_ms)
{
    if (has_ms && s.n > 0)
        std::printf("  %-10s %8s %9.2f %9.2f %9.2f %7d\n", stage, fps, s.mean, s.p95, s.max, s.n);
    else
        std::printf("  %-10s %8s %9s %9s %9s %7s\n", stage, fps, "-", "-", "-", "-");
}

std::string getenv_s(const char *n)
{
    const char *v = std::getenv(n);
    return v ? v : "";
}


} // namespace

ControllerConfig controller_config(const Options &o)
{
    ControllerConfig c;
    c.fake = o.fake;
    c.fake_opt = o.fake_opt;
    c.fake_script = o.fake_script;
    c.force_undervolt = o.fake_undervolt;
    c.device = o.device;
    c.dev_root = o.dev_root;
    c.sys_root = o.sys_root;
    c.persist = !o.fake;
    c.fmt = o.fmt;
    c.box_w = o.box_w;
    c.box_h = o.box_h;
    c.fastdct = o.fastdct;
    c.mode = o.mode;
    c.denom = o.denom;
    c.log_every = o.stats ? 1 : 10;
    c.soc_override = o.soc;
    c.settings_overrides = o.settings_overrides;
    c.folder = o.folder;
    c.clock_synced = o.clock_synced;
    return c;
}

bool parse_box(const std::string &s, int &w, int &h)
{
    int a = 0, b = 0, n = 0;
    if (std::sscanf(s.c_str(), "%dx%d%n", &a, &b, &n) != 2 || static_cast<std::size_t>(n) != s.size()) return false;
    if (a < 16 || b < 16 || a > 4096 || b > 4096) return false;
    w = a;
    h = b;
    return true;
}

std::string cli_usage()
{
    return "usage: vhs [options]                   the app (the launcher starts it so)\n"
           "       vhs --bench SECONDS [options]   capture + decode + fit, no screen; a summary at the end\n"
           "       vhs --list [--device PATH]      the cameras, the chosen one's modes, the board's selection\n"
           "       vhs --selftest                  the pipeline on the fake camera: PASS / FAIL\n"
           "       vhs --version\n"
           "options:\n"
           "  --mode MJPG:1280x720:30   camera mode (format:WxH:fps; MJPG or YUYV); default: the board's selection\n"
           "  --scale auto|1|2|4|8      decode scale 1/n (auto: the fit rule, the whole frame in the box)\n"
           "  --format rgb565|xrgb8888  picture format (app default: the display's)\n"
           "  --box WxH                 bench: the picture box (default 352x198, the 480x320 page's)\n"
           "  --device PATH             camera node (default: discovery, /dev/v4l/by-id/...-video-index0)\n"
           "  --accurate                TurboJPEG without TJFLAG_FASTDCT\n"
           "  --fake[=PROFILE]          the fake camera: finecam (default), portrait, c270, yuyv\n"
           "  --fake-file FILE.mjpg     the fake camera looping a canned MJPEG file\n"
           "  --stats                   the stats line every second (default every 10 s)\n";
}

bool parse_cli(const std::vector<std::string> &args, Cli &out, std::string &err)
{
    Cli c;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        std::string v;
        if (a == "--help" || a == "-h") {
            c.cmd = Cli::Cmd::Help;
        } else if (a == "--version") {
            c.pass.push_back(a);
        } else if (is_opt(a, "--bench")) {
            if (!take_value(args, i, "--bench", v, err)) return false;
            char *end = nullptr;
            const long s = std::strtol(v.c_str(), &end, 10);
            if (!end || *end || s < 1 || s > 86400) {
                err = "--bench: seconds from 1 to 86400";
                return false;
            }
            c.cmd = Cli::Cmd::Bench;
            c.seconds = static_cast<int>(s);
        } else if (a == "--list") {
            c.cmd = Cli::Cmd::List;
        } else if (a == "--selftest") {
            c.cmd = Cli::Cmd::Selftest;
        } else if (is_opt(a, "--mode")) {
            if (!take_value(args, i, "--mode", v, err)) return false;
            if (!parse_mode(v, c.opt.mode)) {
                err = "--mode " + v + ": FORMAT:WxH:FPS, e.g. MJPG:1280x720:30 or YUYV:640x360:30";
                return false;
            }
            c.env.emplace_back("VHS_MODE", v);
        } else if (is_opt(a, "--scale")) {
            if (!take_value(args, i, "--scale", v, err)) return false;
            if (!parse_scale(v, c.opt.denom)) {
                err = "--scale " + v + ": auto, 1, 2, 4 or 8";
                return false;
            }
            c.env.emplace_back("VHS_SCALE", v);
        } else if (is_opt(a, "--format")) {
            if (!take_value(args, i, "--format", v, err)) return false;
            if (!parse_pixfmt(v, c.opt.fmt)) {
                err = "--format " + v + ": rgb565 or xrgb8888";
                return false;
            }
            c.opt.fmt_set = true;
            c.env.emplace_back("VHS_FORMAT", v);
        } else if (is_opt(a, "--box")) {
            if (!take_value(args, i, "--box", v, err)) return false;
            if (!parse_box(v, c.opt.box_w, c.opt.box_h)) {
                err = "--box " + v + ": WxH, e.g. 352x198";
                return false;
            }
        } else if (is_opt(a, "--device")) {
            if (!take_value(args, i, "--device", v, err)) return false;
            c.opt.device = v;
            c.env.emplace_back("VHS_DEVICE", v);
        } else if (a == "--accurate") {
            c.opt.fastdct = false;
            c.env.emplace_back("VHS_ACCURATE", "1");
        } else if (a == "--fake" || a.compare(0, 7, "--fake=") == 0) {
            const std::string p = a == "--fake" ? "finecam" : a.substr(7);
            if (!parse_fake_profile(p, c.opt.fake_opt)) {
                err = a + ": finecam, portrait, c270 or yuyv";
                return false;
            }
            c.opt.fake = true;
            c.env.emplace_back("VHS_FAKE", p);
        } else if (is_opt(a, "--fake-file")) {
            if (!take_value(args, i, "--fake-file", v, err)) return false;
            c.opt.fake = true;
            c.opt.fake_opt.canned_file = v;
            c.env.emplace_back("VHS_FAKE", "1");
            c.env.emplace_back("VHS_FAKE_FILE", v);
        } else if (a == "--stats") {
            c.opt.stats = true;
            c.env.emplace_back("VHS_STATS", "1");
        } else {
            err = "unknown argument " + a;
            return false;
        }
    }
    out = c;
    return true;
}

Options options_from_env(std::string *warnings)
{
    Options o;
    auto warn = [warnings](const std::string &w) {
        if (warnings) *warnings += (warnings->empty() ? "" : "; ") + w;
    };
    const std::string fake = getenv_s("VHS_FAKE");
    if (!fake.empty() && fake != "0") {
        o.fake = true;
        if (!parse_fake_profile(fake, o.fake_opt)) warn("VHS_FAKE=" + fake + ": the FineCam");
    }
    o.fake_opt.canned_file = getenv_s("VHS_FAKE_FILE");
    o.fake_script = getenv_s("VHS_FAKE_SCRIPT");
    o.fake_undervolt = !getenv_s("VHS_FAKE_UNDERVOLT").empty() && getenv_s("VHS_FAKE_UNDERVOLT") != "0";
    if (!getenv_s("VHS_FAKE_RESTART_MS").empty()) o.fake_opt.restart_delay_ms = std::atoi(getenv_s("VHS_FAKE_RESTART_MS").c_str());
    if (!getenv_s("VHS_FAKE_BUS").empty()) o.fake_opt.hc_driver = getenv_s("VHS_FAKE_BUS");
    const std::string ctrl = getenv_s("VHS_FAKE_CTRL");
    if (ctrl == "absent") o.fake_opt.ctrl = static_cast<int>(FakeCtrl::Absent);
    else if (ctrl == "present") o.fake_opt.ctrl = static_cast<int>(FakeCtrl::Present);
    else if (ctrl == "inactive") o.fake_opt.ctrl = static_cast<int>(FakeCtrl::Inactive);
    else if (!ctrl.empty()) warn("VHS_FAKE_CTRL=" + ctrl + " ignored");
    o.device = getenv_s("VHS_DEVICE");
    if (!getenv_s("VHS_DEV_ROOT").empty()) o.dev_root = getenv_s("VHS_DEV_ROOT");
    o.sys_root = getenv_s("VHS_SYS_ROOT");
    o.soc = getenv_s("VHS_SOC");
    o.settings_overrides = getenv_s("VHS_SET");
    o.folder = getenv_s("VHS_FOLDER");
    if (!getenv_s("VHS_CLOCK_SYNCED").empty()) o.clock_synced = getenv_s("VHS_CLOCK_SYNCED") == "0" ? 0 : 1;
    const std::string mode = getenv_s("VHS_MODE");
    if (!mode.empty() && !parse_mode(mode, o.mode)) warn("VHS_MODE=" + mode + " ignored");
    const std::string scale = getenv_s("VHS_SCALE");
    if (!scale.empty() && !parse_scale(scale, o.denom)) warn("VHS_SCALE=" + scale + " ignored");
    const std::string fmt = getenv_s("VHS_FORMAT");
    if (!fmt.empty()) {
        if (parse_pixfmt(fmt, o.fmt)) o.fmt_set = true;
        else warn("VHS_FORMAT=" + fmt + " ignored");
    }
    o.fastdct = getenv_s("VHS_ACCURATE").empty() || getenv_s("VHS_ACCURATE") == "0";
    o.stats = !getenv_s("VHS_STATS").empty() && getenv_s("VHS_STATS") != "0";
    return o;
}

int run_list(const Cli &cli)
{
    const Options &o = cli.opt;
    std::unique_ptr<FakeCamera> fake;
    std::unique_ptr<V4l2Api> real;
    V4l2Api *api = nullptr;
    std::vector<Candidate> cands;
    if (o.fake) {
        fake = std::make_unique<FakeCamera>(o.fake_opt);
        api = fake.get();
        Candidate c;
        c.by_id = true;
        c.ref.path = FakeCamera::kPath;
        c.ref.name = "fake-camera-video-index0";
        c.ref.id = fake->identity();
        c.capture = true;
        c.mjpg = lists_format(fake->modes(), kMJPG);
        c.yuyv = lists_format(fake->modes(), kYUYV);
        cands.push_back(c);
    } else {
        real = make_real_v4l2();
        api = real.get();
        if (!o.device.empty()) {
            Candidate c;
            c.ref.path = o.device;
            c.ref.name = o.device;
            c.ref.id = identity_for_node(o.sys_root, o.device);
            c.capture = true;
            c.mjpg = c.yuyv = true;
            cands.push_back(c);
        } else {
            cands = scan_cameras(*api, o.dev_root, o.sys_root);
        }
    }
    const SocClass soc = detect_soc(o.sys_root);
    std::printf("board: soc %s (photo default %s), ram %d MB\n", soc_name(soc), photo_mode_name(default_photo_mode(soc)),
                ram_mb(o.sys_root));
    std::printf("%s\n", codec_caps_text(detect_codec_caps(o.sys_root, o.dev_root)).c_str());
    TurboJpeg &tj = TurboJpeg::get();
    std::printf("TurboJPEG: %s\n", tj.available() ? ("loaded (" + tj.path() + ")").c_str() : tj.error().c_str());
    if (cands.empty()) std::printf("cameras: none (no %s/v4l/by-id/...-video-index0, no uvcvideo node)\n", o.dev_root.c_str());
    for (const Candidate &c : cands)
        std::printf("camera: %s%s\n  id %s, bus %s, %s%s%s\n", c.ref.name.c_str(), c.by_id ? "" : " (no by-id link)",
                    c.ref.id.valid() ? c.ref.id.key().c_str() : "-", c.ref.id.hc_driver.empty() ? "?" : c.ref.id.hc_driver.c_str(),
                    c.capture ? "video capture" : "NOT a video capture node", c.mjpg ? ", MJPG" : "", c.yuyv ? ", YUYV" : "");
    const int pick = pick_camera(cands, "");
    if (pick < 0) return 1;
    const Candidate &c = cands[static_cast<std::size_t>(pick)];
    const int fd = api->open(c.ref.path.c_str());
    if (fd < 0) {
        std::printf("%s: %s\n", c.ref.path.c_str(), std::strerror(errno));
        return 1;
    }
    const DeviceInfo info = query_device(*api, fd);
    std::printf("chosen: %s\n  card %s, driver %s, bus %s, USB %s / %s\n", c.ref.path.c_str(), info.card.c_str(),
                info.driver.c_str(), info.bus.c_str(), c.ref.id.manufacturer.c_str(), c.ref.id.product.c_str());
    const std::vector<CamMode> modes = enumerate_modes(*api, fd);
    for (const CamMode &m : modes) {
        std::string fps;
        for (int f : m.fps) fps += (fps.empty() ? "" : ", ") + std::to_string(f);
        std::printf("  %s %dx%d  %s fps\n", fourcc_text(m.fourcc).c_str(), m.w, m.h, fps.c_str());
    }
    /* the control, read only (--list sets nothing) */
    struct v4l2_queryctrl q;
    std::memset(&q, 0, sizeof q);
    q.id = kCidExposureAutoPriority;
    if (api->ioctl(fd, VIDIOC_QUERYCTRL, &q) == 0) {
        struct v4l2_control ctl;
        std::memset(&ctl, 0, sizeof ctl);
        ctl.id = kCidExposureAutoPriority;
        const bool got = api->ioctl(fd, VIDIOC_G_CTRL, &ctl) == 0;
        std::printf("  control exposure_auto_priority: %s%s%s\n", got ? std::to_string(ctl.value).c_str() : "?",
                    (q.flags & V4L2_CTRL_FLAG_INACTIVE) ? ", inactive" : "",
                    (q.flags & V4L2_CTRL_FLAG_READ_ONLY) ? ", read-only" : "");
    } else {
        std::printf("  control exposure_auto_priority: absent\n");
    }
    api->close(fd);
    SelectInput in;
    in.listed = modes;
    in.soc = soc;
    in.box_w = o.box_w;
    in.box_h = o.box_h;
    std::printf("%s\n", selection_text(select_modes(in), soc).c_str());
    return 0;
}

int run_bench(const Cli &cli)
{
    Options o = cli.opt;
    o.stats = true; /* a line a second */
    TurboJpeg &tj = TurboJpeg::get();
    ControllerConfig cc = controller_config(o);
    cc.ui_tid = current_tid();
    std::fprintf(stderr, "[vhs] bench %d s: %s, mode %s, scale %s, %s, box %dx%d, %s, TurboJPEG %s\n", cli.seconds,
                 o.fake ? "fake camera" : (o.device.empty() ? "discovery" : o.device.c_str()),
                 o.mode.valid() ? mode_text(o.mode).c_str() : "(the board's selection)", scale_text(o.denom).c_str(),
                 pix_name(o.fmt), o.box_w, o.box_h, o.fastdct ? "fastdct" : "accurate DCT",
                 tj.available() ? tj.path().c_str() : tj.error().c_str());
    g_bench_stop = false;
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_bench_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    Controller ctl(cc);
    Pipeline &pipe = ctl.pipeline();
    ctl.start();
    const std::int64_t t_end = mono_us() + static_cast<std::int64_t>(cli.seconds) * 1000000 + 300000;
    std::vector<double> lat;
    std::uint64_t taken = 0;
    std::int64_t t_first = 0;
    /* the taker: what the page's timer does at the display refresh (33 ms), without drawing */
    while (!g_bench_stop && mono_us() < t_end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        if (pipe.pictures().take()) {
            const Picture &p = pipe.pictures().front();
            pipe.note_shown(p);
            const double l = static_cast<double>(mono_us() - p.ts_us) / 1000.0;
            pipe.note_refresh(-1, -1, l);
            if (!t_first) t_first = mono_us();
            if (mono_us() - t_first > 1000000) {
                lat.push_back(l);
                ++taken;
            }
        }
    }
    const CaptureStatus cam = pipe.camera();
    const Interval last = pipe.has_interval() ? pipe.last_interval() : Interval();
    ctl.stop();
    std::printf("\nvhs bench summary (%d s asked)\n", cli.seconds);
    std::printf("  camera     %s%s%s, state %s\n", cam.device.c_str(), cam.card.empty() ? "" : ", ", cam.card.c_str(),
                state_name(cam.state));
    if (ctl.selection().ok || !o.mode.valid()) std::printf("  %s\n", selection_text(ctl.selection(), ctl.soc()).c_str());
    else std::printf("  modes: --mode %s (the board's selection not used)\n", mode_arg(o.mode).c_str());
    std::printf("  stream     %s; scale %s; %s; box %dx%d; %s\n", last.mode.empty() ? "(not streaming)" : last.mode.c_str(),
                last.scale.c_str(), pix_name(o.fmt), o.box_w, o.box_h, o.fastdct ? "fastdct" : "accurate DCT");
    std::printf("  %s\n", controls_log(cam.exposure).c_str());
    std::printf("  last interval: cap %.1f fps, dec %.1f fps, taken %.1f fps; decode %.2f/%.2f ms, fit %.2f/%.2f ms "
                "(mean/p95)\n",
                last.cap_fps, last.dec_fps, last.shown_fps, last.dec_ms.mean, last.dec_ms.p95, last.conv_ms.mean,
                last.conv_ms.p95);
    const Summary ls = summarize(lat);
    print_table_row("latency", "", ls, true);
    if (last.cores >= 0)
        std::printf("  cpu        %.2f cores (capture %.2f, decode %.2f, main %.2f) in the last interval\n", last.cores,
                    std::max(0.0, last.cap_cores), std::max(0.0, last.dec_cores), std::max(0.0, last.ui_cores));
    if (last.rss_mb >= 0) std::printf("  rss        %.1f MB\n", last.rss_mb);
    if (last.have_thr) std::printf("  throttled  0x%x (%s)\n", last.thr, throttled_text(last.thr).c_str());
    const std::string derr = pipe.last_decode_error();
    if (!derr.empty()) std::printf("  decoder    %s\n", derr.c_str());
    std::printf("  frames     captured %llu, decoded %llu, taken after warm-up %llu, decode failures %llu\n",
                static_cast<unsigned long long>(pipe.captured()), static_cast<unsigned long long>(pipe.decoded()),
                static_cast<unsigned long long>(taken), static_cast<unsigned long long>(pipe.decode_failures()));
    std::fflush(stdout);
    return pipe.decoded() > 0 ? 0 : 1;
}

int run_selftest()
{
    int failed = 0;
    TurboJpeg &tj = TurboJpeg::get();
    std::printf("vhs selftest (the fake camera, no camera needed)\n");
    std::printf("  TurboJPEG: %s\n", tj.available() ? ("loaded (" + tj.path() + ")").c_str() : tj.error().c_str());
    if (!tj.available()) ++failed;
    struct Case {
        const char *profile;
        PixFmt fmt;
        int box_w, box_h;
    };
    const Case cases[] = {
        {"finecam", PixFmt::RGB565, 352, 198},  {"finecam", PixFmt::XRGB8888, 640, 360},
        {"finecam", PixFmt::RGB565, 288, 162},  {"portrait", PixFmt::RGB565, 352, 198},
        {"c270", PixFmt::RGB565, 640, 360},     {"yuyv", PixFmt::RGB565, 352, 198},
    };
    for (const Case &c : cases) {
        Options o;
        o.fake = true;
        parse_fake_profile(c.profile, o.fake_opt);
        o.fmt = c.fmt;
        o.box_w = c.box_w;
        o.box_h = c.box_h;
        ControllerConfig cc = controller_config(o);
        cc.log_every = 1000;
        cc.ui_tid = current_tid();
        Controller ctl(cc);
        const bool need_tj = std::string(c.profile) != "yuyv";
        if (need_tj && !tj.available()) {
            std::printf("  SKIP %-9s %s (no TurboJPEG)\n", c.profile, pix_name(c.fmt));
            continue;
        }
        ctl.start();
        Pipeline &pipe = ctl.pipeline();
        int taken = 0;
        Picture last;
        const std::int64_t end = mono_us() + 1500000;
        while (mono_us() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            if (pipe.pictures().take()) {
                ++taken;
                last = pipe.pictures().front();
            }
        }
        const CaptureStatus st = pipe.camera();
        ctl.stop();
        int fw = 0, fh = 0;
        fit_size(last.mode.w, last.mode.h, c.box_w, c.box_h, fw, fh);
        const bool whole = last.w == fw && last.h == fh && last.w <= c.box_w && last.h <= c.box_h &&
                           (last.w == c.box_w || last.h == c.box_h);
        const bool ok = pipe.captured() >= 10 && pipe.decoded() >= 5 && taken >= 5 && pipe.decode_failures() == 0 &&
                        whole && st.state == CamState::Streaming;
        if (!ok) ++failed;
        std::printf("  %s %-9s %-8s box %dx%d: %s, captured %3llu decoded %3llu taken %3d -> 1/%d %dx%d, fit %dx%d%s%s\n",
                    ok ? "PASS" : "FAIL", c.profile, pix_name(c.fmt), c.box_w, c.box_h, mode_text(last.mode).c_str(),
                    static_cast<unsigned long long>(pipe.captured()), static_cast<unsigned long long>(pipe.decoded()), taken,
                    last.denom, last.dec_w, last.dec_h, last.w, last.h,
                    pipe.last_decode_error().empty() ? "" : " decoder: ", pipe.last_decode_error().c_str());
    }
    std::printf("%s\n", failed ? "selftest FAILED" : "selftest PASSED");
    return failed ? 1 : 0;
}

} // namespace vhs
