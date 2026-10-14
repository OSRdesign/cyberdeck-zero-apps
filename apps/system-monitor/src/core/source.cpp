/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the system source and the fixture source (see source.hpp).
 */
#include "source.hpp"

#include "parsers.hpp"
#include "text.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sm {
namespace {

constexpr std::size_t kMaxRead = 1 << 20;

std::string read_path(const std::string &path, bool *ok)
{
    std::string out;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (ok) *ok = false;
        return out;
    }
    char buf[4096];
    bool good = true;
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            good = false;
            break;
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() >= kMaxRead) break;
    }
    close(fd);
    if (ok) *ok = good;
    return out;
}

std::vector<std::string> list_path(const std::string &dir)
{
    std::vector<std::string> names;
    DIR *d = opendir(dir.c_str());
    if (!d) return names;
    while (struct dirent *e = readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}

std::int64_t mono_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ the system */

class SystemSource : public Source {
public:
    std::string read(const std::string &path, bool *ok) override { return read_path(path, ok); }
    bool read_into(const std::string &path, std::string &out) override
    {
        out.clear();
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        bool good = true;
        for (;;) {
            if (out.capacity() - out.size() < 1024) out.reserve(out.size() + 4096); /* grows once, then stays */
            const std::size_t room = out.capacity() - out.size();
            const std::size_t at = out.size();
            out.resize(out.capacity()); /* within the capacity: no allocation */
            const ssize_t n = ::read(fd, &out[at], room);
            if (n < 0) {
                out.resize(at);
                if (errno == EINTR) continue;
                good = false;
                break;
            }
            out.resize(at + static_cast<std::size_t>(n));
            if (n == 0 || out.size() >= kMaxRead) break;
        }
        close(fd);
        return good;
    }
    bool exists(const std::string &path) override { return access(path.c_str(), F_OK) == 0; }
    std::vector<std::string> list(const std::string &dir) override { return list_path(dir); }
    void list_into(const std::string &dir, std::vector<std::string> &out) override
    {
        std::size_t n = 0;
        DIR *d = opendir(dir.c_str());
        if (d) {
            while (struct dirent *e = readdir(d)) {
                if (e->d_name[0] == '.' && (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
                    continue;
                if (n == out.size()) out.emplace_back();
                out[n++].assign(e->d_name);
            }
            closedir(d);
        }
        out.resize(n);
    }
    CmdResult run(const std::vector<std::string> &argv, int timeout_ms) override
    {
        return run_command(argv, timeout_ms);
    }
    bool fs_usage(const std::string &dir, std::uint64_t &total, std::uint64_t &used, std::uint64_t &avail) override
    {
        struct statvfs v;
        if (statvfs(dir.c_str(), &v) != 0 || v.f_blocks == 0) return false;
        const std::uint64_t unit = v.f_frsize ? v.f_frsize : v.f_bsize;
        total = static_cast<std::uint64_t>(v.f_blocks) * unit;
        used = static_cast<std::uint64_t>(v.f_blocks - v.f_bfree) * unit;
        avail = static_cast<std::uint64_t>(v.f_bavail) * unit;
        return true;
    }
    std::map<std::string, std::string> ipv4() override
    {
        std::map<std::string, std::string> out;
        struct ifaddrs *list = nullptr;
        if (getifaddrs(&list) != 0) return out;
        for (struct ifaddrs *a = list; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !a->ifa_name) continue;
            if (out.count(a->ifa_name)) continue;
            char buf[INET_ADDRSTRLEN] = {};
            const auto *sin = reinterpret_cast<const struct sockaddr_in *>(a->ifa_addr);
            if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
            int prefix = 0;
            if (a->ifa_netmask) {
                const auto *mask = reinterpret_cast<const struct sockaddr_in *>(a->ifa_netmask);
                prefix = __builtin_popcount(ntohl(mask->sin_addr.s_addr));
            }
            out[a->ifa_name] = std::string(buf) + "/" + std::to_string(prefix);
        }
        freeifaddrs(list);
        return out;
    }
    bool root_dev(unsigned &maj, unsigned &min) override
    {
        struct stat st;
        if (stat("/", &st) != 0) return false;
        maj = major(st.st_dev);
        min = minor(st.st_dev);
        return true;
    }
    long page_size() override
    {
        const long p = sysconf(_SC_PAGESIZE);
        return p > 0 ? p : 4096;
    }
    std::int64_t now_ms() override { return mono_ms(); }
    std::string describe() const override { return "system"; }
};

/* ------------------------------------------------------------------ a fixture folder */

struct Range {
    long from = 0, to = 0; /* samples, inclusive */
};

struct Rate {
    std::string name;
    double a = 0, b = 0; /* rx / tx or read / write, bytes per second */
};

struct FakeProc {
    int pid = 0;
    std::string name;
    char state = 'S';
    double cpu = 0;   /* % of one core */
    double rss_kb = 0;
};

struct Scenario {
    int cores = 4;
    std::vector<double> cpu;
    double wave = 10;
    int freq_mhz = 0;
    double temp_start = 50, temp_end = 50;
    double ramp = 60;
    std::vector<Range> flags[4]; /* uv, capped, throttled, softlimit */
    std::uint32_t sticky = 0;
    std::vector<Rate> net, disk;
    double mem_wave_mb = 0;
    double uptime0 = 3600;
    bool no_commands = false; /* no vcgencmd, no iw */
    bool no_hwmon = false;    /* no rpi_volt alarm, no fan speed */
    bool offline = false;     /* every interface down, no address, no route */
    std::vector<FakeProc> procs;
};

std::vector<Range> parse_ranges(const std::string &v)
{
    std::vector<Range> out;
    std::string item;
    for (const char c : v + ",") {
        if (c != ',') {
            item += c;
            continue;
        }
        const std::size_t dash = item.find('-');
        std::int64_t a = 0, b = 0;
        if (dash != std::string::npos && parse_i64(item.substr(0, dash), a) && parse_i64(item.substr(dash + 1), b))
            out.push_back({static_cast<long>(a), static_cast<long>(b)});
        item.clear();
    }
    return out;
}

std::vector<std::string> split_on(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

Scenario parse_scenario(const std::string &text)
{
    Scenario s;
    for (const std::string &raw : split_lines(text)) {
        const std::string line = trim(raw.substr(0, raw.find('#')));
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        double d = 0;
        if (key == "cores" && parse_double(v, d)) s.cores = std::max(1, static_cast<int>(d));
        else if (key == "cpu") {
            s.cpu.clear();
            for (const std::string &p : split_on(v, ','))
                if (parse_double(p, d)) s.cpu.push_back(d);
        } else if (key == "wave" && parse_double(v, d)) s.wave = d;
        else if (key == "freq" && parse_double(v, d)) s.freq_mhz = static_cast<int>(d);
        else if (key == "temp") {
            const std::vector<std::string> p = split_on(v, ':');
            if (p.size() == 2) {
                parse_double(p[0], s.temp_start);
                parse_double(p[1], s.temp_end);
            }
        } else if (key == "ramp" && parse_double(v, d)) s.ramp = std::max(1.0, d);
        else if (key == "uv") s.flags[0] = parse_ranges(v);
        else if (key == "capped") s.flags[1] = parse_ranges(v);
        else if (key == "throttled") s.flags[2] = parse_ranges(v);
        else if (key == "softlimit") s.flags[3] = parse_ranges(v);
        else if (key == "sticky") parse_hex(v, s.sticky);
        else if (key == "net" || key == "disk") {
            const std::vector<std::string> p = split_on(v, ':');
            Rate r;
            if (p.size() == 3 && parse_double(p[1], r.a) && parse_double(p[2], r.b)) {
                r.name = p[0];
                (key == "net" ? s.net : s.disk).push_back(r);
            }
        } else if (key == "mem_wave" && parse_double(v, d)) s.mem_wave_mb = d;
        else if (key == "uptime" && parse_double(v, d)) s.uptime0 = d;
        else if (key == "no_commands") s.no_commands = v == "1";
        else if (key == "no_hwmon") s.no_hwmon = v == "1";
        else if (key == "offline") s.offline = v == "1";
        else if (key == "proc") {
            const std::vector<std::string> f = split_ws(v);
            FakeProc p;
            std::int64_t pid = 0;
            if (f.size() == 5 && parse_i64(f[0], pid) && parse_double(f[3], p.cpu) && parse_double(f[4], p.rss_kb)) {
                p.pid = static_cast<int>(pid);
                p.name = f[1];
                p.state = f[2].empty() ? 'S' : f[2][0];
                s.procs.push_back(p);
            }
        }
    }
    while (static_cast<int>(s.cpu.size()) < s.cores) s.cpu.push_back(s.cpu.empty() ? 10.0 : s.cpu.back());
    return s;
}

bool in_ranges(const std::vector<Range> &ranges, long t)
{
    for (const Range &r : ranges)
        if (t >= r.from && t <= r.to) return true;
    return false;
}

class FixtureSource : public Source {
public:
    FixtureSource(const std::string &dir, const std::string &scenario) : dir_(dir), name_(scenario)
    {
        while (dir_.size() > 1 && dir_.back() == '/') dir_.pop_back();
        if (!scenario.empty()) {
            bool ok = false;
            const std::string text = read_path(dir_ + "/scenario-" + scenario + ".conf", &ok);
            if (ok) {
                scen_ = true;
                sc_ = parse_scenario(text);
                core_.assign(static_cast<std::size_t>(sc_.cores), Core());
                for (std::size_t i = 0; i < core_.size(); ++i) {
                    core_[i].user = 200000 + 1000.0 * static_cast<double>(i);
                    core_[i].system = 60000;
                    core_[i].idle = 3000000;
                }
                for (const Rate &r : sc_.net) net_[r.name] = {5.0e8, 4.0e7};
                for (const Rate &r : sc_.disk) disk_[r.name] = {9.0e8, 3.0e8};
                for (const FakeProc &p : sc_.procs) ticks_[p.pid] = 500.0 + p.cpu * 40.0;
            }
        }
    }

    std::string read(const std::string &path, bool *ok) override
    {
        if (scen_) {
            std::string out;
            if (generate(path, out)) {
                if (ok) *ok = true;
                return out;
            }
        }
        return file(path, ok);
    }
    bool exists(const std::string &path) override
    {
        if (scen_ && is_proc_path(path)) return true;
        return there(path);
    }
    std::vector<std::string> list(const std::string &dir) override
    {
        if (scen_ && sc_.no_hwmon && starts_with(dir, "/sys/class/hwmon")) return {};
        std::vector<std::string> names = dir_list(dir);
        if (scen_ && (dir == "/proc" || dir == "/proc/")) {
            for (const FakeProc &p : sc_.procs) names.push_back(std::to_string(p.pid));
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
        }
        return names;
    }
    CmdResult run(const std::vector<std::string> &argv, int) override
    {
        CmdResult r;
        std::string name;
        for (const std::string &a : argv) name += (name.empty() ? "" : "_") + a;
        bool ok = false;
        std::string text = file("/cmd/" + name, &ok);
        if (!ok || (scen_ && sc_.no_commands)) {
            r.error = ENOENT;
            return r;
        }
        r.started = true;
        r.exit_code = 0;
        if (starts_with(text, "exit=")) {
            const std::size_t nl = text.find('\n');
            std::int64_t code = 0;
            if (parse_i64(text.substr(5, nl == std::string::npos ? std::string::npos : nl - 5), code))
                r.exit_code = static_cast<int>(code);
            text = nl == std::string::npos ? std::string() : text.substr(nl + 1);
        }
        if (scen_ && name == "vcgencmd_get_throttled" && r.exit_code == 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "throttled=0x%x\n", throttle_bits());
            text = buf;
        }
        r.output = text;
        return r;
    }
    bool fs_usage(const std::string &dir, std::uint64_t &total, std::uint64_t &used, std::uint64_t &avail) override
    {
        for (const std::string &line : split_lines(file("/statvfs", nullptr))) {
            const std::vector<std::string> f = split_ws(line);
            if (f.size() < 4) continue;
            std::string name; /* the mount point may hold blanks: the three numbers are the last fields */
            for (std::size_t i = 0; i + 3 < f.size(); ++i) name += (i ? " " : "") + f[i];
            const std::size_t n = f.size();
            if (name == dir && parse_u64(f[n - 3], total) && parse_u64(f[n - 2], used) && parse_u64(f[n - 1], avail))
                return true;
        }
        return false;
    }
    std::map<std::string, std::string> ipv4() override
    {
        std::map<std::string, std::string> out;
        if (scen_ && sc_.offline) return out;
        for (const std::string &line : split_lines(file("/ipv4", nullptr))) {
            const std::vector<std::string> f = split_ws(line);
            if (f.size() == 2 && !out.count(f[0])) out[f[0]] = f[1];
        }
        return out;
    }
    bool root_dev(unsigned &maj, unsigned &min) override
    {
        const std::string t = trim(file("/rootdev", nullptr));
        const std::size_t colon = t.find(':');
        std::uint64_t a = 0, b = 0;
        if (colon == std::string::npos || !parse_u64(t.substr(0, colon), a) || !parse_u64(t.substr(colon + 1), b))
            return false;
        maj = static_cast<unsigned>(a);
        min = static_cast<unsigned>(b);
        return true;
    }
    long page_size() override
    {
        std::uint64_t p = 0;
        return parse_u64(file("/pagesize", nullptr), p) && p > 0 ? static_cast<long>(p) : 4096;
    }
    /* a fixture's time is virtual: one period per sample (the rates of two snapshots are per period) */
    std::int64_t now_ms() override { return vt_ms_; }
    void begin_sample(int period_ms) override
    {
        ++tick_;
        if (tick_ == 0) return; /* the first sample: the counters as they start */
        vt_ms_ += period_ms;
        if (!scen_) return;
        const double dt = period_ms / 1000.0;
        const double hz = 100.0;
        for (std::size_t c = 0; c < core_.size(); ++c) {
            const double busy = busy_of(static_cast<int>(c));
            core_[c].user += busy * 0.8 * hz * dt / 100.0;
            core_[c].system += busy * 0.2 * hz * dt / 100.0;
            core_[c].idle += (100.0 - busy) * hz * dt / 100.0;
        }
        const double t = static_cast<double>(tick_);
        for (const Rate &r : sc_.net) {
            net_[r.name].first += r.a * (1.0 + 0.6 * std::sin(t / 6.0)) * dt;
            net_[r.name].second += r.b * (1.0 + 0.5 * std::sin(t / 4.0 + 1.0)) * dt;
        }
        for (const Rate &r : sc_.disk) {
            disk_[r.name].first += r.a * std::max(0.0, 1.0 + 0.9 * std::sin(t / 9.0)) * dt;
            disk_[r.name].second += r.b * std::max(0.0, 1.0 + 0.9 * std::sin(t / 7.0 + 2.0)) * dt;
        }
        for (const FakeProc &p : sc_.procs)
            ticks_[p.pid] += p.cpu * (1.0 + 0.3 * std::sin(t / 5.0 + p.pid)) * hz * dt / 100.0;
    }
    bool virtual_time() const override { return scen_; }
    std::string describe() const override
    {
        return "fixture " + dir_ + (scen_ ? ", scenario " + name_ : std::string());
    }

private:
    struct Core {
        double user = 0, system = 0, idle = 0;
    };

    double busy_of(int core) const
    {
        const double t = static_cast<double>(tick_);
        const double v = sc_.cpu[static_cast<std::size_t>(core)] +
                         sc_.wave * std::sin(6.2831853 * (t + 7.0 * core) / 37.0);
        return std::min(100.0, std::max(0.0, v));
    }

    std::uint32_t throttle_bits() const
    {
        std::uint32_t bits = sc_.sticky;
        for (int f = 0; f < 4; ++f) {
            if (in_ranges(sc_.flags[f], tick_)) bits |= (1u << f) | (1u << (16 + f));
            for (const Range &r : sc_.flags[f])
                if (tick_ > r.to) bits |= 1u << (16 + f); /* happened earlier: the sticky bit stays */
        }
        return bits;
    }

    bool is_proc_path(const std::string &path) const
    {
        int pid = 0;
        return proc_pid(path, pid);
    }

    bool proc_pid(const std::string &path, int &pid) const
    {
        if (!starts_with(path, "/proc/")) return false;
        const std::size_t slash = path.find('/', 6);
        if (slash == std::string::npos || path.substr(slash) != "/stat") return false;
        std::uint64_t p = 0;
        if (!parse_u64(path.substr(6, slash - 6), p)) return false;
        for (const FakeProc &fp : sc_.procs)
            if (fp.pid == static_cast<int>(p)) {
                pid = fp.pid;
                return true;
            }
        return false;
    }

    bool generate(const std::string &path, std::string &out)
    {
        if (sc_.offline) { /* every interface down, no route, no Wi-Fi association */
            if (starts_with(path, "/sys/class/net/") && path.size() > 10 &&
                path.compare(path.size() - 10, 10, "/operstate") == 0) {
                out = "down\n";
                return true;
            }
            if (path == "/proc/net/route") {
                out = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n";
                return true;
            }
            if (path == "/proc/net/wireless") {
                out = "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n";
                return true;
            }
        }
        char buf[512];
        if (path == "/proc/stat") {
            Core all;
            std::string cores;
            for (std::size_t c = 0; c < core_.size(); ++c) {
                all.user += core_[c].user;
                all.system += core_[c].system;
                all.idle += core_[c].idle;
                std::snprintf(buf, sizeof(buf), "cpu%zu %.0f 0 %.0f %.0f 0 0 0 0 0 0\n", c, core_[c].user,
                              core_[c].system, core_[c].idle);
                cores += buf;
            }
            std::snprintf(buf, sizeof(buf), "cpu  %.0f 0 %.0f %.0f 0 0 0 0 0 0\n", all.user, all.system, all.idle);
            out = buf + cores + "intr 0\nctxt 123456\nbtime 1760000000\nprocesses 4321\nprocs_running 2\n";
            return true;
        }
        if (path == "/proc/uptime") {
            std::snprintf(buf, sizeof(buf), "%.2f %.2f\n", sc_.uptime0 + vt_ms_ / 1000.0,
                          (sc_.uptime0 + vt_ms_ / 1000.0) * sc_.cores * 0.8);
            out = buf;
            return true;
        }
        if (path == "/proc/loadavg") {
            double sum = 0;
            for (int c = 0; c < sc_.cores; ++c) sum += busy_of(c) / 100.0;
            std::snprintf(buf, sizeof(buf), "%.2f %.2f %.2f 2/%zu 4321\n", sum, sum * 0.9, sum * 0.8,
                          sc_.procs.size() + 96);
            out = buf;
            return true;
        }
        if (path == "/sys/class/thermal/thermal_zone0/temp") {
            const double t = static_cast<double>(tick_);
            const double c = sc_.temp_start + (sc_.temp_end - sc_.temp_start) * std::min(1.0, t / sc_.ramp) +
                             0.4 * std::sin(t / 5.0);
            std::snprintf(buf, sizeof(buf), "%.0f\n", c * 1000.0);
            out = buf;
            return true;
        }
        if (path == "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq" && sc_.freq_mhz > 0) {
            std::snprintf(buf, sizeof(buf), "%d\n", sc_.freq_mhz * 1000);
            out = buf;
            return true;
        }
        if (path == "/proc/net/dev" && !sc_.net.empty()) {
            out = "Inter-|   Receive                                                |  Transmit\n"
                  " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop "
                  "fifo colls carrier compressed\n"
                  "    lo:  1234567    9876    0    0    0     0          0         0  1234567    9876    0    0    "
                  "0     0       0          0\n";
            for (const Rate &r : sc_.net) {
                const auto &c = net_.at(r.name);
                std::snprintf(buf, sizeof(buf),
                              "%6s: %.0f %.0f 0 0 0 0 0 0 %.0f %.0f 0 0 0 0 0 0\n", r.name.c_str(), c.first,
                              c.first / 900.0, c.second, c.second / 700.0);
                out += buf;
            }
            return true;
        }
        if (path == "/proc/diskstats" && !sc_.disk.empty()) {
            unsigned maj = 179, min = 2;
            root_dev(maj, min);
            out.clear();
            for (const Rate &r : sc_.disk) {
                const auto &c = disk_.at(r.name);
                const double rs = c.first / 512.0, ws = c.second / 512.0;
                const bool digit_end = !r.name.empty() && r.name.back() >= '0' && r.name.back() <= '9';
                const std::string part = r.name + (digit_end ? "p" : "") + std::to_string(min);
                std::snprintf(buf, sizeof(buf), " %4u %7u %s %.0f 0 %.0f 0 %.0f 0 %.0f 0 0 0 0 0 0 0 0 0 0\n", maj,
                              0u, r.name.c_str(), rs / 8, rs, ws / 8, ws);
                out += buf;
                std::snprintf(buf, sizeof(buf), " %4u %7u %s %.0f 0 %.0f 0 %.0f 0 %.0f 0 0 0 0 0 0 0 0 0 0\n", maj,
                              min, part.c_str(), rs / 8, rs, ws / 8, ws);
                out += buf;
            }
            return true;
        }
        if (path == "/proc/meminfo" && sc_.mem_wave_mb > 0) {
            bool ok = false;
            const std::string base = file(path, &ok);
            if (!ok) return false;
            const double shift_kb = sc_.mem_wave_mb * 1024.0 * std::sin(static_cast<double>(tick_) / 11.0);
            out.clear();
            for (const std::string &line : split_lines(base)) {
                const std::vector<std::string> f = split_ws(line);
                std::uint64_t v = 0;
                if (f.size() >= 2 && (f[0] == "MemAvailable:" || f[0] == "MemFree:") && parse_u64(f[1], v)) {
                    const double nv = std::max(0.0, static_cast<double>(v) - shift_kb);
                    std::snprintf(buf, sizeof(buf), "%-16s%8.0f kB", f[0].c_str(), nv);
                    out += buf;
                } else {
                    out += line;
                }
                out += "\n";
            }
            return true;
        }
        int pid = 0;
        if (proc_pid(path, pid)) {
            for (const FakeProc &p : sc_.procs) {
                if (p.pid != pid) continue;
                const double t = static_cast<double>(tick_);
                const long pages = static_cast<long>(p.rss_kb * (1.0 + 0.02 * std::sin(t / 13.0 + pid)) /
                                                     (static_cast<double>(page_size()) / 1024.0));
                const double ticks = ticks_.at(pid);
                std::snprintf(buf, sizeof(buf),
                              "%d (%s) %c 1 %d %d 0 -1 4194560 100 0 0 0 %.0f %.0f 0 0 20 0 1 0 100 %ld %ld "
                              "18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
                              p.pid, p.name.c_str(), p.state, p.pid, p.pid, ticks * 0.7, ticks * 0.3,
                              pages * 4096L * 3, pages);
                out = buf;
                return true;
            }
        }
        return false;
    }

    /* the fixture's files never change: each is read once (a fixture on /mnt/c is slow to open from WSL) */
    std::string file(const std::string &rel, bool *ok)
    {
        auto it = files_.find(rel);
        if (it == files_.end()) {
            bool good = false;
            std::string text = read_path(dir_ + rel, &good);
            it = files_.emplace(rel, std::make_pair(good, std::move(text))).first;
        }
        if (ok) *ok = it->second.first;
        return it->second.second;
    }
    bool there(const std::string &rel)
    {
        auto it = exists_.find(rel);
        if (it == exists_.end()) it = exists_.emplace(rel, access((dir_ + rel).c_str(), F_OK) == 0).first;
        return it->second;
    }
    std::vector<std::string> dir_list(const std::string &rel)
    {
        auto it = lists_.find(rel);
        if (it == lists_.end()) it = lists_.emplace(rel, list_path(dir_ + rel)).first;
        return it->second;
    }

    std::map<std::string, std::pair<bool, std::string>> files_;
    std::map<std::string, bool> exists_;
    std::map<std::string, std::vector<std::string>> lists_;
    std::string dir_;
    std::string name_;
    bool scen_ = false;
    Scenario sc_;
    long tick_ = -1;
    std::int64_t vt_ms_ = 0;
    std::vector<Core> core_;
    std::map<std::string, std::pair<double, double>> net_, disk_;
    std::map<int, double> ticks_;
};

} // namespace

std::unique_ptr<Source> make_system_source() { return std::make_unique<SystemSource>(); }

std::unique_ptr<Source> make_fixture_source(const std::string &dir, const std::string &scenario)
{
    return std::make_unique<FixtureSource>(dir, scenario);
}

std::unique_ptr<Source> source_from_env(std::string *described)
{
    const char *env = std::getenv("SYSTEM_MONITOR_FIXTURE");
    if (!env || !env[0]) return nullptr;
    std::string spec = env, scenario;
    const std::size_t colon = spec.rfind(':');
    if (colon != std::string::npos && colon > 0) {
        scenario = spec.substr(colon + 1);
        spec = spec.substr(0, colon);
    }
    std::string dir = spec;
    if (dir[0] != '/') {
#ifdef CDK_APP_DIR
        dir = std::string(CDK_APP_DIR) + "/tests/fixtures/" + spec;
#else
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) dir = std::string(cwd) + "/" + spec;
#endif
    }
    std::unique_ptr<Source> src = make_fixture_source(dir, scenario);
    if (described) *described = src->describe();
    return src;
}

} // namespace sm
