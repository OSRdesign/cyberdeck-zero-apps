/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the command line (--bench, --list, --selftest, the options) and the page's VHS_* variables.
 */
#include "check.hpp"

#include "live/bench.hpp"

#include <cstdlib>

using namespace vhs;

namespace {

bool parse(std::vector<std::string> args, Cli &c, std::string &err)
{
    return parse_cli(args, c, err);
}

std::string env_of(const Cli &c, const char *name)
{
    for (const auto &kv : c.env)
        if (kv.first == name) return kv.second;
    return "<none>";
}

void commands()
{
    Cli c;
    std::string err;
    CHECK(parse({}, c, err));
    CHECK(c.cmd == Cli::Cmd::Ui);
    CHECK(c.env.empty());
    CHECK(parse({"--bench", "10", "--mode", "MJPG:1280x720:30", "--scale", "2", "--device", "/dev/video0", "--format",
                 "xrgb8888", "--box", "288x162", "--accurate"},
                c, err));
    CHECK(c.cmd == Cli::Cmd::Bench);
    CHECK_EQ(c.seconds, 10);
    CHECK_EQ(mode_arg(c.opt.mode), std::string("MJPG:1280x720:30"));
    CHECK_EQ(c.opt.denom, 2);
    CHECK_EQ(c.opt.device, std::string("/dev/video0"));
    CHECK(c.opt.fmt == PixFmt::XRGB8888);
    CHECK(c.opt.fmt_set);
    CHECK_EQ(c.opt.box_w, 288);
    CHECK_EQ(c.opt.box_h, 162);
    CHECK(!c.opt.fastdct);
    CHECK(parse({"--bench=5", "--scale=auto", "--mode=YUYV:640x360:30"}, c, err));
    CHECK_EQ(c.seconds, 5);
    CHECK_EQ(c.opt.denom, 0);
    CHECK_EQ(c.opt.mode.fourcc, kYUYV);
    CHECK(parse({"--list"}, c, err));
    CHECK(c.cmd == Cli::Cmd::List);
    CHECK(parse({"--selftest"}, c, err));
    CHECK(c.cmd == Cli::Cmd::Selftest);
    CHECK(parse({"--help"}, c, err));
    CHECK(c.cmd == Cli::Cmd::Help);
    CHECK(parse({"--version"}, c, err));
    CHECK(c.cmd == Cli::Cmd::Ui);
    CHECK_EQ(c.pass.size(), static_cast<std::size_t>(1));
    CHECK(!cli_usage().empty());
}

void page_env()
{
    Cli c;
    std::string err;
    CHECK(parse({"--fake=portrait", "--stats", "--scale", "4", "--format", "rgb565"}, c, err));
    CHECK(c.cmd == Cli::Cmd::Ui);
    CHECK(c.opt.fake && c.opt.fake_opt.portrait && c.opt.stats);
    CHECK_EQ(env_of(c, "VHS_FAKE"), std::string("portrait"));
    CHECK_EQ(env_of(c, "VHS_STATS"), std::string("1"));
    CHECK_EQ(env_of(c, "VHS_SCALE"), std::string("4"));
    CHECK_EQ(env_of(c, "VHS_FORMAT"), std::string("rgb565"));
    CHECK_EQ(env_of(c, "VHS_MODE"), std::string("<none>"));
    CHECK(parse({"--fake=c270"}, c, err));
    CHECK(c.opt.fake_opt.profile == FakeProfile::C270);
    CHECK(!parse({"--fake=c920"}, c, err));
    CHECK(parse({"--fake-file", "/tmp/x.mjpg"}, c, err));
    CHECK(c.opt.fake);
    CHECK_EQ(env_of(c, "VHS_FAKE_FILE"), std::string("/tmp/x.mjpg"));

    /* the page reads them back */
    setenv("VHS_FAKE", "1", 1);
    setenv("VHS_MODE", "YUYV:640x480:30", 1);
    setenv("VHS_SCALE", "8", 1);
    setenv("VHS_FORMAT", "xrgb8888", 1);
    setenv("VHS_ACCURATE", "1", 1);
    setenv("VHS_DEV_ROOT", "/tmp/devroot", 1);
    std::string warnings;
    setenv("VHS_FAKE_SCRIPT", "unplug@100,replug@200", 1);
    setenv("VHS_FAKE_RESTART_MS", "2500", 1);
    setenv("VHS_FAKE_BUS", "dwc_otg", 1);
    setenv("VHS_FAKE_CTRL", "inactive", 1);
    setenv("VHS_SOC", "bcm2837", 1);
    Options o = options_from_env(&warnings);
    CHECK(warnings.empty());
    CHECK(o.fake && !o.fake_opt.portrait);
    CHECK_EQ(o.fake_script, std::string("unplug@100,replug@200"));
    CHECK_EQ(o.fake_opt.restart_delay_ms, 2500);
    CHECK_EQ(o.fake_opt.hc_driver, std::string("dwc_otg"));
    CHECK_EQ(o.fake_opt.ctrl, static_cast<int>(FakeCtrl::Inactive));
    CHECK_EQ(o.soc, std::string("bcm2837"));
    CHECK_EQ(mode_arg(o.mode), std::string("YUYV:640x480:30"));
    CHECK_EQ(o.denom, 8);
    CHECK(o.fmt == PixFmt::XRGB8888 && o.fmt_set);
    CHECK(!o.fastdct);
    CHECK_EQ(o.dev_root, std::string("/tmp/devroot"));
    setenv("VHS_MODE", "H264:1x1:1", 1);
    setenv("VHS_SCALE", "3", 1);
    setenv("VHS_FORMAT", "rgb332", 1);
    o = options_from_env(&warnings);
    CHECK(warnings.find("VHS_MODE") != std::string::npos);
    CHECK(warnings.find("VHS_SCALE") != std::string::npos);
    CHECK(warnings.find("VHS_FORMAT") != std::string::npos);
    CHECK(!o.mode.valid());
    CHECK(!o.fmt_set);
    for (const char *n : {"VHS_FAKE", "VHS_MODE", "VHS_SCALE", "VHS_FORMAT", "VHS_ACCURATE", "VHS_DEV_ROOT",
                          "VHS_FAKE_SCRIPT", "VHS_FAKE_RESTART_MS", "VHS_FAKE_BUS", "VHS_FAKE_CTRL", "VHS_SOC"})
        unsetenv(n);
    o = options_from_env(nullptr);
    CHECK(!o.fake);
    CHECK_EQ(o.dev_root, std::string("/dev"));
    CHECK(o.fastdct);
    CHECK(o.soc.empty());
}

void errors()
{
    Cli c;
    std::string err;
    CHECK(!parse({"--bench"}, c, err));
    CHECK(err.find("--bench") != std::string::npos);
    CHECK(!parse({"--bench", "0"}, c, err));
    CHECK(!parse({"--bench", "ten"}, c, err));
    CHECK(parse({"--mode", "MJPG:1280x720"}, c, err)); /* 30 fps by default: valid */
    CHECK(!parse({"--mode", "nope"}, c, err));
    CHECK(err.find("FORMAT:WxH:FPS") != std::string::npos);
    CHECK(!parse({"--scale", "3"}, c, err));
    CHECK(!parse({"--format", "rgb888"}, c, err));
    CHECK(!parse({"--box", "10x10"}, c, err));
    CHECK(!parse({"--box", "352"}, c, err));
    CHECK(!parse({"--frobnicate"}, c, err));
    CHECK(err.find("unknown") != std::string::npos);
    int w = 0, h = 0;
    CHECK(parse_box("352x198", w, h));
    CHECK_EQ(w, 352);
    CHECK(!parse_box("352x198x2", w, h));
}

} // namespace

void test_cli()
{
    commands();
    page_env();
    errors();
}
