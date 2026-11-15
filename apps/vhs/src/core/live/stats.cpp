/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the measurements (see stats.hpp).
 */
#include "stats.hpp"

#include "camera/camera.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

extern char **environ;

namespace vhs {

Summary summarize(std::vector<double> v)
{
    Summary s;
    if (v.empty()) return s;
    s.n = static_cast<int>(v.size());
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    std::sort(v.begin(), v.end());
    const std::size_t rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(v.size())));
    s.p95 = v[std::min(v.size() - 1, rank > 0 ? rank - 1 : 0)];
    s.max = v.back();
    return s;
}

void SampleSet::add(double ms)
{
    std::lock_guard<std::mutex> lock(m_);
    if (v_.size() < 100000) v_.push_back(ms);
}

std::vector<double> SampleSet::take()
{
    std::lock_guard<std::mutex> lock(m_);
    std::vector<double> out;
    out.swap(v_);
    return out;
}

bool parse_stat_ticks(const std::string &text, std::uint64_t &ticks)
{
    const std::size_t close = text.rfind(')');
    if (close == std::string::npos) return false;
    std::istringstream in(text.substr(close + 1));
    std::string field;
    /* after ')': field 3 (state) is the first; utime = field 14, stime = field 15 */
    std::uint64_t ut = 0, st = 0;
    for (int i = 3; i <= 15; ++i) {
        if (!(in >> field)) return false;
        if (i == 14 || i == 15) {
            char *end = nullptr;
            const unsigned long long v = std::strtoull(field.c_str(), &end, 10);
            if (!end || *end) return false;
            (i == 14 ? ut : st) = v;
        }
    }
    ticks = ut + st;
    return true;
}

bool parse_vmrss_kb(const std::string &text, long &kb)
{
    const std::size_t at = text.find("VmRSS:");
    if (at == std::string::npos) return false;
    char *end = nullptr;
    const long v = std::strtol(text.c_str() + at + 6, &end, 10);
    if (end == text.c_str() + at + 6) return false;
    kb = v;
    return true;
}

bool parse_millideg(const std::string &text, double &c)
{
    char *end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str()) return false;
    c = static_cast<double>(v) / 1000.0;
    return true;
}

bool parse_throttled(const std::string &text0, std::uint32_t &bits)
{
    std::string t = text0;
    const std::size_t eq = t.find('=');
    if (eq != std::string::npos) {
        std::string k = t.substr(0, eq);
        k.erase(std::remove_if(k.begin(), k.end(), [](char c) { return c == ' ' || c == '\t'; }), k.end());
        if (k != "throttled") return false;
        t = t.substr(eq + 1);
    }
    std::size_t i = 0;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
    if (i == t.size()) return false;
    char *end = nullptr;
    const unsigned long v = std::strtoul(t.c_str() + i, &end, 16);
    if (end == t.c_str() + i) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end) return false;
    bits = static_cast<std::uint32_t>(v);
    return true;
}

double cores_between(std::uint64_t t0, std::uint64_t t1, std::int64_t us0, std::int64_t us1, long hz)
{
    if (us1 <= us0 || hz <= 0 || t1 < t0) return -1;
    return (static_cast<double>(t1 - t0) / static_cast<double>(hz)) / (static_cast<double>(us1 - us0) / 1e6);
}

std::string throttled_text(std::uint32_t bits)
{
    if (bits == 0) return "ok";
    static const struct {
        std::uint32_t bit;
        const char *name;
    } kBits[] = {{0x1, "under-voltage now"}, {0x2, "freq-capped now"}, {0x4, "throttled now"}, {0x8, "temp-limit now"},
                 {0x10000, "under-voltage since boot"}, {0x20000, "freq-capped since boot"},
                 {0x40000, "throttled since boot"}, {0x80000, "temp-limit since boot"}};
    std::string out;
    for (const auto &b : kBits)
        if (bits & b.bit) out += (out.empty() ? "" : ", ") + std::string(b.name);
    return out.empty() ? "other bits" : out;
}

std::string read_text(const std::string &path, std::size_t max)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string s(max, '\0');
    f.read(&s[0], static_cast<std::streamsize>(max));
    s.resize(static_cast<std::size_t>(f.gcount()));
    return s;
}

SystemProbe::SystemProbe(std::string root) : root_(std::move(root))
{
    const long hz = sysconf(_SC_CLK_TCK);
    if (hz > 0) hz_ = hz;
    /* the thermal zone: the first whose type names the CPU (cpu-thermal on a Pi, x86_pkg_temp on a PC), else zone 0 */
    const std::string tz = root_ + "/sys/class/thermal";
    std::string first;
    for (const std::string &n : list_dir(tz)) {
        if (n.compare(0, 12, "thermal_zone") != 0) continue;
        const std::string base = tz + "/" + n;
        if (read_text(base + "/temp", 64).empty()) continue;
        if (first.empty()) first = base + "/temp";
        std::string type = read_text(base + "/type", 64);
        if (type.find("cpu") != std::string::npos || type.find("x86_pkg") != std::string::npos ||
            type.find("soc") != std::string::npos) {
            thermal_ = base + "/temp";
            break;
        }
    }
    if (thermal_.empty()) thermal_ = first;
    /* the firmware's get_throttled node: /sys/devices/platform/soc/soc:firmware/get_throttled on most Pi kernels */
    const std::string plat = root_ + "/sys/devices/platform";
    for (const std::string &n : list_dir(plat)) {
        const std::string base = plat + "/" + n;
        struct stat st;
        if (n.find("firmware") != std::string::npos && stat((base + "/get_throttled").c_str(), &st) == 0) {
            throttle_node_ = base + "/get_throttled";
            break;
        }
        if (n.compare(0, 3, "soc") != 0) continue;
        for (const std::string &c : list_dir(base)) {
            if (stat((base + "/" + c + "/get_throttled").c_str(), &st) == 0) {
                throttle_node_ = base + "/" + c + "/get_throttled";
                break;
            }
        }
        if (!throttle_node_.empty()) break;
    }
    /* the rpi_volt hwmon (Pi kernels): in0_lcrit_alarm is 1 while the supply is under-voltage */
    const std::string hw = root_ + "/sys/class/hwmon";
    for (const std::string &n : list_dir(hw)) {
        const std::string base = hw + "/" + n;
        std::string name = read_text(base + "/name", 64);
        while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back();
        if (name == "rpi_volt" && !read_text(base + "/in0_lcrit_alarm", 16).empty()) {
            uv_alarm_ = base + "/in0_lcrit_alarm";
            break;
        }
    }
    if (!throttle_node_.empty()) {
        throttle_src_ = throttle_node_;
    } else if (root_.empty()) {
        for (const char *p : {"/usr/bin/vcgencmd", "/opt/vc/bin/vcgencmd"}) {
            if (access(p, X_OK) == 0) {
                vcgencmd_ = p;
                throttle_src_ = std::string(p) + " get_throttled";
                break;
            }
        }
    }
}

bool SystemProbe::process_ticks(std::uint64_t &ticks) const
{
    return parse_stat_ticks(read_text(root_ + "/proc/self/stat", 4096), ticks);
}

bool SystemProbe::thread_ticks(int tid, std::uint64_t &ticks) const
{
    if (tid <= 0) return false;
    return parse_stat_ticks(read_text(root_ + "/proc/self/task/" + std::to_string(tid) + "/stat", 4096), ticks);
}

double SystemProbe::rss_mb() const
{
    long kb = 0;
    if (!parse_vmrss_kb(read_text(root_ + "/proc/self/status", 16384), kb)) return -1;
    return static_cast<double>(kb) / 1024.0;
}

bool SystemProbe::temp_c(double &c) const
{
    return !thermal_.empty() && parse_millideg(read_text(thermal_, 64), c);
}

bool SystemProbe::throttled(std::uint32_t &bits, std::int64_t now_us)
{
    if (!throttle_node_.empty()) return parse_throttled(read_text(throttle_node_, 64), bits);
    if (vcgencmd_.empty()) return false;
    if (now_us >= next_vcgencmd_us_ && vcgencmd_failures_ < 3) {
        next_vcgencmd_us_ = now_us + 5000000;
        std::string out;
        std::uint32_t b = 0;
        if (run_capture_output({vcgencmd_, "get_throttled"}, 2000, out) && parse_throttled(out, b)) {
            last_bits_ = b;
            have_last_ = true;
            vcgencmd_failures_ = 0;
        } else if (++vcgencmd_failures_ >= 3) {
            std::fprintf(stderr, "[vhs] %s get_throttled fails: given up\n", vcgencmd_.c_str());
        }
    }
    if (!have_last_) return false;
    bits = last_bits_;
    return true;
}

bool SystemProbe::undervolt_alarm(bool &on) const
{
    if (uv_alarm_.empty()) return false;
    const std::string t = read_text(uv_alarm_, 16);
    if (t.empty()) return false;
    on = t[0] == '1';
    return true;
}

bool run_capture_output(const std::vector<std::string> &argv, int timeout_ms, std::string &out)
{
    if (argv.empty()) return false;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    std::vector<char *> args;
    for (const std::string &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return false;
    }
    out.clear();
    bool timed_out = false;
    char buf[512];
    for (;;) {
        struct pollfd p;
        p.fd = fds[0];
        p.events = POLLIN;
        p.revents = 0;
        const int r = poll(&p, 1, timeout_ms);
        if (r == 0) {
            timed_out = true;
            break;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        if (out.size() < 4096) out.append(buf, static_cast<std::size_t>(n));
    }
    close(fds[0]);
    if (timed_out) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

namespace {

std::string fmt_ms(const Summary &s)
{
    char buf[48];
    if (s.n == 0) return "-";
    std::snprintf(buf, sizeof buf, "%.1f/%.1f", s.mean, s.p95);
    return buf;
}

std::string fmt_num(const char *f, double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

} // namespace

std::string format_log(const Interval &iv)
{
    std::string s = "[vhs] stats";
    s += " state=" + iv.state;
    if (!iv.mode.empty()) s += " mode=\"" + iv.mode + "\"";
    if (!iv.scale.empty()) s += " scale=\"" + iv.scale + "\"";
    s += " fmt=" + iv.fmt + " box=" + iv.box;
    if (iv.paused) s += " paused";
    s += fmt_num(" cap=%.1f", iv.cap_fps) + fmt_num(" dec=%.1f", iv.dec_fps) + fmt_num(" shown=%.1f", iv.shown_fps);
    s += " drop=dec:" + std::to_string(iv.drop_dec) + ",ui:" + std::to_string(iv.drop_ui) +
         ",cam:" + std::to_string(iv.gaps);
    s += " err=" + std::to_string(iv.errors) + " decfail=" + std::to_string(iv.dec_fail);
    s += " decode_ms=" + fmt_ms(iv.dec_ms) + " convert_ms=" + fmt_ms(iv.conv_ms) + " ui_ms=" + fmt_ms(iv.ui_ms) +
         " render_ms=" + fmt_ms(iv.render_ms) + " lat_ms=" + fmt_ms(iv.lat_ms);
    if (iv.cores >= 0) {
        s += fmt_num(" cpu=%.2f", iv.cores);
        s += " threads=cap:" + (iv.cap_cores >= 0 ? fmt_num("%.2f", iv.cap_cores) : std::string("-")) +
             ",dec:" + (iv.dec_cores >= 0 ? fmt_num("%.2f", iv.dec_cores) : std::string("-")) +
             ",ui:" + (iv.ui_cores >= 0 ? fmt_num("%.2f", iv.ui_cores) : std::string("-"));
    }
    if (iv.rss_mb >= 0) s += fmt_num(" rss_mb=%.1f", iv.rss_mb);
    if (iv.have_temp) s += fmt_num(" temp_c=%.1f", iv.temp_c);
    if (iv.have_thr) {
        char b[32];
        std::snprintf(b, sizeof b, " throttled=0x%x", iv.thr);
        s += b;
    }
    if (iv.have_uv) s += iv.uv ? " uv=1" : " uv=0";
    return s;
}

std::vector<std::string> format_overlay(const Interval &iv)
{
    std::vector<std::string> l;
    l.push_back(iv.mode.empty() ? iv.state : iv.mode + (iv.paused ? "  PAUSED" : ""));
    l.push_back("scale " + iv.scale + "  " + iv.fmt);
    char b[160];
    std::snprintf(b, sizeof b, "fps cap %.1f  dec %.1f  shown %.1f", iv.cap_fps, iv.dec_fps, iv.shown_fps);
    l.push_back(b);
    std::snprintf(b, sizeof b, "drop dec %llu  ui %llu  cam %llu  err %llu", static_cast<unsigned long long>(iv.drop_dec),
                  static_cast<unsigned long long>(iv.drop_ui), static_cast<unsigned long long>(iv.gaps),
                  static_cast<unsigned long long>(iv.errors + iv.dec_fail));
    l.push_back(b);
    l.push_back("decode " + fmt_ms(iv.dec_ms) + "  conv " + fmt_ms(iv.conv_ms) + " ms");
    l.push_back("ui " + fmt_ms(iv.ui_ms) + " (draw " + fmt_ms(iv.render_ms) + ")  lat " + fmt_ms(iv.lat_ms));
    if (iv.cores >= 0) {
        std::snprintf(b, sizeof b, "cpu %.2f (cap %.2f dec %.2f ui %.2f)", iv.cores, std::max(0.0, iv.cap_cores),
                      std::max(0.0, iv.dec_cores), std::max(0.0, iv.ui_cores));
        l.push_back(b);
    }
    std::string sys;
    if (iv.rss_mb >= 0) sys += fmt_num("rss %.0f MB", iv.rss_mb);
    if (iv.have_temp) sys += fmt_num("  %.1f C", iv.temp_c);
    if (iv.have_thr) {
        std::snprintf(b, sizeof b, "  thr 0x%x", iv.thr);
        sys += b;
    }
    if (!sys.empty()) l.push_back(sys);
    return l;
}

} // namespace vhs
